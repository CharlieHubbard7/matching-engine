#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

#include "me/flat_map.hpp"
#include "me/types.hpp"

namespace me {

struct EngineConfig {
    uint32_t symbols = 1;
    uint32_t max_orders = 1u << 20;  // resting orders, across all books
    uint32_t max_levels = 1u << 18;  // price levels, across all books
};

// Price-time-priority limit order book and matching engine.
//
// Layout: orders and price levels live in pools allocated once in the
// constructor and are linked by 32-bit indices. Each level is a FIFO of
// orders; each side of a book is a sorted list of levels with the best at the
// head. Two fixed-capacity hash maps give O(1) order-id and price lookups.
// Nothing on the add/cancel/match path allocates.
//
// Every call reports through a handler H and ends with exactly one of
// on_accept / on_cancel / on_reject, preceded by zero or more on_trade:
//   void on_trade(const Fill&);
//   void on_accept(OrderId, Qty leaves);   // leaves = quantity now resting
//   void on_cancel(OrderId, Qty removed);
//   void on_reject(OrderId, Reject);
// Handlers must not call back into the engine.
class MatchingEngine {
public:
    struct Quote {
        Price price = 0;
        uint64_t qty = 0;
        bool valid = false;
    };

    explicit MatchingEngine(const EngineConfig& cfg)
        : orders_(cfg.max_orders),
          levels_(cfg.max_levels),
          books_(cfg.symbols),
          order_ids_(size_t{cfg.max_orders} * 2),
          level_keys_(size_t{cfg.max_levels} * 2) {
        for (uint32_t i = 0; i < cfg.max_orders; ++i) orders_[i].next = i + 1 < cfg.max_orders ? i + 1 : nil;
        for (uint32_t i = 0; i < cfg.max_levels; ++i) levels_[i].worse = i + 1 < cfg.max_levels ? i + 1 : nil;
        free_order_ = cfg.max_orders ? 0 : nil;
        free_level_ = cfg.max_levels ? 0 : nil;
    }

    template <class H>
    void add(H& h, OrderId id, SymbolId sym, Side side, Price price, Qty qty, Tif tif = Tif::Gtc,
             Owner owner = 0) {
        if (sym >= books_.size()) [[unlikely]] return h.on_reject(id, Reject::BadSymbol);
        if (qty == 0) [[unlikely]] return h.on_reject(id, Reject::BadQty);
        if (order_ids_.find(id) != nil) [[unlikely]] return h.on_reject(id, Reject::DuplicateId);
        if (tif == Tif::Gtc && (free_order_ == nil || free_level_ == nil)) [[unlikely]]
            return h.on_reject(id, Reject::BookFull);

        Book& book = books_[sym];
        qty = match(h, book, id, sym, side, price, qty);
        if (qty == 0 || tif == Tif::Ioc) return h.on_accept(id, 0);
        rest(book, id, sym, side, price, qty, owner);
        h.on_accept(id, qty);
    }

    template <class H>
    void cancel(H& h, OrderId id) {
        const uint32_t oi = order_ids_.find(id);
        if (oi == nil) return h.on_reject(id, Reject::UnknownOrder);
        const Qty qty = orders_[oi].qty;
        remove(oi);
        h.on_cancel(id, qty);
    }

    // Reduce an order's quantity in place, keeping its queue position.
    template <class H>
    void reduce(H& h, OrderId id, Qty by) {
        const uint32_t oi = order_ids_.find(id);
        if (oi == nil) return h.on_reject(id, Reject::UnknownOrder);
        Order& o = orders_[oi];
        if (by >= o.qty) {
            const Qty qty = o.qty;
            remove(oi);
            return h.on_cancel(id, qty);
        }
        o.qty -= by;
        levels_[o.level].total -= by;
        h.on_cancel(id, by);
    }

    // Cancel old_id and enter new_id for the same symbol, side and owner at a
    // new price/quantity. The new order goes to the back of the queue.
    template <class H>
    void replace(H& h, OrderId old_id, OrderId new_id, Price price, Qty qty) {
        const uint32_t oi = order_ids_.find(old_id);
        if (oi == nil) return h.on_reject(old_id, Reject::UnknownOrder);
        if (qty == 0) [[unlikely]] return h.on_reject(new_id, Reject::BadQty);
        if (new_id != old_id && order_ids_.find(new_id) != nil) [[unlikely]]
            return h.on_reject(new_id, Reject::DuplicateId);
        const Level& lvl = levels_[orders_[oi].level];
        const SymbolId sym = lvl.symbol;
        const Side side = lvl.side;
        const Owner owner = orders_[oi].owner;
        remove(oi);
        add(h, new_id, sym, side, price, qty, Tif::Gtc, owner);
    }

