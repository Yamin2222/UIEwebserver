#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>
#include <assert.h>
#include <vector>

class ThreadPool {
public:
    ThreadPool() = default;
    ThreadPool(ThreadPool&&) = default;
    explicit ThreadPool(int threadCount = 8) : pool_(std::make_shared<Pool>()) {
        assert(threadCount > 0);
        //for循环创建指定数量的线程 
        workers_.reserve(threadCount);
        for (int i = 0; i < threadCount; ++i) {
            workers_.emplace_back([this]() {
                std::unique_lock<std::mutex> locker(pool_->mtx_);
                while (true) {
                    if (!pool_->tasks_.empty()) {
                        auto task = std::move(pool_->tasks_.front());
                        pool_->tasks_.pop();
                        locker.unlock();
                        task();
                        locker.lock();
                    } else if (pool_->isClosed_) {
                        break;
                    } else {
                        pool_->cv_.wait(locker);
                    }
                }
            });
        }
    }

    //析构函数，首先要把isclose关闭，然后唤醒所有线程
    ~ThreadPool() {
        if (pool_) {
            {
                std::unique_lock<std::mutex> locker(pool_->mtx_);
                pool_->isClosed_ = true;
            }
            pool_->cv_.notify_all();
        }
        for (auto &t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    template<typename T>
    void AddTask(T&& task) {
        std::unique_lock<std::mutex> locker(pool_->mtx_);
        //std::forward<T>(task)完美转发，保持task的原始值类别（左值/右值）
        pool_->tasks_.emplace(std::forward<T>(task));
        pool_->cv_.notify_one();
    }

private:
    struct Pool {
        std::mutex mtx_;
        std::condition_variable cv_;
        bool isClosed_ = false;
        std::queue<std::function<void()>> tasks_; //任务队列
    };
    std::shared_ptr<Pool> pool_;
    std::vector<std::thread> workers_;
};


#endif