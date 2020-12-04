#include "nexus/dispatcher/dispatcher.h"

#include "nexus/common/time_util.h"
#include "nexus/common/typedef.h"
#include "nexus/proto/control.pb.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <glog/logging.h>
#include <pthread.h>
#include <sys/socket.h>

#include <algorithm>
#include <boost/asio.hpp>
#include <chrono>
#include <sstream>

#include "nexus/common/config.h"
#include "nexus/common/model_db.h"
#include "nexus/common/model_def.h"
#include "nexus/dispatcher/accessor.h"
#include "nexus/dispatcher/backend_delegate.h"
#include "nexus/dispatcher/frontend_delegate.h"
#include "nexus/dispatcher/inst_info.h"
#include "nexus/dispatcher/session_context.h"

using boost::asio::ip::udp;

namespace {
void PinCpu(pthread_t thread, int cpu) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu, &cpuset);
  int rc = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
  LOG_IF(FATAL, rc != 0) << "Error calling pthread_setaffinity_np: " << rc;
}
}  // namespace

namespace nexus {
namespace dispatcher {

UdpRpcServer::UdpRpcServer(int udp_rpc_port, Dispatcher* dispatcher, int rx_cpu,
                           int worker_cpu)
    : udp_rpc_port_(udp_rpc_port),
      rx_cpu_(rx_cpu),
      worker_cpu_(worker_cpu),
      dispatcher_(dispatcher),
      rx_socket_(io_context_),
      tx_socket_(io_context_) {}

UdpRpcServer::~UdpRpcServer() {
  if (running_) {
    LOG(WARNING) << "Calling Stop() in ~UdpRpcServer()";
    Stop();
  }
}

void UdpRpcServer::Run() {
  rx_socket_.open(udp::v4());
#ifdef SO_REUSEPORT
  typedef boost::asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>
      reuse_port;
  rx_socket_.set_option(reuse_port(true));
#endif
  rx_socket_.bind(udp::endpoint(udp::v4(), udp_rpc_port_));
  tx_socket_.open(udp::v4());
  tx_socket_.bind(udp::endpoint(udp::v4(), 0));

  running_ = true;
  worker_thread_ = std::thread(&UdpRpcServer::WorkerThread, this);
  incoming_request_.reset(new RequestContext);
  AsyncReceive();

  // Pin cpu
  std::stringstream ss;
  ss << "UDP RPC server is listening on " << rx_socket_.local_endpoint();
  if (rx_cpu_ >= 0) {
    PinCpu(pthread_self(), rx_cpu_);
    ss << " (pinned on CPU " << rx_cpu_ << ")";
  }
  ss << " and sending from " << tx_socket_.local_endpoint();
  if (worker_cpu_ >= 0) {
    PinCpu(worker_thread_.native_handle(), worker_cpu_);
    ss << " (pinned on CPU " << worker_cpu_ << ")";
  }
  LOG(INFO) << ss.str();

  // Block until done
  io_context_.run();
}

void UdpRpcServer::Stop() {
  running_ = false;
  io_context_.stop();
  rx_socket_.cancel();
  tx_socket_.cancel();
  worker_thread_.join();
}

void UdpRpcServer::AsyncReceive() {
  rx_socket_.async_receive_from(
      boost::asio::buffer(incoming_request_->buf), incoming_request_->endpoint,
      [this](boost::system::error_code ec, size_t len) {
        if (ec == boost::asio::error::operation_aborted) {
          return;
        }
        if (ec || !len) {
          AsyncReceive();
          return;
        }
        incoming_request_->len = len;
        {
          std::unique_lock<std::mutex> lock(queue_mutex_);
          queue_.emplace_back(std::move(incoming_request_));
          queue_cv_.notify_one();
        }
        incoming_request_.reset(new RequestContext);
        AsyncReceive();
      });
}

void UdpRpcServer::WorkerThread() {
  std::deque<std::unique_ptr<RequestContext>> q;
  while (running_) {
    // Move requests from the global queue to the local queue
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      if (queue_.empty()) {
        // Wait on the CV only when the queue is empty.
        // Hopefully this could reduce the times of context switching.
        queue_cv_.wait(lock, [this] { return !queue_.empty(); });
      }
      while (!queue_.empty()) {
        auto request = std::move(queue_.front());
        queue_.pop_front();
        q.emplace_back(std::move(request));
      }
    }

    // Handle requests
    while (!q.empty()) {
      HandleRequest(std::move(q.front()));
      q.pop_front();
    }
  }
}

namespace {

int ns(const std::chrono::time_point<std::chrono::high_resolution_clock>& x,
       const std::chrono::time_point<std::chrono::high_resolution_clock>& y) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(y - x).count();
}

}  // namespace

