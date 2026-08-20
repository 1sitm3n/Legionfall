// Scheduler tests. No external framework - not worth a submodule for this.
//
//   ./lf_tests                              run everything
//   ./lf_tests deque_steal_no_duplication   run one
//
// CTest registers each one separately so a failure tells you which.

#include "core/ChaseLevDeque.h"
#include "core/JobSystem.h"
#include "core/MutexJobSystem.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace Legionfall;

// Tiny harness.
namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("    FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        const auto _a = (a);                                                   \
        const auto _b = (b);                                                   \
        if (!(_a == _b)) {                                                     \
            std::printf("    FAIL %s:%d  %s == %s  (%lld vs %lld)\n",          \
                        __FILE__, __LINE__, #a, #b,                            \
                        (long long)_a, (long long)_b);                         \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

std::map<std::string, std::function<void()>>& registry() {
    static std::map<std::string, std::function<void()>> r;
    return r;
}

struct Register {
    Register(const char* name, std::function<void()> fn) {
        registry()[name] = std::move(fn);
    }
};

#define TEST(name)                                                             \
    static void name();                                                        \
    static Register reg_##name(#name, name);                                   \
    static void name()

} // namespace

// Single-threaded invariants first.

TEST(deque_push_pop_is_lifo) {
    ChaseLevDeque<int*> d(8);
    int values[5] = {0, 1, 2, 3, 4};
    for (int i = 0; i < 5; ++i) d.push(&values[i]);

    // Owner pops its own end, so LIFO.
    for (int i = 4; i >= 0; --i) {
        int* out = nullptr;
        CHECK(d.pop(out));
        CHECK_EQ(*out, i);
    }
    int* out = nullptr;
    CHECK(!d.pop(out));
}

TEST(deque_steal_is_fifo) {
    ChaseLevDeque<int*> d(8);
    int values[4] = {10, 11, 12, 13};
    for (int i = 0; i < 4; ++i) d.push(&values[i]);

    // Thieves take the other end, so FIFO.
    for (int i = 0; i < 4; ++i) {
        auto r = d.steal();
        CHECK(r.status == StealStatus::Success);
        if (r.status == StealStatus::Success) CHECK_EQ(*r.value, 10 + i);
    }
    CHECK(d.steal().status == StealStatus::Empty);
}

TEST(deque_grows_beyond_initial_capacity) {
    ChaseLevDeque<std::size_t*> d(2);   // tiny on purpose so it has to grow
    constexpr std::size_t N = 10000;
    std::vector<std::size_t> vals(N);
    for (std::size_t i = 0; i < N; ++i) { vals[i] = i; d.push(&vals[i]); }

    CHECK(d.capacity() >= (std::int64_t)N);
    CHECK_EQ(d.sizeApprox(), (std::int64_t)N);

    for (std::size_t i = N; i-- > 0;) {
        std::size_t* out = nullptr;
        CHECK(d.pop(out));
        if (out) CHECK_EQ(*out, i);
    }
}

TEST(deque_empty_pop_and_steal) {
    ChaseLevDeque<int*> d(4);
    int* out = nullptr;
    CHECK(!d.pop(out));
    CHECK(d.steal().status == StealStatus::Empty);

    int v = 7;
    d.push(&v);
    CHECK(d.pop(out));
    CHECK_EQ(*out, 7);
    CHECK(!d.pop(out));
    CHECK(d.steal().status == StealStatus::Empty);
}

// The one that matters.
//
// Owner pushing and popping while 4 thieves steal. Every element has to be
// claimed exactly once - never dropped, never given to two threads. That's what
// the fences are there for, and this is the test that starts failing if you
// delete the seq_cst fence in pop() and run it on ARM.

