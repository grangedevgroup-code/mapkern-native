#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace mapkern {

class CancelToken {
 public:
  CancelToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
  void cancel() const { flag_->store(true); }
  bool cancelled() const { return flag_->load(); }

 private:
  std::shared_ptr<std::atomic<bool>> flag_;
};

class Mailbox {
 public:
  void post(std::function<void()> fn);
  size_t drain(size_t max = SIZE_MAX);
  bool empty();
  void setWake(std::function<void()> wake);

 private:
  std::mutex mutex_;
  std::deque<std::function<void()>> items_;
  std::function<void()> wake_;
};

class ThreadPool {
 public:
  explicit ThreadPool(unsigned threads, const char* name = "mapkern");
  ~ThreadPool();
  void submit(std::function<void()> job, int priority = 0);
  unsigned size() const { return static_cast<unsigned>(threads_.size()); }
  size_t pending();

 private:
  struct Job {
    int priority;
    uint64_t order;
    std::function<void()> fn;
  };
  void run();
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<Job> jobs_;
  std::vector<std::thread> threads_;
  uint64_t order_ = 0;
  bool stop_ = false;
};

ThreadPool& workerPool();
unsigned hardwareThreads();
uint64_t physicalMemory();
size_t autoTileCacheBytes();

}
