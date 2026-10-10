#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

#include "me/engine.hpp"

using namespace me;

#define CHECK(x)                                                                    \
    do {                                                                            \
        if (!(x)) {                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
            std::exit(1);                                                           \
        }                                                                           \
    } while (0)

namespace {

struct Ev {
    char type;  // T trade, A accept, X cancel, J reject
    uint64_t a, b;
    uint32_t price, qty;
    bool operator==(const Ev&) const = default;
};

struct Rec {
    std::vector<Ev> ev;
    void on_trade(const Fill& f) { ev.push_back({'T', f.resting, f.incoming, f.price, f.qty}); }
    void on_accept(OrderId id, Qty leaves) { ev.push_back({'A', id, 0, 0, leaves}); }
    void on_cancel(OrderId id, Qty q) { ev.push_back({'X', id, 0, 0, q}); }
    void on_reject(OrderId id, Reject r) { ev.push_back({'J', id, 0, 0, uint32_t(r)}); }
    std::vector<Ev> take() { return std::exchange(ev, {}); }
};

using Evs = std::vector<Ev>;
constexpr Side B = Side::Buy, S = Side::Sell;
Ev trade(uint64_t resting, uint64_t incoming, uint32_t p, uint32_t q) { return {'T', resting, incoming, p, q}; }
Ev accept(uint64_t id, uint32_t leaves) { return {'A', id, 0, 0, leaves}; }
Ev canceled(uint64_t id, uint32_t q) { return {'X', id, 0, 0, q}; }
Ev reject(uint64_t id, Reject r) { return {'J', id, 0, 0, uint32_t(r)}; }

void test_price_time_priority() {
    MatchingEngine e({.symbols = 1, .max_orders = 64, .max_levels = 64});
    Rec r;
    e.add(r, 1, 0, S, 101, 10);
    e.add(r, 2, 0, S, 100, 10);  // better price
    e.add(r, 3, 0, S, 100, 10);  // same price, later
    r.take();
    CHECK(e.best(0, S).price == 100 && e.best(0, S).qty == 20);

    // Sweeps 100 in arrival order, then 101; trades at the resting price.
    e.add(r, 4, 0, B, 105, 25);
    CHECK((r.take() == Evs{trade(2, 4, 100, 10), trade(3, 4, 100, 10), trade(1, 4, 101, 5), accept(4, 0)}));
    CHECK(e.best(0, S).price == 101 && e.best(0, S).qty == 5);
    CHECK(!e.best(0, B).valid);

    // The remainder of a partially filled order rests.
    e.add(r, 5, 0, B, 101, 8);
    CHECK((r.take() == Evs{trade(1, 5, 101, 5), accept(5, 3)}));
    CHECK(e.best(0, B).price == 101 && e.best(0, B).qty == 3);
    CHECK(!e.best(0, S).valid && e.live_orders() == 1);
}

void test_no_cross_rests() {
    MatchingEngine e({.symbols = 2, .max_orders = 64, .max_levels = 64});
    Rec r;
    e.add(r, 1, 0, B, 99, 10);
    e.add(r, 2, 0, S, 100, 10);
    e.add(r, 3, 1, S, 99, 10);  // other symbol: must not match order 1
    CHECK((r.take() == Evs{accept(1, 10), accept(2, 10), accept(3, 10)}));
    CHECK(e.best(0, B).price == 99 && e.best(0, S).price == 100 && e.best(1, S).price == 99);
}

void test_cancel_reduce_replace() {
    MatchingEngine e({.symbols = 1, .max_orders = 64, .max_levels = 64});
    Rec r;
    e.add(r, 1, 0, B, 100, 10);
    e.add(r, 2, 0, B, 100, 10);
    e.add(r, 3, 0, B, 100, 10);
    r.take();

    e.reduce(r, 1, 4);  // keeps priority
    e.replace(r, 2, 20, 100, 10);  // loses priority
    CHECK((r.take() == Evs{canceled(1, 4), accept(20, 10)}));
    e.add(r, 4, 0, S, 100, 20);
    CHECK((r.take() == Evs{trade(1, 4, 100, 6), trade(3, 4, 100, 10), trade(20, 4, 100, 4), accept(4, 0)}));

    e.cancel(r, 20);
    e.cancel(r, 20);
    e.reduce(r, 999, 1);
    e.replace(r, 999, 1000, 100, 1);
    CHECK((r.take() == Evs{canceled(20, 6), reject(20, Reject::UnknownOrder), reject(999, Reject::UnknownOrder),
                           reject(999, Reject::UnknownOrder)}));
    CHECK(e.live_orders() == 0 && e.live_levels() == 0 && !e.best(0, B).valid);
}

void test_ioc_and_rejects() {
    MatchingEngine e({.symbols = 1, .max_orders = 2, .max_levels = 8});
    Rec r;
    e.add(r, 1, 0, S, 100, 5);
    e.add(r, 2, 0, B, 100, 8, Tif::Ioc);  // fills 5, remainder dropped
    e.add(r, 3, 0, B, 100, 8, Tif::Ioc);  // nothing to hit
    CHECK((r.take() == Evs{accept(1, 5), trade(1, 2, 100, 5), accept(2, 0), accept(3, 0)}));
    CHECK(e.live_orders() == 0);

    e.add(r, 1, 0, B, 90, 1);
    e.add(r, 1, 0, B, 90, 1);
    e.add(r, 4, 5, B, 90, 1);
    e.add(r, 5, 0, B, 90, 0);
    e.add(r, 6, 0, B, 90, 1);
    e.add(r, 7, 0, B, 90, 1);  // pool of 2 is exhausted
    CHECK((r.take() == Evs{accept(1, 1), reject(1, Reject::DuplicateId), reject(4, Reject::BadSymbol),
                           reject(5, Reject::BadQty), accept(6, 1), reject(7, Reject::BookFull)}));
    e.cancel(r, 1);
    e.add(r, 7, 0, B, 90, 1);  // freed slot is reusable
    CHECK((r.take() == Evs{canceled(1, 1), accept(7, 1)}));
}

void test_flat_map() {
    FlatMap m(1024);
    std::unordered_map<uint64_t, uint32_t> ref;
    std::mt19937_64 rng(7);
    for (int i = 0; i < 200'000; ++i) {
        const uint64_t key = rng() % 700 * 0x1000;  // clustered keys, up to ~68% load
        if (rng() & 1) {
            if (!ref.count(key)) {
                m.insert(key, uint32_t(i));
                ref[key] = uint32_t(i);
            }
        } else {
            m.erase(key);
            ref.erase(key);
        }
        const uint64_t probe = rng() % 700 * 0x1000;
        const auto it = ref.find(probe);
        CHECK(m.find(probe) == (it == ref.end() ? FlatMap::npos : it->second));
        CHECK(m.size() == ref.size());
    }
}

// Straightforward reference book built on std::map, used to cross-check the
// engine on random order flow.
struct Reference {
    struct O {
        uint64_t id;
        uint32_t qty;
    };
    struct Where {
        Side side;
        uint32_t price;
    };
    std::map<uint32_t, std::deque<O>> book[2];
    std::unordered_map<uint64_t, Where> live;
    Rec r;

