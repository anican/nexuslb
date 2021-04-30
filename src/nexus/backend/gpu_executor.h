#ifndef NEXUS_BACKEND_BASE_GPU_EXECUTOR_H_
#define NEXUS_BACKEND_BASE_GPU_EXECUTOR_H_

#include <atomic>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ario/ario.h"
#include "nexus/backend/batch_plan_context.h"
#include "nexus/backend/model_exec.h"
#include "nexus/common/time_util.h"
#include "nexus/common/typedef.h"
#include "nexus/proto/control.pb.h"

namespace nexus {
namespace backend {

class GpuExecutor {
 public:
  GpuExecutor() : duty_cycle_us_(0.) {}

  virtual ~GpuExecutor() {}

  void SetDutyCycle(double duty_cycle_us) {
    duty_cycle_us_.store(duty_cycle_us);
  }

  virtual void Start(int core = -1) = 0;
  virtual void Stop() = 0;
  virtual void AddModel(std::shared_ptr<ModelExecutor> model) = 0;
  virtual void RemoveModel(std::shared_ptr<ModelExecutor> model) = 0;
  virtual double CurrentUtilization() = 0;

 protected:
  std::atomic<double> duty_cycle_us_;
};

class GpuExecutorMultiBatching : public GpuExecutor {
 public:
  GpuExecutorMultiBatching(int gpu_id);

  inline int gpu_id() { return gpu_id_; }

  void Start(int core = -1) final;

  void Stop() final;

  void AddModel(std::shared_ptr<ModelExecutor> model) final;

  void RemoveModel(std::shared_ptr<ModelExecutor> model) final;

  double CurrentUtilization() final;

 private:
  void Run();

  int gpu_id_;
  std::atomic_bool running_;
  std::thread thread_;
  std::vector<std::shared_ptr<ModelExecutor>> models_;
  std::vector<std::shared_ptr<ModelExecutor>> backup_models_;
  std::mutex models_mu_;
  double utilization_;
  TimePoint last_check_time_;
  std::mutex util_mu_;
};

class GpuExecutorNoMultiBatching : public GpuExecutor {
 public:
  GpuExecutorNoMultiBatching(int gpu_id);

  inline int gpu_id() { return gpu_id_; }

  void Start(int core = -1);

  void Stop();

  void AddModel(std::shared_ptr<ModelExecutor> model) final;

  void RemoveModel(std::shared_ptr<ModelExecutor> model) final;

  double CurrentUtilization() final;

 private:
  int gpu_id_;
  int core_;
  std::mutex mu_;
  std::unordered_map<std::string, std::unique_ptr<GpuExecutorMultiBatching>>
      threads_;
};

class GpuExecutorPlanFollower {
 public:
  GpuExecutorPlanFollower(int gpu_id, ario::PollerType poller_type);
  virtual ~GpuExecutorPlanFollower();
  void Start(int core = -1);
  void Stop();
  void AddModel(std::shared_ptr<ModelExecutor> model);
  void RemoveModel(std::shared_ptr<ModelExecutor> model);
  void AddBatchPlan(std::shared_ptr<BatchPlanContext> plan);

 private:
  void UpdateTimer() /* REQUIRES(mutex_) */;
  void OnTimer(ario::ErrorCode error);

  int gpu_id_;
  std::thread thread_;

  ario::EpollExecutor executor_;

  std::atomic_flag is_executing_ = ATOMIC_FLAG_INIT;
  std::mutex mutex_;
  std::vector<std::shared_ptr<BatchPlanContext>>
      plans_ /* GUARDED_BY(mutex_) */;
  std::unordered_map<std::string, std::shared_ptr<ModelExecutor>>
      models_ /* GUARDED_BY(mutex_) */;
  ario::Timer next_timer_ /* GUARDED_BY(mutex_) */;
};

}  // namespace backend
}  // namespace nexus

#endif  // NEXUS_BACKEND_BASE_GPU_EXECUTOR_H_
