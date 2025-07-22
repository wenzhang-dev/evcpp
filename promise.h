#pragma once

#include <event_loop.h>

#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <optional>

namespace evcpp {

template <typename T, typename E>
class Promise;

template <typename T>
struct IsPromise : std::false_type {};

template <typename T, typename E>
struct IsPromise<Promise<T, E>> : std::true_type {};

enum class PromiseStatus {
    kInit,
    kPreResolved,
    kResolved,
    kPreRejected,
    kRejected,
    kCancelled,
};

class PromiseStateBase : public std::enable_shared_from_this<PromiseStateBase> {
   public:
    PromiseStateBase(PromiseStatus status, Executor* exec)
        : status_(status), exec_(exec), next_(nullptr), prev_() {}

    virtual ~PromiseStateBase() { BreakPromiseChain(); }

    struct Propagator {
        virtual ~Propagator() {}

        virtual void PropagateResult(void*) = 0;
        virtual void PropagatePromise(void*) = 0;
    };

    virtual Propagator* propagator() { return nullptr; }
    virtual Propagator* next_propagator() { return nullptr; }

    PromiseStateBase* previous() { return prev_.get(); }
    PromiseStateBase* next() { return next_; }

    PromiseStatus GetStatus() const { return status_; }

    Executor* GetExecutor() { return exec_; }
    const Executor* GetExecutor() const { return exec_; }

    virtual bool OnCancel() = 0;
    bool Cancel() {
        switch (status_) {
            case PromiseStatus::kInit:
            case PromiseStatus::kPreRejected:
            case PromiseStatus::kPreResolved:
                status_ = PromiseStatus::kCancelled;

                if (co_handle_) {
                    co_handle_.destroy();
                }

                return OnCancel();
            default:
                return false;
        }
    }

    // the previous Promise holds the next shared_ptr
    void Watch(PromiseStateBase* other) {
        prev_ = other->shared_from_this();
        other->next_ = this;
    }

    void BreakPromiseChain() {
        if (prev_) {
            prev_->next_ = nullptr;
            prev_.reset();
        }
    }

    void AttachCoroutineHandle(std::coroutine_handle<> handle) {
        co_handle_ = handle;
    }

   protected:
    PromiseStatus status_;
    Executor* exec_;

    // although a Promise<void, E> cannot link promises backwards,
    // but it may still link promise<T, E> forwards
    PromiseStateBase* next_;
    std::shared_ptr<PromiseStateBase> prev_;

    // it used to cancel promise and release the resource of coroutine
    std::coroutine_handle<> co_handle_;
};

template <typename T, typename E>
class PromiseStateInternal : public PromiseStateBase {
   public:
    using Callback = VariantCallback<void(Result<T, E>&&)>;

    PromiseStateInternal(PromiseStatus status, Executor* exec)
        : PromiseStateBase(status, exec) {}

    bool OnCancel() override {
        cb_ = std::nullopt;
        storage_ = std::nullopt;

        return next_ ? next_->Cancel() : true;
    }

    bool HasHandler() const { return cb_.has_value(); }

    void AddCallback(Callback&& cb, Executor* exec) {
        exec_ = exec;
        cb_.emplace(std::move(cb));

        TryInvokeCallback();
    }

    void TryInvokeCallback() {
        if (!cb_.has_value()) {
            return;
        }

        if (status_ != PromiseStatus::kPreResolved &&
            status_ != PromiseStatus::kPreRejected) {
            return;
        }

        auto cb = std::move(cb_.value());
        cb_ = std::nullopt;

        auto val = std::move(storage_.value());
        storage_ = std::nullopt;

        if (status_ == PromiseStatus::kPreResolved) {
            status_ = PromiseStatus::kResolved;
        } else if (status_ == PromiseStatus::kPreRejected) {
            status_ = PromiseStatus::kRejected;
        }

        RunInExecutor(WrapVariantCallback(std::move(cb), std::move(val)));
    }