    void add(uint64_t id, Side side, uint32_t price, uint32_t qty, Tif tif) {
        if (qty == 0) return r.on_reject(id, Reject::BadQty);
        if (live.count(id)) return r.on_reject(id, Reject::DuplicateId);
        auto& opp = book[int(opposite(side))];
        while (qty && !opp.empty()) {
            const auto lvl = side == B ? opp.begin() : std::prev(opp.end());
            if (side == B ? lvl->first > price : lvl->first < price) break;
            O& o = lvl->second.front();
            const uint32_t fill = std::min(qty, o.qty);
            r.on_trade(Fill{o.id, id, 0, lvl->first, fill, 0});
            qty -= fill;
            o.qty -= fill;
            if (o.qty == 0) {
                live.erase(o.id);
                lvl->second.pop_front();
                if (lvl->second.empty()) opp.erase(lvl);
            }
        }
        if (qty == 0 || tif == Tif::Ioc) return r.on_accept(id, 0);
        book[int(side)][price].push_back({id, qty});
        live[id] = {side, price};
        r.on_accept(id, qty);
    }

    // Removes up to `by` shares; returns the amount removed.
    uint32_t take(uint64_t id, uint32_t by) {
        const Where w = live.at(id);
        auto& q = book[int(w.side)][w.price];
        for (auto it = q.begin();; ++it) {
            if (it->id != id) continue;
            const uint32_t removed = std::min(by, it->qty);
            it->qty -= removed;
            if (it->qty == 0) {
                q.erase(it);
                if (q.empty()) book[int(w.side)].erase(w.price);
                live.erase(id);
            }
            return removed;
        }
    }

