#include "core/MutexJobSystem.h"

#include <algorithm>

namespace Legionfall {

MutexJobSystem::MutexJobSystem(std::size_t workerCount) {
    if (workerCount == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        workerCount = (hw > 1) ? static_cast<std::size_t>(hw - 1) : 1;
    }
    m_workers.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i) {
        m_workers.emplace_back(&MutexJobSystem::workerLoop, this);
    }
}

MutexJobSystem::~MutexJobSystem() {
    m_shutdown.store(true, std::memory_order_release);
    m_taskAvailable.notify_all();
    for (auto& w : m_workers) {
        if (w.joinable()) w.join();
    }
}

void MutexJobSystem::schedule(std::function<void()> task) {
    // The pending count is guarded by m_waitMutex, not by an atomic. This is
    // the fix for the original lost-wakeup race: the state the waiter's
    // predicate reads must be modified under the waiter's own mutex.
    {
        std::lock_guard<std::mutex> wl(m_waitMutex);
        ++m_pendingTasks;
    }
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_tasks.push(std::move(task));
    }
    m_taskAvailable.notify_one();
}

void MutexJobSystem::wait() {
    std::unique_lock<std::mutex> lock(m_waitMutex);
    m_taskComplete.wait(lock, [this] { return m_pendingTasks == 0; });
}

void MutexJobSystem::workerLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_taskAvailable.wait(lock, [this] {
                return m_shutdown.load(std::memory_order_acquire) || !m_tasks.empty();
            });
            if (m_shutdown.load(std::memory_order_acquire) && m_tasks.empty()) {
                return;
            }
            if (m_tasks.empty()) continue;
            task = std::move(m_tasks.front());
            m_tasks.pop();
        }

        task();

        bool drained = false;
        {
            std::lock_guard<std::mutex> wl(m_waitMutex);
            drained = (--m_pendingTasks == 0);
        }
        if (drained) m_taskComplete.notify_all();
    }
}

} // namespace Legionfall
