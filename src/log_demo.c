/*
 * log_demo - proves the buffer holds up at a 60 kHz sample rate.
 *
 * A producer thread generates one sample every 1/60000 s (16.7 us) and pushes
 * it. The main thread plays logger: it wakes about once a millisecond, drains
 * whatever accumulated and writes it to a binary file. Every sample carries a
 * sequence number, so any loss or reordering is detected.
 *
 * usage: log_demo [seconds] [output-file]
 */
#include "cbuf.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define SAMPLE_RATE_HZ 60000u
#define RING_CAPACITY  8192u /* ~136 ms of headroom at 60 kHz */
#define BATCH          4096u

typedef struct {
    uint64_t seq;
    uint64_t t_ns; /* time the sample was taken */
    float    ch[4];
} sample_t; /* 32 bytes -> 1.92 MB/s at 60 kHz */

typedef struct {
    cbuf_t  *cb;
    uint64_t total;
    uint64_t max_late_ns; /* worst lateness against the ideal sample time */
    uint64_t max_push_ns;
    uint64_t sum_push_ns;
} producer_t;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *producer_main(void *arg)
{
    producer_t *p = arg;
    uint64_t t0 = now_ns();

    for (uint64_t i = 0; i < p->total; i++) {
        /* Absolute deadlines so timing error never accumulates. Spin rather
         * than sleep: no OS sleep is accurate at a 16.7 us period. On real
         * hardware this loop is the ADC/timer interrupt. */
        uint64_t deadline = t0 + i * 1000000000ull / SAMPLE_RATE_HZ;
        uint64_t t;
        while ((t = now_ns()) < deadline)
            ;
        if (t - deadline > p->max_late_ns)
            p->max_late_ns = t - deadline;

        sample_t s = { .seq = i, .t_ns = t,
                       .ch = { (float)i, (float)i * 0.5f, -(float)i, 1.0f } };
        cbuf_push(p->cb, &s); /* a failed push is counted in cb->dropped */

        uint64_t dt = now_ns() - t;
        p->sum_push_ns += dt;
        if (dt > p->max_push_ns)
            p->max_push_ns = dt;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    double seconds = argc > 1 ? atof(argv[1]) : 5.0;
    const char *path = argc > 2 ? argv[2] : "build/log.bin";
    if (seconds <= 0) {
        fprintf(stderr, "usage: %s [seconds] [output-file]\n", argv[0]);
        return 2;
    }

    sample_t *storage = malloc(RING_CAPACITY * sizeof *storage);
    sample_t *batch = malloc(BATCH * sizeof *batch);
    cbuf_t cb;
    if (!storage || !batch ||
        !cbuf_init(&cb, storage, RING_CAPACITY, sizeof(sample_t))) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 1;
    }

    producer_t prod = { .cb = &cb,
                        .total = (uint64_t)(seconds * SAMPLE_RATE_HZ) };
    pthread_t th;
    uint64_t start = now_ns();
    pthread_create(&th, NULL, producer_main, &prod);

    uint64_t logged = 0, expect_seq = 0, seq_errors = 0;
    size_t max_fill = 0;
    for (;;) {
        size_t fill = cbuf_size(&cb);
        if (fill > max_fill)
            max_fill = fill;

        size_t n = cbuf_pop_many(&cb, batch, BATCH);
        if (n == 0) {
            if (logged + cbuf_dropped(&cb) >= prod.total)
                break; /* every sample is accounted for */
            usleep(1000);
            continue;
        }
        for (size_t i = 0; i < n; i++) {
            if (batch[i].seq != expect_seq)
                seq_errors++;
            expect_seq = batch[i].seq + 1;
        }
        if (fwrite(batch, sizeof *batch, n, f) != n) {
            perror("fwrite");
            return 1;
        }
        logged += n;
    }
    pthread_join(th, NULL);
    double elapsed = (double)(now_ns() - start) / 1e9;
    fclose(f);

    size_t dropped = cbuf_dropped(&cb);
    printf("target rate      : %u Hz\n", SAMPLE_RATE_HZ);
    printf("achieved rate    : %.1f Hz over %.3f s\n",
           (double)prod.total / elapsed, elapsed);
    printf("samples produced : %llu\n", (unsigned long long)prod.total);
    printf("samples logged   : %llu  (%.2f MB -> %s)\n",
           (unsigned long long)logged,
           (double)logged * sizeof(sample_t) / 1e6, path);
    printf("dropped          : %zu\n", dropped);
    printf("sequence errors  : %llu\n", (unsigned long long)seq_errors);
    printf("max buffer fill  : %zu / %u (%.1f%%)\n", max_fill, RING_CAPACITY,
           100.0 * (double)max_fill / RING_CAPACITY);
    printf("push cost        : avg %llu ns, worst %llu ns (budget 16667 ns)\n",
           (unsigned long long)(prod.sum_push_ns / prod.total),
           (unsigned long long)prod.max_push_ns);
    printf("worst sample jitter: %llu ns late\n",
           (unsigned long long)prod.max_late_ns);

    int ok = dropped == 0 && seq_errors == 0 && logged == prod.total;
    puts(ok ? "RESULT: PASS" : "RESULT: FAIL");
    free(storage);
    free(batch);
    return ok ? 0 : 1;
}
