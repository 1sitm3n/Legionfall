#pragma once
// Chase-Lev work-stealing deque.
//
// Papers: Chase & Lev, SPAA 2005 (the algorithm)
//         Le, Pop, Cohen, Zappa Nardelli, PPoPP 2013 (the C11 orderings)
//
// The 2005 proof assumes sequential consistency. On ARM/POWER it's not safe as
// written - the owner's store to bottom can float past its load of top, and
// then the owner and a thief both take the same job. The 2013 paper works out
// the minimum orderings that fix it. That's what's implemented here, so the
// fences below aren't decoration.
//
// Rules: only the owner thread calls push/pop. Any thread can call steal.
// Break the single-owner rule and it fails silently.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <cassert>
#include <type_traits>

namespace Legionfall {

// Apple Silicon is 128, x86-64 is 64. Padding to the bigger one is always
// correct, just slightly wasteful on x86.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t kCacheLine = 128;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

// Empty and Abort have to stay separate. Empty means the victim had nothing.
// Abort means it had something but another thief won the CAS. Treating Abort
// as Empty makes idle workers think the whole system is drained and park early.
enum class StealStatus { Success, Empty, Abort };

template <typename T>
struct StealResult {
    StealStatus status;
    T           value;
};

template <typename T>
class ChaseLevDeque {
    static_assert(std::is_trivially_copyable_v<T>,
                  "slots are std::atomic<T> - store Job* not Job");

public:
    explicit ChaseLevDeque(std::int64_t initialCapacity = 1024) {
        // 0 passes the pow2 test and gives mask = -1, which writes out of
        // bounds on the first push. INT64_MIN passes it too. Check both, and
        // clamp as well because Release builds define NDEBUG.
        assert(initialCapacity > 0 &&
               (initialCapacity & (initialCapacity - 1)) == 0 && "must be a positive power of two");
        if (initialCapacity <= 0) initialCapacity = 1024;
        Array* a = new Array(initialCapacity);
        m_array.store(a, std::memory_order_relaxed);
        m_retired.push_back(a);
        m_top.store(0, std::memory_order_relaxed);
        m_bottom.store(0, std::memory_order_relaxed);
    }

    ~ChaseLevDeque() {
        // Old buffers are kept until shutdown instead of freed on grow. A thief
        // can still be holding a pointer to one while the owner swaps it, so
        // freeing early is a use-after-free. Doing it properly needs hazard
        // pointers or epoch reclamation - that's the hard part of lock-free and
        // it's out of scope here. Growth is log(peak depth) so the list is tiny.
        for (Array* a : m_retired) delete a;
    }

    ChaseLevDeque(const ChaseLevDeque&)            = delete;
    ChaseLevDeque& operator=(const ChaseLevDeque&) = delete;

    // Owner only.
    void push(T item) {
        const std::int64_t b = m_bottom.load(std::memory_order_relaxed);
        const std::int64_t t = m_top.load(std::memory_order_acquire);
        Array* a = m_array.load(std::memory_order_relaxed);

        if (b - t > a->capacity - 1) a = grow(a, b, t);

        a->put(b, item);

        // Release store publishes the slot: everything written to the job
        // before this is visible to a thief that acquire-loads bottom.
        //
        // The paper writes this as a release fence plus a relaxed store. Same
        // semantics, but a release store is one stlr on AArch64 where the fence
        // version is dmb ish + str, and TSan doesn't model standalone fences so
        // the fence version reports false races on every job payload. Two
        // reasons to prefer the store, no reason to prefer the fence.
        m_bottom.store(b + 1, std::memory_order_release);
    }

