// ring_pool_test.cpp - unit tests for include/RingBufferPool.hpp (C++14).
#include "RingBufferPool.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using ::testing::_;
using ::testing::MockFunction;

namespace {

struct Sample { float ch[4]; std::uint8_t fault; };
struct Row { std::uint32_t file; double t; std::uint8_t fault; };

const double kRate = 8.0;   // samples per second

// Rows of one file must be a complete 6 s window around its fault: in time order, no gaps.
void expect_complete_file(const std::vector<Row>& rows, std::uint32_t file, double fault) {
    std::vector<Row> f;
    for (const Row& r : rows) if (r.file == file) f.push_back(r);
    ASSERT_FALSE(f.empty()) << "file " << file;
    EXPECT_NEAR(static_cast<double>(f.size()), 6 * kRate, 1.5) << "file " << file;
    for (std::size_t i = 1; i < f.size(); ++i) {
        const double step = f[i].t - f[i - 1].t;
        EXPECT_GT(step, 0.124) << "file " << file << " row " << i;
        EXPECT_LT(step, 0.126) << "file " << file << " row " << i;
    }
    EXPECT_GE(f.front().t, fault - 3 - 1e-9) << "file " << file;
    EXPECT_LE(f.back().t, fault + 3 + 1e-9) << "file " << file;
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

template <std::size_t Capacity, std::size_t Count>
class RingPoolFixture : public ::testing::Test {
protected:
    cbuf::RingPool<Sample, Capacity, Count> pool;
    std::vector<Row> rows;
    std::function<void(std::uint32_t, double, const Sample&)> sink =
        [this](std::uint32_t file, double t, const Sample& s) { rows.push_back({file, t, s.fault}); };
};

using RingPoolTest = RingPoolFixture<32, 5>;
using RingPoolPageSizedTest = RingPoolFixture<16, 7>;

TEST_F(RingPoolTest, NoFaultWritesNothing) {
    MockFunction<void(std::uint32_t, double, const Sample&)> mock_sink;
    EXPECT_CALL(mock_sink, Call(_, _, _)).Times(0);
    auto sink = mock_sink.AsStdFunction();
    run(pool, sink, 0, 40);
    EXPECT_EQ(pool.dropped(), 0u);
    EXPECT_FALSE(pool.fault_flag());
}

TEST_F(RingPoolTest, OneFaultWritesOneCompleteFile) {
    run(pool, sink, 0, 40);
    pool.raise_fault(40.0);
    run(pool, sink, 40, 50);
    EXPECT_TRUE(pool.fault_flag());
    EXPECT_EQ(pool.fault_count(), 1u);
    EXPECT_EQ(pool.dropped(), 0u);
    expect_complete_file(rows, 1, 40.0);
}

TEST_F(RingPoolTest, BackToBackFaultsEachGetACompleteFile) {
    run(pool, sink, 0, 50);
    pool.raise_fault(50.0);  run(pool, sink, 50, 50.5);
    pool.raise_fault(50.5);  run(pool, sink, 50.5, 51.25);
    pool.raise_fault(51.25); run(pool, sink, 51.25, 60);
    expect_complete_file(rows, 1, 50.0);
    expect_complete_file(rows, 2, 50.5);
    expect_complete_file(rows, 3, 51.25);
    EXPECT_EQ(pool.fault_count(), 3u);
    EXPECT_EQ(pool.dropped(), 0u);
}

TEST_F(RingPoolTest, SimultaneousFaultsEachGetAFile) {
    run(pool, sink, 0, 60);
    pool.raise_fault(60.0, 3);
    run(pool, sink, 60, 70);
    EXPECT_EQ(pool.fault_count(), 3u);
    expect_complete_file(rows, 1, 60.0);
    expect_complete_file(rows, 2, 60.0);
    expect_complete_file(rows, 3, 60.0);
}

TEST_F(RingPoolTest, FaultStormDropsNothing) {
    run(pool, sink, 0, 70);
    for (int i = 0; i < 120; ++i) {
        pool.raise_fault(70.0 + i * 0.25);
        run(pool, sink, 70.0 + i * 0.25, 70.0 + (i + 1) * 0.25);
    }
    run(pool, sink, 100, 110);
    for (int i = 0; i < 120; ++i) expect_complete_file(rows, 1 + static_cast<std::uint32_t>(i), 70.0 + i * 0.25);
    EXPECT_EQ(pool.dropped(), 0u);
    EXPECT_EQ(pool.fault_count(), 120u);
}

TEST_F(RingPoolTest, FaultFlagLatchesUntilReset) {
    pool.raise_fault(0.0);
    run(pool, sink, 0, 10);
    EXPECT_TRUE(pool.fault_flag());
    pool.reset();
    EXPECT_FALSE(pool.fault_flag());
    EXPECT_EQ(pool.fault_count(), 0u);
    EXPECT_EQ(pool.dropped(), 0u);
}

// The size the simulator picks for 8 Hz (16 slots x 7 rings) also survives a fault storm.
TEST_F(RingPoolPageSizedTest, SurvivesFaultStorm) {
    run(pool, sink, 0, 6);
    for (int i = 0; i < 120; ++i) {
        pool.raise_fault(6.0 + i * 0.25);
        run(pool, sink, 6.0 + i * 0.25, 6.0 + (i + 1) * 0.25);
    }
    run(pool, sink, 36, 46);
    for (int i = 0; i < 120; ++i) expect_complete_file(rows, 1 + static_cast<std::uint32_t>(i), 6.0 + i * 0.25);
    EXPECT_EQ(pool.dropped(), 0u);
}

// Failure logger off: faults latch the flag and the count, but nothing is locked or written.
TEST_F(RingPoolPageSizedTest, FailureLoggerOffWritesNothing) {
    MockFunction<void(std::uint32_t, double, const Sample&)> mock_sink;
    EXPECT_CALL(mock_sink, Call(_, _, _)).Times(0);
    auto silent = mock_sink.AsStdFunction();

    pool.set_failure_logger(false);
    run(pool, silent, 0, 6);
    pool.raise_fault(6.0, 2);
    run(pool, silent, 6, 20);
    EXPECT_TRUE(pool.fault_flag());
    EXPECT_EQ(pool.fault_count(), 2u);
    EXPECT_EQ(pool.dropped(), 0u);

    pool.set_failure_logger(true);
    pool.raise_fault(20.0);
    run(pool, sink, 20, 30);
    EXPECT_EQ(pool.fault_count(), 3u);
    expect_complete_file(rows, 1, 20.0);

    pool.reset();                                    // a power cycle keeps the on/off setting
    EXPECT_TRUE(pool.failure_logger());
    EXPECT_FALSE(pool.fault_flag());
}

// A pool that is too small drops samples (and counts them) instead of overwriting a locked window.
TEST(RingPoolTooSmall, DropsAndCountsInsteadOfOverwriting) {
    cbuf::RingPool<Sample, 8, 2> tiny;
    MockFunction<void(std::uint32_t, double, const Sample&)> mock_sink;
    EXPECT_CALL(mock_sink, Call(_, _, _)).Times(::testing::AtLeast(1));
    auto sink = mock_sink.AsStdFunction();
    std::size_t pushed = 0;
    for (long n = 0; n < 400; ++n) {
        const double t = n / kRate;
        if (n % 8 == 0) tiny.raise_fault(t);
        if (tiny.push(t, Sample{})) ++pushed;
        tiny.service(t, sink);
    }
    EXPECT_GT(tiny.dropped(), 0u);
    EXPECT_EQ(pushed + tiny.dropped(), 400u);
}

// pool_size picks power-of-two rings and the ring count that needs the fewest slots.
constexpr bool is_pow2(std::size_t n) { return n >= 2 && (n & (n - 1)) == 0; }
static_assert(cbuf::pool_size(8).capacity == 16 && cbuf::pool_size(8).rings == 7, "8 Hz");
static_assert(cbuf::pool_size(1).capacity == 4 && cbuf::pool_size(1).rings == 5, "1 Hz");
static_assert(cbuf::pool_size(60000).capacity == 131072 && cbuf::pool_size(60000).rings == 7, "60 kHz");
static_assert(is_pow2(cbuf::pool_size(999).capacity), "power of two");

class PoolSizeTest : public ::testing::TestWithParam<double> {};

TEST_P(PoolSizeTest, PicksMinimalPowerOfTwoLayout) {
    const double r = GetParam();
    const cbuf::PoolSize z = cbuf::pool_size(r);
    EXPECT_TRUE(is_pow2(z.capacity));
    EXPECT_GE(z.rings, 3u);
    EXPECT_LE(z.rings, 8u);
    EXPECT_GE(static_cast<double>((z.rings - 2) * z.capacity), 9 * r);
    for (std::size_t n = 3; n <= 8; ++n)
        for (std::size_t c = 4; c <= z.capacity * 8; c *= 2)
            if (static_cast<double>((n - 2) * c) >= 9 * r) EXPECT_GE(n * c, z.rings * z.capacity);
}

INSTANTIATE_TEST_SUITE_P(Rates, PoolSizeTest,
                         ::testing::Values(1.0, 2.0, 7.0, 8.0, 9.0, 10.0, 100.0, 999.0, 1000.0, 44100.0, 60000.0));

}  // namespace
