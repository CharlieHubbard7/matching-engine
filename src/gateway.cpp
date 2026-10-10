// TCP gateway: a single-threaded poll() loop that frames the binary protocol
// off each connection and feeds the matching engine. Responses go to the
// connection that sent the request; a fill is also reported to the connection
// that owns the resting order.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

#include "me/protocol.hpp"

using namespace me;

namespace {

constexpr size_t kRxCap = 1 << 16;
constexpr size_t kTxCap = 1 << 20;
constexpr int kMaxConns = 16;

struct Conn {
    int fd = -1;
    Owner session = kNoOwner;  // unique per accepted connection, never reused
    size_t rx_len = 0, tx_len = 0;
    uint64_t msgs = 0;
    bool dead = false;
    // Allocated once at startup, reused across connections.
    std::unique_ptr<uint8_t[]> rx{new uint8_t[kRxCap]};
    std::unique_ptr<uint8_t[]> tx{new uint8_t[kTxCap]};
};

// Write as much of the tx buffer as the socket will take.
void flush(Conn& c) {
    size_t off = 0;
    while (off < c.tx_len) {
        const ssize_t n = ::send(c.fd, c.tx.get() + off, c.tx_len - off, 0);
        if (n > 0) {
            off += size_t(n);
        } else if (errno == EINTR) {
            continue;
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) c.dead = true;
            break;
        }
    }
    std::memmove(c.tx.get(), c.tx.get() + off, c.tx_len - off);
    c.tx_len -= off;
}

template <class M>
void emit(Conn& c, const M& m) {
    if (c.tx_len + sizeof m > kTxCap) {
        // The client is not reading fast enough: wait briefly, then drop it.
        for (int tries = 0; tries < 100 && !c.dead && c.tx_len + sizeof m > kTxCap; ++tries) {
            pollfd p{c.fd, POLLOUT, 0};
            ::poll(&p, 1, 10);
            flush(c);
        }
        if (c.tx_len + sizeof m > kTxCap) c.dead = true;
    }
    if (c.dead) return;
    std::memcpy(c.tx.get() + c.tx_len, &m, sizeof m);
    c.tx_len += sizeof m;
}

struct Gateway {
    explicit Gateway(const EngineConfig& cfg) : engine(cfg) {}

    MatchingEngine engine;
    Conn conns[kMaxConns];
    int nconns = 0;
    Owner next_session = 1;

    Conn* find(Owner session) {
        for (int i = 0; i < nconns; ++i)
            if (conns[i].session == session) return &conns[i];
        return nullptr;  // the owner has disconnected
    }
};

// Engine handler that serialises events into tx buffers.
struct Session {
    Gateway& gw;
    Conn& c;

    void on_trade(const Fill& f) {
        const wire::Trade m{wire::header<wire::Trade>(wire::kTrade), f.resting, f.incoming, f.price, f.qty, f.symbol, 0};
        emit(c, m);
        if (f.resting_owner != c.session)
            if (Conn* maker = gw.find(f.resting_owner)) emit(*maker, m);
    }
    void on_accept(OrderId id, Qty leaves) {
        emit(c, wire::Accepted{wire::header<wire::Accepted>(wire::kAccepted), id, leaves});
    }
    void on_cancel(OrderId id, Qty qty) {
        emit(c, wire::Canceled{wire::header<wire::Canceled>(wire::kCanceled), id, qty});
    }
    void on_reject(OrderId id, Reject r) {
        emit(c, wire::Rejected{wire::header<wire::Rejected>(wire::kRejected), id, uint8_t(r), {}});
    }
};

