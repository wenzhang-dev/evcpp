#pragma once

#include <ev.h>
#include <event_loop.h>

#include <mutex>
#include <vector>

namespace evcpp {

class EventLoopLibevImpl;

template <typename T>
struct DoubleLinkObject {
    T* prev;
    T* next;

    DoubleLinkObject() : prev(Self()), next(Self()) {}

    void Unlink() {
        if (prev && next) {
            prev->next = next;
            next->prev = prev;

            prev = Self();
            next = Self();
        }
    }

    void Link(T* head) {
        prev = head;
        next = head->next;
        head->next->prev = Self();
        head->next = Self();
    }

    static bool Iterate(T* head, std::function<bool(T*)>&& cb) {
        auto node = head->next;
        while (node != head) {
            auto next_node = node->next;

            if (!cb(node)) {
                return false;
            }

            node = next_node;
        }

        return true;
    }

    auto Self() { return static_cast<T*>(this); }
};

class TimerEventLibevImpl : public DoubleLinkObject<TimerEventLibevImpl>,
                            public TimerEvent {
   public:
    TimerEventLibevImpl()
        : repeat_(false), cancelled_(false), fired_(false), ev_(nullptr) {}

    TimerEventLibevImpl(EventLoopLibevImpl* ev, VariantCallback<void()>&& cb,
                        std::chrono::milliseconds after, bool repeat)
        : cb_(std::move(cb)),
          after_(after),
          repeat_(repeat),
          cancelled_(false),
          fired_(false),
          ev_(ev) {
        Init();
    }

    TimerEventLibevImpl(TimerEventLibevImpl&&) = default;
    TimerEventLibevImpl(const TimerEventLibevImpl&) = delete;

    TimerEventLibevImpl& operator=(TimerEventLibevImpl&&) = default;
    TimerEventLibevImpl& operator=(const TimerEventLibevImpl&) = delete;

   public:
    void Cancel() override;

    bool Fired() const override { return fired_; }
    bool Cancelled() const override { return cancelled_; }

    ~TimerEventLibevImpl() override { Cancel(); }

   private:
    void Init();

    static void TimerCallback(EV_P_ ev_timer* w, int revents) {
        auto impl = static_cast<TimerEventLibevImpl*>(w->data);

        InvokeVariantCallback(impl->cb_);
        impl->fired_ = true;

        if (!impl->repeat_) {
            ev_timer_stop(EV_A_ w);
            impl->Unlink();
        }
    }

    struct ev_timer watcher_;
    VariantCallback<void()> cb_;
    std::chrono::milliseconds after_;

    bool repeat_;
    bool cancelled_;
    bool fired_;

    EventLoopLibevImpl* ev_;
};

class IOEventLibevImpl : public DoubleLinkObject<IOEventLibevImpl>,
                         public IOEvent {
   public:
    IOEventLibevImpl() : cancelled_(false), fired_(false), ev_(nullptr) {}

    IOEventLibevImpl(EventLoopLibevImpl* ev, Fd fd, IOEventType type,
                     VariantCallback<void()>&& cb)
        : fd_(fd),
          cb_(std::move(cb)),
          type_(type),
          cancelled_(false),
          fired_(false),
          ev_(ev) {
        Init();
    }

    void Cancel() override;

    bool Fired() const override { return fired_; }
    bool Cancelled() const override { return cancelled_; }

    ~IOEventLibevImpl() override { Cancel(); }

   private:
    void Init();

    static void IOCallback(EV_P_ ev_io* w, int revents) {
        auto impl = static_cast<IOEventLibevImpl*>(w->data);

        InvokeVariantCallback(impl->cb_);
        impl->fired_ = true;

        ev_io_stop(EV_A_ w);
        impl->Unlink();
    }

    struct ev_io watcher_;

    Fd fd_;
    VariantCallback<void()> cb_;
    IOEventType type_;

    bool cancelled_;
    bool fired_;

    EventLoopLibevImpl* ev_;
};

class EventLoopLibevImpl : public EventLoop {
   public:
    EventLoopLibevImpl(std::chrono::milliseconds sys_timer_interval =
                           std::chrono::milliseconds(5))
        : status_(Status::kInit),
          sys_timer_interval_(sys_timer_interval),
          loop_(ev_loop_new(0)) {
        Initialize();
        tls_loop = this;
    }

    EventLoopLibevImpl(const EventLoopLibevImpl&) = delete;
    EventLoopLibevImpl& operator=(const EventLoopLibevImpl&) = delete;

    ~EventLoopLibevImpl() override {
        ev_loop_destroy(loop_);
        tls_loop = nullptr;
    }

   public:
    void Dispatch(VariantCallback<void()>&& cb,
                  Priority prio = Priority::kLow) override {
        std::lock_guard<std::mutex> lock(mu_);
        Post(std::move(cb), prio);
    }

