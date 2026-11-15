// ring_pool_stress.cpp - runs the three contexts of ring_pool.hpp as three real threads and checks
// every file the logger writes, then times push(). Build with -pthread.
//   producer: pushes samples as fast as it can      poller: raises faults      logger: writes the files
#include "ring_pool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

using cbuf::Tick;
using Clock = std::chrono::steady_clock;

struct Sample {
    std::uint64_t seq;      // equals the tick, so a torn or misplaced row is detectable
    std::uint64_t check;    // seq * kMagic
    float ch[4];
};
const std::uint64_t kMagic = 0x9E3779B97F4A7C15ull;

const Tick kPre = 3000, kPost = 3000;            // one tick per sample
using Pool = cbuf::RingPool<Sample, 65536, 8>;
Pool pool(kPre, kPost);                           // static storage: no heap

struct File { Tick first = 0, last = 0; std::uint64_t rows = 0; bool in_order = true, intact = true; };

int stress(std::uint64_t samples) {
    std::atomic<Tick> clock{0};
    std::atomic<bool> producing{true};
    std::vector<Tick> faults;                    // written by the poller, read after it has stopped
    std::vector<File> files;                     // written by the logger, read after it has stopped
    files.reserve(1 << 16); faults.reserve(1 << 16);

    std::thread producer([&] {
        for (std::uint64_t n = 0; n < samples; ++n) {
            pool.push(n, Sample{n, n * kMagic, {0, 0, 0, 0}});
            clock.store(n, std::memory_order_relaxed);
            if ((n & 0xFFF) == 0) std::this_thread::yield();   // let the other threads run on small machines
        }
        producing.store(false);
    });
    std::thread poller([&] {
        Tick last = 0;
        while (producing.load()) {
            const Tick t = clock.load(std::memory_order_relaxed);
            if (t >= last + 2000 && faults.size() < faults.capacity()) {   // windows overlap: faults closer than pre + post
                const std::uint32_t missed = pool.missed_faults();
                pool.raise_fault(t);
                if (pool.missed_faults() == missed) faults.push_back(t);
                last = t;
            }
            std::this_thread::yield();
        }
    });
    std::thread logger([&] {
        auto sink = [&](std::uint32_t file, Tick t, const Sample& s) {
            if (file > files.size()) files.push_back(File{t, t, 0, true, true});
            File& f = files[file - 1];
            if (f.rows > 0 && t != f.last + 1) f.in_order = false;
            if (s.seq != t || s.check != t * kMagic) f.intact = false;
            f.last = t;
            ++f.rows;
        };
        while (producing.load()) pool.service(clock.load(std::memory_order_relaxed), sink);
        pool.service(samples + kPost + 1, sink);                 // whatever is still pending
    });
    producer.join(); poller.join(); logger.join();

    std::size_t complete = 0, torn = 0, gaps = 0;
    for (std::size_t i = 0; i < files.size() && i < faults.size(); ++i) {
        const File& f = files[i];
        const Tick lo = faults[i] > kPre ? faults[i] - kPre : 0, hi = std::min<Tick>(faults[i] + kPost, samples - 1);
        if (!f.intact) ++torn;
        if (!f.in_order) ++gaps;
        if (f.intact && f.in_order && f.first == lo && f.last == hi && f.rows == hi - lo + 1) ++complete;
    }
    std::printf("stress: %llu samples, %zu faults, %zu files, %zu complete, %zu with gaps, %zu torn, %llu dropped, %u faults missed\n",
                static_cast<unsigned long long>(samples), faults.size(), files.size(), complete, gaps, torn,
                static_cast<unsigned long long>(pool.dropped()), pool.missed_faults());
    if (torn != 0) { std::printf("FAIL: a row did not match its tick\n"); return 1; }
    if (files.size() != faults.size()) { std::printf("FAIL: not one file per fault\n"); return 1; }
    if (pool.dropped() == 0 && complete != files.size()) { std::printf("FAIL: nothing was dropped, yet a file is incomplete\n"); return 1; }
    if (files.empty()) { std::printf("FAIL: no fault was captured\n"); return 1; }
    return 0;
}

// How long one push() takes, alone on its thread.
void latency() {
    pool.reset();
    const std::uint64_t n = 20000000;
    const auto t0 = Clock::now();
    for (std::uint64_t i = 0; i < n; ++i) pool.push(i, Sample{i, i * kMagic, {0, 0, 0, 0}});
    const double avg = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / static_cast<double>(n);

    pool.reset();
    std::vector<std::uint32_t> ns(2000000);
    for (std::uint64_t i = 0; i < ns.size(); ++i) {              // each call timed on its own; includes the clock's own cost
        const auto a = Clock::now();
        pool.push(i, Sample{i, i * kMagic, {0, 0, 0, 0}});
        ns[i] = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - a).count());
    }
    std::sort(ns.begin(), ns.end());
    std::printf("push: %.1f ns average over %llu calls; timed one by one: median %u ns, 99.9%% under %u ns, worst %u ns\n",
                avg, static_cast<unsigned long long>(n), ns[ns.size() / 2], ns[ns.size() - ns.size() / 1000], ns.back());
}

}  // namespace

int main(int argc, char** argv) {
    const std::uint64_t samples = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 50000000ull;
    const int rc = stress(samples);
    latency();
    std::printf(rc == 0 ? "ok\n" : "FAILED\n");
    return rc;
}
