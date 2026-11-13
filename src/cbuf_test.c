/* Single-threaded correctness checks for cbuf.h. */
#include "cbuf.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    uint32_t storage[8];
    uint32_t out[8];
    cbuf_t cb;

    /* bad arguments */
    assert(!cbuf_init(&cb, storage, 6, sizeof(uint32_t))); /* not pow2 */
    assert(!cbuf_init(&cb, storage, 8, 0));
    assert(!cbuf_init(&cb, NULL, 8, sizeof(uint32_t)));
    assert(cbuf_init(&cb, storage, 8, sizeof(uint32_t)));

    /* empty */
    assert(cbuf_size(&cb) == 0);
    assert(!cbuf_pop(&cb, out));

    /* fill, then overflow drops the new sample and keeps the old ones */
    for (uint32_t i = 0; i < 8; i++)
        assert(cbuf_push(&cb, &i));
    uint32_t extra = 99;
    assert(!cbuf_push(&cb, &extra));
    assert(cbuf_dropped(&cb) == 1);
    assert(cbuf_size(&cb) == 8);

    /* FIFO order */
    assert(cbuf_pop_many(&cb, out, 3) == 3);
    assert(out[0] == 0 && out[1] == 1 && out[2] == 2);

    /* wraparound: batch pop spans the end of storage */
    for (uint32_t i = 8; i < 11; i++)
        assert(cbuf_push(&cb, &i));
    assert(cbuf_pop_many(&cb, out, 8) == 8);
    for (uint32_t i = 0; i < 8; i++)
        assert(out[i] == i + 3);
    assert(cbuf_size(&cb) == 0);

    /* many laps around the ring */
    uint32_t next = 0;
    for (uint32_t i = 0; i < 100000; i++) {
        assert(cbuf_push(&cb, &i));
        if (i % 5 == 4) {
            size_t n = cbuf_pop_many(&cb, out, 8);
            assert(n == 5);
            for (size_t k = 0; k < n; k++)
                assert(out[k] == next++);
        }
    }
    assert(next == 100000 && cbuf_dropped(&cb) == 1);

    puts("cbuf_test: all checks passed");
    return 0;
}
