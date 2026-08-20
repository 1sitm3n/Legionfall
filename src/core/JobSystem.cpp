#include "core/JobSystem.h"

#include <algorithm>
#include <cstdio>

#if defined(__APPLE__)
#  include <pthread.h>
#endif

namespace Legionfall {

namespace {

// Which deque does the calling thread own. Set on worker entry and for the
// submitting thread in the ctor.
thread_local std::size_t      tlsQueueIndex = static_cast<std::size_t>(-1);
thread_local const JobSystem* tlsOwner      = nullptr;

// xorshift64*, seeded per thread. Victims have to be picked at random - if
// every idle worker walks the list in the same order they all pile onto the
// same deque, fight over its top cache line and lose most of the CAS races.
thread_local std::uint64_t tlsRng = 0;

inline std::uint64_t nextRandom() noexcept {
    std::uint64_t x = tlsRng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    tlsRng = x;
    return x * 0x2545F4914F6CDD1DULL;
}

// Spin first, then yield, then park. The next job usually shows up within a few
// hundred ns and a park/unpark round trip is microseconds, so it's worth
// burning a bit of time before giving up the core.
inline void spinHint(int iteration) noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
    // ISB is the usual spin hint on AArch64. YIELD is a nop on a lot of parts
    // and WFE needs a paired event to wake it.
    for (int i = 0; i < (1 << std::min(iteration, 6)); ++i)
        asm volatile("isb" ::: "memory");
#elif defined(__x86_64__) || defined(_M_X64)
    for (int i = 0; i < (1 << std::min(iteration, 6)); ++i)
        asm volatile("pause" ::: "memory");
#else
    (void)iteration;
    std::this_thread::yield();
#endif
}

} // namespace

JobSystem::JobSystem(std::size_t workerCount) {
    if (workerCount == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        // hardware_concurrency() is allowed to return 0. The old code did
        // hw - 1 on an unsigned, which wraps to 4294967295, and only a later
        // min() stopped it being a disaster. Handle it properly.
        workerCount = (hw > 1) ? static_cast<std::size_t>(hw - 1) : 1;
    }

    const std::size_t queues = workerCount + 1;   // +1 for the submitter
    m_submitterQueue = workerCount;

    m_deques.reserve(queues);
    m_pools.reserve(queues);
    for (std::size_t i = 0; i < queues; ++i) {
        m_deques.emplace_back(std::make_unique<ChaseLevDeque<Job*>>(1024));
        m_pools.emplace_back(std::make_unique<JobPool>());
    }
    m_stats      = std::make_unique<WorkerStats[]>(workerCount);
    m_statsCount = workerCount;

    // Whoever constructs it is the submitter.
    tlsQueueIndex = m_submitterQueue;
    tlsOwner      = this;
    tlsRng        = 0x9E3779B97F4A7C15ULL;

    m_workers.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i)
        m_workers.emplace_back(&JobSystem::workerLoop, this, i);
}

JobSystem::~JobSystem() {
    m_stop.store(true, std::memory_order_release);
    m_events.notifyAll();
    for (auto& t : m_workers) if (t.joinable()) t.join();

    if (tlsOwner == this) {
        tlsQueueIndex = kUnregistered;
        tlsOwner      = nullptr;
    }
}

std::size_t JobSystem::queueIndex() const noexcept {
    return (tlsOwner == this) ? tlsQueueIndex : kUnregistered;
}

// Own deque first (LIFO, cache-hot), then steal from a random victim.
// False only when everything looked empty.
bool JobSystem::tryGetJob(std::size_t self, Job*& out) {
    if (m_deques[self]->pop(out)) return true;

    const std::size_t n = m_deques.size();
    if (n <= 1) return false;

    WorkerStats* st = (self < m_statsCount) ? &m_stats[self] : nullptr;

    // One sweep from a random offset. Cheaper than re-rolling per victim and
    // still enough to stop everyone converging on the same deque.
    const std::size_t start = static_cast<std::size_t>(nextRandom() % n);
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t victim = (start + k) % n;
        if (victim == self) continue;

        if (st) bump(st->stealAttempts);
        StealResult<Job*> r = m_deques[victim]->steal();

        if (r.status == StealStatus::Success) {
            out = r.value;
            if (st) bump(st->stolen);
            return true;
        }
        if (r.status == StealStatus::Abort) {
            // Lost the CAS, so there WAS work here. Don't count that as empty -
            // retry this victim once before moving on.
            if (st) bump(st->stealAborts);
            r = m_deques[victim]->steal();
            if (r.status == StealStatus::Success) {
                out = r.value;
                if (st) bump(st->stolen);
                return true;
            }
        }
    }
    return false;
}

void JobSystem::unregisteredCaller(const char* fn) {
    // Silently dropping the job would lose work; indexing m_deques with
    // (size_t)-1 would corrupt memory. Neither is better than stopping.
    std::fprintf(stderr,
        "JobSystem::%s() called from a thread that owns no deque. It must be "
        "called from the thread that constructed the JobSystem, or from inside "
        "a running job. See the ordering contract in JobSystem.h.\n", fn);
    std::abort();
}

