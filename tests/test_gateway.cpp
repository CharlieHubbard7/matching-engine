// End-to-end test: starts the gateway binary and talks to it over TCP.
//
//   test_gateway <path-to-gateway>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "me/protocol.hpp"

using namespace me;

namespace {

pid_t g_gateway = -1;

[[noreturn]] void fail(const char* file, int line, const char* what) {
    std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, what);
    if (g_gateway > 0) ::kill(g_gateway, SIGKILL);
    std::exit(1);
}
#define CHECK(x) \
    do { \
        if (!(x)) fail(__FILE__, __LINE__, #x); \
    } while (0)

int try_connect(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        ::close(fd);
        return -1;
    }
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    const timeval timeout{5, 0};  // a missing response fails the test instead of hanging it
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    return fd;
}

template <class M>
void send_msg(int fd, const M& m) {
    CHECK(::send(fd, &m, sizeof m, 0) == ssize_t(sizeof m));
}

// Reads exactly one message of type M and checks its header.
template <class M>
M expect(int fd, uint8_t type) {
    M m;
    size_t got = 0;
    while (got < sizeof m) {
        const ssize_t n = ::recv(fd, reinterpret_cast<uint8_t*>(&m) + got, sizeof m - got, 0);
        CHECK(n > 0);
        got += size_t(n);
    }
    CHECK(m.h.type == type && m.h.len == sizeof m);
    return m;
}

wire::NewOrder new_order(uint64_t id, Side side, uint32_t price, uint32_t qty) {
    return {wire::header<wire::NewOrder>(wire::kNew), id, price, qty, 7, uint8_t(side), 0};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-gateway>\n", argv[0]);
        return 2;
    }
    const uint16_t port = uint16_t(20000 + ::getpid() % 20000);
    const std::string port_arg = std::to_string(port);
    g_gateway = ::fork();
    if (g_gateway == 0) {
        ::execl(argv[1], argv[1], port_arg.c_str(), "1024", static_cast<char*>(nullptr));
        std::perror("exec gateway");
        std::_Exit(127);
    }

    int maker = -1;
    for (int i = 0; i < 100 && maker < 0; ++i) {
        maker = try_connect(port);
        if (maker < 0) ::usleep(50'000);
    }
    CHECK(maker >= 0);
    const int taker = try_connect(port);
    CHECK(taker >= 0);

    // The maker rests a sell order.
    send_msg(maker, new_order(1, Side::Sell, 100, 10));
    const auto rested = expect<wire::Accepted>(maker, wire::kAccepted);
    CHECK(rested.id == 1 && rested.leaves == 10);

    // The taker lifts part of it: both sides hear about the trade.
    send_msg(taker, new_order(2, Side::Buy, 105, 4));
    for (const int fd : {taker, maker}) {
        const auto t = expect<wire::Trade>(fd, wire::kTrade);
        CHECK(t.resting_id == 1 && t.incoming_id == 2 && t.price == 100 && t.qty == 4 && t.symbol == 7);
    }
    const auto done = expect<wire::Accepted>(taker, wire::kAccepted);
    CHECK(done.id == 2 && done.leaves == 0);

    // A session cannot cancel, reduce or replace another session's order.
    send_msg(taker, wire::Cancel{wire::header<wire::Cancel>(wire::kCancel), 1});
    send_msg(taker, wire::Reduce{wire::header<wire::Reduce>(wire::kReduce), 1, 1});
    send_msg(taker, wire::Replace{wire::header<wire::Replace>(wire::kReplace), 1, 9, 100, 1});
    for (int i = 0; i < 3; ++i) {
        const auto j = expect<wire::Rejected>(taker, wire::kRejected);
        CHECK(j.id == 1 && j.reason == uint8_t(Reject::UnknownOrder));
    }

    // The owner can; a replaced order keeps its owner.
    send_msg(maker, wire::Replace{wire::header<wire::Replace>(wire::kReplace), 1, 3, 101, 6});
    const auto replaced = expect<wire::Accepted>(maker, wire::kAccepted);
    CHECK(replaced.id == 3 && replaced.leaves == 6);
    send_msg(maker, wire::Cancel{wire::header<wire::Cancel>(wire::kCancel), 3});
    const auto canceled = expect<wire::Canceled>(maker, wire::kCanceled);
    CHECK(canceled.id == 3 && canceled.qty == 6);

    // A message split across two TCP segments is reassembled.
    const auto split = new_order(4, Side::Buy, 90, 1);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&split);
    CHECK(::send(maker, bytes, 5, 0) == 5);
    ::usleep(50'000);
    CHECK(::send(maker, bytes + 5, sizeof split - 5, 0) == ssize_t(sizeof split - 5));
    CHECK(expect<wire::Accepted>(maker, wire::kAccepted).id == 4);

    // A malformed frame gets the connection closed, and nothing else.
    const int bad = try_connect(port);
    CHECK(bad >= 0);
    send_msg(bad, wire::Header{999, wire::kNew, 0});
    uint8_t byte;
    CHECK(::recv(bad, &byte, 1, 0) == 0);
    send_msg(maker, wire::Cancel{wire::header<wire::Cancel>(wire::kCancel), 4});
    CHECK(expect<wire::Canceled>(maker, wire::kCanceled).id == 4);

    ::kill(g_gateway, SIGTERM);
    ::waitpid(g_gateway, nullptr, 0);
    std::puts("gateway test passed");
}
