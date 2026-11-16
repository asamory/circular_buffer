// cbuf_test.cpp - single-threaded correctness checks for include/cbuf.h.
// gtest must be included first: <stdatomic.h> macros clash with libc++ headers.
#include <gtest/gtest.h>

#include "RingBuffer.h"

namespace {

class RingBufferTest : public ::testing::Test {
protected:
    uint32_t storage[8];
    uint32_t out[8];
    cbuf_t cb;

    void SetUp() override { ASSERT_TRUE(cbuf_init(&cb, storage, 8, sizeof(uint32_t))); }
    void push_range(uint32_t from, uint32_t to) {
        for (uint32_t i = from; i < to; i++) ASSERT_TRUE(cbuf_push(&cb, &i));
    }
};

TEST(RingBufferInit, RejectsBadArguments) {
    uint32_t storage[8];
    cbuf_t cb;
    EXPECT_FALSE(cbuf_init(&cb, storage, 6, sizeof(uint32_t)));  // not pow2
    EXPECT_FALSE(cbuf_init(&cb, storage, 8, 0));
    EXPECT_FALSE(cbuf_init(&cb, NULL, 8, sizeof(uint32_t)));
    EXPECT_TRUE(cbuf_init(&cb, storage, 8, sizeof(uint32_t)));
}

TEST_F(RingBufferTest, StartsEmpty) {
    EXPECT_EQ(cbuf_size(&cb), 0u);
    EXPECT_FALSE(cbuf_pop(&cb, out));
}

TEST_F(RingBufferTest, OverflowDropsNewSampleAndKeepsOld) {
    push_range(0, 8);
    uint32_t extra = 99;
    EXPECT_FALSE(cbuf_push(&cb, &extra));
    EXPECT_EQ(cbuf_dropped(&cb), 1u);
    EXPECT_EQ(cbuf_size(&cb), 8u);
    ASSERT_EQ(cbuf_pop_many(&cb, out, 3), 3u);
    EXPECT_EQ(out[0], 0u);
    EXPECT_EQ(out[1], 1u);
    EXPECT_EQ(out[2], 2u);
}

TEST_F(RingBufferTest, BatchPopSpansWraparound) {
    push_range(0, 8);
    ASSERT_EQ(cbuf_pop_many(&cb, out, 3), 3u);
    push_range(8, 11);
    ASSERT_EQ(cbuf_pop_many(&cb, out, 8), 8u);
    for (uint32_t i = 0; i < 8; i++) EXPECT_EQ(out[i], i + 3);
    EXPECT_EQ(cbuf_size(&cb), 0u);
}

TEST_F(RingBufferTest, ManyLapsPreserveFifoOrder) {
    uint32_t next = 0;
    for (uint32_t i = 0; i < 100000; i++) {
        ASSERT_TRUE(cbuf_push(&cb, &i));
        if (i % 5 == 4) {
            size_t n = cbuf_pop_many(&cb, out, 8);
            ASSERT_EQ(n, 5u);
            for (size_t k = 0; k < n; k++) ASSERT_EQ(out[k], next++);
        }
    }
    EXPECT_EQ(next, 100000u);
    EXPECT_EQ(cbuf_dropped(&cb), 0u);
}

}  // namespace
