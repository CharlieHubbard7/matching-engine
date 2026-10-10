#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "me/engine.hpp"

// Binary wire protocol: fixed-size little-endian messages, each starting with
// a 4-byte header {u16 total length, u8 type, u8 reserved}.
//
// Every inbound message is answered by zero or more Trade messages followed by
// exactly one of Accepted / Canceled / Rejected. A client also receives a
// Trade, unprompted, whenever another client's order fills one of its own.
namespace me::wire {

static_assert(std::endian::native == std::endian::little, "wire format is little-endian");

enum : uint8_t {
    // client -> gateway
    kNew = 'N',
    kCancel = 'C',
    kReduce = 'M',
    kReplace = 'R',
    // gateway -> client
    kAccepted = 'A',
    kTrade = 'T',
    kCanceled = 'X',
    kRejected = 'J',
};

// Recorded-feed convention (tools/itch_convert.cpp): an IOC order standing in
// for an exchange execution has this bit set in its id, and the remaining
// bits name the resting order the exchange filled.
constexpr uint64_t kExecutionFlag = 1ull << 63;

#pragma pack(push, 1)
struct Header {
    uint16_t len;
    uint8_t type;
    uint8_t reserved;
};
struct NewOrder {
    Header h;
    uint64_t id;
    uint32_t price;
    uint32_t qty;
    uint16_t symbol;
    uint8_t side;  // 0 = buy, 1 = sell
    uint8_t tif;   // 0 = GTC, 1 = IOC
};
struct Cancel {
    Header h;
    uint64_t id;
};
struct Reduce {
    Header h;
    uint64_t id;
    uint32_t qty;
};
struct Replace {
    Header h;
    uint64_t old_id;
    uint64_t new_id;
    uint32_t price;
    uint32_t qty;
};
struct Accepted {
    Header h;
    uint64_t id;
    uint32_t leaves;
};
struct Trade {
    Header h;
    uint64_t resting_id;
    uint64_t incoming_id;
    uint32_t price;
    uint32_t qty;
    uint16_t symbol;
    uint16_t reserved;
};
struct Canceled {
    Header h;
    uint64_t id;
    uint32_t qty;
};
struct Rejected {
    Header h;
    uint64_t id;
    uint8_t reason;
    uint8_t reserved[3];
};
#pragma pack(pop)

template <class M>
constexpr Header header(uint8_t type) noexcept {
    return Header{uint16_t(sizeof(M)), type, 0};
}

// Size of a message of the given type, or 0 if the type is unknown.
constexpr size_t inbound_size(uint8_t type) noexcept {
    switch (type) {
        case kNew: return sizeof(NewOrder);
        case kCancel: return sizeof(Cancel);
        case kReduce: return sizeof(Reduce);
        case kReplace: return sizeof(Replace);
        default: return 0;
    }
}
constexpr size_t outbound_size(uint8_t type) noexcept {
    switch (type) {
        case kAccepted: return sizeof(Accepted);
        case kTrade: return sizeof(Trade);
        case kCanceled: return sizeof(Canceled);
        case kRejected: return sizeof(Rejected);
        default: return 0;
    }
}

// Apply one complete, length-checked inbound message to the engine.
template <class H>
inline void dispatch(MatchingEngine& engine, H& h, const uint8_t* p, Owner owner = 0) {
    switch (p[offsetof(Header, type)]) {
        case kNew: {
            NewOrder m;
            std::memcpy(&m, p, sizeof m);
            engine.add(h, m.id, m.symbol, Side(m.side & 1), m.price, m.qty, Tif(m.tif & 1), owner);
            break;
        }
        case kCancel: {
            Cancel m;
            std::memcpy(&m, p, sizeof m);
            engine.cancel(h, m.id);
            break;
        }
        case kReduce: {
            Reduce m;
            std::memcpy(&m, p, sizeof m);
            engine.reduce(h, m.id, m.qty);
            break;
        }
        case kReplace: {
            Replace m;
            std::memcpy(&m, p, sizeof m);
            engine.replace(h, m.old_id, m.new_id, m.price, m.qty);
            break;
        }
    }
}

// Issue the engine's cache hints for one complete inbound message.
inline void prefetch(const MatchingEngine& engine, const uint8_t* p) noexcept {
    OrderId id;  // every inbound message carries its order id first
    std::memcpy(&id, p + sizeof(Header), sizeof id);
    if (p[offsetof(Header, type)] == kNew) {
        NewOrder m;
        std::memcpy(&m, p, sizeof m);
        engine.prefetch_add(id, m.symbol, Side(m.side & 1), m.price);
    } else {
        engine.prefetch_order(id);
    }
}

// Runs kDistance messages ahead of the consumer through a buffer of queued
// inbound messages, prefetching for each. Construct it over the unprocessed
// bytes, then call advance() once per message processed.
class Lookahead {
public:
    static constexpr int kDistance = 12;

    Lookahead(const MatchingEngine& engine, const uint8_t* begin, const uint8_t* end) noexcept
        : engine_(engine), p_(begin), end_(end) {
        for (int i = 0; i < kDistance; ++i) advance();
    }

    void advance() noexcept {
        if (size_t(end_ - p_) < sizeof(Header)) return;
        Header h;
        std::memcpy(&h, p_, sizeof h);
        if (h.len != inbound_size(h.type) || size_t(end_ - p_) < h.len) {
            p_ = end_;  // partial or malformed: the consumer deals with it
            return;
        }
        prefetch(engine_, p_);
        p_ += h.len;
    }

private:
    const MatchingEngine& engine_;
    const uint8_t* p_;
    const uint8_t* end_;
};

}  // namespace me::wire
