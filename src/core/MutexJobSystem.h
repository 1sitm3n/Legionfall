#pragma once
// The original Legionfall scheduler, kept as a benchmark baseline so the
// lock-free version can be measured against something instead of just claimed
// to be faster.
//
// This is the old design: one global std::queue<std::function<void()>> behind a
// mutex, workers asleep on a condvar, atomic pending counter for the barrier.
//
// One change from what shipped - the lost-wakeup race in wait() is fixed here.
// Originally the last worker decremented m_pendingTasks and called
// m_taskComplete.notify_all() without ever taking m_waitMutex, so a waiter that
// had checked its predicate but not yet blocked could miss the notify and sleep
// forever. Benchmarking against something that can deadlock proves nothing, and
// the fix costs nothing measurable, so what's left is a straight architectural
// comparison: global queue + lock vs per-worker deques + stealing.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace Legionfall {

class MutexJobSystem {
public:
    explicit MutexJobSystem(std::size_t workerCount = 0);
    ~MutexJobSystem();

    MutexJobSystem(const MutexJobSystem&)            = delete;
    MutexJobSystem& operator=(const MutexJobSystem&) = delete;

    void schedule(std::function<void()> task);
    void wait();

    std::size_t threadCount() const noexcept { return m_workers.size(); }

private:
    void workerLoop();

    std::vector<std::thread>          m_workers;
    std::queue<std::function<void()>> m_tasks;

    std::mutex                 m_queueMutex;
    std::condition_variable    m_taskAvailable;

    std::mutex                 m_waitMutex;
    std::condition_variable    m_taskComplete;

    long long                  m_pendingTasks = 0;   // guarded by m_waitMutex
    std::atomic<bool>          m_shutdown{false};
};

} // namespace Legionfall