// Run one job if there is one, otherwise back off. Used by schedule() when the
// ring is full and by wait() while draining.
void JobSystem::helpOnce(std::size_t self) {
    Job* job = nullptr;
    if (tryGetJob(self, job)) {
        job->run();
        if (self == m_submitterQueue) bump(m_mainExecuted);
        else if (self < m_statsCount) bump(m_stats[self].executed);
        return;
    }
    std::this_thread::yield();
}

void JobSystem::workerLoop(std::size_t index) {
    tlsQueueIndex = index;
    tlsOwner      = this;
    tlsRng        = 0x9E3779B97F4A7C15ULL * (index + 1) + 0xDEADBEEF;  // never 0

#if defined(__APPLE__)
    char name[32];
    std::snprintf(name, sizeof(name), "legionfall-worker-%zu", index);
    pthread_setname_np(name);            // shows up in Instruments and lldb
#endif

    WorkerStats& st = m_stats[index];
    int spins = 0;

    while (!m_stop.load(std::memory_order_acquire)) {
        Job* job = nullptr;

        if (tryGetJob(index, job)) {
            job->run();
            bump(st.executed);
            spins = 0;
            continue;
        }

        if (spins < 16) { spinHint(spins++); continue; }
        if (spins < 24) { ++spins; std::this_thread::yield(); continue; }

        // About to park. Read the epoch BEFORE the last scan - get this order
        // wrong and that's the lost wakeup.
        const std::uint64_t token = m_events.prepareWait();

        if (tryGetJob(index, job)) {
            job->run();
            bump(st.executed);
            spins = 0;
            continue;
        }
        if (m_stop.load(std::memory_order_acquire)) break;

        bump(st.parked);
        m_events.commitWait(token);
        spins = 0;
    }

    tlsQueueIndex = kUnregistered;
    tlsOwner      = nullptr;
}

// Drain the batch by helping.
//
// The old wait() blocked the submitter on a condvar while N workers did the
// work - a wasted core at every barrier. Worse, the notify happened without
// holding the waiter's mutex, so a waiter that had checked its predicate but
// hadn't slept yet could miss it and hang.
//
// Helping fixes both. There's no condvar here to miss, and the submitter is
// doing work instead of sitting there.
void JobSystem::wait() {
    const std::size_t self = queueIndex();
    if (self == kUnregistered) unregisteredCaller("wait");

    // Only the submitting thread may wait. A job that calls wait() is itself
    // still counted in m_pending, so once everything else drains the loop below
    // spins forever on pending == 1 with nothing left to help with. Caught by
    // review rather than by a test, because nothing in-tree does it.
    assert(self == m_submitterQueue &&
           "wait() from inside a job would deadlock - it waits on itself");

    int spins = 0;
    while (m_pending.value.load(std::memory_order_acquire) > 0) {
        Job* job = nullptr;
        if (tryGetJob(self, job)) {
            job->run();
            bump(m_mainExecuted);
            spins = 0;
            continue;
        }
        // Jobs still running elsewhere but nothing queued. Spin then yield -
        // never park here, the batch is nearly done and a park round trip
        // costs more than what's left.
        if (spins < 32) spinHint(spins++);
        else            std::this_thread::yield();
    }
}

JobSystem::Stats JobSystem::stats() const {
    Stats s;
    for (std::size_t i = 0; i < m_statsCount; ++i) {
        const WorkerSnapshot w = worker(i);
        s.executed      += w.executed;
        s.stolen        += w.stolen;
        s.stealAttempts += w.stealAttempts;
        s.stealAborts   += w.stealAborts;
        s.parked        += w.parked;
    }
    s.mainExecuted = m_mainExecuted.load(std::memory_order_relaxed);
    s.executed    += s.mainExecuted;
    return s;
}

WorkerSnapshot JobSystem::worker(std::size_t i) const {
    WorkerSnapshot w;
    if (i >= m_statsCount) return w;
    const WorkerStats& s = m_stats[i];
    w.executed      = s.executed.load(std::memory_order_relaxed);
    w.stolen        = s.stolen.load(std::memory_order_relaxed);
    w.stealAttempts = s.stealAttempts.load(std::memory_order_relaxed);
    w.stealAborts   = s.stealAborts.load(std::memory_order_relaxed);
    w.parked        = s.parked.load(std::memory_order_relaxed);
    return w;
}

void JobSystem::resetStats() {
    for (std::size_t i = 0; i < m_statsCount; ++i) {
        WorkerStats& w = m_stats[i];
        w.executed.store(0, std::memory_order_relaxed);
        w.stolen.store(0, std::memory_order_relaxed);
        w.stealAttempts.store(0, std::memory_order_relaxed);
        w.stealAborts.store(0, std::memory_order_relaxed);
        w.parked.store(0, std::memory_order_relaxed);
    }
    m_mainExecuted.store(0, std::memory_order_relaxed);
}

} // namespace Legionfall
