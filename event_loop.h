#pragma once

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <variant>

#define ASSERT(expr)                                                     \
    do {                                                                 \
        if (!(expr)) {                                                   \
            std::cerr << "Assertion failed: " << #expr << ", file "      \
                      << __FILE__ << ", line " << __LINE__ << std::endl; \
            std::abort();                                                \
        }                                                                \
    } while (0)

namespace evcpp {

template <typename T, typename E = std::error_code>
class Result {
   public:
    using ValueType = T;
    using ErrorType = E;
    static_assert(!std::is_same_v<T, E>, "T must not be E");
    static_assert(!std::is_same_v<E, void>, "E must not be void");

    Result() = default;
    Result(T&& r) : storage_(std::forward<T>(r)) {}
    Result(E&& err) : storage_(std::forward<E>(err)) {}

    Result(Result&&) = default;
    Result(const Result&) = default;
    Result& operator=(Result&&) = default;
    Result& operator=(const Result&) = default;

    bool IsError() const { return storage_.index() == kError; }
    bool IsValue() const { return storage_.index() == kValue; }

    operator bool() { return IsValue(); }

    T& Value() {
        auto ptr = std::get_if<T>(&storage_);
        return *ptr;
    }
    const T& Value() const {
        auto ptr = std::get_if<T>(&storage_);
        return *ptr;
    }

    E& Error() {
        auto ptr = std::get_if<E>(&storage_);
        return *ptr;
    }
    const E& Error() const {
        auto ptr = std::get_if<E>(&storage_);
        return *ptr;
    }

    T ValueOr(T&& default_value) {
        auto ptr = std::get_if<T>(&storage_);
        return ptr ? *ptr : default_value;
    }

    E ErrorOr(E&& default_error) {
        auto ptr = std::get_if<E>(&storage_);
        return ptr ? *ptr : default_error;
    }

   private:
    // order matters
    enum Type {
        kNull = 0,
        kValue,
        kError,
    };
    std::variant<std::monostate, T, E> storage_;
};

template <typename E>
class Result<void, E> {
   public:
    using ValueType = void;
    using ErrorType = E;
    static_assert(!std::is_same_v<E, void>, "E must not be void");

    Result() = default;
    Result(E&& err) : err_(err) {}

    Result(Result&&) = default;
    Result(const Result&) = default;
    Result& operator=(Result&&) = default;
    Result& operator=(const Result&) = default;

    bool IsError() const { return err_.has_value(); }
    bool IsValue() const { return false; }

    operator bool() { return false; }

    E& Error() { return err_.value(); }
    const E& Error() const { return err_.value(); }

    E ErrorOr(E&& default_error) {
        return err_.has_value() ? err_.value() : default_error;
    }

   private:
    std::optional<E> err_;
};

template <typename T>
struct IsResult : std::false_type {};

template <typename T, typename E>
struct IsResult<Result<T, E>> : std::true_type {};

using Callback = std::function<void()>;

template <typename S>
struct MoveOnlyCallable;

template <typename R, typename... Args>
struct MoveOnlyCallable<R(Args...)> {
    struct CallableBase {
        virtual ~CallableBase() = default;
        virtual R Invoke(Args&&... args) = 0;
    };

    template <typename F>
    struct CallableImpl : public CallableBase {
        F func;

        CallableImpl(F&& f) : func(std::move(f)) {}

        R Invoke(Args&&... args) override {
            return func(std::forward<Args>(args)...);
        }
    };

    template <typename F>
    MoveOnlyCallable(F&& f)
        : cb(new CallableImpl<std::decay_t<F>>(std::forward<F>(f))) {}

    R operator()(Args&&... args) {
        return cb->Invoke(std::forward<Args>(args)...);
    }

