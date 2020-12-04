#include "nexus/dispatcher/delayed_scheduler.h"

#include <glog/logging.h>

#include <algorithm>
#include <boost/asio/post.hpp>
#include <chrono>
#include <memory>
#include <mutex>

#include "nexus/common/metric.h"
#include "nexus/common/model_db.h"
#include "nexus/common/model_def.h"
#include "nexus/common/time_util.h"
#include "nexus/common/typedef.h"

namespace nexus {
namespace dispatcher {
namespace delayed {

namespace {

constexpr uint32_t kCountIntervalSec = 1;
constexpr uint32_t kAvgIntervalSec = 5;

bool OrderQueryContextByDeadlineASC(const std::shared_ptr<QueryContext>& lhs,
                                    const std::shared_ptr<QueryContext>& rhs) {
  return lhs->deadline > rhs->deadline;
}

}  // namespace

QueryContext::QueryContext(QueryProto query_without_input, TimePoint deadline)
    : proto(std::move(query_without_input)),
      global_id(proto.global_id()),
      deadline(deadline) {}

InstanceContext::InstanceContext(ModelSession model_session, NodeId backend_id,
                                 const ModelProfile& profile)
    : model_session(std::move(model_session)),
      backend_id(backend_id),
      profile(profile) {
  max_batch =
      profile.GetMaxBatchWithFullBudget(this->model_session.latency_sla());
}

ModelSessionContext::ModelSessionContext(ModelSession model_session)
    : model_session(std::move(model_session)),
      req_rate(kCountIntervalSec, kAvgIntervalSec) {
  string_id = ModelSessionToString(this->model_session);
  req_counter =
      MetricRegistry::Singleton().CreateIntervalCounter(kCountIntervalSec);
}

double ModelSessionContext::GetRequestRate() {
  for (auto nreq : req_counter->GetHistory()) {
    if (req_rate.rate() < 0 && nreq == 0) {
      continue;
    }
    req_rate.AddSample(nreq);
  }
  return req_rate.rate();
}

BackendContext::BackendContext(NodeId backend_id,
                               std::shared_ptr<BackendDelegate> delegate)
    : backend_id(backend_id),
      delegate(std::move(delegate)),
      next_available_time(std::chrono::nanoseconds(0)) {}

DelayedScheduler::DelayedScheduler(DispatcherAccessor dispatcher)
    : dispatcher_(std::move(dispatcher)),
      io_context_work_guard_(io_context_.get_executor()) {}

void DelayedScheduler::RunAsWorker() { io_context_.run(); }

void DelayedScheduler::Stop() { io_context_work_guard_.reset(); }

void DelayedScheduler::AddModelSession(ModelSession model_session) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Add model session
  auto mctx = std::make_shared<ModelSessionContext>(std::move(model_session));
  if (models_.count(mctx->string_id)) {
    LOG(ERROR) << "Model session already exists. model_session="
               << mctx->string_id;
    return;
  }
  models_[mctx->string_id] = std::move(mctx);

  // Add instances
  auto profile_id = ModelSessionToProfileID(mctx->model_session);
  for (auto& pair : backends_) {
    auto& bctx = pair.second;
    const auto* profile = ModelDatabase::Singleton().GetModelProfile(
        bctx->delegate->gpu_device(), bctx->delegate->gpu_uuid(), profile_id);
    if (!profile) {
      continue;
    }
    auto inst = std::make_shared<InstanceContext>(mctx->model_session,
                                                  bctx->backend_id, *profile);
    mctx->instances[bctx->backend_id] = inst;
    bctx->instances[mctx->string_id] = inst;
  }
}

void DelayedScheduler::AddBackend(NodeId backend_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Add backend
  auto delegate = dispatcher_.GetBackend(backend_id);
  if (!delegate) {
    LOG(ERROR) << "Cannot find backend delegate. backend_id=" << backend_id.t;
    return;
  }
  auto bctx = std::make_shared<BackendContext>(backend_id, delegate);
  if (backends_.count(bctx->backend_id)) {
    LOG(ERROR) << "Backend already exists. backend_id=" << backend_id.t;
    return;
  }
  backends_[backend_id] = std::move(bctx);

  // Add instances
  for (auto& pair : models_) {
    auto& mctx = pair.second;
    auto profile_id = ModelSessionToProfileID(mctx->model_session);
    const auto* profile = ModelDatabase::Singleton().GetModelProfile(
        bctx->delegate->gpu_device(), bctx->delegate->gpu_uuid(), profile_id);
    if (!profile) {
      continue;
    }
    auto inst = std::make_shared<InstanceContext>(mctx->model_session,
                                                  bctx->backend_id, *profile);
    mctx->instances[bctx->backend_id] = inst;
    bctx->instances[mctx->string_id] = inst;
  }
}

void DelayedScheduler::EnqueueQuery(QueryProto query_without_input) {
  ModelSession model_session;
  ParseModelSession(query_without_input.model_session_id(), &model_session);
  auto deadline = TimePoint(
      std::chrono::nanoseconds(query_without_input.clock().frontend_recv_ns()) +
      std::chrono::microseconds(model_session.latency_sla()));
  auto qctx =
      std::make_shared<QueryContext>(std::move(query_without_input), deadline);

  // Add to pending queries
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queries_.count(qctx->global_id)) {
      LOG(ERROR) << "Query already exists. global_id=" << qctx->global_id.t;
      return;
    }
    queries_[qctx->global_id] = qctx;
    auto& mctx = models_.at(qctx->proto.model_session_id());
    mctx->sorted_queries.push_back(qctx);
    std::push_heap(mctx->sorted_queries.begin(), mctx->sorted_queries.end(),
                   OrderQueryContextByDeadlineASC);
  }

  // Trigger full schedule on the worker thread.
  boost::asio::post(io_context_, [this] { WorkFullSchedule(); });
}

void DelayedScheduler::WorkFullSchedule() {}

}  // namespace delayed
}  // namespace dispatcher
}  // namespace nexus