void UdpRpcServer::HandleRequest(std::unique_ptr<RequestContext> ctx) {
  auto dispatcher_recv_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch())
          .count();
  DispatchRequest request;
  // Validate request
  bool ok = request.ParseFromString(
      std::string(ctx->buf.data(), ctx->buf.data() + ctx->len));
  if (!ok) {
    LOG_EVERY_N(ERROR, 128)
        << "Bad request. Failed to ParseFromString. Total length = "
        << ctx->len;
    return;
  }
  auto client_endpoint = boost::asio::ip::udp::endpoint(ctx->endpoint.address(),
                                                        request.udp_rpc_port());

  // Handle request
  DispatchReply reply;
  *reply.mutable_model_session() = request.model_session();
  reply.set_query_id(request.query_id());
  QueryProto query;
  query.Swap(request.mutable_query_without_input());
  query.mutable_clock()->set_dispatcher_recv_ns(dispatcher_recv_ns);
  dispatcher_->DispatchRequest(std::move(query), &reply);

  // Send reply. I think using blocking APIs should be okay here?
  auto msg = reply.SerializeAsString();
  if (msg.empty()) {
    LOG(ERROR) << "Failed to reply.SerializeAsString()";
    return;
  }

  auto len = tx_socket_.send_to(boost::asio::buffer(msg), client_endpoint);
  if (len != msg.size()) {
    LOG(WARNING) << "UDP RPC server reply sent " << len << " bytes, expecting "
                 << msg.size() << " bytes";
  }
}

Dispatcher::Dispatcher(std::string rpc_port, int udp_port, int num_udp_threads,
                       std::vector<int> pin_cpus)
    : udp_port_(udp_port),
      num_udp_threads_(num_udp_threads),
      pin_cpus_(std::move(pin_cpus)),
      rpc_service_(this, rpc_port, 1) {
#ifndef SO_REUSEPORT
  CHECK_EQ(num_udp_threads, 1) << "SO_REUSEPORT is not supported. UDP RPC "
                                  "server must be run in single threaded mode.";
#endif
  if (!pin_cpus_.empty()) {
    CHECK_EQ(num_udp_threads_ * 2, pin_cpus_.size())
        << "UDP RPC thread affinity settings should contain exactly twice the "
           "number of thread.";
  }
}

Dispatcher::~Dispatcher() {
  if (running_) {
    Stop();
  }
}

void Dispatcher::Run() {
  running_ = true;

  // Start RPC service
  rpc_service_.Start();

  // Run UDP RPC server
  for (int i = 0; i < num_udp_threads_; ++i) {
    int cpu1 = pin_cpus_.empty() ? -1 : pin_cpus_.at(i * 2);
    int cpu2 = pin_cpus_.empty() ? -1 : pin_cpus_.at(i * 2 + 1);
    udp_rpc_servers_.emplace_back(
        new UdpRpcServer(udp_port_, this, cpu1, cpu2));
    workers_.emplace_back(&UdpRpcServer::Run, udp_rpc_servers_.back().get());
  }

  // Nothing to do here
  for (;;) {
    std::this_thread::sleep_for(std::chrono::hours(24));
  }
}

void Dispatcher::Stop() {
  LOG(INFO) << "Shutting down the dispatcher.";
  running_ = false;

  // Stop RPC service
  rpc_service_.Stop();

  // Stop UDP RPC server
  for (auto& server : udp_rpc_servers_) {
    server->Stop();
  }
  for (auto& thread : workers_) {
    thread.join();
  }
}

