#pragma once
// Job storage.
//
// The old version put std::function<void()> in a std::queue. That's a heap
// allocation per task, an indirect call through type-erased storage, and a
// cache miss to get to the closure. At 60Hz with 8 jobs a frame you never see
// it. Push the scheduling rate up and it dominates, and calling into malloc on
// a deadline path is a non-starter whatever its average cost.
//
// So the closure goes inline in a fixed buffer and jobs come from a per-thread
// ring. Nothing allocates after construction.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <new>
#include <vector>
#include <memory>
#include <cassert>

#include "core/ChaseLevDeque.h"   // kCacheLine

namespace Legionfall {

struct Job {
    // 64 fits the updateEnemiesParallel closure (this + 2 size_t + 4 float +
    // 2 bool = 40) with room spare. Too big is a compile error at the call
    // site, not a silent malloc.
    static constexpr std::size_t kPayloadBytes = 64;

    using Thunk = void (*)(void*) noexcept;

    Thunk invoke  = nullptr;
    Thunk destroy = nullptr;                        // null if trivially destructible
    std::atomic<std::int64_t>* pending = nullptr;
    class JobPool*             pool    = nullptr;   // where this slot came from
    Job*                       next    = nullptr;   // free-list link when idle

    alignas(16) std::byte payload[kPayloadBytes];

    inline void run() noexcept;                     // defined below JobPool
};

// Job storage pool. One per submitting thread.
//
// A pool, not a ring, and the reason is worth writing down because a ring is
// the obvious thing to reach for here and it does not work.
//
// A fixed ring handed out cyclically needs fewer than capacity jobs live at
// once. The fork/join barrier does not give you that - wait() runs after the
// whole batch is submitted, so any batch bigger than the ring wraps onto slots
// that are still queued or mid-run, and jobs get dropped or run twice.
//
// Gating that with a completion counter does not fix it either. A counter
// assumes jobs retire in issue order; they don't. The owner's pop() takes from
// the bottom of its deque, so helping runs the newest job and frees the newest
// slot, while the slot about to be reused is the oldest one.
//
// A pool has no ordering constraint - any free slot will do. Finished jobs come
// back on a lock-free stack, and the owner takes the whole stack in one exchange
// rather than popping node by node, which is cheaper and sidesteps ABA entirely
// (the owner never CASes a pop). If nothing is free the pool carves a new block,
// doubling each time, so it grows to the workload's peak once and then never
// allocates again.
class JobPool {
public:
    explicit JobPool(std::size_t firstBlock = 1024) : m_nextBlock(firstBlock) {}

    JobPool(const JobPool&)            = delete;
    JobPool& operator=(const JobPool&) = delete;

    // Owner only.
    Job* acquire() {
        if (!m_localFree) {
            // Bulk-steal every returned job in one atomic op. Cheaper than
            // popping one at a time and immune to ABA.
            m_localFree = m_returned.exchange(nullptr, std::memory_order_acquire);
        }
        if (m_localFree) {
            Job* j = m_localFree;
            m_localFree = j->next;
            j->next = nullptr;
            return j;
        }
        return carve();
    }

    // Any thread - whichever one finished the job.
    void retire(Job* j) noexcept {
        Job* head = m_returned.load(std::memory_order_relaxed);
        do {
            j->next = head;
        } while (!m_returned.compare_exchange_weak(
                     head, j, std::memory_order_release, std::memory_order_relaxed));
    }

    std::size_t capacity() const noexcept { return m_capacity; }
    std::size_t blocks()   const noexcept { return m_blocks.size(); }

private:
    Job* carve() {
        // Growth only. After the first few frames the pool has reached the
        // workload's peak concurrency and acquire() never gets here again.
        m_blocks.emplace_back(std::make_unique<Job[]>(m_nextBlock));
        Job* block = m_blocks.back().get();
        for (std::size_t i = 1; i < m_nextBlock; ++i) {
            block[i].next = m_localFree;
            m_localFree   = &block[i];
        }
        m_capacity += m_nextBlock;
        m_nextBlock *= 2;
        return &block[0];
    }

    std::vector<std::unique_ptr<Job[]>> m_blocks;      // owner only
    Job*        m_localFree = nullptr;                 // owner only
    std::size_t m_nextBlock;
    std::size_t m_capacity  = 0;

    alignas(kCacheLine) std::atomic<Job*> m_returned{nullptr};
};

inline void Job::run() noexcept {
    invoke(payload);
    if (destroy) destroy(payload);

    // Copy both out before handing the slot back - the moment retire() lands
    // the owner may reuse this Job and overwrite these members.
    std::atomic<std::int64_t>* p = pending;
    JobPool*                   r = pool;

    // Release so whatever the job wrote is visible to whoever sees the counter
    // hit zero in wait().
    p->fetch_sub(1, std::memory_order_release);
    r->retire(this);
}

template <typename Fn>
Job* makeJob(JobPool& pool, std::atomic<std::int64_t>* pending, Fn&& fn) noexcept {
    using Decayed = std::decay_t<Fn>;

    static_assert(sizeof(Decayed) <= Job::kPayloadBytes,
                  "closure too big for inline job storage - capture less or "
                  "raise kPayloadBytes, don't fall back to the heap");
    static_assert(alignof(Decayed) <= 16, "closure over-aligned");
    static_assert(std::is_nothrow_move_constructible_v<Decayed>,
                  "job closures must be nothrow-move-constructible");

    Job* job = pool.acquire();
    ::new (static_cast<void*>(job->payload)) Decayed(std::forward<Fn>(fn));

    job->invoke = [](void* p) noexcept { (*static_cast<Decayed*>(p))(); };

    if constexpr (std::is_trivially_destructible_v<Decayed>) {
        job->destroy = nullptr;
    } else {
        job->destroy = [](void* p) noexcept { static_cast<Decayed*>(p)->~Decayed(); };
    }

    job->pending = pending;
    job->pool    = &pool;
    return job;
}

} // namespace Legionfall
