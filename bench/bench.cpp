// In-process benchmark: runs a stream of wire messages straight through the
// engine (no sockets) and reports throughput, per-message latency, and the
// number of heap allocations made while matching.
//
// Messages are consumed the way the gateway consumes a full receive buffer:
// in order, with cache hints issued a few messages ahead (wire::Lookahead).
// Set ME_NO_PREFETCH=1 to measure without the hints.
//
//   bench                                    synthetic order flow
//   bench data/itch.bin [data/itch.seed.bin] recorded flow from itch_convert;
//                                            the seed (resting orders at the start
//                                            of the recording) is loaded untimed
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <unordered_map>
#include <vector>

#include "me/util.hpp"

using namespace me;

// Count every heap allocation in the process so the timed sections can prove
// they made none.
static uint64_t g_allocs = 0;
void* operator new(size_t n) {
    ++g_allocs;
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace {

struct Counter {
    uint64_t trades = 0, accepts = 0, cancels = 0, rejects = 0;
    // For recorded feeds: fills of exchange executions, and how many of them
    // hit the order the exchange itself filled.
    uint64_t exec_fills = 0, exec_same_order = 0;
    void on_trade(const Fill& f) {
        ++trades;
        if (f.incoming & wire::kExecutionFlag) {
            ++exec_fills;
            exec_same_order += f.resting == (f.incoming & ~wire::kExecutionFlag);
        }
    }
    void on_accept(OrderId, Qty) { ++accepts; }
    void on_cancel(OrderId, Qty) { ++cancels; }
    void on_reject(OrderId, Reject) { ++rejects; }
};

template <class M>
void append(std::vector<uint8_t>& out, const M& m) {
    const auto* p = reinterpret_cast<const uint8_t*>(&m);
    out.insert(out.end(), p, p + sizeof m);
}

// Synthetic flow over a few symbols: passive limit orders around a drifting
// mid, some marketable orders, and cancels of orders that are still resting
// (tracked with a shadow engine so cancels are not wasted on filled orders).
// The first kWarmup messages only add, so the books have depth to work against.
std::vector<uint8_t> synthetic(size_t n, const EngineConfig& cfg) {
    struct Tracker {
        std::unordered_map<OrderId, Qty> live;
        void on_trade(const Fill& f) {
            const auto it = live.find(f.resting);
            if (it != live.end() && (it->second -= f.qty) == 0) live.erase(it);
        }
        void on_accept(OrderId id, Qty leaves) {
            if (leaves) live[id] = leaves;
        }
        void on_cancel(OrderId id, Qty) { live.erase(id); }
        void on_reject(OrderId, Reject) {}
    } tracker;

    MatchingEngine shadow(cfg);
    std::mt19937_64 rng(42);
    std::vector<uint8_t> out;
    out.reserve(n * sizeof(wire::NewOrder));
    std::vector<OrderId> candidates;
    constexpr int kSymbols = 4;
    constexpr size_t kWarmup = 200'000;
    Price mid[kSymbols] = {10'000, 20'000, 30'000, 40'000};
    OrderId next_id = 1;

    for (size_t i = 0; i < n; ++i) {
        const size_t before = out.size();
        const unsigned r = unsigned(rng() % 100);
        const SymbolId sym = SymbolId(rng() % kSymbols);
        if (rng() % 50 == 0) mid[sym] += (rng() & 1) ? 1 : -1;

        if (i >= kWarmup && r >= 60 && r < 95) {  // cancel a resting order
            while (!candidates.empty()) {
                const size_t k = rng() % candidates.size();
                const OrderId id = candidates[k];
                candidates[k] = candidates.back();
                candidates.pop_back();
                if (tracker.live.count(id)) {
                    append(out, wire::Cancel{wire::header<wire::Cancel>(wire::kCancel), id});
                    break;
                }
            }
        }
        if (out.size() == before) {  // new order (also the fallback when nothing is cancelable)
            const bool buy = rng() & 1;
            const bool ioc = r >= 95;
            const bool aggressive = ioc || rng() % 100 < 15;
            const Price off = aggressive ? Price(rng() % 3) : Price(1 + rng() % 10);
            const Price px = (buy != aggressive) ? mid[sym] - off : mid[sym] + off;
            const OrderId id = next_id++;
            append(out, wire::NewOrder{wire::header<wire::NewOrder>(wire::kNew), id, px, Qty(1 + rng() % 500),
                                       sym, uint8_t(buy ? 0 : 1), uint8_t(ioc ? 1 : 0)});
            if (!ioc) candidates.push_back(id);
        }
        wire::dispatch(shadow, tracker, out.data() + before);
    }
    return out;
}

// A buffer of queued inbound messages, consumed one at a time.
class Stream {
public:
    Stream(MatchingEngine& engine, const std::vector<uint8_t>& buf, bool prefetch)
        : engine_(engine),
          p_(buf.data()),
          end_(buf.data() + buf.size()),
          ahead_(engine, prefetch ? p_ : end_, end_) {}

    bool done() const noexcept { return p_ == end_; }

    template <class H>
    void step(H& h) {
        uint16_t len;
        std::memcpy(&len, p_, sizeof len);
        ahead_.advance();
        wire::dispatch(engine_, h, p_);
        p_ += len;
    }

private:
    MatchingEngine& engine_;
    const uint8_t* p_;
    const uint8_t* end_;
    wire::Lookahead ahead_;
};

unsigned long long ull(uint64_t v) { return v; }

}  // namespace

int main(int argc, char** argv) {
    // Capacity for bench runs; a recorded stream never rests more orders than
    // its seed and messages combined.
    const EngineConfig cfg{.symbols = 65536, .max_orders = 1u << 22, .max_levels = 1u << 21};
    const bool prefetch = std::getenv("ME_NO_PREFETCH") == nullptr;

    std::vector<uint8_t> msgs;
    if (argc > 1) {
        msgs = read_file(argv[1]);
        std::printf("input       %s\n", argv[1]);
    } else {
        msgs = synthetic(5'000'000, cfg);
        std::printf("input       synthetic (seed 42)\n");
    }
    const std::vector<uint8_t> seed = argc > 2 ? read_file(argv[2]) : std::vector<uint8_t>{};
    const size_t total = count_inbound(msgs);
    const size_t seeded = count_inbound(seed);
    // Put the seed orders in the book before anything is timed.
    const auto load = [&](MatchingEngine& engine) {
        Counter ignored;
        for (Stream s(engine, seed, true); !s.done();) s.step(ignored);
    };
    if (seeded) std::printf("seed        %zu resting orders loaded before timing\n", seeded);
    std::printf("messages    %zu%s\n", total, prefetch ? "" : "   (prefetch disabled)");

    // Throughput: time the whole stream with no per-message instrumentation.
    double best_secs = 1e300;
    for (int run = 0; run < 3; ++run) {
        MatchingEngine engine(cfg);
        load(engine);
        Counter counter;
        const uint64_t allocs = g_allocs;
        const uint64_t t0 = now_ns();
        for (Stream s(engine, msgs, prefetch); !s.done();) s.step(counter);
        const double secs = double(now_ns() - t0) / 1e9;
        const uint64_t hot_allocs = g_allocs - allocs;
        best_secs = std::min(best_secs, secs);
        if (run == 0) {
            std::printf("events      %llu trades, %llu accepts, %llu cancels, %llu rejects\n", ull(counter.trades),
                        ull(counter.accepts), ull(counter.cancels), ull(counter.rejects));
            if (counter.exec_fills)
                std::printf("fidelity    %.1f%% of %llu exchange-execution fills hit the order the exchange filled\n",
                            100.0 * double(counter.exec_same_order) / double(counter.exec_fills),
                            ull(counter.exec_fills));
            std::printf("end state   %zu resting orders on %zu price levels\n", engine.live_orders(),
                        engine.live_levels());
        }
        std::printf("run %d       %.2f M msgs/sec  (%.1f ns/msg mean)  heap allocations: %llu\n", run + 1,
                    double(total) / secs / 1e6, secs * 1e9 / double(total), ull(hot_allocs));
        if (hot_allocs != 0) {
            std::fprintf(stderr, "FAIL: the matching path allocated\n");
            return 1;
        }
    }
    std::printf("throughput  %.2f M msgs/sec (best of 3)\n", double(total) / best_secs / 1e6);

    // Latency: time each message, or each small batch when the clock is too
    // coarse to resolve a single message.
    const uint64_t res = clock_resolution_ns();
    const size_t batch = res > 5 ? 32 : 1;
    std::vector<uint32_t> samples;
    samples.reserve(total / batch + 1);
    {
        MatchingEngine engine(cfg);
        load(engine);
        Counter counter;
        for (Stream s(engine, msgs, prefetch); !s.done();) {
            const uint64_t t0 = now_ns();
            size_t n = 0;
            for (; n < batch && !s.done(); ++n) s.step(counter);
            samples.push_back(uint32_t((now_ns() - t0) / n));
        }
    }
    std::printf("latency     p50 %u ns   p99 %u ns   p99.9 %u ns", percentile(samples, 0.50),
                percentile(samples, 0.99), percentile(samples, 0.999));
    if (batch > 1)
        std::printf("   (per message, averaged over %zu-message batches; clock step is %llu ns)\n", batch, ull(res));
    else
        std::printf("   (per message, includes ~%llu ns of timer overhead)\n", ull(res));
}