    std::unique_ptr<CallableBase> cb;
};

using MoveOnlyCallback = MoveOnlyCallable<void()>;

template <typename T>
struct FunctionTraits;

template <typename R, typename... Args>
struct FunctionTraits<R (*)(Args...)> {
    using Signature = R(Args...);
    using WrapperSignature = Signature;
};

template <typename R, typename... Args>
struct FunctionTraits<R (&)(Args...)> {
    using Signature = R(Args...);
    using WrapperSignature = Signature;
};

template <typename T, typename R, typename... Args>
struct FunctionTraits<R (T::*)(Args...)> {
    using Signature = R(Args...);
    using WrapperSignature = Signature;
};

template <typename T, typename R, typename... Args>
struct FunctionTraits<R (T::*)(Args...) const> {
    using Signature = R(Args...) const;
    using WrapperSignature = R(Args...);
};

template <typename F>
struct FunctionTraits : FunctionTraits<decltype(&F::operator())> {};

template <typename Sig>
using VariantCallback = std::variant<std::function<Sig>, MoveOnlyCallable<Sig>>;

template <typename Sig, typename... Args>
void InvokeVariantCallback(VariantCallback<Sig>& cb, Args&&... args) {
    std::visit([&](auto&& f) mutable { f(std::forward<Args>(args)...); }, cb);
}

template <typename T>
std::string TypeToString() {
#if defined(__clang__) || defined(__GNUC__)
    return __PRETTY_FUNCTION__;
#elif defined(_MSC_VER)
    return __FUNCSIG__;
#else
    return "Unknown compiler";
#endif
}

template <typename F>
auto MakeCallback(F&& f) {
    using Decayed = std::decay_t<F>;
    using Signature = typename FunctionTraits<Decayed>::WrapperSignature;

    constexpr bool can_use_std_function =
        std::is_copy_constructible_v<Decayed> &&
        std::is_constructible_v<std::function<Signature>, F&&>;

    if constexpr (can_use_std_function) {
        return std::function<Signature>(std::forward<F>(f));
    } else {
        return MoveOnlyCallable<Signature>(std::forward<F>(f));
    }
}

template <typename Sig, typename Arg0>
VariantCallback<void()> WrapVariantCallback(VariantCallback<Sig>&& cb,
                                            Arg0&& arg0) {
    if (std::holds_alternative<std::function<Sig>>(cb)) {
        return MakeCallback(
            [arg0 = std::forward<Arg0>(arg0),
             cb = std::move(std::get<std::function<Sig>>(cb))]() mutable {
                cb(std::forward<Arg0>(arg0));
            });
    } else {
        return MakeCallback(
            [arg0 = std::forward<Arg0>(arg0),
             cb = std::move(std::get<MoveOnlyCallable<Sig>>(cb))]() mutable {
                cb(std::forward<Arg0>(arg0));
            });
    }
}

enum class IOEventType {
    kRead,
    kWrite,
};

enum class Priority {
    kLow = 0,
    kMedium,
    kHigh,
};

using Fd = int;

class TimerEvent {
   public:
    virtual ~TimerEvent() = default;
    virtual void Cancel() = 0;

    virtual bool Fired() const = 0;
    virtual bool Cancelled() const = 0;
};

class IOEvent {
   public:
    virtual ~IOEvent() = default;
    virtual void Cancel() = 0;

    virtual bool Fired() const = 0;
    virtual bool Cancelled() const = 0;
};

class Executor {
   public:
    virtual ~Executor() = default;

    virtual void Post(VariantCallback<void()>&& cb,
                      Priority prio = Priority::kLow) = 0;
};

class RemoteExecutor {
   public:
    virtual ~RemoteExecutor() = default;

    virtual void Dispatch(VariantCallback<void()>&& cb,
                          Priority prio = Priority::kLow) = 0;
};

class TimerProvider {
   public:
    virtual ~TimerProvider() = default;

    virtual std::unique_ptr<TimerEvent> RunAfter(
        std::chrono::milliseconds delay, VariantCallback<void()>&& cb) = 0;

    virtual std::unique_ptr<TimerEvent> RunEvery(
        std::chrono::milliseconds interval, VariantCallback<void()>&& cb) = 0;
};

class IOProvider {
   public:
    virtual ~IOProvider() = default;

    virtual std::unique_ptr<IOEvent> AddIOEvent(
        Fd fd, IOEventType type, VariantCallback<void()>&& cb) = 0;
};

class EventLoop;
inline thread_local EventLoop* tls_loop;

class EventLoop : public Executor,
                  public RemoteExecutor,
                  public TimerProvider,
                  public IOProvider {
   public:
    enum class Status {
        kInit,
        kRunning,
        kStopping,
        kStopped,
    };

    virtual ~EventLoop() = default;

    virtual void RunForever() = 0;
    virtual void Stop() = 0;

    virtual Status GetStatus() const = 0;

    static EventLoop* Current() { return tls_loop; }
};

}  // namespace evcpp