    // Owner only. Takes from the bottom - LIFO, so the job just pushed is still
    // hot in this core's L1.
    bool pop(T& out) {
        const std::int64_t b = m_bottom.load(std::memory_order_relaxed) - 1;
        Array* a = m_array.load(std::memory_order_relaxed);

        // Release, not relaxed, and this is a C++20 thing rather than a paper
        // thing. P0982R1 removed the "same thread" arm from the release
        // sequence definition, so after this store bottom's publisher is a
        // relaxed store that heads no release sequence - a thief that
        // acquire-loads it gets no synchronizes-with edge back to the push that
        // wrote the slot. Formally a data race on the job payload. Not
        // realisable on AArch64 or x86 (the preceding release store to the same
        // location already ordered everything), but it costs one stlr on a path
        // that immediately executes a dmb ish anyway.
        m_bottom.store(b, std::memory_order_release);

        // The important one. Orders our store to bottom before our load of top.
        // Needed on x86 too - x86 allows StoreLoad reordering, which is exactly
        // this pair. Compiles to mfence on x86, dmb ish on AArch64. Delete it
        // and the owner and a thief can both claim the last job.
        //
        // Build with -DLF_BREAK_POP_FENCE=ON to prove that - the duplication
        // test then starts failing on ARM.
#ifdef LF_BREAK_POP_FENCE
        std::atomic_thread_fence(std::memory_order_relaxed);
#else
        std::atomic_thread_fence(std::memory_order_seq_cst);
#endif

        std::int64_t t = m_top.load(std::memory_order_relaxed);

        if (t > b) {                                   // already empty
            m_bottom.store(b + 1, std::memory_order_relaxed);
            return false;
        }

        out = a->get(b);
        if (t != b) return true;                       // >1 left, no contention

        // Exactly one left and a thief may be going for it. CAS decides.
        bool won = m_top.compare_exchange_strong(
            t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed);

        m_bottom.store(b + 1, std::memory_order_relaxed);
        return won;
    }

    // Any thread. Takes from the top - the oldest job, least likely to be in
    // the owner's cache.
    StealResult<T> steal() {
        std::int64_t t = m_top.load(std::memory_order_acquire);

        // Orders top before bottom. Without it you can pair a fresh top with a
        // stale bottom and get the emptiness check wrong.
        std::atomic_thread_fence(std::memory_order_seq_cst);

        const std::int64_t b = m_bottom.load(std::memory_order_acquire);

        if (t >= b) return {StealStatus::Empty, T{}};

        // Load the array after top/bottom - the owner may have grown it and the
        // old buffer's contents for this index are stale.
        Array* a = m_array.load(std::memory_order_acquire);
        T item = a->get(t);

        if (!m_top.compare_exchange_strong(t, t + 1,
                                           std::memory_order_seq_cst,
                                           std::memory_order_relaxed)) {
            return {StealStatus::Abort, T{}};
        }
        return {StealStatus::Success, item};
    }

    // Approximate - stats and heuristics only, never correctness.
    std::int64_t sizeApprox() const {
        const std::int64_t n = m_bottom.load(std::memory_order_relaxed)
                             - m_top.load(std::memory_order_relaxed);
        return n < 0 ? 0 : n;
    }

    std::int64_t capacity() const {
        return m_array.load(std::memory_order_relaxed)->capacity;
    }

private:
    struct Array {
        const std::int64_t capacity;
        const std::int64_t mask;
        std::atomic<T>*    slots;

        explicit Array(std::int64_t cap)
            : capacity(cap), mask(cap - 1), slots(new std::atomic<T>[cap]) {}
        ~Array() { delete[] slots; }

        void put(std::int64_t i, T v) { slots[i & mask].store(v, std::memory_order_relaxed); }
        T    get(std::int64_t i) const { return slots[i & mask].load(std::memory_order_relaxed); }
    };

    // Owner only, from push().
    Array* grow(Array* old, std::int64_t b, std::int64_t t) {
        Array* fresh = new Array(old->capacity * 2);
        for (std::int64_t i = t; i < b; ++i) fresh->put(i, old->get(i));
        m_array.store(fresh, std::memory_order_release);
        m_retired.push_back(fresh);          // old one stays alive, see dtor
        return fresh;
    }

    // Different threads hit top and bottom on every op. Share a line and every
    // steal invalidates the owner's copy. Worth a few percent under load.
    alignas(kCacheLine) std::atomic<std::int64_t> m_top;
    alignas(kCacheLine) std::atomic<std::int64_t> m_bottom;
    alignas(kCacheLine) std::atomic<Array*>       m_array;

    std::vector<Array*> m_retired;           // owner only
};

} // namespace Legionfall
