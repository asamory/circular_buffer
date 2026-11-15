// ring_pool_example.cpp - logs 20 s of samples at 8 Hz, raises one fault at
// t = 10 s, and writes its 6 s window to fault_capture_1.csv.
#include "ring_pool.hpp"

#include <cstdio>

struct Sample {
    float ch[4];
    std::uint32_t fault_count;
};

constexpr double kRate = 8.0;                             // samples per second
constexpr cbuf::PoolSize kSize = cbuf::pool_size(kRate);  // 16 slots x 7 rings

int main() {
    // static: the whole pool is one fixed block of memory, no heap
    static cbuf::RingPool<Sample, kSize.capacity, kSize.rings> pool;

    std::FILE* file = nullptr;
    std::uint32_t open_no = 0;
    std::size_t rows = 0;

    // The sink gets every row of every fault file, oldest sample first.
    auto to_csv = [&](std::uint32_t file_no, double t, const Sample& s) {
        if (file_no != open_no) {                         // first row of a new file
            if (file) std::fclose(file);
            char name[64];
            std::snprintf(name, sizeof name, "fault_capture_%u.csv", file_no);
            file = std::fopen(name, "w");
            open_no = file_no;
            if (file) std::fputs("time_s,ch0,ch1,ch2,ch3,fault\n", file);
        }
        if (!file) return;
        std::fprintf(file, "%.6f,%.3f,%.3f,%.3f,%.3f,%u\n", t, static_cast<double>(s.ch[0]),
                     static_cast<double>(s.ch[1]), static_cast<double>(s.ch[2]),
                     static_cast<double>(s.ch[3]), s.fault_count);
        ++rows;
    };

    for (long n = 0; n < 20 * 8; ++n) {
        const double t = static_cast<double>(n) / kRate;
        if (n == 80) pool.raise_fault(t);                 // the fault, at t = 10 s
        pool.push(t, Sample{{static_cast<float>(n), 0, 0, 0}, pool.fault_count()});
        pool.service(t, to_csv);                          // writes the file at t = 13 s
    }
    if (file) std::fclose(file);

    std::printf("%zu slots x %zu rings, %zu rows written, %llu dropped, fault count %u\n",
                kSize.capacity, kSize.rings, rows,
                static_cast<unsigned long long>(pool.dropped()), pool.fault_count());
    return 0;
}
