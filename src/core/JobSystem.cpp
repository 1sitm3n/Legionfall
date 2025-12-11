#include "core/JobSystem.h"
#include <algorithm>
#include <iostream>

namespace Legionfall {

JobSystem::JobSystem() {
    // Use fewer threads to avoid overhead - max 8 or hardware - 1
    size_t numThreads = std::min(8u, std::max(1u, std::thread::hardware_concurrency() - 1));
    
    std::cout << "JobSystem: starting with " << numThreads << " worker threads" << std::endl;
    
    m_workers.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i) {
        m_workers.emplace_back(&JobSystem::workerLoop, this);
    }
}

JobSystem::~JobSystem() {
    // Signal shutdown
    m_shutdown = true;
    m_taskAvailable.notify_all();
    
    // Join all workers
    for (auto& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void JobSystem::schedule(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_pendingTasks.fetch_add(1, std::memory_order_release);
        m_tasks.push(std::move(task));
    }
    m_taskAvailable.notify_one();
}

void JobSystem::wait() {
    std::unique_lock<std::mutex> lock(m_waitMutex);
    m_taskComplete.wait(lock, [this] {
        return m_pendingTasks.load(std::memory_order_acquire) == 0;
    });
}

void JobSystem::workerLoop() {
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
            
            if (m_tasks.empty()) {
                continue;
            }
            
            task = std::move(m_tasks.front());
            m_tasks.pop();
        }
        
        // Execute task outside lock
        if (task) {
            task();
        }
        
        // Signal completion
        int remaining = m_pendingTasks.fetch_sub(1, std::memory_order_release) - 1;
        if (remaining == 0) {
            m_taskComplete.notify_all();
        }
    }
}

}