    void RunInExecutor(VariantCallback<void()>&& cb) {
        if (exec_) {
            exec_->Post(std::move(cb));
        } else {
            InvokeVariantCallback(cb);
        }
    }

   protected:
    std::optional<Result<T, E>> storage_;
    std::optional<Callback> cb_;
};

template <typename T, typename E>
class PromiseState : public PromiseStateInternal<T, E>,
                     public PromiseStateBase::Propagator {
   public:
    using Base = PromiseStateInternal<T, E>;

    static_assert(!std::is_same_v<E, void>, "E must not be void");

    explicit PromiseState(Executor* exec = nullptr)
        : Base(PromiseStatus::kInit, exec) {}

    PromiseState(PromiseState&&) = default;
    PromiseState& operator=(PromiseState&&) = default;

    PromiseState(const PromiseState&) = delete;
    PromiseState& operator=(const PromiseState&) = delete;

   public:
    bool Reject(E&& err) {
        if (Base::status_ != PromiseStatus::kInit) {
            return false;
        }

        Base::status_ = PromiseStatus::kPreRejected;
        Base::storage_.emplace(std::forward<E>(err));

        Base::TryInvokeCallback();
        return true;
    }

    bool Resolve(T&& v) {
        if (Base::status_ != PromiseStatus::kInit) {
            return false;
        }

        Base::status_ = PromiseStatus::kPreResolved;
        Base::storage_.emplace(std::forward<T>(v));

        Base::TryInvokeCallback();
        return true;
    }

   public:
    void PropagateResult(void* result) override;
    void PropagatePromise(void* promise) override;

    Propagator* propagator() override { return this; }
    Propagator* next_propagator() override {
        return Base::next_ ? Base::next_->propagator() : nullptr;
    }

   public:
    // specialization 1:
    // Promise<int> p;
    // p.Then([&](Result<int>&& r) -> void {
    //    return;
    // }, &executor);
    template <typename F, typename R = std::invoke_result_t<F, T>,
              std::enable_if_t<std::is_void<R>::value, int> _ = 0>
    void Attach(F&& callback, Executor* exec) {
        auto weak_promise = this->weak_from_this();
        auto cb = [f = std::forward<F>(callback),
                   weak_promise = std::move(weak_promise)](
                      Result<T, E>&& v) mutable -> void {
            if (auto p = weak_promise.lock(); p) {
                std::invoke(std::forward<F>(f), std::move(v));
            }
        };

        Base::AddCallback(MakeCallback(std::move(cb)), exec);
    }

    // specialization 2:
    // Promise<int> p;
    // auto p1 = p.Then([&](Result<int>&& r) -> Result<bool> {
    //    return false;
    // }, &exectuor);
    template <typename _U, typename _E, typename F,
              typename R = std::invoke_result_t<F, T>,
              std::enable_if_t<IsResult<R>::value, int> _ = 0>
    void Attach(PromiseState<_U, _E>* next, F&& callback, Executor* exec) {
        next->Watch(this);

        auto weak_state = this->weak_from_this();
        auto cb = [f = std::forward<F>(callback),
                   weak_state = std::move(weak_state)](
                      Result<T, E>&& v) mutable -> void {
            if (auto state_ptr = weak_state.lock(); state_ptr) {
                auto pp = state_ptr->next_propagator();
                auto result = std::invoke(std::forward<F>(f), std::move(v));

                if (pp) {
                    pp->PropagateResult(&result);
                }
            }
        };

        Base::AddCallback(MakeCallback(std::move(cb)), exec);
    }

    // specialization 3:
    // Promise<int> p;
    // Promise<bool> p1 = p.Then([&](Result<int>&& r) -> Promise<bool> {
    //  return true;
    // }, &exec);
    template <typename _U, typename _E, typename F,
              typename R = std::invoke_result_t<F, T>,
              std::enable_if_t<IsPromise<R>::value, int> _ = 0>
    void Attach(PromiseState<_U, _E>* next, F&& callback, Executor* exec) {
        next->Watch(this);

        auto weak_state = this->weak_from_this();
        auto cb = [f = std::forward<F>(callback),
                   weak_state = std::move(weak_state)](
                      Result<T, E>&& v) mutable -> void {
            if (auto state_ptr = weak_state.lock(); state_ptr) {
                auto pp = state_ptr->next_propagator();
                auto inner_promise =
                    std::invoke(std::forward<F>(f), std::move(v));

                // the type T and E of inner_promise should be same to the pp
                // it's safe to convert from inner_promise to Promise<pp'T,pp'E>
                if (pp) {
                    pp->PropagatePromise(&inner_promise);
                }
            }
        };

        Base::AddCallback(MakeCallback(std::move(cb)), exec);
    }

    template <typename _T, typename _E>
    friend class PromiseState;
};

template <typename E>
class PromiseState<void, E> : public PromiseStateInternal<void, E> {
   public:
    using Base = PromiseStateInternal<void, E>;

