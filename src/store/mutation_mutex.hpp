#pragma once

#include <atomic>
#include <coroutine>
#include <mutex>
#include <new>
#include <stop_token>

#include <boost/capy/continuation.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/io_result.hpp>

namespace liquid::store_detail {

// An asynchronous mutex whose lock, wait queue, and unlock are safe from any thread.
//
// Capy's async_mutex is documented for one executor, while liquid runs every io_context on several
// runner threads and Corosio resumes a coroutine after file I/O directly on the completing runner.
// A coroutine that holds the store's mutation lock across a pack write therefore comes back on an
// arbitrary thread, and the lock has to protect its own state. Waiters are queued in FIFO order
// under a short native mutex and are resumed through the executor they suspended on. The awaiter
// follows Capy's IoAwaitable protocol (io_env, continuation, stop_token), so it is used exactly
// like async_mutex::scoped_lock. A waiter becomes visible to other threads only after every field
// they may read is written, and nothing touches the awaiter after it is published; the atomic
// state resolves the race between unlock and cancellation.
class mutation_mutex {
public:
    class lock_awaiter;
    class lock_guard;
    class lock_guard_awaiter;

    mutation_mutex() = default;
    mutation_mutex(const mutation_mutex&) = delete;
    mutation_mutex& operator=(const mutation_mutex&) = delete;

    [[nodiscard]] lock_guard_awaiter scoped_lock() noexcept;

    void unlock() noexcept;

private:
    enum class waiter_state : unsigned char { waiting, acquired, canceled };

    void remove(lock_awaiter& waiter) noexcept;

    std::mutex state_mutex_;
    bool locked_{};
    lock_awaiter* head_{};
    lock_awaiter* tail_{};
};

class mutation_mutex::lock_awaiter {
    friend class mutation_mutex;

    struct cancel {
        lock_awaiter* waiter;

        void operator()() const noexcept {
            auto expected = waiter_state::waiting;
            if (!waiter->state_.compare_exchange_strong(
                    expected, waiter_state::canceled, std::memory_order_acq_rel)) {
                return;
            }
            waiter->mutex_->remove(*waiter);
            waiter->executor_.post(waiter->continuation_);
        }
    };

    using stop_callback = std::stop_callback<cancel>;

    mutation_mutex* mutex_;
    lock_awaiter* next_{};
    bool linked_{};
    boost::capy::continuation continuation_;
    boost::capy::executor_ref executor_;
    std::atomic<waiter_state> state_{waiter_state::waiting};
    bool callback_active_{};
    alignas(stop_callback) unsigned char callback_storage_[sizeof(stop_callback)];

    stop_callback& callback() noexcept {
        return *std::launder(reinterpret_cast<stop_callback*>(callback_storage_));
    }

public:
    explicit lock_awaiter(mutation_mutex* mutex) noexcept : mutex_(mutex) {}

    ~lock_awaiter();

    // Capy's task moves the awaitable into its own storage before await_suspend runs; nothing
    // points at the awaiter until then, so a move only carries the mutex pointer.
    lock_awaiter(lock_awaiter&& other) noexcept : mutex_(other.mutex_) {}

    lock_awaiter(const lock_awaiter&) = delete;
    lock_awaiter& operator=(const lock_awaiter&) = delete;
    lock_awaiter& operator=(lock_awaiter&&) = delete;

    bool await_ready() noexcept;

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> handle,
                                          const boost::capy::io_env* environment) noexcept;

    boost::capy::io_result<> await_resume() noexcept;
};

class [[nodiscard]] mutation_mutex::lock_guard {
    mutation_mutex* mutex_{};

public:
    lock_guard() noexcept = default;

    explicit lock_guard(mutation_mutex* mutex) noexcept : mutex_(mutex) {}

    ~lock_guard() {
        if (mutex_ != nullptr) {
            mutex_->unlock();
        }
    }

    lock_guard(lock_guard&& other) noexcept : mutex_(other.mutex_) {
        other.mutex_ = nullptr;
    }

    lock_guard& operator=(lock_guard&& other) noexcept {
        if (this != &other) {
            if (mutex_ != nullptr) {
                mutex_->unlock();
            }
            mutex_ = other.mutex_;
            other.mutex_ = nullptr;
        }
        return *this;
    }

    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;
};

class mutation_mutex::lock_guard_awaiter {
    mutation_mutex* mutex_;
    lock_awaiter inner_;

public:
    explicit lock_guard_awaiter(mutation_mutex* mutex) noexcept : mutex_(mutex), inner_(mutex) {}

