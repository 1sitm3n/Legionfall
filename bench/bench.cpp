// Scheduler benchmark - old mutex pool vs the lock-free work-stealing one.
//
// Three workloads, because they stress different things:
//
//   uniform   every job costs the same. Best case for the old scheduler, since
//             static partitioning is already balanced. Measures raw dispatch
//             overhead and nothing else.
//
//   skewed    the expensive items are CLUSTERED at the front, not sprinkled
//             evenly. That matters - sprinkle them and static splitting is
//             accidentally balanced, so you measure nothing. Clustering is also
//             the realistic case: enemies bunched near the hero are the ones
//             doing the expensive chase maths. Static splitting hands one core
//             the whole cluster and everyone else waits at the barrier.
//
//   tiny      hundreds of thousands of near-empty jobs. Pure per-job cost -
//             mutex acquire + heap alloc for std::function vs a deque push.
//
// On a P/E core machine the skewed numbers are the interesting ones. Equal item
// counts per core means the barrier waits for the E-cores every single time.

#include "core/JobSystem.h"
#include "core/MutexJobSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace Legionfall;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// Something the optimiser can't delete. Roughly what the enemy update does.
inline double burn(int iterations, double seed) noexcept {
    double acc = seed;
    for (int k = 0; k < iterations; ++k) {
        acc += std::sin(acc * 0.5 + k) * std::cos(acc * 0.25);
        acc  = std::tanh(acc);
    }
    return acc;
}

struct Result {
    double medianMs;
    double minMs;
    double p95Ms;
};

Result summarise(std::vector<double>& samples) {
    std::sort(samples.begin(), samples.end());
    const std::size_t n = samples.size();
    return { samples[n / 2], samples.front(), samples[(n * 95) / 100] };
}

// Report the median, not the mean. One scheduling hiccup skews a mean and tells
// you nothing about the steady state.
template <typename SetupFn>
Result timeIt(int warmup, int runs, SetupFn&& body) {
    for (int i = 0; i < warmup; ++i) body();
    std::vector<double> samples;
    samples.reserve(runs);
    for (int i = 0; i < runs; ++i) {
        const auto t0 = Clock::now();
        body();
        samples.push_back(msSince(t0));
    }
    return summarise(samples);
}

void bar(double value, double maxValue, int width) {
    const int filled = (maxValue > 0.0)
        ? (int)std::lround((value / maxValue) * width) : 0;
    for (int i = 0; i < width; ++i) std::fputs(i < filled ? "█" : "·", stdout);
}

void header(const char* title) {
    std::printf("\n%s\n", title);
    for (std::size_t i = 0; i < std::string(title).size(); ++i) std::fputc('-', stdout);
    std::fputc('\n', stdout);
}