    void reduce(uint64_t id, uint32_t by) {
        if (!live.count(id)) return r.on_reject(id, Reject::UnknownOrder);
        r.on_cancel(id, take(id, by));
    }
    void cancel(uint64_t id) { reduce(id, UINT32_MAX); }
    void replace(uint64_t old_id, uint64_t new_id, uint32_t price, uint32_t qty) {
        if (!live.count(old_id)) return r.on_reject(old_id, Reject::UnknownOrder);
        if (qty == 0) return r.on_reject(new_id, Reject::BadQty);
        if (new_id != old_id && live.count(new_id)) return r.on_reject(new_id, Reject::DuplicateId);
        const Side side = live.at(old_id).side;
        take(old_id, UINT32_MAX);
        add(new_id, side, price, qty, Tif::Gtc);
    }
};

void test_random_against_reference() {
    MatchingEngine e({.symbols = 1, .max_orders = 1 << 16, .max_levels = 1 << 10});
    Reference ref;
    Rec r;
    std::mt19937_64 rng(1234);
    uint64_t next_id = 1;
    uint64_t trades = 0;

    for (int i = 0; i < 300'000; ++i) {
        const auto some_id = [&] { return 1 + rng() % next_id; };
        const uint32_t price = 90 + uint32_t(rng() % 21);
        const uint32_t qty = uint32_t(rng() % 50);  // occasionally 0
        const Side side = (rng() & 1) ? B : S;
        switch (rng() % 10) {
            case 0: case 1: case 2: case 3: case 4: {
                const uint64_t id = rng() % 50 == 0 ? some_id() : next_id++;
                const Tif tif = rng() % 8 == 0 ? Tif::Ioc : Tif::Gtc;
                e.add(r, id, 0, side, price, qty, tif);
                ref.add(id, side, price, qty, tif);
                break;
            }
            case 5: case 6: case 7: {
                const uint64_t id = some_id();
                e.cancel(r, id);
                ref.cancel(id);
                break;
            }
            case 8: {
                const uint64_t id = some_id();
                e.reduce(r, id, qty);
                ref.reduce(id, qty);
                break;
            }
            default: {
                const uint64_t old_id = some_id();
                const uint64_t new_id = rng() % 20 == 0 ? old_id : next_id++;
                e.replace(r, old_id, new_id, price, qty);
                ref.replace(old_id, new_id, price, qty);
            }
        }
        for (const Ev& ev : r.ev) trades += ev.type == 'T';
        CHECK(r.take() == ref.r.take());
        CHECK(e.live_orders() == ref.live.size());
        CHECK(e.live_levels() == ref.book[0].size() + ref.book[1].size());
        for (const Side s : {B, S}) {
            const auto& side_book = ref.book[int(s)];
            const auto q = e.best(0, s);
            CHECK(q.valid == !side_book.empty());
            if (q.valid) CHECK(q.price == (s == B ? std::prev(side_book.end())->first : side_book.begin()->first));
        }
    }
    CHECK(trades > 10'000);  // the flow really did exercise matching
}

}  // namespace

int main() {
    test_price_time_priority();
    test_no_cross_rests();
    test_cancel_reduce_replace();
    test_ioc_and_rejects();
    test_flat_map();
    test_random_against_reference();
    std::puts("all tests passed");
}
