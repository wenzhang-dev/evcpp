#include <evcpp.h>

#include <iostream>
#include <string>
#include <thread>

int main() {
    evcpp::EventLoop* loop;
    std::thread t([&loop]() mutable {
        evcpp::EventLoopLibevImpl el;
        loop = &el;

        el.RunForever();

        std::cout << "children thread exit..." << std::endl;
    });

    std::this_thread::sleep_for(std::chrono::seconds(1));

    // case 1
    evcpp::Promise<int> p1(loop);
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        p1.Then([](evcpp::Result<int>&& r) mutable -> void {
            std::cout << "case 1 done: " << r.Value() << std::endl;
        });
    }));

    // case 2
    evcpp::Promise<std::string> p2(loop);
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        p2.Then(
            [](evcpp::Result<std::string>&& r) mutable -> evcpp::Result<int> {
                auto num = std::stoi(r.Value());
                std::cout << "case 2 done: " << num << std::endl;
                return num;
            });
    }));

    // case 3
    evcpp::Promise<double> p3(loop);
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        p3.Then([](evcpp::Result<double>&& r) mutable -> evcpp::Promise<int> {
            evcpp::Promise<int> p;
            p.GetResolver().Resolve(int(r.Value()));
            std::cout << "case 3 done: " << r.Value() << std::endl;
            return p;
        });
    }));

    // case 4
    evcpp::Promise<void> p4(loop);
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        p4.Then([](evcpp::Result<void>&& r) mutable -> void {
            std::cout << "case 4 done" << std::endl;
        });
    }));

    // case 5
    evcpp::Promise<std::string> p5;
    std::optional<evcpp::Promise<double>::ResolverType> p5_resolver;
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        evcpp::Promise<double> promise(loop);

        p5_resolver.emplace(promise.GetResolver());
        p5 = promise
                 .Then([](evcpp::Result<double>&& r) mutable
                           -> evcpp::Result<int> {
                     std::cout << "case 5 done1: " << r.Value() << std::endl;
                     return int(r.Value());
                 })
                 .Then([](evcpp::Result<int>&& r) mutable
                           -> evcpp::Result<std::string> {
                     std::cout << "case 5 done2: " << r.Value() << std::endl;
                     return std::to_string(r.Value());
                 });
    }));

    // case 6
    evcpp::Promise<int> p6(loop);
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        p6.Then([](evcpp::Result<int>&& r) mutable -> void {
            std::cout << "case 6 done: " << r.Value() << std::endl;
        });
    }));

    // case 7
    evcpp::Promise<int> p7(loop);
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        p7.Then([](evcpp::Result<int>&& r) mutable -> void {
            std::cout << "case 7 done: " << r.Error() << std::endl;
        });
    }));

    // case 8
    evcpp::Promise<int> p8(loop);
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        p8.Then(
            evcpp::MakeCallback([ptr = std::make_unique<int>(5)](
                                    evcpp::Result<int>&& r) mutable -> void {
                std::cout << "case 8 done: " << r.Value() << " " << *ptr
                          << std::endl;
            }));
    }));

    // case 9
    evcpp::Promise<std::vector<int>> p9(loop);
    std::vector<evcpp::Promise<int>> p9s(3);
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        p9 = MkAllPromise(p9s, loop);
        p9.Then(evcpp::MakeCallback(
            [](evcpp::Result<std::vector<int>>&& r) mutable {
                auto& v = r.Value();
                std::cout << "case 9 done: " << v[0] << " " << v[1] << " "
                          << v[2] << std::endl;
            }));
    }));

    // case 10
    evcpp::Promise<void> p10(loop);
    std::vector<evcpp::Promise<void>> p10s(3);
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        p10 = MkAllPromise(p10s, loop);
        p10.Then(evcpp::MakeCallback([](evcpp::Result<void>&& r) mutable {
            std::cout << "case 10 done" << std::endl;
        }));
    }));

    // case 11
    evcpp::Promise<int, std::vector<std::error_code>> p11(loop);
    std::vector<evcpp::Promise<int>> p11s(3);
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        p11 = MkAnyPromise(p11s, loop);
        p11.Then(evcpp::MakeCallback(
            [](evcpp::Result<int, std::vector<std::error_code>>&& r) mutable {
                std::cout << "case 11 done: " << r.Value() << std::endl;
            }));
    }));

    // case 12
    evcpp::Promise<void, std::vector<std::error_code>> p12(loop);
    std::vector<evcpp::Promise<void>> p12s(3);
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        p12 = MkAnyPromise(p12s, loop);
        p12.Then(evcpp::MakeCallback(
            [](evcpp::Result<void, std::vector<std::error_code>>&& r) mutable {
                std::cout << "case 12 done" << std::endl;
            }));
    }));

    // case 13
    evcpp::Promise<int> p13(loop);
    std::vector<evcpp::Promise<int>> p13s(3);
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        p13 = MkRacePromise(p13s, loop);
        p13.Then(evcpp::MakeCallback([](evcpp::Result<int>&& r) mutable {
            std::cout << "case 13 done: " << r.Value() << std::endl;
        }));
    }));

    // case 14
    evcpp::Promise<void> p14(loop);
    std::vector<evcpp::Promise<void>> p14s(3);
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        p14 = MkRacePromise(p12s, loop);
        p14.Then(evcpp::MakeCallback([](evcpp::Result<void>&& r) mutable {
            std::cout << "case 14 done" << std::endl;
        }));
    }));

    std::this_thread::sleep_for(std::chrono::seconds(1));

    // resolve case 1
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        std::cout << "resolve case 1 promise" << std::endl;
        p1.GetResolver().Resolve(123);
    }));

    // resolve case 2
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        std::cout << "resolve case 2 promise" << std::endl;
        p2.GetResolver().Resolve(std::string("456"));
    }));

    // resolve case 3
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        std::cout << "resolve case 3 promise" << std::endl;
        p3.GetResolver().Resolve(3.14);
    }));

    // resolve case 4
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        std::cout << "resolve case 4 promise" << std::endl;
        p4.GetResolver().Resolve();
    }));

    // resolve case 5
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        std::cout << "resolve case 5 promise" << std::endl;
        p5_resolver->Resolve(3.333);
    }));

    // cancel case 6
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        std::cout << "cancel case 6 promise" << std::endl;
        p6.GetResolver().Cancel();
    }));

    // reject case 7
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        std::cout << "reject case 7 promise" << std::endl;
        p7.GetResolver().Reject(
            std::make_error_code(std::errc::result_out_of_range));
    }));

    // resolve case 8
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        std::cout << "resolve case 8 promise" << std::endl;
        p8.GetResolver().Resolve(6);
    }));

    // resolve case 9
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        std::cout << "resolve case 9 promise" << std::endl;
        p9s[0].GetResolver().Resolve(1);
        p9s[1].GetResolver().Resolve(2);
        p9s[2].GetResolver().Resolve(3);
    }));

    // resolve case 10
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        std::cout << "resolve case 10 promise" << std::endl;
        p10s[0].GetResolver().Resolve();
        p10s[1].GetResolver().Resolve();
        p10s[2].GetResolver().Resolve();
    }));

    // resolve case 11
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        std::cout << "resolve case 11 promise" << std::endl;
        p11s[0].GetResolver().Reject(
            std::make_error_code(std::errc::invalid_argument));
        p11s[1].GetResolver().Reject(
            std::make_error_code(std::errc::invalid_argument));
        p11s[2].GetResolver().Resolve(111);
    }));

    // resolve case 12
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        std::cout << "resolve case 12 promise" << std::endl;
        p12s[0].GetResolver().Reject(
            std::make_error_code(std::errc::invalid_argument));
        p12s[1].GetResolver().Reject(
            std::make_error_code(std::errc::invalid_argument));
        p12s[2].GetResolver().Resolve();
    }));

    // resolve case 13
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        std::cout << "resolve case 13 promise" << std::endl;
        p13s[0].GetResolver().Resolve(222);
    }));

    // resolve case 14
    loop->Dispatch(evcpp::MakeCallback([&]() mutable {
        std::cout << "resolve case 14 promise" << std::endl;
        p14s[0].GetResolver().Resolve();
    }));

    std::this_thread::sleep_for(std::chrono::seconds(5));

    std::cout << "main thread prepare to exit..." << std::endl;

    loop->Stop();
    t.join();

    return 0;
}