void Dispatcher::DispatchRequest(QueryProto query_without_input,
                                 DispatchReply* reply) {
  // Update punch clock
  auto dispatcher_sched_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch())
          .count();
  query_without_input.mutable_clock()->set_dispatcher_sched_ns(
      dispatcher_sched_ns);

  // Assign GlobalId
  auto global_id = next_global_id_.fetch_add(1);
  query_without_input.set_global_id(global_id);

  // Run round-robin
  std::shared_ptr<BackendDelegate> backend;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto iter = models_.find(query_without_input.model_session_id());
    if (iter == models_.end()) {
      reply->set_status(CtrlStatus::MODEL_NOT_FOUND);
    } else {
      auto backend_info = iter->second.GetBackend();
      reply->set_status(CtrlStatus::CTRL_OK);
      auto backend_iter = backends_.find(NodeId(backend_info.node_id()));
      if (backend_iter != backends_.end()) {
        backend = backend_iter->second;
      } else {
        LOG(ERROR) << "Cannot find BackendDelegate for Backend "
                   << backend_info.node_id();
      }
    }
  }

  // Send the query to the backend.
  if (!backend) {
    return;
  }
  using namespace std::chrono;
  auto inst_info =
      backend->GetInstanceInfo(query_without_input.model_session_id());
  const auto& profile = inst_info->profile();
  auto plan_id = next_plan_id_.fetch_add(1);
  TimePoint now = Clock::now();
  constexpr auto kNetworkLatency = microseconds(5000);
  ModelSession model_session;
  ParseModelSession(query_without_input.model_session_id(), &model_session);

  // Define deadline
  auto frontend_recv_time =
      TimePoint(nanoseconds(query_without_input.clock().frontend_recv_ns()));
  auto deadline =
      frontend_recv_time + microseconds(model_session.latency_sla());
  auto deadline_ns =
      duration_cast<nanoseconds>(deadline.time_since_epoch()).count();

  // Build a simple BatchPlan
  BatchPlanProto request;
  request.set_plan_id(plan_id);
  request.set_model_session_id(query_without_input.model_session_id());
  *request.add_queries_without_input() = std::move(query_without_input);
  auto exec_time = now + kNetworkLatency;
  auto exec_time_ns =
      duration_cast<nanoseconds>(exec_time.time_since_epoch()).count();
  request.set_exec_time_ns(exec_time_ns);
  request.set_deadline_ns(deadline_ns);
  auto exec_elapse_ns = inst_info->profile().GetForwardLatency(1) * 1000;
  auto finish_time_ns = exec_time_ns + exec_elapse_ns;
  request.set_expected_finish_time_ns(finish_time_ns);

  // Update punch clock
  auto dispatcher_dispatch_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch())
          .count();
  for (auto& query : *request.mutable_queries_without_input()) {
    query.mutable_clock()->set_dispatcher_dispatch_ns(dispatcher_dispatch_ns);
  }

  // Send the BatchPlan to the backend
  backend->EnqueueBatchPlan(request);
}

void Dispatcher::UpdateModelRoutes(const ModelRouteUpdates& request,
                                   RpcReply* reply) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& model_route : request.model_route()) {
    auto iter = models_.find(model_route.model_session_id());
    if (iter == models_.end()) {
      auto res = models_.emplace(model_route.model_session_id(), ModelRoute());
      iter = res.first;
    }
    iter->second.Update(model_route);
  }
  reply->set_status(CTRL_OK);
}

void ModelRoute::Update(const ModelRouteProto& route) {
  LOG(INFO) << "Update model route for " << route.model_session_id();

  // Save the current DRR backend
  const auto current_drr_backend_id =
      backends_.empty() ? 0 : backends_[current_drr_index_].info().node_id();

  // Update from the proto
  model_session_id_.assign(route.model_session_id());
  backends_.assign(route.backend_rate().begin(), route.backend_rate().end());
  total_throughput_ = 0.;

  // Calculate quantum:rate ratio
  min_rate_ = std::numeric_limits<double>::max();
  for (const auto& backend : backends_) {
    min_rate_ = std::min(min_rate_, backend.throughput());
  }

  // Give quantum to new backends
  std::unordered_map<uint32_t, size_t> backend_idx;
  for (size_t i = 0; i < backends_.size(); ++i) {
    const auto& backend = backends_[i];
    const auto backend_id = backend.info().node_id();
    const auto rate = backend.throughput();
    total_throughput_ += rate;
    LOG(INFO) << "  backend " << backend_id << ": " << rate << " rps";
    backend_quanta_.emplace(backend_id, rate);
    backend_idx.emplace(backend_id, i);
  }
  LOG(INFO) << "  total throughput: " << total_throughput_ << " rps";

  // Remove quantum of old backends
  for (auto iter = backend_quanta_.begin(); iter != backend_quanta_.end();) {
    if (backend_idx.count(iter->first) == 0) {
      iter = backend_quanta_.erase(iter);
    } else {
      ++iter;
    }
  }

  // Recover the current DRR backend
  auto backend_idx_iter = backend_idx.find(current_drr_backend_id);
  if (backend_idx_iter != backend_idx.end()) {
    current_drr_index_ = backend_idx_iter->second;
  } else {
    if (backends_.empty()) {
      current_drr_index_ = 0;
    } else {
      current_drr_index_ %= backends_.size();
    }
  }
}

