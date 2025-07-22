#include <evcpp.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

evcpp::Promise<evcpp::Fd> AsyncAccept(evcpp::EventLoop* loop,
                                      evcpp::Fd listen_fd) {
    evcpp::Promise<evcpp::Fd> promise;

    auto io_event = loop->AddIOEvent(
        listen_fd, evcpp::IOEventType::kRead,
        evcpp::MakeCallback(
            [listen_fd, resolver = promise.GetResolver()]() mutable -> void {
                evcpp::Fd client_fd = ::accept(listen_fd, nullptr, nullptr);
                if (client_fd >= 0) {
                    resolver.Resolve(std::move(client_fd));
                } else {
                    auto ec = std::error_code(errno, std::generic_category());
                    resolver.Reject(std::move(ec));
                }
            }));

    co_return co_await promise;
}

evcpp::Promise<ssize_t> AsyncRead(evcpp::EventLoop* loop, evcpp::Fd fd,
                                  char* buf, size_t size) {
    evcpp::Promise<ssize_t> promise;

    auto io_event = loop->AddIOEvent(
        fd, evcpp::IOEventType::kRead,
        evcpp::MakeCallback(
            [fd, buf, size,
             resolver = promise.GetResolver()]() mutable -> void {
                auto n = ::read(fd, buf, size);
                if (n >= 0) {
                    resolver.Resolve(ssize_t(n));
                } else {
                    auto ec = std::error_code(errno, std::generic_category());
                    resolver.Reject(std::move(ec));
                }
            }));

    co_return co_await promise;
}

evcpp::Promise<ssize_t> AsyncWrite(evcpp::EventLoop* loop, evcpp::Fd fd,
                                   const char* buf, size_t size) {
    evcpp::Promise<ssize_t> promise;

    auto io_event = loop->AddIOEvent(
        fd, evcpp::IOEventType::kWrite,
        evcpp::MakeCallback(
            [fd, buf, size,
             resolver = promise.GetResolver()]() mutable -> void {
                auto n = ::write(fd, buf, size);
                if (n >= 0) {
                    resolver.Resolve(std::move(n));
                } else {
                    auto ec = std::error_code(errno, std::generic_category());
                    resolver.Reject(std::move(ec));
                }
            }));

    co_return co_await promise;
}

evcpp::Promise<void> HandleClient(evcpp::EventLoop* loop, evcpp::Fd client_fd) {
    char buffer[1024];

    while (true) {
        auto r1 = co_await AsyncRead(loop, client_fd, buffer, sizeof(buffer));
        if (r1.IsError() || r1.Value() == 0) {
            std::cerr << "fd #" << client_fd << " disconnect" << std::endl;
            break;
        }

        auto size = r1.Value();
        auto r2 = co_await AsyncWrite(loop, client_fd, buffer, size);
        if (r2.IsError()) {
            std::cerr << "fd #" << client_fd << " disconnect" << std::endl;
            break;
        }
    }

    ::close(client_fd);

    co_return evcpp::Result<void>();
}

evcpp::Promise<void> StartEchoServer(evcpp::EventLoop* loop, uint16_t port) {
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    ::sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = ::htons(port);

    ::bind(listen_fd, (sockaddr*)&addr, sizeof(addr));
    ::listen(listen_fd, SOMAXCONN);

    while (true) {
        auto result = co_await AsyncAccept(loop, listen_fd);
        if (result) {
            int client_fd = result.Value();
            HandleClient(loop, client_fd);
        } else {
            std::cerr << "Accept failed: " << ::strerror(result.Error().value())
                      << std::endl;
        }
    }

    ::close(listen_fd);

    co_return evcpp::Result<void>();
}

int main() {
    auto loop = std::make_unique<evcpp::EventLoopLibevImpl>();
    StartEchoServer(loop.get(), 18080);
    loop->RunForever();

    return 0;
}