    // Cache hints for a request that will arrive shortly. On a large book the
    // two hash-map probes are the first cache misses each request takes;
    // issuing them a few messages early lets them overlap with other work.
    // Hints never change state and are safe with any arguments.
    void prefetch_add(OrderId id, SymbolId sym, Side side, Price price) const noexcept {
        order_ids_.prefetch(id);
        level_keys_.prefetch(level_key(sym, side, price));
    }
    void prefetch_order(OrderId id) const noexcept { order_ids_.prefetch(id); }

    // Owner of a resting order, or kNoOwner if the id is not in the book.
    Owner owner(OrderId id) const noexcept {
        const uint32_t oi = order_ids_.find(id);
        return oi == nil ? kNoOwner : orders_[oi].owner;
    }

    Quote best(SymbolId sym, Side side) const noexcept {
        const uint32_t li = books_[sym].best[int(side)];
        if (li == nil) return {};
        return {levels_[li].price, levels_[li].total, true};
    }

    size_t live_orders() const noexcept { return order_ids_.size(); }
    size_t live_levels() const noexcept { return level_keys_.size(); }

private:
    static constexpr uint32_t nil = UINT32_MAX;

    struct Order {
        OrderId id;
        Qty qty;
        uint32_t prev, next;  // neighbours in the level's FIFO; next doubles as the free list
        uint32_t level;
        Owner owner;
    };
    struct Level {
        uint64_t total;
        Price price;
        uint32_t head, tail;     // oldest / newest order
        uint32_t better, worse;  // neighbouring levels; worse doubles as the free list
        SymbolId symbol;
        Side side;
    };
    struct Book {
        uint32_t best[2] = {nil, nil};  // indexed by Side
    };

    static uint64_t level_key(SymbolId sym, Side side, Price price) noexcept {
        return (uint64_t{sym} << 33) | (uint64_t(side) << 32) | price;
    }

    template <class H>
    Qty match(H& h, Book& book, OrderId id, SymbolId sym, Side side, Price price, Qty qty) {
        const int opp = int(opposite(side));
        while (qty != 0) {
            const uint32_t li = book.best[opp];
            if (li == nil) break;
            Level& lvl = levels_[li];
            if (side == Side::Buy ? lvl.price > price : lvl.price < price) break;
            const uint32_t oi = lvl.head;
            Order& o = orders_[oi];
            const Qty fill = std::min(qty, o.qty);
            h.on_trade(Fill{o.id, id, o.owner, lvl.price, fill, sym});
            qty -= fill;
            if (fill == o.qty) {
                remove(oi);
            } else {
                o.qty -= fill;
                lvl.total -= fill;
            }
        }
        return qty;
    }

    void rest(Book& book, OrderId id, SymbolId sym, Side side, Price price, Qty qty, Owner owner) noexcept {
        const uint64_t key = level_key(sym, side, price);
        uint32_t li = level_keys_.find(key);
        if (li == nil) {
            li = free_level_;
            free_level_ = levels_[li].worse;
            // Walk from the best price to the insertion point. New levels
            // overwhelmingly appear near the top of the book.
            uint32_t above = nil, below = book.best[int(side)];
            while (below != nil &&
                   (side == Side::Buy ? levels_[below].price > price : levels_[below].price < price)) {
                above = below;
                below = levels_[below].worse;
            }
            levels_[li] = Level{0, price, nil, nil, above, below, sym, side};
            (above == nil ? book.best[int(side)] : levels_[above].worse) = li;
            if (below != nil) levels_[below].better = li;
            level_keys_.insert(key, li);
        }
        const uint32_t oi = free_order_;
        free_order_ = orders_[oi].next;
        Level& lvl = levels_[li];
        orders_[oi] = Order{id, qty, lvl.tail, nil, li, owner};
        (lvl.tail == nil ? lvl.head : orders_[lvl.tail].next) = oi;
        lvl.tail = oi;
        lvl.total += qty;
        order_ids_.insert(id, oi);
    }

    void remove(uint32_t oi) noexcept {
        Order& o = orders_[oi];
        const uint32_t li = o.level;
        Level& lvl = levels_[li];
        lvl.total -= o.qty;
        (o.prev == nil ? lvl.head : orders_[o.prev].next) = o.next;
        (o.next == nil ? lvl.tail : orders_[o.next].prev) = o.prev;
        if (lvl.head == nil) {
            Book& book = books_[lvl.symbol];
            (lvl.better == nil ? book.best[int(lvl.side)] : levels_[lvl.better].worse) = lvl.worse;
            if (lvl.worse != nil) levels_[lvl.worse].better = lvl.better;
            level_keys_.erase(level_key(lvl.symbol, lvl.side, lvl.price));
            lvl.worse = free_level_;
            free_level_ = li;
        }
        order_ids_.erase(o.id);
        o.next = free_order_;
        free_order_ = oi;
    }

    std::vector<Order> orders_;
    std::vector<Level> levels_;
    std::vector<Book> books_;
    FlatMap order_ids_;   // order id -> index into orders_
    FlatMap level_keys_;  // (symbol, side, price) -> index into levels_
    uint32_t free_order_ = nil;
    uint32_t free_level_ = nil;
};

}  // namespace me
