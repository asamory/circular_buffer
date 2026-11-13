# circular_buffer

A lock-free circular buffer in C11 for logging fixed-size samples at high rates
(tested at 60 kHz). It is header-only: include `include/cbuf.h`.

One context pushes samples (a sampling thread or interrupt handler); another
drains them in batches and does the slow work, such as writing to disk. A push
is a `memcpy` plus one atomic store, with no locks, system calls or allocation,
so the producer never waits on the logger.

## Constraints

- **One producer, one consumer.** Exactly one context may push and exactly one
  may pop. More than one of either is not safe.
- **Capacity must be a power of two.**
- **When the buffer is full, new samples are dropped and counted.** Old data is
  never overwritten. Read the count with `cbuf_dropped()`.

## Usage

```c
#include "cbuf.h"

typedef struct { uint64_t seq; float ch[4]; } sample_t;

static sample_t storage[8192];
static cbuf_t cb;

cbuf_init(&cb, storage, 8192, sizeof(sample_t));

/* producer */
sample_t s = { /* ... */ };
if (!cbuf_push(&cb, &s)) {
    /* buffer full: sample dropped */
}

/* consumer */
sample_t batch[4096];
size_t n = cbuf_pop_many(&cb, batch, 4096);
/* write batch[0..n) to disk */
```

## API

| Function | Called by | Description |
|---|---|---|
| `cbuf_init(cb, storage, capacity, elem_size)` | setup | Attach caller-owned storage of `capacity * elem_size` bytes. Returns `false` on bad arguments. |
| `cbuf_push(cb, elem)` | producer | Copy one element in. Returns `false` and counts a drop if full. |
| `cbuf_pop_many(cb, out, max)` | consumer | Copy up to `max` elements out. Returns the number copied. |
| `cbuf_pop(cb, out)` | consumer | Copy one element out. Returns `false` if empty. |
| `cbuf_size(cb)` | either | Current fill. Approximate while the other side is running. |
| `cbuf_dropped(cb)` | either | Number of pushes rejected because the buffer was full. |

## Sizing

Capacity sets how long the logger can stall before samples are dropped:

    headroom = capacity / sample rate

The demo uses 8,192 slots at 60 kHz, which is about 136 ms. Size it for the
longest pause you expect from the consumer (a slow disk write, a scheduling
delay), with margin.

## Build and test

```bash
make test
```

This builds into `build/` and runs two programs:

- `cbuf_test` checks single-threaded correctness: argument validation, FIFO
  order, wraparound and behaviour when full.
- `log_demo [seconds] [output-file]` runs a producer thread at 60 kHz while the
  main thread drains the buffer to a binary file about once a millisecond.
  Every sample carries a sequence number, so loss or reordering is detected. It
  defaults to 5 seconds and `build/log.bin`.

A typical 5-second run on an Apple silicon Mac logs all 300,000 samples with no
drops and a peak fill below 3% of the buffer.

## Timing on a desktop OS

The demo paces samples by spinning on the clock, and macOS will occasionally
suspend the producer thread for a millisecond or more. The buffer absorbs these
stalls, so nothing is lost, but the samples are not evenly spaced: the logged
timestamps show a gap followed by a burst. Even spacing has to come from the
sampling source, such as a hardware timer interrupt or a real-time thread.

## Layout

    include/cbuf.h     the buffer
    src/cbuf_test.c    correctness checks
    src/log_demo.c     60 kHz logging demo
    Makefile
