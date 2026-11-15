// ring_pool.hpp - C++14 implementation of the buffers shown in the simulator.
// Every buffer is a plain fixed-size array; nothing is allocated while logging.
#ifndef RING_POOL_HPP
#define RING_POOL_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace cbuf {

constexpr double kNever = -std::numeric_limits<double>::infinity();
constexpr double kForever = std::numeric_limits<double>::infinity();

// Pool size for a logging rate, chosen the way the simulator does it.
// Speed first: a ring is always a power of two, so a slot is found with one mask.
// Space second: the pool must hold pre + post + write seconds plus two rings of
// slack; of the ring counts 3 to 8, the one needing the fewest slots is used.
struct PoolSize {
    std::size_t capacity;   // slots per ring
    std::size_t rings;      // rings in the pool
};

constexpr PoolSize pool_size(double rate_hz, double pre_s = 3.0, double post_s = 3.0,
                             double write_s = 3.0) {
    const double need = (pre_s + post_s + write_s) * rate_hz;
    PoolSize best{0, 0};
    for (std::size_t n = 3; n <= 8; ++n) {
        std::size_t cap = 4;
        while (static_cast<double>(cap * (n - 2)) < need) cap *= 2;
        if (best.rings == 0 || n * cap < best.rings * best.capacity) best = PoolSize{cap, n};
    }
    return best;
}

// One ring: two parallel arrays (sample time, sample) and a write index.
// A slot is live when its time is >= 0; clearing a slot sets its time to -1.
template <typename Sample, std::size_t Capacity>
class Ring {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

public:
    Ring() { time_.fill(-1.0); }

    bool full() const { return head_ == Capacity; }
    std::size_t live() const { return live_; }
    double last_time() const { return last_; }   // newest sample of this lap

    // Start a new lap at slot 0. Old slots stay until they are overwritten.
    void restart() { head_ = 0; last_ = kNever; }

    // Copy one sample into slot (head & mask), then publish head + 1.
    void push(double t, const Sample& s) {
        const std::size_t slot = head_ & (Capacity - 1);
        if (time_[slot] < 0.0) ++live_;
        time_[slot] = t;
        data_[slot] = s;
        last_ = t;
        ++head_;
    }

    // Visit the live slots of this lap with lo <= time <= hi, oldest first.
    template <typename Visit>
    void for_each_in(double lo, double hi, Visit&& visit) const {
        for (std::size_t i = 0; i < head_; ++i)
            if (time_[i] >= lo && time_[i] <= hi) visit(time_[i], data_[i]);
    }

    // Clear every live slot older than t (it has been written to a file).
    void clear_before(double t) {
        for (std::size_t i = 0; i < Capacity; ++i)
            if (time_[i] >= 0.0 && time_[i] < t) { time_[i] = -1.0; --live_; }
        if (live_ == 0) last_ = kNever;   // a clean ring, free to reuse
    }

private:
    std::array<double, Capacity> time_;
    std::array<Sample, Capacity> data_{};
    std::size_t head_ = 0;       // next slot to write in this lap
    std::size_t live_ = 0;       // slots that hold a record
    double last_ = kNever;
};

// A pool of Count rings, all allocated up front as one array of arrays.
// push() fills one ring, then moves to the oldest ring that is not locked.
// A fault locks the window [fault - pre, fault + post] until service() has
// handed it to the sink; then the records are cleared.
template <typename Sample, std::size_t Capacity, std::size_t Count,
          std::size_t MaxPending = 16>
class RingPool {
    static_assert(Count >= 2, "the pool needs at least two rings");

public:
    explicit RingPool(double pre_s = 3.0, double post_s = 3.0)
        : pre_(pre_s), post_(post_s) {}

    // Producer: log one sample taken at time t (seconds, never decreasing).
    // Returns false when every ring is locked: the sample is dropped and counted.
    bool push(double t, const Sample& s) {
        if (active_ == kNone || ring_buffer_[active_].full()) {
            active_ = next_ring();
            if (active_ == kNone) { ++dropped_; return false; }
            ring_buffer_[active_].restart();
        }
        ring_buffer_[active_].push(t, s);
        return true;
    }

