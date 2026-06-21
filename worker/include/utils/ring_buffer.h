#pragma once

#include <mutex>
#include <vector>

namespace monitor {

template <typename T>
class RingBuffer {
 public:
  explicit RingBuffer(size_t capacity)
      : buffer_(capacity), capacity_(capacity) {}

  void Push(const T& item) {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_[head_] = item;
    head_ = (head_ + 1) % capacity_;
    if (size_ < capacity_) {
      ++size_;
    } else {
      tail_ = (tail_ + 1) % capacity_;
    }
  }

  void Push(T&& item) {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_[head_] = std::move(item);
    head_ = (head_ + 1) % capacity_;
    if (size_ < capacity_) {
      ++size_;
    } else {
      tail_ = (tail_ + 1) % capacity_;
    }
  }

  size_t Drain(std::vector<T>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    out.clear();
    out.reserve(size_);
    for (size_t i = 0; i < size_; ++i) {
      out.push_back(std::move(buffer_[(tail_ + i) % capacity_]));
    }
    size_t drained = size_;
    head_ = 0;
    tail_ = 0;
    size_ = 0;
    return drained;
  }

  bool Empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_ == 0;
  }

  size_t Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
  }

 private:
  std::vector<T> buffer_;
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t size_ = 0;
  size_t capacity_;
  mutable std::mutex mutex_;
};

}  // namespace monitor