    static_assert(!std::is_same_v<E, void>, "E must not be void");

    explicit PromiseState(Executor* exec = nullptr)
        : PromiseStateInternal<void, E>(PromiseStatus::kInit, exec) {}

    PromiseState(PromiseState&&) = default;
    PromiseState& operator=(PromiseState&&) = default;

    PromiseState(const PromiseState&) = delete;
    PromiseState& operator=(const PromiseState&) = delete;

   public:
    bool Reject(E&& err) {
        if (Base::status_ != PromiseStatus::kInit) {
            return false;
        }

        Base::status_ = PromiseStatus::kPreRejected;
        Base::storage_.emplace(std::forward<E>(err));

        Base::TryInvokeCallback();
        return true;
    }

    bool Resolve() {
        if (Base::status_ != PromiseStatus::kInit) {
            return false;
        }

        Base::status_ = PromiseStatus::kPreResolved;
        Base::storage_.emplace(Result<void, E>());

        Base::TryInvokeCallback();
        return true;
    }

   public:
    // specialization 1:
    // Promise<int> p;
    // p.Then([&](Result<int>&& r) -> void {
    //    return;
    // }, &executor);
    template <typename F, typename R = std::invoke_result_t<F, E>>
    void Attach(F&& callback, Executor* exec) {
        static_assert(std::is_same_v<R, void>, "callback must return void");

        auto weak_promise = this->weak_from_this();
        auto cb = [f = std::forward<F>(callback),
                   weak_promise = std::move(weak_promise)](
                      Result<void, E>&& v) mutable -> void {
            if (auto p = weak_promise.lock(); p) {
                std::invoke(std::forward<F>(f), std::move(v));
            }
        };

        Base::AddCallback(MakeCallback(std::move(cb)), exec);
    }
};

template <typename T, typename E = std::error_code>
class Resolver {
   public:
    Resolver(const std::shared_ptr<PromiseState<T, E>>& ptr) : stat_(ptr) {}

    Resolver(Resolver&&) = default;
    Resolver(const Resolver&) = default;
    Resolver& operator=(Resolver&&) = default;
    Resolver& operator=(const Resolver&) = default;

   public:
    bool Cancel() {
        if (auto stat = stat_.lock(); stat) {
            return stat->Cancel();
        }
        return false;
    }

    template <typename U>
    bool Reject(U&& e) {
        if (auto stat = stat_.lock(); stat) {
            return stat->Reject(std::forward<U>(e));
        }
        return false;
    }

    template <typename U>
    bool Resolve(U&& v) {
        if (auto stat = stat_.lock(); stat) {
            return stat->Resolve(std::forward<U>(v));
        }
        return false;
    }

    std::optional<PromiseStatus> GetStatus() const {
        if (auto stat = stat_.lock(); stat) {
            return stat->GetStatus();
        }
        return {};
    }

