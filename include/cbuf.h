/*
 * cbuf.h - lock-free single-producer / single-consumer circular buffer.
 *
 * Built for high-rate data logging: one context (ISR, sampling thread) pushes
 * fixed-size samples, another (logger thread) drains them in batches and does
 * the slow work (disk, network). At 60 kHz a sample arrives every 16.7 us;
 * a push here is a memcpy plus one atomic store, with no locks, syscalls or
 * allocation, so the producer never waits on the consumer.
 *
 * Rules:
 *   - exactly ONE producer context calls cbuf_push()
 *   - exactly ONE consumer context calls cbuf_pop() / cbuf_pop_many()
 *   - capacity must be a power of two
 *   - when full, new samples are dropped and counted (old data is kept)
 */
#ifndef CBUF_H
#define CBUF_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Keeps producer and consumer indices on separate cache lines (no false
 * sharing). 128 covers Apple arm64; it is merely generous on x86 (64). */
#ifndef CBUF_CACHELINE
#define CBUF_CACHELINE 128
#endif

typedef struct {
    /* read-only after init */
    uint8_t *data;
    size_t   elem_size;
    size_t   capacity; /* in elements, power of two */
    size_t   mask;

    /* producer side */
    _Alignas(CBUF_CACHELINE) atomic_size_t head; /* next slot to write */
    size_t        tail_cache;                    /* producer's view of tail */
    atomic_size_t dropped;                       /* pushes rejected when full */

    /* consumer side */
    _Alignas(CBUF_CACHELINE) atomic_size_t tail; /* next slot to read */
    size_t head_cache;                           /* consumer's view of head */
} cbuf_t;

/* storage must hold capacity * elem_size bytes. Returns false on bad args. */
static inline bool cbuf_init(cbuf_t *cb, void *storage, size_t capacity,
                             size_t elem_size)
{
    if (!cb || !storage || elem_size == 0 || capacity < 2 ||
        (capacity & (capacity - 1)) != 0)
        return false;
    cb->data = (uint8_t *)storage;
    cb->elem_size = elem_size;
    cb->capacity = capacity;
    cb->mask = capacity - 1;
    atomic_init(&cb->head, 0);
    atomic_init(&cb->tail, 0);
    atomic_init(&cb->dropped, 0);
    cb->tail_cache = 0;
    cb->head_cache = 0;
    return true;
}

/* Producer only. Wait-free. Returns false (and counts a drop) if full. */
static inline bool cbuf_push(cbuf_t *cb, const void *elem)
{
    /* Indices run freely and wrap with size_t; head - tail is the fill. */
    size_t head = atomic_load_explicit(&cb->head, memory_order_relaxed);

    if (head - cb->tail_cache == cb->capacity) {
        /* Looks full: refresh our view of the consumer's progress. */
        cb->tail_cache = atomic_load_explicit(&cb->tail, memory_order_acquire);
        if (head - cb->tail_cache == cb->capacity) {
            atomic_fetch_add_explicit(&cb->dropped, 1, memory_order_relaxed);
            return false;
        }
    }
    memcpy(cb->data + (head & cb->mask) * cb->elem_size, elem, cb->elem_size);
    atomic_store_explicit(&cb->head, head + 1, memory_order_release);
    return true;
}

/* Consumer only. Copies up to max elements into out; returns the count. */
static inline size_t cbuf_pop_many(cbuf_t *cb, void *out, size_t max)
{
    size_t tail = atomic_load_explicit(&cb->tail, memory_order_relaxed);
    size_t avail = cb->head_cache - tail;

    if (avail < max) {
        cb->head_cache = atomic_load_explicit(&cb->head, memory_order_acquire);
        avail = cb->head_cache - tail;
    }
    size_t n = avail < max ? avail : max;
    if (n == 0)
        return 0;

    /* At most two contiguous runs: up to the end of storage, then the wrap. */
    size_t idx = tail & cb->mask;
    size_t first = cb->capacity - idx;
    if (first > n)
        first = n;
    memcpy(out, cb->data + idx * cb->elem_size, first * cb->elem_size);
    memcpy((uint8_t *)out + first * cb->elem_size, cb->data,
           (n - first) * cb->elem_size);

    atomic_store_explicit(&cb->tail, tail + n, memory_order_release);
    return n;
}

/* Consumer only. */
static inline bool cbuf_pop(cbuf_t *cb, void *out)
{
    return cbuf_pop_many(cb, out, 1) == 1;
}

/* Approximate when called concurrently; exact when the other side is idle. */
static inline size_t cbuf_size(const cbuf_t *cb)
{
    size_t head = atomic_load_explicit(&cb->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&cb->tail, memory_order_acquire);
    return head - tail;
}

static inline size_t cbuf_dropped(const cbuf_t *cb)
{
    return atomic_load_explicit(&cb->dropped, memory_order_relaxed);
}

#endif /* CBUF_H */