BackendInfo ModelRoute::GetBackend() {
  for (size_t i = 0;; ++i) {
    const auto& backend = backends_[current_drr_index_];
    const uint32_t backend_id = backend.info().node_id();
    if (backend_quanta_.at(backend_id) >= min_rate_) {
      backend_quanta_[backend_id] -= min_rate_;
      return backend.info();
    } else {
      const auto rate = backend.throughput();
      backend_quanta_[backend_id] += rate;
      current_drr_index_ = (current_drr_index_ + 1) % backends_.size();
    }

    CHECK_LE(i, backends_.size()) << "DRR could not decide.";
  }
}

void Dispatcher::HandleRegister(const grpc::ServerContext& ctx,
                                const RegisterRequest& request,
                                RegisterReply* reply) {
  std::vector<std::string> tokens;
  SplitString(ctx.peer(), ':', &tokens);
  std::string ip = tokens[1];
  LOG(INFO) << "Register server: " << request.DebugString();
  switch (request.node_type()) {
    case NodeType::FRONTEND_NODE: {
      auto frontend = std::make_shared<FrontendDelegate>(
          request.node_id(), ip, request.server_port(), request.rpc_port(),
          beacon_interval_sec_);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        auto frontend_id = NodeId(frontend->node_id());
        if (frontends_.find(frontend_id) != frontends_.end()) {
          reply->set_status(CtrlStatus::CTRL_FRONTEND_NODE_ID_CONFLICT);
          return;
        }
        frontends_[frontend_id] = frontend;
      }

      // UpdateBackendList
      BackendListUpdates update;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto iter : backends_) {
          update.add_backends()->CopyFrom(iter.second->backend_info());
        }
      }
      VLOG(1) << "Send UpdateBackendList: frontend_id=" << frontend->node_id();
      frontend->UpdateBackendList(update);
      VLOG(1) << "Finish sending UpdateBackendList: frontend_id="
              << frontend->node_id();

      reply->set_status(CtrlStatus::CTRL_OK);
      reply->set_beacon_interval_sec(BEACON_INTERVAL_SEC);
      VLOG(1) << "Finish registering frontend_id=" << frontend->node_id();
      break;
    }
    case NodeType::BACKEND_NODE: {
      auto backend = std::make_shared<BackendDelegate>(
          request.node_id(), ip, request.server_port(), request.rpc_port(),
          request.gpu_device_name(), request.gpu_uuid(),
          request.gpu_available_memory(), beacon_interval_sec_);
      std::unordered_map<std::string, std::shared_ptr<ModelSessionContext>>
          sessions;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        auto backend_id = NodeId(backend->node_id());
        if (backends_.find(backend_id) != backends_.end()) {
          reply->set_status(CtrlStatus::CTRL_BACKEND_NODE_ID_CONFLICT);
          return;
        }
        backends_[backend_id] = backend;
        sessions = sessions_;
      }

      // Load Models
      for (auto iter : sessions) {
        const auto& model_session = iter.second->model_session();
        auto profile_id = ModelSessionToProfileID(model_session);
        auto* profile = ModelDatabase::Singleton().GetModelProfile(
            backend->gpu_device(), backend->gpu_uuid(), profile_id);
        if (!profile) {
          reply->set_status(CtrlStatus::CTRL_INVALID_LOAD_MODEL_REQUEST);
          continue;
        }
        auto inst = std::make_shared<InstanceInfo>(
            model_session, backend->node_id(), *profile);
        auto model_sess_id = ModelSessionToString(model_session);
        backend->AddInstanceInfo(model_sess_id, inst);
        iter.second->AddInstanceInfo(backend->node_id(), inst);

        // LoadModel RPC
        VLOG(1) << "SendLoadModelCommand: backend_id=" << backend->node_id()
                << ", model_session=" << model_sess_id;
        backend->SendLoadModelCommand(model_session, inst->max_batch());
        VLOG(1) << "Finish SendLoadModelCommand: backend_id="
                << backend->node_id() << ", model_session=" << model_sess_id;
      }

      // UpdateBackendList
      BackendListUpdates update;
      update.add_backends()->CopyFrom(backend->backend_info());
      decltype(frontends_) frontends;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        frontends = frontends_;
      }
      for (auto iter : frontends) {
        VLOG(1) << "UpdateBackendList (adding backend_id=" << backend->node_id()
                << "): frontend_id=" << iter.second->node_id();
        iter.second->UpdateBackendList(update);
        VLOG(1) << "Finish UpdateBackendList (adding backend_id="
                << backend->node_id()
                << "): frontend_id=" << iter.second->node_id();
      }

      reply->set_status(CtrlStatus::CTRL_OK);
      reply->set_beacon_interval_sec(BEACON_INTERVAL_SEC);
      VLOG(1) << "Finish registering backend_id=" << backend->node_id();
      break;
    }
    default: {
      LOG(ERROR) << "Unknown node type: " << NodeType_Name(request.node_type());
      reply->set_status(CtrlStatus::CTRL_SERVER_NOT_REGISTERED);
    }
  }
}