   private:
    std::weak_ptr<PromiseState<T, E>> stat_;
};

template <typename E>
class Resolver<void, E> {
   public:
    Resolver(const std::shared_ptr<PromiseState<void, E>>& ptr) : stat_(ptr) {}

    Resolver(Resolver&&) = default;
    Resolver(const Resolver&) = default;
    Resolver& operator=(Resolver&&) = default;
    Resolver& operator=(const Resolver&) = default;

   public:
    bool Cancel() {
        if (auto stat = stat_.lock(); stat) {
            return stat->Cancel();
        }
        return false;
    }

    template <typename U>
    bool Reject(U&& e) {
        if (auto stat = stat_.lock(); stat) {
            return stat->Reject(std::forward<U>(e));
        }
        return false;
    }

    bool Resolve() {
        if (auto stat = stat_.lock(); stat) {
            return stat->Resolve();
        }
        return false;
    }

    std::optional<PromiseStatus> GetStatus() const {
        if (auto stat = stat_.lock(); stat) {
            return stat->GetStatus();
        }
        return {};
    }

   private:
    std::weak_ptr<PromiseState<void, E>> stat_;
};

template <typename T, typename E = std::error_code>
class Promise {
   public:
    using ValueType = T;
    using ErrorType = E;
    using ResolverType = Resolver<T, E>;

    explicit Promise(Executor* exec = nullptr)
        : stat_(std::make_shared<PromiseState<T, E>>(exec)) {}

    explicit Promise(std::shared_ptr<PromiseState<T, E>>&& stat)
        : stat_(std::move(stat)) {}

    Promise(Promise&&) = default;
    Promise& operator=(Promise&&) = default;

    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;

   public:
    // specialization 1: the return type is void
    template <typename F, typename R = std::invoke_result_t<F, T>>
    std::enable_if_t<std::is_void_v<R>, void> Then(F&& callback,
                                                   Executor* exec = nullptr) {
        stat_->Attach(std::forward<F>(callback), exec);
    }

    // specialization 2: the return type is promise
    template <typename F, typename R = std::invoke_result_t<F, T>>
    std::enable_if_t<IsPromise<R>::value,
                     Promise<typename R::ValueType, typename R::ErrorType>>
    Then(F&& callback, Executor* exec = nullptr) {
        Promise<typename R::ValueType, typename R::ErrorType> next_promise(
            PreferExecutor(exec));
        stat_->Attach(next_promise.StatPtr(), std::forward<F>(callback), exec);
        return next_promise;
    }

    // specialization 3: the return type is result
    template <typename F, typename R = std::invoke_result_t<F, T>>
    std::enable_if_t<IsResult<R>::value,
                     Promise<typename R::ValueType, typename R::ErrorType>>
    Then(F&& callback, Executor* exec = nullptr) {
        Promise<typename R::ValueType, typename R::ErrorType> next_promise(
            PreferExecutor(exec));
        stat_->Attach(next_promise.StatPtr(), std::forward<F>(callback), exec);
        return next_promise;
    }

   public:
    Resolver<T, E> GetResolver() { return Resolver(stat_); }
    PromiseStatus GetStatus() const { return stat_->GetStatus(); }
    bool IsPending() const {
        auto status = GetStatus();
        return status == PromiseStatus::kPreResolved ||
               status == PromiseStatus::kPreRejected;
    }

    bool HasHandler() const { return stat_->HasHandler(); }

    Executor* GetExecutor() { return stat_->GetExecutor(); }
    const Executor* GetExecutor() const { return stat_->GetExecutor(); }

   private:
    PromiseState<T, E>* StatPtr() { return stat_.get(); }
    std::shared_ptr<PromiseState<T, E>> SharedPtr() { return stat_; }

    Executor* PreferExecutor(Executor* prefer) {
        return prefer ? prefer : GetExecutor();
    }

    std::shared_ptr<PromiseState<T, E>> stat_;