    bool await_ready() noexcept {
        return inner_.await_ready();
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> handle,
                                          const boost::capy::io_env* environment) noexcept {
        return inner_.await_suspend(handle, environment);
    }

    boost::capy::io_result<lock_guard> await_resume() noexcept {
        auto result = inner_.await_resume();
        if (result.ec) {
            return {result.ec, {}};
        }
        return {{}, lock_guard(mutex_)};
    }
};

inline mutation_mutex::lock_guard_awaiter mutation_mutex::scoped_lock() noexcept {
    return lock_guard_awaiter(this);
}

inline void mutation_mutex::remove(lock_awaiter& waiter) noexcept {
    std::lock_guard lock(state_mutex_);
    if (!waiter.linked_) {
        return;
    }
    lock_awaiter* previous = nullptr;
    for (auto* current = head_; current != nullptr; previous = current, current = current->next_) {
        if (current != &waiter) {
            continue;
        }
        (previous != nullptr ? previous->next_ : head_) = current->next_;
        if (tail_ == current) {
            tail_ = previous;
        }
        break;
    }
    waiter.linked_ = false;
}

inline void mutation_mutex::unlock() noexcept {
    lock_awaiter* next = nullptr;
    {
        std::lock_guard lock(state_mutex_);
        while (head_ != nullptr) {
            auto* candidate = head_;
            head_ = candidate->next_;
            if (head_ == nullptr) {
                tail_ = nullptr;
            }
            candidate->linked_ = false;
            auto expected = waiter_state::waiting;
            if (candidate->state_.compare_exchange_strong(
                    expected, waiter_state::acquired, std::memory_order_acq_rel)) {
                next = candidate;
                break;
            }
        }
        if (next == nullptr) {
            locked_ = false;
            return;
        }
    }
    // The waiter owns the lock from here; it may resume and finish before this returns, so it is
    // not touched again.
    next->executor_.post(next->continuation_);
}

inline bool mutation_mutex::lock_awaiter::await_ready() noexcept {
    std::lock_guard lock(mutex_->state_mutex_);
    if (mutex_->locked_) {
        return false;
    }
    mutex_->locked_ = true;
    state_.store(waiter_state::acquired, std::memory_order_relaxed);
    return true;
}

inline std::coroutine_handle<>
mutation_mutex::lock_awaiter::await_suspend(std::coroutine_handle<> handle,
                                            const boost::capy::io_env* environment) noexcept {
    continuation_.h = handle;
    executor_ = environment->executor;
    // Register cancellation before anyone else can see this waiter. From here on the stop callback
    // may run on another thread and resume the coroutine, so only the state machine below reads
    // or writes this object.
    callback_active_ = true;
    ::new (callback_storage_) stop_callback(environment->stop_token, cancel{this});
    {
        std::lock_guard lock(mutex_->state_mutex_);
        if (state_.load(std::memory_order_acquire) != waiter_state::waiting) {
            return std::noop_coroutine();
        }
        if (!mutex_->locked_) {
            auto expected = waiter_state::waiting;
            if (!state_.compare_exchange_strong(
                    expected, waiter_state::acquired, std::memory_order_acq_rel)) {
                return std::noop_coroutine();
            }
            mutex_->locked_ = true;
            return handle;
        }
        next_ = nullptr;
        if (mutex_->tail_ != nullptr) {
            mutex_->tail_->next_ = this;
        } else {
            mutex_->head_ = this;
        }
        mutex_->tail_ = this;
        linked_ = true;
    }
    return std::noop_coroutine();
}

inline boost::capy::io_result<> mutation_mutex::lock_awaiter::await_resume() noexcept {
    if (callback_active_) {
        callback().~stop_callback();
        callback_active_ = false;
    }
    if (state_.load(std::memory_order_acquire) == waiter_state::canceled) {
        return {boost::capy::make_error_code(boost::capy::error::canceled)};
    }
    return {{}};
}

inline mutation_mutex::lock_awaiter::~lock_awaiter() {
    if (!callback_active_) {
        return;
    }
    // The frame is destroyed while suspended, for example at context shutdown. Leave the queue
    // and give the lock back if it was granted but never observed.
    callback().~stop_callback();
    callback_active_ = false;
    auto expected = waiter_state::waiting;
    if (state_.compare_exchange_strong(
            expected, waiter_state::canceled, std::memory_order_acq_rel)) {
        mutex_->remove(*this);
        return;
    }
    if (state_.load(std::memory_order_acquire) == waiter_state::acquired) {
        mutex_->unlock();
    }
}

} // namespace liquid::store_detail
