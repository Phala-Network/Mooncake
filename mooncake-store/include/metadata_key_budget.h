#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace mooncake {

// Counts live metadata plus slots reserved by concurrent inserts. A reservation
// moves into the metadata only after insertion; every failure releases it.
class MetadataKeyBudget {
 public:
    class Reservation {
     public:
        Reservation() = default;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        Reservation(Reservation&& other) noexcept
            : budget_(std::exchange(other.budget_, nullptr)),
              count_(std::exchange(other.count_, 0)) {}
        Reservation& operator=(Reservation&& other) noexcept {
            if (this != &other) {
                Release();
                budget_ = std::exchange(other.budget_, nullptr);
                count_ = std::exchange(other.count_, 0);
            }
            return *this;
        }
        ~Reservation() { Release(); }

        Reservation TakeOne() {
            assert(budget_ && count_ > 0);
            --count_;
            return Reservation(budget_, 1);
        }

     private:
        friend class MetadataKeyBudget;
        Reservation(MetadataKeyBudget* budget, uint64_t count)
            : budget_(budget), count_(count) {}
        void Release() {
            if (budget_) {
                budget_->used_.fetch_sub(count_, std::memory_order_relaxed);
                budget_ = nullptr;
            }
        }
        MetadataKeyBudget* budget_ = nullptr;
        uint64_t count_ = 0;
    };

    explicit MetadataKeyBudget(uint64_t limit = 0) : limit_(limit) {}
    std::optional<Reservation> TryAcquire(uint64_t count = 1) {
        const uint64_t limit = limit_ ? limit_ : UINT64_MAX;
        auto used = used_.load(std::memory_order_relaxed);
        do {
            if (used > limit || count > limit - used) return std::nullopt;
        } while (!used_.compare_exchange_weak(
            used, used + count, std::memory_order_relaxed));
        return Reservation(this, count);
    }
    uint64_t Used() const { return used_.load(std::memory_order_relaxed); }

 private:
    const uint64_t limit_;
    std::atomic<uint64_t> used_{0};
};

}  // namespace mooncake