    friend class PromiseState<T, E>;

    template <typename _T, typename _E>
    friend class Promise;

    template <typename _T, typename _E>
    friend auto operator co_await(Promise<_T, _E>&) noexcept;

    template <typename _T, typename _E>
    friend auto operator co_await(Promise<_T, _E>&&) noexcept;

    template <typename _T, typename _E>
    friend class PromiseAwaiter;

    template <typename _T, typename _E>
    friend class CoroutineTrait;
};

template <typename E>
class Promise<void, E> {
   public:
    using ValueType = void;
    using ErrorType = E;

    explicit Promise(Executor* exec = nullptr)
        : stat_(std::make_shared<PromiseState<void, E>>(exec)) {}

    explicit Promise(std::shared_ptr<PromiseState<void, E>>&& stat)
        : stat_(std::move(stat)) {}

    Promise(Promise&&) = default;
    Promise& operator=(Promise&&) = default;

    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;

   public:
    // specialization 1: the return type is void
    template <typename F>
    void Then(F&& callback, Executor* exec = nullptr) {
        stat_->Attach(std::forward<F>(callback), exec);
    }

   public:
    Resolver<void, E> GetResolver() { return Resolver(stat_); }
    PromiseStatus GetStatus() const { return stat_->GetStatus(); }
    bool IsPending() const {
        auto status = GetStatus();
        return status == PromiseStatus::kPreResolved ||
               status == PromiseStatus::kPreRejected;
    }

    bool HasHandler() const { return stat_->HasHandler(); }

    Executor* GetExecutor() { return stat_->GetExecutor(); }
    const Executor* GetExecutor() const { return stat_->GetExecutor(); }

   private:
    PromiseState<void, E>* StatPtr() { return stat_.get(); }
    std::shared_ptr<PromiseState<void, E>> SharedPtr() { return stat_; }

    Executor* PreferExecutor(Executor* prefer) {
        return prefer ? prefer : GetExecutor();
    }

    std::shared_ptr<PromiseState<void, E>> stat_;

    friend class PromiseState<void, E>;

    template <typename _T, typename _E>
    friend auto operator co_await(Promise<_T, _E>&) noexcept;

    template <typename _T, typename _E>
    friend auto operator co_await(Promise<_T, _E>&&) noexcept;

    template <typename _T, typename _E>
    friend class PromiseAwaiter;

