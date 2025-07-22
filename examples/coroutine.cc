#include <evcpp.h>

#include <iostream>
#include <string>
#include <thread>

evcpp::Promise<int> TestCase1() {
    std::cout << "case 1 done" << std::endl;
    co_return 2;
}

evcpp::Promise<std::string> TestCase2() {
    auto ev = evcpp::EventLoop::Current();
    evcpp::Promise<int> promise(ev);

    // coroutine hold this context
    auto timer_event = ev->RunAfter(
        std::chrono::milliseconds(100),
        evcpp::MakeCallback([resolver = promise.GetResolver()]() mutable {
            resolver.Resolve(123);
        }));

    auto result = co_await promise;

    std::cout << "case 2 done: " << result.Value() << std::endl;

    co_return std::to_string(result.Value());
}

evcpp::Promise<void> TestCase3() {
    std::cout << "case 3 done" << std::endl;
    co_return evcpp::Result<void>();
}

evcpp::Promise<int> TestCase4() {
    auto ev = evcpp::EventLoop::Current();
    evcpp::Promise<int> promise(ev);

    // never timeout
    auto timer_event = ev->RunAfter(
        std::chrono::seconds(10000),
        evcpp::MakeCallback([resolver = promise.GetResolver()]() mutable {
            resolver.Resolve(123);
        }));

    auto result = co_await promise;

    std::cout << "case 4 nerver done" << std::endl;

    co_return result;
}

evcpp::Promise<std::vector<int>> TestCase5() {
    auto ev = evcpp::EventLoop::Current();
    std::vector<evcpp::Promise<int>> promises(3);

    auto timer_event =
        ev->RunAfter(std::chrono::milliseconds(100),
                     evcpp::MakeCallback([&promises]() mutable -> void {
                         promises[0].GetResolver().Resolve(1);
                         promises[1].GetResolver().Resolve(2);
                         promises[2].GetResolver().Resolve(3);
                     }));

    auto result = co_await MkAllPromise(promises, ev);

    auto v = result.Value();
    std::cout << "case 5 done: " << v[0] << " " << v[1] << " " << v[2]
              << std::endl;

    co_return result;
}

int main() {
    evcpp::EventLoop* loop;
    std::thread t([&loop]() mutable {
        evcpp::EventLoopLibevImpl el;
        loop = &el;

        el.RunForever();

        std::cout << "children thread exit..." << std::endl;
    });

    std::this_thread::sleep_for(std::chrono::seconds(1));

    // test 1
    evcpp::Promise<int> p1;
    loop->Dispatch(evcpp::MakeCallback([&]() { p1 = TestCase1(); }));

    // test 2
    evcpp::Promise<std::string> p2;
    loop->Dispatch(evcpp::MakeCallback([&]() { p2 = TestCase2(); }));

    // test 3
    evcpp::Promise<void> p3;
    loop->Dispatch(evcpp::MakeCallback([&]() { p3 = TestCase3(); }));

    // test 4
    evcpp::Promise<int> p4;
    loop->Dispatch(evcpp::MakeCallback([&]() { p4 = TestCase4(); }));

    // test 5
    evcpp::Promise<std::vector<int>> p5;
    loop->Dispatch(evcpp::MakeCallback([&]() { p5 = TestCase5(); }));

    std::this_thread::sleep_for(std::chrono::seconds(5));

    // resolve test 1: no need
    // resolve test 2: no need
    // resolve test 3: no need
    // cancel test 4
    loop->Dispatch(evcpp::MakeCallback([&]() {
        std::cout << "cancel test 4" << std::endl;
        p4.GetResolver().Cancel();
    }));

    // resolve test 5: no need

    std::cout << "main thread prepare to exit..." << std::endl;

    loop->Stop();
    t.join();

    return 0;
}
