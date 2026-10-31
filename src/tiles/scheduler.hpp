#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mapkern {

enum class JobState { Queued, Active, Done, Cancelled };

struct JobHandle {
  std::string group;
  double priority = 0;
  JobState state = JobState::Queued;
  std::function<void(std::function<void()> done)> run;
};

class RequestScheduler {
 public:
  explicit RequestScheduler(int maxConcurrent = 24) : maxConcurrent(maxConcurrent) {}
  int maxConcurrent;
  void setGroupLimit(const std::string& group, int limit);
  void removeGroup(const std::string& group);
  std::shared_ptr<JobHandle> enqueue(const std::string& group, double priority, std::function<void(std::function<void()> done)> run);
  void cancel(const std::shared_ptr<JobHandle>& handle);
  size_t queuedCount() const { return queued_.size(); }
  size_t activeCount() const { return active_; }
  int activeInGroup(const std::string& group) const;
  void pump();
  void destroy();

 private:
  void finish(const std::shared_ptr<JobHandle>& handle);
  std::vector<std::shared_ptr<JobHandle>> queued_;
  size_t active_ = 0;
  std::unordered_map<std::string, int> running_;
  std::unordered_map<std::string, int> limits_;
  bool destroyed_ = false;
  bool pumping_ = false;
};

}