    template <typename _T, typename _E>
    friend class CoroutineTrait;
};

template <typename T, typename E>
void PromiseState<T, E>::PropagateResult(void* result) {
    auto* r = static_cast<Result<T, E>*>(result);
    if (r) {
        Resolve(std::move(r->Value()));
    } else {
        Reject(std::move(r->Error()));
    }
}

template <typename T, typename E>
void PromiseState<T, E>::PropagatePromise(void* promise) {
    auto* inner_promise = static_cast<Promise<T, E>*>(promise);
    auto* state = inner_promise->StatPtr();

    state->Attach(
        this, [](Result<T, E>&& r) mutable -> Result<T, E> { return r; },
        nullptr);
}

// the return promise can not hold promise container. caller should make sure
// their lifetime. usually, the return type is promise<std::vector<T>, E>. if
// the type of container element is promise<void>, return type of the method is
// also promise<void>
template <typename It,
          typename TraitType = typename std::iterator_traits<It>::value_type,
          typename ValueType = typename TraitType::ValueType,
          typename ErrorType = typename TraitType::ErrorType,
          typename R = std::conditional_t<std::is_void_v<ValueType>, void,
                                          std::vector<ValueType>>>
Promise<R, ErrorType> MkAllPromise(It begin, It end, Executor* exec) {
    Promise<R, ErrorType> promise;

    if (begin == end) {
        if constexpr (std::is_void_v<ValueType>) {
            promise.GetResolver().Resolve();
        } else {
            promise.GetResolver().Resolve(R{});
        }

        return promise;
    }

    struct Ctx {
        std::size_t success_counter;

        // void is incomplete type, so use void*
        std::conditional_t<std::is_void_v<R>, void*, R> results;

        Ctx(std::size_t c) : success_counter(c) {
            if constexpr (!std::is_void_v<ValueType>) {
                results.resize(c);
            }
        }
    };

    auto ctx = std::make_shared<Ctx>(std::distance(begin, end));
    for (auto it = begin, idx = 0; it != end; ++it, ++idx) {
        it->Then(
            MakeCallback([idx, ctx, resolver = promise.GetResolver()](
                             Result<ValueType, ErrorType>&& r) mutable -> void {
                if (r.IsError()) {
                    resolver.Reject(std::move(r.Error()));
                    return;
                }

                if constexpr (!std::is_void_v<ValueType>) {
                    ctx->results[idx] = std::move(r.Value());
                }

                if (--ctx->success_counter > 0) {
                    return;
                }

                if constexpr (std::is_void_v<ValueType>) {
                    resolver.Resolve();
                } else {
                    resolver.Resolve(std::move(ctx->results));
                }
            }),
            exec);
    }

    return promise;
}

template <typename Cntr>
auto MkAllPromise(Cntr&& container, Executor* exec) {
    return MkAllPromise(std::begin(container), std::end(container), exec);
}

template <typename It,
          typename TraitType = typename std::iterator_traits<It>::value_type,
          typename ValueType = typename TraitType::ValueType,
          typename ErrorType = typename TraitType::ErrorType,
          typename ErrorList = std::vector<ErrorType>>
Promise<ValueType, ErrorList> MkAnyPromise(It begin, It end, Executor* exec) {
    Promise<ValueType, ErrorList> promise;

    ASSERT(begin != end);

    struct Ctx {
        std::size_t failure_counter;
        ErrorList errors;

        Ctx(std::size_t c) : failure_counter(c), errors(c) {}
    };

    auto ctx = std::make_shared<Ctx>(std::distance(begin, end));
    for (auto it = begin, idx = 0; it != end; ++it, ++idx) {
        it->Then(
            MakeCallback([ctx, idx, resolver = promise.GetResolver()](
                             Result<ValueType, ErrorType>&& r) mutable -> void {
                if (r.IsError()) {
                    ctx->errors[idx] = std::move(r.Error());

                    if (--ctx->failure_counter == 0) {
                        resolver.Reject(std::move(ctx->errors));
                    }

                    return;
                }

                if constexpr (std::is_void_v<ValueType>) {
                    resolver.Resolve();
                } else {
                    resolver.Resolve(std::move(r.Value()));
                }
            }),
            exec);
    }

    return promise;
}

template <typename Cntr>
auto MkAnyPromise(Cntr&& container, Executor* exec) {
    return MkAnyPromise(std::begin(container), std::end(container), exec);
}

template <typename It,
          typename TraitType = typename std::iterator_traits<It>::value_type,
          typename ValueType = typename TraitType::ValueType,
          typename ErrorType = typename TraitType::ErrorType>
Promise<ValueType, ErrorType> MkRacePromise(It begin, It end, Executor* exec) {
    Promise<ValueType, ErrorType> promise;

    ASSERT(begin != end);

    for (auto it = begin; it != end; ++it) {
        it->Then(
            MakeCallback([resolver = promise.GetResolver()](
                             Result<ValueType, ErrorType>&& r) mutable -> void {
                if (r.IsError()) {
                    resolver.Reject(std::move(r.Error()));
                    return;
                }

                if constexpr (std::is_void_v<ValueType>) {
                    resolver.Resolve();
                } else {
                    resolver.Resolve(std::move(r.Value()));
                }
            }),
            exec);
    }

    return promise;
}

template <typename Cntr>
auto MkRacePromise(Cntr&& container, Executor* exec) {
    return MkRacePromise(std::begin(container), std::end(container), exec);
}

}  // namespace evcpp
