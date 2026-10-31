#include "core/async.hpp"

#include <algorithm>

namespace mapkern {

void Mailbox::post(std::function<void()> fn) {
  std::function<void()> wake;
  {
    std::lock_guard lock(mutex_);
    items_.push_back(std::move(fn));
    wake = wake_;
  }
  if (wake) wake();
}

size_t Mailbox::drain(size_t max) {
  size_t n = 0;
  while (n < max) {
    std::function<void()> fn;
    {
      std::lock_guard lock(mutex_);
      if (items_.empty()) break;
      fn = std::move(items_.front());
      items_.pop_front();
    }
    fn();
    n++;
  }
  return n;
}

bool Mailbox::empty() {
  std::lock_guard lock(mutex_);
  return items_.empty();
}

void Mailbox::setWake(std::function<void()> wake) {
  std::lock_guard lock(mutex_);
  wake_ = std::move(wake);
}

unsigned hardwareThreads() {
  const unsigned n = std::thread::hardware_concurrency();
  return n == 0 ? 4 : n;
}

ThreadPool::ThreadPool(unsigned threads, const char*) {
  threads = std::max(1u, threads);
  for (unsigned i = 0; i < threads; i++) threads_.emplace_back([this] { run(); });
}

ThreadPool::~ThreadPool() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  for (auto& t : threads_) t.join();
}

void ThreadPool::submit(std::function<void()> job, int priority) {
  {
    std::lock_guard lock(mutex_);
    jobs_.push_back({priority, order_++, std::move(job)});
    std::push_heap(jobs_.begin(), jobs_.end(), [](const Job& a, const Job& b) {
      return a.priority != b.priority ? a.priority > b.priority : a.order > b.order;
    });
  }
  cv_.notify_one();
}

size_t ThreadPool::pending() {
  std::lock_guard lock(mutex_);
  return jobs_.size();
}

void ThreadPool::run() {
  auto cmp = [](const Job& a, const Job& b) { return a.priority != b.priority ? a.priority > b.priority : a.order > b.order; };
  while (true) {
    std::function<void()> fn;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
      if (stop_ && jobs_.empty()) return;
      std::pop_heap(jobs_.begin(), jobs_.end(), cmp);
      fn = std::move(jobs_.back().fn);
      jobs_.pop_back();
    }
    try {
      fn();
    } catch (...) {
    }
  }
}

ThreadPool& workerPool() {
  static ThreadPool pool(std::clamp(hardwareThreads() - 1, 2u, 8u), "mapkern-worker");
  return pool;
}

}