void on_readable(Gateway& gw, Conn& c) {
    const ssize_t n = ::recv(c.fd, c.rx.get() + c.rx_len, kRxCap - c.rx_len, 0);
    if (n == 0) {
        c.dead = true;
        return;
    }
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) c.dead = true;
        return;
    }
    c.rx_len += size_t(n);

    Session session{gw, c};
    wire::Lookahead ahead(gw.engine, c.rx.get(), c.rx.get() + c.rx_len);
    size_t off = 0;
    while (c.rx_len - off >= sizeof(wire::Header) && !c.dead) {
        const uint8_t* p = c.rx.get() + off;
        wire::Header h;
        std::memcpy(&h, p, sizeof h);
        if (h.len != wire::inbound_size(h.type)) {  // unknown type or wrong length
            c.dead = true;
            break;
        }
        if (c.rx_len - off < h.len) break;
        off += h.len;
        ++c.msgs;
        ahead.advance();
        if (h.type != wire::kNew) {
            // Cancel, Reduce and Replace all carry the target order id first.
            // A session may only touch its own orders.
            OrderId id;
            std::memcpy(&id, p + sizeof h, sizeof id);
            if (gw.engine.owner(id) != c.session) {
                session.on_reject(id, Reject::UnknownOrder);
                continue;
            }
        }
        wire::dispatch(gw.engine, session, p, c.session);
    }
    std::memmove(c.rx.get(), c.rx.get() + off, c.rx_len - off);
    c.rx_len -= off;
}

int listen_on(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || ::listen(fd, 16) < 0) {
        std::perror("listen");
        std::exit(1);
    }
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

}  // namespace

int main(int argc, char** argv) {
    const uint16_t port = argc > 1 ? uint16_t(std::atoi(argv[1])) : 9000;
    const uint32_t max_orders = argc > 2 ? uint32_t(std::strtoul(argv[2], nullptr, 10)) : 1u << 22;
    std::signal(SIGPIPE, SIG_IGN);

    const auto gw_ptr = std::make_unique<Gateway>(
        EngineConfig{.symbols = 65536, .max_orders = max_orders, .max_levels = max_orders / 2});
    Gateway& gw = *gw_ptr;
    pollfd fds[1 + kMaxConns];
    fds[0] = {listen_on(port), POLLIN, 0};
    std::printf("gateway listening on 127.0.0.1:%u (max %u resting orders)\n", unsigned(port), max_orders);
    std::fflush(stdout);

    for (;;) {
        if (::poll(fds, nfds_t(1 + gw.nconns), -1) < 0) {
            if (errno == EINTR) continue;
            std::perror("poll");
            return 1;
        }

        for (int i = 0; i < gw.nconns; ++i) {
            Conn& c = gw.conns[i];
            const short ev = fds[1 + i].revents;
            if (ev & (POLLERR | POLLNVAL)) c.dead = true;
            if (c.dead || !(ev & (POLLIN | POLLHUP))) continue;
            on_readable(gw, c);
            // Send this batch's responses now, including fills owed to other
            // connections, rather than after the remaining connections' reads.
            for (int j = 0; j < gw.nconns; ++j)
                if (gw.conns[j].tx_len && !gw.conns[j].dead) flush(gw.conns[j]);
        }
        for (int i = 0; i < gw.nconns; ++i) {
            Conn& c = gw.conns[i];
            if (!c.dead && (fds[1 + i].revents & POLLOUT)) flush(c);
            fds[1 + i].events = short(POLLIN | (c.tx_len ? POLLOUT : 0));
        }

        if (fds[0].revents & POLLIN) {
            for (;;) {
                const int fd = ::accept(fds[0].fd, nullptr, nullptr);
                if (fd < 0) break;
                if (gw.nconns == kMaxConns) {
                    ::close(fd);
                    continue;
                }
                const int one = 1;
                ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
                Conn& c = gw.conns[gw.nconns];
                c.fd = fd;
                c.session = gw.next_session++;
                c.rx_len = c.tx_len = 0;
                c.msgs = 0;
                c.dead = false;
                fds[1 + gw.nconns] = {fd, POLLIN, 0};
                ++gw.nconns;
            }
        }

        // Reap closed connections. Their resting orders stay in the book.
        for (int i = 0; i < gw.nconns;) {
            if (!gw.conns[i].dead) {
                ++i;
                continue;
            }
            std::printf("session %u closed: %llu messages, %zu orders resting in total\n", gw.conns[i].session,
                        static_cast<unsigned long long>(gw.conns[i].msgs), gw.engine.live_orders());
            std::fflush(stdout);
            ::close(gw.conns[i].fd);
            --gw.nconns;
            std::swap(gw.conns[i], gw.conns[gw.nconns]);
            fds[1 + i] = fds[1 + gw.nconns];
        }
    }
}
