#include "tiles/scheduler.hpp"

#include <algorithm>

namespace mapkern {

void RequestScheduler::setGroupLimit(const std::string& group, int limit) { limits_[group] = std::max(1, limit); }

void RequestScheduler::removeGroup(const std::string& group) {
  limits_.erase(group);
  for (auto& h : queued_)
    if (h->group == group) h->state = JobState::Cancelled;
  std::erase_if(queued_, [](const auto& h) { return h->state == JobState::Cancelled; });
}

std::shared_ptr<JobHandle> RequestScheduler::enqueue(const std::string& group, double priority,
                                                     std::function<void(std::function<void()>)> run) {
  auto handle = std::make_shared<JobHandle>();
  handle->group = group;
  handle->priority = priority;
  handle->run = std::move(run);
  if (destroyed_) {
    handle->state = JobState::Cancelled;
    return handle;
  }
  queued_.push_back(handle);
  return handle;
}

void RequestScheduler::cancel(const std::shared_ptr<JobHandle>& handle) {
  if (!handle || handle->state != JobState::Queued) return;
  handle->state = JobState::Cancelled;
  std::erase(queued_, handle);
}

int RequestScheduler::activeInGroup(const std::string& group) const {
  auto it = running_.find(group);
  return it == running_.end() ? 0 : it->second;
}

void RequestScheduler::pump() {
  if (destroyed_ || pumping_ || queued_.empty() || static_cast<int>(active_) >= maxConcurrent) return;
  pumping_ = true;
  std::stable_sort(queued_.begin(), queued_.end(), [](const auto& a, const auto& b) { return a->priority < b->priority; });
  std::vector<std::shared_ptr<JobHandle>> start;
  for (auto it = queued_.begin(); it != queued_.end();) {
    if (static_cast<int>(active_ + start.size()) >= maxConcurrent) break;
    const auto& h = *it;
    const int running = running_[h->group];
    auto lim = limits_.find(h->group);
    if (running >= (lim == limits_.end() ? maxConcurrent : lim->second)) {
      ++it;
      continue;
    }
    running_[h->group] = running + 1;
    h->state = JobState::Active;
    start.push_back(h);
    it = queued_.erase(it);
  }
  active_ += start.size();
  pumping_ = false;
  for (auto& h : start) {
    std::weak_ptr<JobHandle> weak = h;
    auto finished = std::make_shared<bool>(false);
    try {
      h->run([this, h, finished]() {
        if (*finished) return;
        *finished = true;
        finish(h);
      });
    } catch (...) {
      if (!*finished) {
        *finished = true;
        finish(h);
      }
    }
  }
}

void RequestScheduler::finish(const std::shared_ptr<JobHandle>& handle) {
  handle->state = JobState::Done;
  if (active_ > 0) active_--;
  auto it = running_.find(handle->group);
  if (it != running_.end() && --it->second <= 0) running_.erase(it);
  handle->run = nullptr;
}

void RequestScheduler::destroy() {
  destroyed_ = true;
  for (auto& h : queued_) h->state = JobState::Cancelled;
  queued_.clear();
}

}
