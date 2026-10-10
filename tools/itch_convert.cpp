// Converts a Nasdaq TotalView-ITCH 5.0 feed into this engine's inbound wire
// messages, so recorded exchange activity can be replayed as order flow.
//
// ITCH publishes the *result* of matching, so the flow is reconstructed:
//   A/F  add order           -> NewOrder (GTC), id = ITCH order reference
//   X    partial cancel      -> Reduce
//   D    delete              -> Cancel
//   U    replace             -> Replace
//   E/C  order executed      -> NewOrder (IOC) on the opposite side at the
//                               resting order's price, for the executed shares
//                               (its id is the executed order's reference with
//                               the top bit set, so a replay can check that it
//                               filled the same order the exchange did)
// The stock locate code is the symbol id; prices stay in ITCH's 1/10000
// dollar ticks.
//
//   itch_convert <feed.itch | -> <out.bin> [max_messages] [HH:MM:SS seed.bin]
//
// With a start time, everything before it is folded into a snapshot of the
// resting orders at that moment, written to seed.bin as NewOrders in time
// priority. Load the seed first, then replay out.bin.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "me/protocol.hpp"

using namespace me;

namespace {

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t(be16(p)) << 16 | be16(p + 2); }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

struct Resting {
    uint32_t price;
    uint32_t qty;
    uint16_t symbol;
    uint8_t side;
};

template <class M>
void put(std::FILE* f, const M& m) {
    if (std::fwrite(&m, sizeof m, 1, f) != 1) {
        std::perror("write");
        std::exit(1);
    }
}

wire::NewOrder new_order(uint64_t id, const Resting& r, uint32_t qty, uint8_t side, uint8_t tif) {
    return {wire::header<wire::NewOrder>(wire::kNew), id, r.price, qty, r.symbol, side, tif};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4 && argc != 6) {
        std::fprintf(stderr, "usage: %s <feed.itch | -> <out.bin> [max_messages] [HH:MM:SS seed.bin]\n", argv[0]);
        return 2;
    }
    std::FILE* in = std::strcmp(argv[1], "-") == 0 ? stdin : std::fopen(argv[1], "rb");
    std::FILE* out = std::fopen(argv[2], "wb");
    const uint64_t max_msgs = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : UINT64_MAX;
    if (!in || !out) {
        std::perror("open");
        return 1;
    }
    uint64_t start_ns = 0;
    if (argc == 6) {
        unsigned h = 0, m = 0, s = 0;
        if (std::sscanf(argv[4], "%u:%u:%u", &h, &m, &s) != 3) {
            std::fprintf(stderr, "bad start time: %s\n", argv[4]);
            return 2;
        }
        start_ns = (h * 3600ull + m * 60ull + s) * 1'000'000'000ull;
    }

    std::unordered_map<uint64_t, Resting> book;
    uint64_t adds = 0, executions = 0, reduces = 0, cancels = 0, replaces = 0, skipped = 0;
    uint64_t emitted = 0, seeded = 0;
    bool started = start_ns == 0;
    uint8_t lenbuf[2], m[65536];

    // Each ITCH message is prefixed with a 2-byte big-endian length.
    while (emitted < max_msgs && std::fread(lenbuf, 1, 2, in) == 2) {
        const size_t len = be16(lenbuf);
        if (std::fread(m, 1, len, in) != len) break;  // truncated tail
        if (len < 11) continue;
        // Common prefix: type(1) locate(2) tracking(2) timestamp(6), body at 11.
        const uint16_t locate = be16(m + 1);
        const uint64_t ts = uint64_t(be16(m + 5)) << 32 | be32(m + 7);  // ns since midnight

        if (!started && ts >= start_ns) {
            started = true;
            // ITCH order references increase over time, so sorting by
            // reference restores each level's queue order.
            std::vector<uint64_t> refs;
            refs.reserve(book.size());
            for (const auto& [ref, r] : book) refs.push_back(ref);
            std::sort(refs.begin(), refs.end());
            std::FILE* seed = std::fopen(argv[5], "wb");
            if (!seed) {
                std::perror(argv[5]);
                return 1;
            }
            for (const uint64_t ref : refs) {
                const Resting& r = book[ref];
                put(seed, new_order(ref, r, r.qty, r.side, 0));
            }
            std::fclose(seed);
            seeded = refs.size();
        }

        switch (m[0]) {
            case 'A':
            case 'F': {
                const uint64_t ref = be64(m + 11);
                const Resting r{be32(m + 32), be32(m + 20), locate, uint8_t(m[19] == 'B' ? 0 : 1)};
                book[ref] = r;
                if (!started) continue;
                put(out, new_order(ref, r, r.qty, r.side, 0));
                ++adds;
                break;
            }
            case 'E':
            case 'C': {
                const uint64_t ref = be64(m + 11);
                const uint32_t qty = be32(m + 19);
                const auto it = book.find(ref);
                if (it == book.end()) {
                    skipped += started;
                    continue;
                }
                Resting& r = it->second;
                if (started) put(out, new_order(wire::kExecutionFlag | ref, r, qty, uint8_t(r.side ^ 1), 1));
                if (qty >= r.qty) book.erase(it);
                else r.qty -= qty;
                if (!started) continue;
                ++executions;
                break;
            }
            case 'X': {
                const uint64_t ref = be64(m + 11);
                const uint32_t qty = be32(m + 19);
                const auto it = book.find(ref);
                if (it == book.end()) {
                    skipped += started;
                    continue;
                }
                if (qty >= it->second.qty) book.erase(it);
                else it->second.qty -= qty;
                if (!started) continue;
                put(out, wire::Reduce{wire::header<wire::Reduce>(wire::kReduce), ref, qty});
                ++reduces;
                break;
            }
            case 'D': {
                const uint64_t ref = be64(m + 11);
                if (book.erase(ref) == 0) {
                    skipped += started;
                    continue;
                }
                if (!started) continue;
                put(out, wire::Cancel{wire::header<wire::Cancel>(wire::kCancel), ref});
                ++cancels;
                break;
            }
            case 'U': {
                const uint64_t old_ref = be64(m + 11), new_ref = be64(m + 19);
                const uint32_t qty = be32(m + 27), price = be32(m + 31);
                const auto it = book.find(old_ref);
                if (it == book.end()) {
                    skipped += started;
                    continue;
                }
                const Resting r = it->second;
                book.erase(it);
                book[new_ref] = {price, qty, r.symbol, r.side};
                if (!started) continue;
                put(out, wire::Replace{wire::header<wire::Replace>(wire::kReplace), old_ref, new_ref, price, qty});
                ++replaces;
                break;
            }
            default:
                continue;  // system, directory, trade and imbalance messages
        }
        ++emitted;
    }
    std::fclose(out);

    if (start_ns) {
        if (!started) {
            std::fprintf(stderr, "feed ended before %s\n", argv[4]);
            return 1;
        }
        std::printf("wrote %llu resting orders as of %s to %s\n", static_cast<unsigned long long>(seeded), argv[4],
                    argv[5]);
    }
    std::printf("wrote %llu messages to %s\n", static_cast<unsigned long long>(emitted), argv[2]);
    std::printf("  %llu adds, %llu executions (as IOC orders), %llu reduces, %llu cancels, %llu replaces\n",
                static_cast<unsigned long long>(adds), static_cast<unsigned long long>(executions),
                static_cast<unsigned long long>(reduces), static_cast<unsigned long long>(cancels),
                static_cast<unsigned long long>(replaces));
    if (skipped)
        std::printf("  %llu messages skipped (referenced an unknown order)\n",
                    static_cast<unsigned long long>(skipped));
}