    // Failure logger on/off. Off: a fault still latches the flag and adds to the
    // count, but no window is locked and nothing is written.
    void set_failure_logger(bool on) { enabled_ = on; }
    bool failure_logger() const { return enabled_; }

    // A fault occurred at time t. The flag latches (only reset() clears it),
    // the count goes up, and the fault is queued for its own file.
    void raise_fault(double t, std::uint32_t how_many = 1) {
        flag_ = true;
        fault_count_ += how_many;
        if (!enabled_) return;
        for (std::uint32_t i = 0; i < how_many && pending_ < MaxPending; ++i)
            fault_at_[pending_++] = t;
    }

    // Call regularly. For every fault whose post time has passed, sends its
    // 6 s window to sink(file_number, time, sample), oldest sample first, then
    // clears every record no later fault still needs. Returns files written.
    template <typename Sink>
    std::size_t service(double now, Sink&& sink) {
        std::size_t files = 0;
        while (pending_ > 0 && now >= fault_at_[0] + post_) {
            const double lo = fault_at_[0] - pre_, hi = fault_at_[0] + post_;
            ++file_no_;
            ++files;

            std::array<std::size_t, Count> order;      // rings, oldest data first
            for (std::size_t i = 0; i < Count; ++i) order[i] = i;
            for (std::size_t i = 1; i < Count; ++i)    // insertion sort, Count is small
                for (std::size_t j = i; j > 0 &&
                     ring_buffer_[order[j]].last_time() < ring_buffer_[order[j - 1]].last_time(); --j) {
                    const std::size_t tmp = order[j]; order[j] = order[j - 1]; order[j - 1] = tmp;
                }
            const std::uint32_t file = file_no_;
            for (std::size_t i = 0; i < Count; ++i)
                ring_buffer_[order[i]].for_each_in(lo, hi,
                    [&](double t, const Sample& s) { sink(file, t, s); });

            for (std::size_t i = 1; i < pending_; ++i) fault_at_[i - 1] = fault_at_[i];
            --pending_;
            const double keep_from = pending_ > 0 ? fault_at_[0] - pre_ : kForever;
            const double clear_to = hi < keep_from ? hi : keep_from;
            for (std::size_t i = 0; i < Count; ++i) ring_buffer_[i].clear_before(clear_to);
        }
        return files;
    }

    bool fault_flag() const { return flag_; }
    std::uint32_t fault_count() const { return fault_count_; }
    std::uint64_t dropped() const { return dropped_; }

    // Power cycle: the only thing that clears the fault flag and the count.
    void reset() {
        const bool on = enabled_;
        *this = RingPool(pre_, post_);
        enabled_ = on;
    }

private:
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    // A ring is locked while it holds a record of a window not yet written.
    bool locked(std::size_t r) const {
        return pending_ > 0 && ring_buffer_[r].last_time() >= fault_at_[0] - pre_;
    }

    // The oldest ring that is not locked; an empty ring counts as oldest.
    std::size_t next_ring() const {
        std::size_t pick = kNone;
        for (std::size_t r = 0; r < Count; ++r) {
            if (r == active_ || locked(r)) continue;
            if (pick == kNone || ring_buffer_[r].last_time() < ring_buffer_[pick].last_time()) pick = r;
        }
        return pick;
    }

    std::array<Ring<Sample, Capacity>, Count> ring_buffer_;   // the whole pool
    std::array<double, MaxPending> fault_at_{};         // queued fault times
    std::size_t pending_ = 0;
    std::size_t active_ = kNone;
    double pre_, post_;
    bool flag_ = false;
    bool enabled_ = true;
    std::uint32_t fault_count_ = 0;
    std::uint32_t file_no_ = 0;
    std::uint64_t dropped_ = 0;
};

template <typename Sample, std::size_t Capacity, std::size_t Count, std::size_t MaxPending>
constexpr std::size_t RingPool<Sample, Capacity, Count, MaxPending>::kNone;

}  // namespace cbuf

#endif  // RING_POOL_HPP
