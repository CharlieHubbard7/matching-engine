// Load-test client: replays a file of inbound wire messages against the
// gateway over TCP and reports throughput and round-trip latency.
//
// Messages are sent `window` at a time; the client then waits for all of
// their responses before sending more. window=1 measures pure request/response
// round-trip latency; larger windows measure pipelined throughput.
//
// An optional seed file (the orders resting at the start of the recording) is
// sent first and is not timed.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "me/util.hpp"

using namespace me;

namespace {

struct Client {
    int fd = -1;
    std::vector<uint8_t> rx = std::vector<uint8_t>(1 << 16);
    size_t rx_len = 0;
    uint64_t trades = 0, rejects = 0;

    // Send `window` messages at a time and wait for their responses. If
    // latency_ns is given, records one sample per message: the time from the
    // window's send to the read that completed that message's response.
    void run(const std::vector<uint8_t>& in, size_t window, std::vector<uint32_t>* latency_ns) {
        for (size_t off = 0; off < in.size();) {
            size_t end = off, pending = 0;
            while (end < in.size() && pending < window) {
                uint16_t len;
                std::memcpy(&len, in.data() + end, sizeof len);
                end += len;
                ++pending;
            }

            const uint64_t t0 = now_ns();
            while (off < end) {
                const ssize_t n = ::send(fd, in.data() + off, end - off, 0);
                if (n <= 0) die("send failed");
                off += size_t(n);
            }

            // Each request ends with exactly one non-Trade response.
            while (pending > 0) {
                const ssize_t n = ::recv(fd, rx.data() + rx_len, rx.size() - rx_len, 0);
                if (n <= 0) die("gateway closed the connection");
                const uint64_t t1 = now_ns();
                rx_len += size_t(n);
                size_t p = 0;
                while (rx_len - p >= sizeof(wire::Header)) {
                    wire::Header h;
                    std::memcpy(&h, rx.data() + p, sizeof h);
                    if (h.len != wire::outbound_size(h.type)) die("malformed response");
                    if (rx_len - p < h.len) break;
                    p += h.len;
                    if (h.type == wire::kTrade) {
                        ++trades;
                        continue;
                    }
                    rejects += h.type == wire::kRejected;
                    if (latency_ns) latency_ns->push_back(uint32_t(t1 - t0));
                    --pending;
                }
                std::memmove(rx.data(), rx.data() + p, rx_len - p);
                rx_len -= p;
            }
        }
    }

    [[noreturn]] static void die(const char* what) {
        std::fprintf(stderr, "%s\n", what);
        std::exit(1);
    }
};

}  // namespace

int main(int argc, char** argv) {
    const char *path = nullptr, *seed_path = nullptr, *host = "127.0.0.1";
    size_t window = 1;
    uint16_t port = 9000;
    for (int i = 1; i < argc; ++i) {
        const auto value = [&] { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--seed")) seed_path = value();
        else if (!std::strcmp(argv[i], "--window")) window = size_t(std::max(1, std::atoi(value())));
        else if (!std::strcmp(argv[i], "--host")) host = value();
        else if (!std::strcmp(argv[i], "--port")) port = uint16_t(std::atoi(value()));
        else path = argv[i];
    }
    if (!path) {
        std::fprintf(stderr, "usage: %s <messages.bin> [--seed seed.bin] [--window 1] [--host 127.0.0.1] [--port 9000]\n",
                     argv[0]);
        return 2;
    }
    const std::vector<uint8_t> in = read_file(path);
    const std::vector<uint8_t> seed = seed_path ? read_file(seed_path) : std::vector<uint8_t>{};
    const size_t total = count_inbound(in);
    const size_t seeded = count_inbound(seed);

    Client client;
    client.fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1 ||
        ::connect(client.fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        std::perror("connect");
        return 1;
    }
    const int one = 1;
    ::setsockopt(client.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    client.run(seed, 1024, nullptr);
    if (client.trades || client.rejects) {
        std::fprintf(stderr, "seed did not load cleanly (%llu trades, %llu rejects); restart the gateway\n",
                     static_cast<unsigned long long>(client.trades), static_cast<unsigned long long>(client.rejects));
        return 1;
    }

    std::vector<uint32_t> latency_ns;
    latency_ns.reserve(total);
    const uint64_t start = now_ns();
    client.run(in, window, &latency_ns);
    const double secs = double(now_ns() - start) / 1e9;
    ::close(client.fd);

    if (seeded) std::printf("seed        %zu resting orders loaded before timing\n", seeded);
    std::printf("messages    %zu (window %zu)\n", total, window);
    std::printf("trades      %llu\n", static_cast<unsigned long long>(client.trades));
    std::printf("rejects     %llu\n", static_cast<unsigned long long>(client.rejects));
    std::printf("elapsed     %.3f s\n", secs);
    std::printf("throughput  %.0f msgs/sec\n", double(total) / secs);
    std::printf("round trip  p50 %.1f us   p99 %.1f us   p99.9 %.1f us   (send of window -> response)\n",
                percentile(latency_ns, 0.50) / 1e3, percentile(latency_ns, 0.99) / 1e3,
                percentile(latency_ns, 0.999) / 1e3);
}
