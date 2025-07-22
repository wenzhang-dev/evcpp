#pragma once

#include <promise.h>

#include <coroutine>

namespace evcpp {

template <typename T, typename E>
class CoroutineTrait {
   public:
    class promise_type {
       public:
        promise_type() = default;
        promise_type(promise_type&&) = delete;
        promise_type(const promise_type&) = delete;

        // Notes, when enter coroutine function scope for the first time, the
        // function `get_return_object` will be invoked to create a Promise
        // However, the Promise object disallows copy. So, we create the Promise
        // with the same underlaying state. It's a trick way, but works
        Promise<T, E> get_return_object() noexcept {
            promise_.StatPtr()->AttachCoroutineHandle(
                std::coroutine_handle<promise_type>::from_promise(*this));

            return Promise<T, E>(promise_.SharedPtr());
        }

        void return_value(T&& val) noexcept {
            promise_.GetResolver().Resolve(std::forward<T>(val));
        }

        void return_value(E&& e) noexcept {
            promise_.GetResolver().Reject(std::forward<E>(e));
        }

        template <typename _T, typename _E>
        void return_value(Result<_T, _E>&& r) noexcept {
            if (r)
                promise_.GetResolver().Resolve(std::move(r.Value()));
            else
                promise_.GetResolver().Reject(std::move(r.Error()));
        }

        // unhandle any exceptions
        void unhandled_exception() {
            std::rethrow_exception(std::current_exception());
        }

        std::suspend_never initial_suspend() const noexcept { return {}; }
        std::suspend_never final_suspend() const noexcept { return {}; }

       private:
        Promise<T, E> promise_;
    };
};

template <typename E>
struct CoroutineTrait<void, E> {
    class promise_type {
       public:
        promise_type() = default;
        promise_type(promise_type&&) = delete;
        promise_type(const promise_type&) = delete;

        Promise<void, E> get_return_object() noexcept {
            promise_.StatPtr()->AttachCoroutineHandle(
                std::coroutine_handle<promise_type>::from_promise(*this));

            return Promise<void, E>(promise_.SharedPtr());
        }

        // the return_void and return_value method cannot co-exist
        // when co_await promise<void>, need to use Result<void, E> explicitly
        void return_value(E&& e) noexcept {
            promise_.GetResolver().Reject(std::forward<E>(e));
        }

        template <typename _E>
        void return_value(Result<void, _E>&& r) noexcept {
            if (!r.IsValue()) {
                promise_.GetResolver().Resolve();
            } else {
                promise_.GetResolver().Reject(std::move(r.Error()));
            }
        }

        // unhandle any exceptions
        void unhandled_exception() noexcept {
            std::rethrow_exception(std::current_exception());
        }

        std::suspend_never initial_suspend() const noexcept { return {}; }
        std::suspend_never final_suspend() const noexcept { return {}; }

       private:
        Promise<void, E> promise_;
    };
};

template <typename T, typename E>
class PromiseAwaiter {
   public:
    explicit PromiseAwaiter(Promise<T, E>&& promise)
        : promise_(std::move(promise)) {}

    ~PromiseAwaiter() {}

    bool await_ready() noexcept { return promise_.IsPending(); }

    void await_suspend(std::coroutine_handle<> handle) noexcept {
        auto current = EventLoop::Current();

        promise_.Then(
            [this, handle](Result<T, E>&& r) mutable {
                res_ = std::move(r);
                handle.resume();
            },
            current);
    }

    auto await_resume() {
        // Notes, for the resolved/rejected promise, await_ready will return
        // true and resume the coroutine immediately In this case, the result
        // isn't initialized. So we need get the result using the async
        // executor, that is, nullptr
        if (promise_.IsPending()) {
            promise_.Then(
                [this](Result<T, E>&& r) mutable { res_ = std::move(r); },
                nullptr);
        }
        return std::move(res_);
    }

   private:
    Promise<T, E> promise_;
    Result<T, E> res_;
};

template <typename T, typename E>
auto operator co_await(Promise<T, E>&& p) noexcept {
    return PromiseAwaiter<T, E>(std::move(p));
}

template <typename T, typename E>
auto operator co_await(Promise<T, E>& p) noexcept {
    // copy a new promise with the shared promise state
    return PromiseAwaiter<T, E>(Promise<T, E>(p.SharedPtr()));
}

}  // namespace evcpp

namespace std {

template <typename T, typename E, typename... Args>
struct coroutine_traits<::evcpp::Promise<T, E>, Args...>
    : ::evcpp::CoroutineTrait<T, E> {};

}  // namespace std
