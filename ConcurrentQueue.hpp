#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>

template <typename T>
class MPSCQueue {
public:
    MPSCQueue() : stub_(new Node()), head_(stub_), tail_(stub_) {}

    MPSCQueue(const MPSCQueue&) = delete;
    MPSCQueue& operator=(const MPSCQueue&) = delete;

    ~MPSCQueue() {
        T ignored{};
        while (tryPop(ignored)) {
        }
        delete tail_;
    }

    void push(T value) {
        Node* node = new Node(std::move(value));
        node->next.store(nullptr, std::memory_order_relaxed);

        // Multiple producers serialize by atomically swapping the head pointer.
        // acq_rel prevents reordering the node payload after publication and keeps producer links ordered.
        Node* previous = head_.exchange(node, std::memory_order_acq_rel);

        // Release makes this node visible to the single consumer when it loads previous->next with acquire.
        previous->next.store(node, std::memory_order_release);

        size_.fetch_add(1, std::memory_order_relaxed);
        cv_.notify_one();
    }

    bool tryPop(T& out) {
        Node* tail = tail_;
        Node* next = tail->next.load(std::memory_order_acquire);
        if (next == nullptr) {
            return false;
        }

        out = std::move(next->value.value());
        tail_ = next;
        delete tail;
        size_.fetch_sub(1, std::memory_order_relaxed);
        return true;
    }

    bool waitPop(T& out, std::atomic<bool>& running) {
        while (running.load(std::memory_order_acquire)) {
            if (tryPop(out)) {
                return true;
            }

            std::unique_lock<std::mutex> lock(cvMutex_);
            cv_.wait_for(lock, std::chrono::microseconds(100), [&] {
                return !running.load(std::memory_order_acquire) || size_.load(std::memory_order_relaxed) > 0;
            });
        }

        return tryPop(out);
    }

    void notifyAll() {
        cv_.notify_all();
    }

    [[nodiscard]] std::size_t approximateSize() const noexcept {
        return size_.load(std::memory_order_relaxed);
    }

private:
    struct Node {
        std::optional<T> value;
        std::atomic<Node*> next{nullptr};

        Node() = default;
        explicit Node(T v) : value(std::move(v)) {}
    };

    Node* stub_;
    alignas(64) std::atomic<Node*> head_;
    alignas(64) Node* tail_;
    alignas(64) std::atomic<std::size_t> size_{0};
    std::mutex cvMutex_;
    std::condition_variable cv_;
};
