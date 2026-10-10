#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "me/protocol.hpp"

namespace me {

inline uint64_t now_ns() noexcept {
    using namespace std::chrono;
    return uint64_t(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

// Smallest observable step of now_ns(). On Apple silicon this is ~42 ns.
inline uint64_t clock_resolution_ns() noexcept {
    uint64_t best = UINT64_MAX;
    for (int i = 0; i < 1000; ++i) {
        const uint64_t t0 = now_ns();
        uint64_t t1;
        do t1 = now_ns();
        while (t1 == t0);
        best = std::min(best, t1 - t0);
    }
    return best;
}

// q in [0, 1]. Reorders v.
template <class T>
T percentile(std::vector<T>& v, double q) {
    if (v.empty()) return T{};
    const size_t k = std::min(v.size() - 1, size_t(q * double(v.size())));
    std::nth_element(v.begin(), v.begin() + ptrdiff_t(k), v.end());
    return v[k];
}

inline std::vector<uint8_t> read_file(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::perror(path);
        std::exit(1);
    }
    std::fseek(f, 0, SEEK_END);
    std::vector<uint8_t> buf(size_t(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
        std::perror(path);
        std::exit(1);
    }
    std::fclose(f);
    return buf;
}

// Number of inbound wire messages in buf; exits if the stream is malformed.
inline size_t count_inbound(const std::vector<uint8_t>& buf) {
    size_t n = 0, off = 0;
    while (off < buf.size()) {
        wire::Header h{};
        if (buf.size() - off >= sizeof h) std::memcpy(&h, buf.data() + off, sizeof h);
        if (h.len == 0 || h.len != wire::inbound_size(h.type) || buf.size() - off < h.len) {
            std::fprintf(stderr, "malformed message stream at byte %zu\n", off);
            std::exit(1);
        }
        off += h.len;
        ++n;
    }
    return n;
}

}  // namespace me
