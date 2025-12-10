#include "core/JobSystem.h"
#include <algorithm>

namespace Legionfall {

JobSystem::JobSystem() {
    size_t numThreads = std::max(1u, std::thread::hardware_concurrency() - 1);
    m_workers.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i) {
        m_workers.emplace_back(&JobSystem::workerLoop, this);
    }
}

JobSystem::~JobSystem() {
    { std::lock_guard<std::mutex> lock(m_mutex); m_shutdown = true; }
    m_taskAvailable.notify_all();
    for (auto& w : m_workers) if (w.joinable()) w.join();
}

void JobSystem::schedule(std::function<void()> task) {
    { std::lock_guard<std::mutex> lock(m_mutex); m_tasks.push(std::move(task)); ++m_pendingTasks; }
    m_taskAvailable.notify_one();
}

void JobSystem::wait() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_taskComplete.wait(lock, [this] { return m_pendingTasks == 0; });
}

void JobSystem::workerLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_taskAvailable.wait(lock, [this] { return m_shutdown || !m_tasks.empty(); });
            if (m_shutdown && m_tasks.empty()) return;
            task = std::move(m_tasks.front());
            m_tasks.pop();
        }
        task();
        --m_pendingTasks;
        m_taskComplete.notify_all();
    }
}

}
