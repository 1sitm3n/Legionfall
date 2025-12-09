#pragma once

#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <cstdint>
#include <iostream>

// Simple thread-pool based job system.
// Header-only so it's easy to reuse across the engine.
class JobSystem {
public:
    explicit JobSystem(uint32_t threadCount = 0);
    ~JobSystem();

    // Schedule a job (lambda or std::function) to be run on a worker thread.
    void schedule(const std::function<void()>& job);

    // Block until all scheduled jobs have finished.
    void wait();

    // Number of worker threads in the pool.
    uint32_t threadCount() const;

private:
    void workerLoop();

    std::vector<std::thread> m_workers;
    std::queue<std::function<void()>> m_jobs;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_doneCv;
    bool m_stop = false;
    int m_activeJobs = 0;
};

// ========================= Implementation ===================================

inline JobSystem::JobSystem(uint32_t threadCount)
{
    if (threadCount == 0) {
        threadCount = std::thread::hardware_concurrency();
    }
    if (threadCount == 0) {
        threadCount = 1;
    }

    m_workers.reserve(threadCount);
    for (uint32_t i = 0; i < threadCount; ++i) {
        m_workers.emplace_back([this]() { workerLoop(); });
    }

    std::cout << "JobSystem: started with " << threadCount << " worker threads\n";
}

inline JobSystem::~JobSystem()
{
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();

    for (auto& t : m_workers) {
        if (t.joinable()) {
            t.join();
        }
    }
}

inline void JobSystem::schedule(const std::function<void()>& job)
{
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_jobs.push(job);
        ++m_activeJobs;
    }
    m_cv.notify_one();
}

inline void JobSystem::wait()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_doneCv.wait(lock, [this]() {
        return m_jobs.empty() && m_activeJobs == 0;
    });
}

inline uint32_t JobSystem::threadCount() const
{
    return static_cast<uint32_t>(m_workers.size());
}

inline void JobSystem::workerLoop()
{
    while (true) {
        std::function<void()> job;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() {
                return m_stop || !m_jobs.empty();
            });

            if (m_stop && m_jobs.empty()) {
                return;
            }

            job = std::move(m_jobs.front());
            m_jobs.pop();
        }

        job();

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            --m_activeJobs;
            if (m_jobs.empty() && m_activeJobs == 0) {
                m_doneCv.notify_all();
            }
        }
    }
}