TEST(deque_steal_no_duplication) {
    constexpr std::size_t kItems   = 200000;
    constexpr int         kThieves = 4;
    constexpr int         kRounds  = 5;

    for (int round = 0; round < kRounds; ++round) {
        ChaseLevDeque<std::size_t*> d(64);
        std::vector<std::size_t>    values(kItems);
        std::vector<std::atomic<int>> claimed(kItems);
        for (std::size_t i = 0; i < kItems; ++i) {
            values[i] = i;
            claimed[i].store(0, std::memory_order_relaxed);
        }

        std::atomic<bool>          go{false};
        std::atomic<std::size_t>   totalClaimed{0};
        std::atomic<bool>          producerDone{false};

        auto claim = [&](std::size_t* p) {
            const int prev = claimed[*p].fetch_add(1, std::memory_order_relaxed);
            CHECK_EQ(prev, 0);            // 1 here means it got handed out twice
            totalClaimed.fetch_add(1, std::memory_order_relaxed);
        };

        std::vector<std::thread> thieves;
        for (int t = 0; t < kThieves; ++t) {
            thieves.emplace_back([&] {
                while (!go.load(std::memory_order_acquire)) {}
                for (;;) {
                    auto r = d.steal();
                    if (r.status == StealStatus::Success) {
                        claim(r.value);
                    } else if (producerDone.load(std::memory_order_acquire) &&
                               d.sizeApprox() == 0) {
                        // Might just be a drain race - one more go before quitting.
                        auto r2 = d.steal();
                        if (r2.status == StealStatus::Success) { claim(r2.value); continue; }
                        if (r2.status == StealStatus::Empty) break;
                    }
                }
            });
        }

        go.store(true, std::memory_order_release);

        // Interleaving pushes and pops is the interesting bit - it hits the
        // single-element race between pop() and steal().
        for (std::size_t i = 0; i < kItems; ++i) {
            d.push(&values[i]);
            if ((i & 3) == 0) {
                std::size_t* out = nullptr;
                if (d.pop(out)) claim(out);
            }
        }
        producerDone.store(true, std::memory_order_release);

        // Owner takes whatever's left.
        for (;;) {
            std::size_t* out = nullptr;
            if (!d.pop(out)) break;
            claim(out);
        }

        for (auto& t : thieves) t.join();

        // Exactly once each.
        CHECK_EQ(totalClaimed.load(), kItems);
        std::size_t unclaimed = 0, doubled = 0;
        for (std::size_t i = 0; i < kItems; ++i) {
            const int c = claimed[i].load(std::memory_order_relaxed);
            if (c == 0) ++unclaimed;
            if (c > 1)  ++doubled;
        }
        CHECK_EQ(unclaimed, 0u);
        CHECK_EQ(doubled, 0u);
    }
}

// JobSystem.

TEST(jobsystem_runs_every_job_exactly_once) {
    JobSystem js(4);
    constexpr int N = 20000;
    std::vector<std::atomic<int>> hits(N);
    for (auto& h : hits) h.store(0, std::memory_order_relaxed);

    auto* hp = hits.data();
    for (int i = 0; i < N; ++i) {
        js.schedule([hp, i]() noexcept {
            hp[i].fetch_add(1, std::memory_order_relaxed);
        });
    }
    js.wait();

    int wrong = 0;
    for (int i = 0; i < N; ++i) {
        if (hits[i].load(std::memory_order_relaxed) != 1) ++wrong;
    }
    CHECK_EQ(wrong, 0);
    CHECK_EQ(js.stats().executed, (std::uint64_t)N);
}