    void Post(VariantCallback<void()>&& cb,
              Priority prio = Priority::kLow) override {
        auto idx = static_cast<int>(prio);
        cbs_[idx].push_back(std::move(cb));
    }

   public:
    std::unique_ptr<TimerEvent> RunAfter(
        std::chrono::milliseconds delay,
        VariantCallback<void()>&& cb) override {
        return std::unique_ptr<TimerEvent>(
            new TimerEventLibevImpl(this, std::move(cb), delay, false));
    }

    std::unique_ptr<TimerEvent> RunEvery(
        std::chrono::milliseconds interval,
        VariantCallback<void()>&& cb) override {
        return std::unique_ptr<TimerEvent>(
            new TimerEventLibevImpl(this, std::move(cb), interval, true));
    }

   public:
    std::unique_ptr<IOEvent> AddIOEvent(Fd fd, IOEventType type,
                                        VariantCallback<void()>&& cb) override {
        return std::unique_ptr<IOEvent>(
            new IOEventLibevImpl(this, fd, type, std::move(cb)));
    }

   public:
    void RunForever() override {
        status_ = Status::kRunning;
        ev_run(loop_, 0);
    }

    void Stop() override {
        // ev_xxx must not be invoked across thread
        Dispatch(MakeCallback([this]() mutable {
            status_ = Status::kStopping;

            CancelAllEvents();
            ev_break(loop_, EVBREAK_ALL);

            status_ = Status::kStopped;
        }));
    }

    Status GetStatus() const override { return status_; }

   private:
    void Initialize() {
        sys_timer_ =
            RunEvery(sys_timer_interval_,
                     MakeCallback([this]() mutable { SysTimerCallback(); }));
    }

    void SysTimerCallback() {
        sys_timer_iterations_++;

        std::vector<VariantCallback<void()>> cbs;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cbs.swap(cbs_[0]);
        }

        high_task_num_ += RunTasks(std::move(cbs));

        cbs.clear();
        {
            std::lock_guard<std::mutex> lock(mu_);
            cbs.swap(cbs_[1]);
        }

        medium_task_num_ += RunTasks(std::move(cbs));

        cbs.clear();
        {
            std::lock_guard<std::mutex> lock(mu_);
            cbs.swap(cbs_[2]);
        }

        low_task_num_ += RunTasks(std::move(cbs));
    }

    std::uint64_t RunTasks(std::vector<VariantCallback<void()>>&& cbs) {
        for (auto& cb : cbs) {
            InvokeVariantCallback(cb);
        }

        return cbs.size();
    }

    void CancelAllEvents() {
        IOEventLibevImpl::Iterate(&io_head_,
                                  [](IOEventLibevImpl* event) -> bool {
                                      event->Cancel();
                                      return true;
                                  });

        TimerEventLibevImpl::Iterate(&timer_head_,
                                     [](TimerEventLibevImpl* event) -> bool {
                                         event->Cancel();
                                         return true;
                                     });
    }

    Status status_;

    std::array<std::vector<VariantCallback<void()>>, 3> cbs_;

    std::mutex mu_;

    IOEventLibevImpl io_head_;
    TimerEventLibevImpl timer_head_;

    std::chrono::milliseconds sys_timer_interval_;
    std::unique_ptr<TimerEvent> sys_timer_;

    std::uint64_t sys_timer_iterations_;
    std::uint64_t high_task_num_;
    std::uint64_t medium_task_num_;
    std::uint64_t low_task_num_;

    struct ev_loop* loop_;

    friend class IOEventLibevImpl;
    friend class TimerEventLibevImpl;
};

void TimerEventLibevImpl::Cancel() {
    if (cancelled_ || !ev_) {
        return;
    }

    ev_timer_stop(ev_->loop_, &watcher_);
    cancelled_ = true;
    Unlink();
}

void TimerEventLibevImpl::Init() {
    double interval = after_.count() / 1000.0;
    ev_timer_init(&watcher_, TimerCallback, interval, repeat_ ? interval : 0.0);
    watcher_.data = this;

    Link(&ev_->timer_head_);

    ev_timer_start(ev_->loop_, &watcher_);
}

void IOEventLibevImpl::Cancel() {
    if (cancelled_ || !ev_) {
        return;
    }

    ev_io_stop(ev_->loop_, &watcher_);
    cancelled_ = true;
    Unlink();
}

void IOEventLibevImpl::Init() {
    int ev_flag = 0;
    if (type_ == IOEventType::kRead) {
        ev_flag = EV_READ;
    } else if (type_ == IOEventType::kWrite) {
        ev_flag = EV_WRITE;
    }

    ev_io_init(&watcher_, IOCallback, fd_, ev_flag);
    watcher_.data = this;

    Link(&ev_->io_head_);

    ev_io_start(ev_->loop_, &watcher_);
}

}  // namespace evcpp
