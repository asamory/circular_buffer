// ring_pool_test.cpp - self-check for include/ring_pool.hpp (C++14).
// Exits 0 and prints "ok" when every case passes; prints the failing check and exits 1 otherwise.
#include "ring_pool.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            std::exit(1);                                                           \
        }                                                                           \
    } while (0)

namespace {

struct Sample { float ch[4]; std::uint8_t fault; };
struct Row { std::uint32_t file; double t; std::uint8_t fault; };

const double kRate = 8.0;   // samples per second

// Rows of one file must be a complete 6 s window around its fault: in time order, no gaps.
std::size_t check_file(const std::vector<Row>& rows, std::uint32_t file, double fault) {
    std::vector<Row> f;
    for (const Row& r : rows) if (r.file == file) f.push_back(r);
    CHECK(std::fabs(static_cast<double>(f.size()) - 6 * kRate) <= 1.5);
    for (std::size_t i = 1; i < f.size(); ++i) {
        const double step = f[i].t - f[i - 1].t;
        CHECK(step > 0.124 && step < 0.126);
    }
    CHECK(f.front().t >= fault - 3 - 1e-9 && f.back().t <= fault + 3 + 1e-9);
    return f.size();
}

// Logs samples at kRate from time `from` up to (not including) `to`, servicing the pool after each one.
template <typename Pool, typename Sink>
void run(Pool& pool, Sink& sink, double from, double to) {
    for (long n = std::lround(from * kRate); n < std::lround(to * kRate); ++n) {
        const double t = n / kRate;
        Sample s{{static_cast<float>(n), 0, 0, 0}, static_cast<std::uint8_t>(pool.fault_count())};
        pool.push(t, s);
        pool.service(t, sink);
    }
}

void main_cases() {
    cbuf::RingPool<Sample, 32, 5> pool;
    std::vector<Row> rows;
    auto sink = [&](std::uint32_t file, double t, const Sample& s) { rows.push_back({file, t, s.fault}); };

    // 1. no fault: nothing written, nothing dropped, flag clear
    run(pool, sink, 0, 40);
    CHECK(rows.empty() && pool.dropped() == 0 && !pool.fault_flag());

    // 2. one fault: one complete 6 s file, and logging carries on
    pool.raise_fault(40.0);
    run(pool, sink, 40, 50);
    CHECK(pool.fault_flag() && pool.fault_count() == 1 && pool.dropped() == 0);
    check_file(rows, 1, 40.0);

    // 3. back-to-back faults: one complete file each
    pool.raise_fault(50.0);  run(pool, sink, 50, 50.5);
    pool.raise_fault(50.5);  run(pool, sink, 50.5, 51.25);
    pool.raise_fault(51.25); run(pool, sink, 51.25, 60);
    check_file(rows, 2, 50.0);
    check_file(rows, 3, 50.5);
    check_file(rows, 4, 51.25);
    CHECK(pool.fault_count() == 4 && pool.dropped() == 0);

    // 4. several faults at once: the count goes up by that many and each gets a file
    pool.raise_fault(60.0, 3);
    run(pool, sink, 60, 70);
    CHECK(pool.fault_count() == 7);
    check_file(rows, 5, 60.0);
    check_file(rows, 6, 60.0);
    check_file(rows, 7, 60.0);

    // 5. fault storm: a fault every 0.25 s for 30 s; every file complete, nothing dropped
    for (int i = 0; i < 120; ++i) {
        pool.raise_fault(70.0 + i * 0.25);
        run(pool, sink, 70.0 + i * 0.25, 70.0 + (i + 1) * 0.25);
    }
    run(pool, sink, 100, 110);
    for (int i = 0; i < 120; ++i) check_file(rows, 8 + static_cast<std::uint32_t>(i), 70.0 + i * 0.25);
    CHECK(pool.dropped() == 0 && pool.fault_count() == 127);

    // 6. the fault flag stays up until a power cycle
    CHECK(pool.fault_flag());
    pool.reset();
    CHECK(!pool.fault_flag() && pool.fault_count() == 0 && pool.dropped() == 0);
}

// 7. the size the simulator picks for 8 Hz (16 slots x 7 rings) also survives a fault storm
void sized_like_the_page() {
    cbuf::RingPool<Sample, 16, 7> pool;
    std::vector<Row> rows;
    auto sink = [&](std::uint32_t file, double t, const Sample& s) { rows.push_back({file, t, s.fault}); };
    run(pool, sink, 0, 6);
    for (int i = 0; i < 120; ++i) {
        pool.raise_fault(6.0 + i * 0.25);
        run(pool, sink, 6.0 + i * 0.25, 6.0 + (i + 1) * 0.25);
    }
    run(pool, sink, 36, 46);
    for (int i = 0; i < 120; ++i) check_file(rows, 1 + static_cast<std::uint32_t>(i), 6.0 + i * 0.25);
    CHECK(pool.dropped() == 0);
}

// 8. a pool that is too small drops samples (and counts them) instead of overwriting a locked window
void too_small() {
    cbuf::RingPool<Sample, 8, 2> tiny;
    std::size_t wrote = 0, pushed = 0;
    auto count = [&](std::uint32_t, double, const Sample&) { ++wrote; };
    for (long n = 0; n < 400; ++n) {
        const double t = n / kRate;
        if (n % 8 == 0) tiny.raise_fault(t);
        if (tiny.push(t, Sample{})) ++pushed;
        tiny.service(t, count);
    }
    CHECK(tiny.dropped() > 0);
    CHECK(pushed + tiny.dropped() == 400);
    CHECK(wrote > 0);
}

// 9. failure logger off: faults latch the flag and the count, but nothing is locked or written
void failure_logger_off() {
    cbuf::RingPool<Sample, 16, 7> pool;
    std::vector<Row> rows;
    auto sink = [&](std::uint32_t file, double t, const Sample& s) { rows.push_back({file, t, s.fault}); };
    pool.set_failure_logger(false);
    run(pool, sink, 0, 6);
    pool.raise_fault(6.0, 2);
    run(pool, sink, 6, 20);
    CHECK(pool.fault_flag() && pool.fault_count() == 2 && rows.empty() && pool.dropped() == 0);
    pool.set_failure_logger(true);
    pool.raise_fault(20.0);
    run(pool, sink, 20, 30);
    CHECK(pool.fault_count() == 3);
    check_file(rows, 1, 20.0);
    pool.reset();                                    // a power cycle keeps the on/off setting
    CHECK(pool.failure_logger() && !pool.fault_flag());
}

// 10. pool_size picks power-of-two rings and the ring count that needs the fewest slots
constexpr bool is_pow2(std::size_t n) { return n >= 2 && (n & (n - 1)) == 0; }
static_assert(cbuf::pool_size(8).capacity == 16 && cbuf::pool_size(8).rings == 7, "8 Hz");
static_assert(cbuf::pool_size(1).capacity == 4 && cbuf::pool_size(1).rings == 5, "1 Hz");
static_assert(cbuf::pool_size(60000).capacity == 131072 && cbuf::pool_size(60000).rings == 7, "60 kHz");
static_assert(is_pow2(cbuf::pool_size(999).capacity), "power of two");

void sizing() {
    const double rates[] = {1, 2, 7, 8, 9, 10, 100, 999, 1000, 44100, 60000};
    for (double r : rates) {
        const cbuf::PoolSize z = cbuf::pool_size(r);
        CHECK(is_pow2(z.capacity) && z.rings >= 3 && z.rings <= 8);
        CHECK(static_cast<double>((z.rings - 2) * z.capacity) >= 9 * r);
        for (std::size_t n = 3; n <= 8; ++n)
            for (std::size_t c = 4; c <= z.capacity * 8; c *= 2)
                if (static_cast<double>((n - 2) * c) >= 9 * r) CHECK(n * c >= z.rings * z.capacity);
    }
}

}  // namespace

int main() {
    main_cases();
    sized_like_the_page();
    too_small();
    failure_logger_off();
    sizing();
    std::printf("ok: ring_pool self-check passed (10 cases)\n");
    return 0;
}