void row(const char* name, Result r, double baselineMedian) {
    const double speedup = baselineMedian / r.medianMs;
    std::printf("  %-22s %8.2f %8.2f %8.2f   ", name, r.medianMs, r.minMs, r.p95Ms);
    if (baselineMedian > 0.0) std::printf("%5.2fx", speedup);
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv) {
    std::size_t workers = 0;
    if (argc > 1) workers = (std::size_t)std::atoi(argv[1]);
    if (workers == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        workers = (hw > 1) ? hw - 1 : 1;
    }

    std::printf("Legionfall scheduler benchmark\n");
    std::printf("  hardware_concurrency : %u\n", std::thread::hardware_concurrency());
    std::printf("  workers              : %zu (+1 submitting thread)\n", workers);
    std::printf("  cache line assumed   : %zu bytes\n", kCacheLine);
    std::printf("\n  median / min / p95 in ms, lower is better. Speedup vs mutex pool.\n");

    // ---------------------------------------------------------------- uniform
    {
        constexpr int kJobs = 512;
        constexpr int kWork = 400;

        header("uniform  -  512 jobs, identical cost");
        std::printf("  %-22s %8s %8s %8s   %6s\n", "", "median", "min", "p95", "vs old");

        MutexJobSystem oldJs(workers);
        Result oldR = timeIt(3, 15, [&] {
            for (int i = 0; i < kJobs; ++i)
                oldJs.schedule([i] { volatile double s = burn(kWork, i); (void)s; });
            oldJs.wait();
        });
        row("mutex + global queue", oldR, oldR.medianMs);

        JobSystem newJs(workers);
        Result newR = timeIt(3, 15, [&] {
            for (int i = 0; i < kJobs; ++i)
                newJs.schedule([i]() noexcept { volatile double s = burn(kWork, i); (void)s; });
            newJs.wait();
        });
        row("lock-free stealing", newR, oldR.medianMs);
    }

    // ----------------------------------------------------------------- skewed
    {
        constexpr int kJobs  = 512;
        constexpr int kLight = 100;
        constexpr int kHeavy = 2000;

        header("skewed  -  512 items, first 12% cost 20x (clustered)");
        std::printf("  %-22s %8s %8s %8s   %6s\n", "", "median", "min", "p95", "vs old");

        // Clustered, not sprinkled. See the note at the top of the file.
        auto cost = [](int i) { return (i < 64) ? kHeavy : kLight; };

        // The way the game does it now - one job per thread, equal item counts.
        MutexJobSystem oldJs(workers);
        Result oldR = timeIt(3, 15, [&] {
            const int perChunk = kJobs / (int)workers;
            for (std::size_t t = 0; t < workers; ++t) {
                const int b = (int)t * perChunk;
                const int e = (t + 1 == workers) ? kJobs : b + perChunk;
                oldJs.schedule([b, e, cost] {
                    volatile double s = 0;
                    for (int i = b; i < e; ++i) s = burn(cost(i), i);
                    (void)s;
                });
            }
            oldJs.wait();
        });
        row("static split (old)", oldR, oldR.medianMs);

        // Same total work, but chopped fine enough that stealing can rebalance.
        JobSystem newJs(workers);
        Result newR = timeIt(3, 15, [&] {
            newJs.parallelFor(0, kJobs, 8, [cost](std::size_t b, std::size_t e) noexcept {
                volatile double s = 0;
                for (std::size_t i = b; i < e; ++i) s = burn(cost((int)i), (double)i);
                (void)s;
            });
            newJs.wait();
        });
        row("parallelFor + stealing", newR, oldR.medianMs);

        // Where did the work actually land? On a P/E machine the spread across
        // workers is the whole story - even counts would mean no rebalancing.
        newJs.resetStats();
        newJs.parallelFor(0, kJobs, 8, [cost](std::size_t b, std::size_t e) noexcept {
            volatile double s = 0;
            for (std::size_t i = b; i < e; ++i) s = burn(cost((int)i), (double)i);
            (void)s;
        });
        newJs.wait();

        std::printf("\n  jobs executed per worker (one pass, %d chunks of 8 items).\n",
                    kJobs / 8);
        std::printf("  Uneven counts here are the point - whoever drew the heavy\n");
        std::printf("  chunks runs fewer of them, and the rest steal the remainder.\n\n");
        std::uint64_t peak = newJs.stats().mainExecuted;
        for (std::size_t i = 0; i < newJs.workerStatsCount(); ++i)
            peak = std::max(peak, newJs.worker(i).executed);

        for (std::size_t i = 0; i < newJs.workerStatsCount(); ++i) {
            const auto w = newJs.worker(i);
            std::printf("    worker %-2zu ", i);
            bar((double)w.executed, (double)peak, 32);
            std::printf(" %4llu   (stole %llu)\n",
                        (unsigned long long)w.executed,
                        (unsigned long long)w.stolen);
        }
        std::printf("    submitter ");
        bar((double)newJs.stats().mainExecuted, (double)peak, 32);
        std::printf(" %4llu   (via wait())\n",
                    (unsigned long long)newJs.stats().mainExecuted);
    }

    // ------------------------------------------------------------------- tiny
    {
        constexpr int kJobs = 200000;

        header("tiny  -  200k near-empty jobs, pure dispatch cost");
        std::printf("  %-22s %8s %8s %8s   %6s\n", "", "median", "min", "p95", "vs old");

        std::atomic<long long> sink{0};

        MutexJobSystem oldJs(workers);
        Result oldR = timeIt(1, 5, [&] {
            for (int i = 0; i < kJobs; ++i)
                oldJs.schedule([&sink] { sink.fetch_add(1, std::memory_order_relaxed); });
            oldJs.wait();
        });
        row("mutex + global queue", oldR, oldR.medianMs);

        JobSystem newJs(workers);
        Result newR = timeIt(1, 5, [&] {
            for (int i = 0; i < kJobs; ++i)
                newJs.schedule([&sink]() noexcept { sink.fetch_add(1, std::memory_order_relaxed); });
            newJs.wait();
        });
        row("lock-free stealing", newR, oldR.medianMs);

        const auto s = newJs.stats();
        std::printf("\n  steal attempts %llu, successful %llu, CAS aborts %llu (%.2f%%), parks %llu\n",
                    (unsigned long long)s.stealAttempts,
                    (unsigned long long)s.stolen,
                    (unsigned long long)s.stealAborts,
                    s.stealAttempts ? 100.0 * (double)s.stealAborts / (double)s.stealAttempts : 0.0,
                    (unsigned long long)s.parked);
        std::printf("  jobs run by the submitting thread in wait(): %llu\n",
                    (unsigned long long)s.mainExecuted);
    }

    // ---------------------------------------------------------------- scaling
    {
        header("scaling  -  skewed workload, worker count swept");
        std::printf("  %8s %10s %8s\n", "workers", "median ms", "speedup");

        constexpr int kJobs  = 512;
        auto cost = [](int i) { return (i < 64) ? 2000 : 100; };

        double oneThread = 0.0;
        const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
        for (std::size_t w = 1; w <= hw; w *= 2) {
            JobSystem js(w);
            Result r = timeIt(2, 9, [&] {
                js.parallelFor(0, kJobs, 8, [cost](std::size_t b, std::size_t e) noexcept {
                    volatile double s = 0;
                    for (std::size_t i = b; i < e; ++i) s = burn(cost((int)i), (double)i);
                    (void)s;
                });
                js.wait();
            });
            if (w == 1) oneThread = r.medianMs;
            std::printf("  %8zu %10.2f %7.2fx\n", w, r.medianMs, oneThread / r.medianMs);
        }
        std::printf("\n  Note: speedup is vs 1 worker + the submitting thread, so it is\n");
        std::printf("  already 2 cores at the left edge. Expect it to flatten once the\n");
        std::printf("  E-cores join - they are slower, so the last few add less.\n");
    }

    std::printf("\n");
    return 0;
}