void Dispatcher::HandleUnregister(const grpc::ServerContext& ctx,
                                  const UnregisterRequest& request,
                                  RpcReply* reply) {
  // TODO
  LOG(ERROR) << "HandleUnregister not implemented. Request: "
             << request.DebugString();
  reply->set_status(CtrlStatus::CTRL_OK);
}

void Dispatcher::HandleLoadModel(const grpc::ServerContext& ctx,
                                 const LoadModelRequest& request,
                                 LoadModelReply* reply) {
  auto model_info = ModelDatabase::Singleton().GetModelInfo(
      ModelSessionToModelID(request.model_session()));
  if (!model_info) {
    LOG(ERROR) << "HandleLoadModel: model not found. model="
               << ModelSessionToModelID(request.model_session());
    reply->set_status(CtrlStatus::MODEL_NOT_FOUND);
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto model_sess_id = ModelSessionToString(request.model_session());
  VLOG(1) << "HandleLoadModel: model_sess_id=" << model_sess_id;
  {
    auto it = sessions_.find(model_sess_id);
    if (it != sessions_.end()) {
      // Model already loaded. Just skip.
      reply->set_status(CtrlStatus::CTRL_OK);
      return;
    }
  }
  reply->set_status(CtrlStatus::CTRL_OK);

  // Update DRR
  CHECK_EQ(models_.count(model_sess_id), 0);
  models_[model_sess_id] = ModelRoute();
  ModelRouteProto mr;
  {
    // Adaptor to the old API
    *mr.mutable_model_session_id() = model_sess_id;
    for (auto backend_iter : backends_) {
      auto* rate = mr.add_backend_rate();
      *rate->mutable_info() = backend_iter.second->backend_info();
      rate->set_throughput(1);
    }
  }
  models_[model_sess_id].Update(mr);

  // Add the model session
  auto sctx = std::make_shared<ModelSessionContext>(request.model_session());
  sessions_[model_sess_id] = sctx;

  // Ask backends to load the model
  auto profile_id = ModelSessionToProfileID(request.model_session());
  for (auto backend_iter : backends_) {
    auto backend = backend_iter.second;
    auto* profile = ModelDatabase::Singleton().GetModelProfile(
        backend->gpu_device(), backend->gpu_uuid(), profile_id);
    if (!profile) {
      reply->set_status(CtrlStatus::CTRL_INVALID_LOAD_MODEL_REQUEST);
      continue;
    }
    auto inst = std::make_shared<InstanceInfo>(request.model_session(),
                                               backend->node_id(), *profile);
    backend->AddInstanceInfo(model_sess_id, inst);
    sctx->AddInstanceInfo(backend->node_id(), inst);

    // LoadModel RPC
    VLOG(1) << "SendLoadModelCommand: backend_id=" << backend->node_id()
            << ", model_session=" << model_sess_id;
    backend->SendLoadModelCommand(request.model_session(), inst->max_batch());
    VLOG(1) << "Finish SendLoadModelCommand: backend_id=" << backend->node_id()
            << ", model_session=" << model_sess_id;
  }
}

void Dispatcher::HandleKeepAlive(const grpc::ServerContext& ctx,
                                 const KeepAliveRequest& request,
                                 RpcReply* reply) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto node_id = NodeId(request.node_id());
  switch (request.node_type()) {
    case NodeType::FRONTEND_NODE: {
      auto it = frontends_.find(node_id);
      if (it == frontends_.end()) {
        reply->set_status(CtrlStatus::CTRL_SERVER_NOT_REGISTERED);
      } else {
        it->second->Tick();
        reply->set_status(CtrlStatus::CTRL_OK);
      }
      break;
    }
    case NodeType::BACKEND_NODE: {
      auto it = backends_.find(node_id);
      if (it == backends_.end()) {
        reply->set_status(CtrlStatus::CTRL_SERVER_NOT_REGISTERED);
      } else {
        it->second->Tick();
        reply->set_status(CtrlStatus::CTRL_OK);
      }
      break;
    }
    default: {
      LOG(ERROR) << "Unknown node type: " << NodeType_Name(request.node_type());
      reply->set_status(CtrlStatus::CTRL_SERVER_NOT_REGISTERED);
    }
  }
}

}  // namespace dispatcher
}  // namespace nexus
