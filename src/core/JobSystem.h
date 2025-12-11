#pragma once
#include <functional>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace Legionfall {

class JobSystem {
public:
    JobSystem();
    ~JobSystem();
    
    void schedule(std::function<void()> task);
    void wait();
    size_t threadCount() const { return m_workers.size(); }

private:
    void workerLoop();
    
    std::vector<std::thread> m_workers;
    std::queue<std::function<void()>> m_tasks;
    
    std::mutex m_queueMutex;
    std::condition_variable m_taskAvailable;
    
    std::mutex m_waitMutex;
    std::condition_variable m_taskComplete;
    
    std::atomic<int> m_pendingTasks{0};
    std::atomic<bool> m_shutdown{false};
};

}