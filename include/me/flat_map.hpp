// Open-addressing hash map from order id to a slot number, with linear probing.
// Entries are never erased: an order that leaves the book keeps its id with the value
// kGone, so a reused id is still caught as a duplicate. That makes probing simple (no
// tombstones) and every lookup a short scan of adjacent cache lines.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "me/types.hpp"

namespace me {

class IdMap {
public:
    static constexpr std::int32_t kAbsent = -1;  // never seen
    static constexpr std::int32_t kGone = -2;    // seen, no longer resting

    explicit IdMap(std::size_t expected = 1024) { rebuild(capacity_for(expected)); }

    // The value for id, or kAbsent.
    std::int32_t find(OrderId id) const {
        std::size_t i = slot(id);
        while (true) {
            const Entry& e = table_[i];
            if (e.value == kEmpty) return kAbsent;
            if (e.key == id) return e.value;
            i = (i + 1) & mask_;
        }
    }

    // Sets id's value, inserting it if it is new.
    void set(OrderId id, std::int32_t value) {
        if ((size_ + 1) * 2 > table_.size()) rebuild(table_.size() * 2);
        std::size_t i = slot(id);
        while (true) {
            Entry& e = table_[i];
            if (e.value == kEmpty) {
                e = {id, value};
                ++size_;
                return;
            }
            if (e.key == id) {
                e.value = value;
                return;
            }
            i = (i + 1) & mask_;
        }
    }

    std::size_t size() const { return size_; }

private:
    static constexpr std::int32_t kEmpty = -3;
    struct Entry {
        OrderId key;
        std::int32_t value;
    };

    static std::size_t capacity_for(std::size_t n) {
        std::size_t c = 16;
        while (c < n * 2) c *= 2;
        return c;
    }
    std::size_t slot(OrderId id) const {
        // Fibonacci hashing: spreads sequential ids across the table.
        return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ULL) >> shift_);
    }
    void rebuild(std::size_t cap) {
        std::vector<Entry> old;
        old.swap(table_);
        table_.assign(cap, Entry{0, kEmpty});
        mask_ = cap - 1;
        shift_ = 64;
        for (std::size_t c = cap; c > 1; c >>= 1) --shift_;
        size_ = 0;
        for (const Entry& e : old)
            if (e.value != kEmpty) set(e.key, e.value);
    }

    std::vector<Entry> table_;
    std::size_t mask_ = 0;
    unsigned shift_ = 64;
    std::size_t size_ = 0;
};

}  // namespace me