TEST(jobsystem_wait_is_a_real_barrier) {
    // Repeated fork/join. Early return from wait() fails the count, a missed
    // wakeup hangs and CTest times it out.
    JobSystem js(4);
    std::atomic<long long> counter{0};

    for (int round = 0; round < 500; ++round) {
        constexpr int kJobs = 64;
        for (int i = 0; i < kJobs; ++i) {
            js.schedule([&counter]() noexcept {
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }
        js.wait();
        CHECK_EQ(counter.load(), (long long)(kJobs * (round + 1)));
    }
}

TEST(jobsystem_handles_uneven_job_costs) {
    // The case static partitioning gets wrong - one job costs way more than the
    // rest. Stealing should still finish and every job still has to run.
    JobSystem js(4);
    std::atomic<int> done{0};

    for (int i = 0; i < 200; ++i) {
        js.schedule([&done, i]() noexcept {
            volatile double sink = 0.0;
            const int iterations = (i == 0) ? 2000000 : 100;
            for (int k = 0; k < iterations; ++k) sink += k * 0.5;
            (void)sink;
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }
    js.wait();
    CHECK_EQ(done.load(), 200);
}

TEST(jobsystem_nested_scheduling) {
    // A job scheduling more jobs pushes onto its own worker deque. Covers the
    // thread-local index path and the recursive drain.
    JobSystem js(4);
    std::atomic<int> leaves{0};

    for (int i = 0; i < 32; ++i) {
        js.schedule([&js, &leaves]() noexcept {
            for (int k = 0; k < 8; ++k) {
                js.schedule([&leaves]() noexcept {
                    leaves.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }
    js.wait();
    CHECK_EQ(leaves.load(), 32 * 8);
}

TEST(jobsystem_parallel_for_covers_range_exactly) {
    JobSystem js(4);
    constexpr std::size_t N = 100000;
    std::vector<std::atomic<int>> hits(N);
    for (auto& h : hits) h.store(0, std::memory_order_relaxed);
    auto* hp = hits.data();

    js.parallelFor(0, N, 256, [hp](std::size_t b, std::size_t e) noexcept {
        for (std::size_t i = b; i < e; ++i) {
            hp[i].fetch_add(1, std::memory_order_relaxed);
        }
    });
    js.wait();

    std::size_t wrong = 0;
    for (std::size_t i = 0; i < N; ++i) {
        if (hits[i].load(std::memory_order_relaxed) != 1) ++wrong;
    }
    CHECK_EQ(wrong, 0u);
}

TEST(jobsystem_parallel_for_edge_cases) {
    JobSystem js(2);
    std::atomic<int> n{0};

    js.parallelFor(0, 0, 16, [&](std::size_t, std::size_t) noexcept {
        n.fetch_add(1, std::memory_order_relaxed);
    });
    js.wait();
    CHECK_EQ(n.load(), 0);          // empty range does nothing

    js.parallelFor(0, 1, 64, [&](std::size_t b, std::size_t e) noexcept {
        CHECK_EQ(b, 0u); CHECK_EQ(e, 1u);
        n.fetch_add(1, std::memory_order_relaxed);
    });
    js.wait();
    CHECK_EQ(n.load(), 1);          // grain > range is one chunk

    n.store(0);
    js.parallelFor(0, 10, 0, [&](std::size_t, std::size_t) noexcept {
        n.fetch_add(1, std::memory_order_relaxed);
    });
    js.wait();
    CHECK_EQ(n.load(), 10);         // grain 0 clamps to 1 instead of hanging
}

TEST(jobsystem_single_worker_still_works) {
    // One worker plus the submitter. Barely anything to steal, so the help path
    // does most of it.
    JobSystem js(1);
    std::atomic<int> n{0};
    for (int i = 0; i < 1000; ++i) {
        js.schedule([&n]() noexcept { n.fetch_add(1, std::memory_order_relaxed); });
    }
    js.wait();
    CHECK_EQ(n.load(), 1000);
}

TEST(jobsystem_submitter_actually_helps) {
    // wait() should be running jobs, not sitting there.
    //
    // First version of this just queued a big batch and checked mainExecuted >
    // 0. That's timing-dependent - under TSan the workers drained the queue
    // before the main thread even reached wait(), so it went red on a build
    // that was completely fine. Flaky test, not a flaky scheduler.
    //
    // This version makes it structural instead. Schedule one more job than
    // there are workers, and have every job wait until all of them are running
    // at once. With only N workers the last job can't start unless the
    // submitting thread picks it up, so mainExecuted has to be > 0.
    JobSystem js(3);
    const std::size_t target = js.threadCount() + 1;

    std::atomic<std::size_t> arrived{0};

    for (std::size_t i = 0; i < target; ++i) {
        js.schedule([&arrived, target]() noexcept {
            arrived.fetch_add(1, std::memory_order_acq_rel);
            // Bounded, so a regression fails the check instead of hanging CI.
            const auto deadline = std::chrono::steady_clock::now()
                                + std::chrono::seconds(10);
            while (arrived.load(std::memory_order_acquire) < target &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
        });
    }
    js.wait();

    CHECK_EQ(arrived.load(), target);
    CHECK(js.stats().mainExecuted > 0);
}

TEST(jobsystem_ring_wraps_safely) {
    // Schedule far more jobs than the pool starts with, every one gated so they
    // pile up in the deques instead of draining. The original fixed 8192-slot
    // ring silently reused live slots here - 808 jobs never ran and 808 ran
    // twice. Gating matters: an ungated version drains fast enough to hide it.
    const int N = 9000;
    JobSystem js(3);

    std::vector<std::atomic<int>> hits(N);
    for (auto& h : hits) h.store(0, std::memory_order_relaxed);
    auto* hp = hits.data();

    std::atomic<bool> go{false};
    std::thread opener([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        go.store(true, std::memory_order_release);
    });

    for (int i = 0; i < N; ++i) {
        js.schedule([hp, i, &go]() noexcept {
            // Bounded, so a regression fails rather than hanging CI.
            const auto deadline = std::chrono::steady_clock::now()
                                + std::chrono::seconds(20);
            while (!go.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {}
            hp[i].fetch_add(1, std::memory_order_relaxed);
        });
    }
    js.wait();
    opener.join();

    int zero = 0, dup = 0;
    for (int i = 0; i < N; ++i) {
        const int c = hits[i].load(std::memory_order_relaxed);
        if (c == 0) ++zero;
        if (c > 1)  ++dup;
    }
    CHECK_EQ(zero, 0);
    CHECK_EQ(dup, 0);
    CHECK_EQ(js.stats().executed, (std::uint64_t)N);
}

TEST(jobsystem_stealing_actually_happens) {
    // Everything is pushed by the submitter onto one deque, so any job a worker
    // runs must have been stolen. Zero here means stealing isn't working.
    //
    // Has to be gated. The first version just queued 20000 tiny jobs, but the
    // submitting thread pops its own deque in wait() and can finish the whole
    // batch before the workers even wake from a park - so it went red on a
    // perfectly healthy build. Gating forces every job to be in flight at once,
    // which means workers must participate.
    JobSystem js(4);
    const std::size_t target = js.threadCount() + 1;
    std::atomic<std::size_t> arrived{0};
    std::atomic<int> ran{0};

    for (std::size_t i = 0; i < target; ++i) {
        js.schedule([&arrived, &ran, target]() noexcept {
            arrived.fetch_add(1, std::memory_order_acq_rel);
            const auto deadline = std::chrono::steady_clock::now()
                                + std::chrono::seconds(10);
            while (arrived.load(std::memory_order_acquire) < target &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            ran.fetch_add(1, std::memory_order_relaxed);
        });
    }
    js.wait();

    CHECK_EQ(ran.load(), (int)target);
    CHECK(js.stats().stolen > 0);
}

// Baseline held to the same standard, otherwise the comparison isn't fair.

TEST(mutex_jobsystem_baseline_is_correct) {
    MutexJobSystem js(4);
    std::atomic<int> n{0};
    for (int round = 0; round < 200; ++round) {
        for (int i = 0; i < 32; ++i) {
            js.schedule([&n] { n.fetch_add(1, std::memory_order_relaxed); });
        }
        js.wait();
        CHECK_EQ(n.load(), 32 * (round + 1));
    }
}

//==============================================================================

int main(int argc, char** argv) {
    auto& tests = registry();

    if (argc > 1) {
        auto it = tests.find(argv[1]);
        if (it == tests.end()) {
            std::printf("unknown test: %s\n", argv[1]);
            return 2;
        }
        std::printf("[ RUN ] %s\n", argv[1]);
        const auto t0 = std::chrono::steady_clock::now();
        it->second();
        const auto ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count();
        std::printf("[ %s ] %s (%.1f ms)\n", g_failures ? "FAIL" : "  OK", argv[1], ms);
        return g_failures ? 1 : 0;
    }

    for (auto& [name, fn] : tests) {
        const int before = g_failures;
        std::printf("[ RUN ] %s\n", name.c_str());
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        const auto ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count();
        std::printf("[ %s ] %s (%.1f ms)\n",
                    g_failures > before ? "FAIL" : "  OK", name.c_str(), ms);
    }

    std::printf("\n%d test(s), %d failure(s)\n", (int)tests.size(), g_failures);
    return g_failures ? 1 : 0;
}
