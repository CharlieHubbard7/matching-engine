#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace me {

// Fixed-capacity uint64 -> uint32 hash map: open addressing, linear probing,
// backward-shift deletion (no tombstones). All memory is allocated in the
// constructor; find/insert/erase never allocate. The caller keeps the load
// factor bounded (the engine sizes it at 2x the pool it indexes).
class FlatMap {
public:
    static constexpr uint32_t npos = UINT32_MAX;

    explicit FlatMap(size_t min_slots) {
        const size_t n = std::bit_ceil(min_slots < 16 ? size_t{16} : min_slots);
        slots_.resize(n);
        mask_ = n - 1;
        shift_ = 64 - std::countr_zero(n);
    }

    uint32_t find(uint64_t key) const noexcept {
        for (size_t i = home(key);; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (s.value == npos) return npos;
            if (s.key == key) return s.value;
        }
    }

    // Precondition: key is absent and the map is not full.
    void insert(uint64_t key, uint32_t value) noexcept {
        size_t i = home(key);
        while (slots_[i].value != npos) i = (i + 1) & mask_;
        slots_[i] = Slot{key, value};
        ++size_;
    }

    void erase(uint64_t key) noexcept {
        size_t i = home(key);
        while (slots_[i].value != npos && slots_[i].key != key) i = (i + 1) & mask_;
        if (slots_[i].value == npos) return;
        // Shift later entries of the probe chain back into the hole.
        for (size_t j = i;;) {
            j = (j + 1) & mask_;
            if (slots_[j].value == npos) break;
            const size_t h = home(slots_[j].key);
            if (((j - h) & mask_) >= ((j - i) & mask_)) {
                slots_[i] = slots_[j];
                i = j;
            }
        }
        slots_[i].value = npos;
        --size_;
    }

    // Start loading the slot a later find/insert/erase of key will probe first.
    void prefetch(uint64_t key) const noexcept { __builtin_prefetch(&slots_[home(key)]); }

    size_t size() const noexcept { return size_; }

private:
    struct Slot {
        uint64_t key = 0;
        uint32_t value = npos;
    };

    size_t home(uint64_t key) const noexcept { return (key * 0x9E3779B97F4A7C15ull) >> shift_; }

    std::vector<Slot> slots_;
    size_t mask_ = 0;
    size_t size_ = 0;
    int shift_ = 0;
};

}  // namespace me
