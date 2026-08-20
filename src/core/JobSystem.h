#pragma once
// Lock-free work-stealing job system.
//
// How it works:
//   Every worker owns a Chase-Lev deque. It pushes and pops its own bottom with
//   no atomic RMW in the normal case. Idle workers steal off the top of a
//   random victim with one CAS. The submitting thread gets a deque too, so
//   schedule() never touches anything shared.
//
//   Jobs hold their closure inline. Nothing allocates after construction.
//
//   wait() helps instead of blocking - it runs jobs until the batch drains.
//   That gets a core back at every barrier, and it kills the lost-wakeup bug
//   the old version had by removing the condition variable from that path.
//
// What's actually lock-free: push, pop, steal and wait. No thread can block
// another's progress there, and a thread descheduled mid-op can't stall anyone.
//
// Parking is not - it uses a mutex and condvar, and only when a worker has
// already failed to find work and is going to sleep. Lock-free parking needs
// futex-level primitives, and the alternative is spinning forever, which burns
// battery for nothing on a workload that's idle by definition. The lock never
// gets taken when there's work to do.
//
// schedule() must be called from the thread that made the JobSystem or from
// inside a running job - both own a deque. Anything else asserts. Supporting
// arbitrary submitters needs an MPMC injector queue and this doesn't need one.

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/ChaseLevDeque.h"
#include "core/Job.h"

namespace Legionfall {

// Park/wake without losing wakeups.
//
// Order matters:
//   1. token = prepareWait()   - read the epoch BEFORE scanning
//   2. scan every deque
//   3. found something -> run it. found nothing -> commitWait(token)
//
// If a producer publishes between 2 and 3 it bumps the epoch, so commitWait
// sees token != epoch under the lock and doesn't sleep. If it publishes after
// we've taken the lock it blocks on that same lock until cv.wait() drops it, so
// the notify can't be missed. Both windows covered.
class EventCount {
public:
    std::uint64_t prepareWait() const noexcept {
        return m_epoch.load(std::memory_order_acquire);
    }

    void commitWait(std::uint64_t token) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_epoch.load(std::memory_order_acquire) != token) return;
        // Timeout is a safety net, not part of the argument above. If this ever
        // only makes progress when the 2ms fires, the protocol is broken and
        // that's a bug to fix, not to paper over.
        m_cv.wait_for(lock, std::chrono::milliseconds(2));
    }

    void notifyAll() {
        m_epoch.fetch_add(1, std::memory_order_release);
        { std::lock_guard<std::mutex> lock(m_mutex); }   // pairs with commitWait
        m_cv.notify_all();
    }

private:
    mutable std::atomic<std::uint64_t> m_epoch{0};
    std::mutex                         m_mutex;
    std::condition_variable            m_cv;
};

// Padded - these get written on every job and would false-share otherwise,
// which is the exact thing this scheduler exists to avoid.
//
// Atomic because stats() reads them from the submitting thread while workers
// are still spinning, and an idle worker still bumps stealAttempts every time
// it scans. TSan caught that as a real race on plain ints. Relaxed is enough:
// exactly one thread writes each counter, readers only want a rough number, and
// nothing branches on them.
struct alignas(kCacheLine) WorkerStats {
    std::atomic<std::uint64_t> executed{0};
    std::atomic<std::uint64_t> stolen{0};
    std::atomic<std::uint64_t> stealAttempts{0};
    std::atomic<std::uint64_t> stealAborts{0};   // lost the CAS to another thief
    std::atomic<std::uint64_t> parked{0};
    std::byte pad[kCacheLine - 5 * sizeof(std::atomic<std::uint64_t>)]{};
};

// Single writer per counter, so this is a relaxed load and store rather than an
// atomic RMW. No lock prefix / no ldaxr-stlxr loop, just ordinary instructions
// that TSan understands.
inline void bump(std::atomic<std::uint64_t>& c) noexcept {
    c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

// Plain values, safe to print.
struct WorkerSnapshot {
    std::uint64_t executed      = 0;
    std::uint64_t stolen        = 0;
    std::uint64_t stealAttempts = 0;
    std::uint64_t stealAborts   = 0;
    std::uint64_t parked        = 0;
};

class JobSystem {
public:
    // 0 means hardware_concurrency() - 1, leaving a core for the submitter,
    // which pulls its weight through wait().
    explicit JobSystem(std::size_t workerCount = 0);
    ~JobSystem();

    JobSystem(const JobSystem&)            = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    template <typename Fn>
    void schedule(Fn&& fn) {
        const std::size_t q = queueIndex();
        // Not just an assert - Release builds define NDEBUG, and falling
        // through would index m_deques[(size_t)-1]. Fail loudly instead.
        if (q == kUnregistered) unregisteredCaller("schedule");

        m_pending.value.fetch_add(1, std::memory_order_relaxed);
        Job* job = makeJob(*m_pools[q], &m_pending.value, std::forward<Fn>(fn));
        m_deques[q]->push(job);
        m_events.notifyAll();
    }

    // Chop [begin,end) into chunks of grain. Use this instead of splitting into
    // one job per thread. Fixed splitting gives every core the same item count,
    // so on a P/E core CPU or with uneven per-item cost the barrier always waits
    // on the slowest one. Small chunks let the stealer rebalance on its own.
    template <typename Fn>
    void parallelFor(std::size_t begin, std::size_t end, std::size_t grain, Fn fn) {
        if (begin >= end) return;
        if (grain == 0) grain = 1;
        for (std::size_t s = begin; s < end; s += grain) {
            const std::size_t e = (s + grain < end) ? s + grain : end;
            schedule([fn, s, e]() noexcept { fn(s, e); });
        }
    }

    // Drains the batch by helping, not idling.
    void wait();

    std::size_t threadCount() const noexcept { return m_workers.size(); }

    struct Stats {
        std::uint64_t executed      = 0;
        std::uint64_t stolen        = 0;
        std::uint64_t stealAttempts = 0;
        std::uint64_t stealAborts   = 0;
        std::uint64_t parked        = 0;
        std::uint64_t mainExecuted  = 0;   // run by the waiting thread
    };
    Stats          stats() const;
    WorkerSnapshot worker(std::size_t i) const;
    std::size_t    workerStatsCount() const noexcept { return m_statsCount; }
    void           resetStats();

private:
    static constexpr std::size_t kUnregistered = static_cast<std::size_t>(-1);

    void        workerLoop(std::size_t index);
    bool        tryGetJob(std::size_t self, Job*& out);
    std::size_t queueIndex() const noexcept;
    void        helpOnce(std::size_t self);          // run one job, or back off
    [[noreturn]] static void unregisteredCaller(const char* fn);

    // Every worker hits this on every completion, so give it its own line.
    struct alignas(kCacheLine) PaddedCounter {
        std::atomic<std::int64_t> value{0};
        std::byte pad[kCacheLine - sizeof(std::atomic<std::int64_t>)]{};
    };

    std::vector<std::unique_ptr<ChaseLevDeque<Job*>>> m_deques;   // workers + submitter
    std::vector<std::unique_ptr<JobPool>>             m_pools;
    std::vector<std::thread>                          m_workers;

    // unique_ptr array, not a vector - std::atomic isn't movable so vector
    // can't resize it.
    std::unique_ptr<WorkerStats[]> m_stats;
    std::size_t                    m_statsCount = 0;

    PaddedCounter     m_pending;
    EventCount        m_events;
    std::atomic<bool> m_stop{false};
    std::size_t       m_submitterQueue = 0;
    std::atomic<std::uint64_t> m_mainExecuted{0};
};

} // namespace Legionfall
