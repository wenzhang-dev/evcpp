#include <evcpp.h>

#include <iostream>
#include <thread>

int main() {
    evcpp::EventLoop *loop;
    std::thread t([&loop]() mutable {
        evcpp::EventLoopLibevImpl el;
        loop = &el;

        auto event1 = el.RunEvery(
            std::chrono::milliseconds(200), evcpp::MakeCallback([]() {
                std::cout << "200 ms timeout" << std::endl;
            }));

        auto event2 =
            el.RunAfter(std::chrono::seconds(1), evcpp::MakeCallback([]() {
                            std::cout << "1 s timeout" << std::endl;
                        }));

        el.Post(evcpp::MakeCallback(
            []() { std::cout << "post task" << std::endl; }));

        el.RunForever();

        std::cout << "children thread exit..." << std::endl;
    });

    std::this_thread::sleep_for(std::chrono::seconds(1));

    loop->Dispatch(
        evcpp::MakeCallback([] { std::cout << "dispatch task" << std::endl; }));

    std::this_thread::sleep_for(std::chrono::seconds(5));

    std::cout << "main thread prepare to exit..." << std::endl;

    loop->Stop();
    t.join();

    return 0;
}
