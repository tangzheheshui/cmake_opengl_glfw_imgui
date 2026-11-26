//
//  threadPool.h
//  轻量线程池：持久工作线程 + 任务队列，避免每帧创建线程的开销。
//
//  用法（每帧）：
//    pool.submit(fn1); pool.submit(fn2); ...   // 提交任务（pending 计数）
//    主线程做自己的那批
//    pool.waitAll();                            // 等所有已提交任务完成
//
//  约定：submit 全部调用完之后再 waitAll（pending 计数回到 0 才返回）。
//
#ifndef threadPool_h
#define threadPool_h

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <vector>
#include <atomic>

class ThreadPool {
public:
    explicit ThreadPool(int n) {
        for (int i = 0; i < n; ++i)
            m_threads.emplace_back([this] { workerLoop(); });
    }
    ~ThreadPool() { stop(); }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(std::function<void()> fn) {
        m_pending.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_tasks.push(std::move(fn));
        }
        m_cv.notify_one();
    }

    void waitAll() {
        std::unique_lock<std::mutex> lk(m_doneMutex);
        m_doneCv.wait(lk, [this] { return m_pending.load(std::memory_order_acquire) == 0; });
    }

    int threadCount() const { return static_cast<int>(m_threads.size()); }

private:
    void workerLoop() {
        while (true) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lk(m_mutex);
                m_cv.wait(lk, [this] { return m_stop || !m_tasks.empty(); });
                if (m_stop && m_tasks.empty()) return;
                fn = std::move(m_tasks.front());
                m_tasks.pop();
            }
            fn();
            if (m_pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard<std::mutex> lk(m_doneMutex);
                m_doneCv.notify_all();
            }
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        for (auto& t : m_threads) if (t.joinable()) t.join();
    }

    std::vector<std::thread> m_threads;
    std::queue<std::function<void()>> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false;
    std::atomic<int> m_pending{0};
    std::mutex m_doneMutex;
    std::condition_variable m_doneCv;
};

#endif /* threadPool_h */
