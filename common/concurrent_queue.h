#ifndef CONCURRENT_QUEUE_H
#define CONCURRENT_QUEUE_H

#include <cassert>
#include <thread>
#include <mutex>
#include <functional>
#include <condition_variable>
#include <queue>
#include <utility>
#include <vector>

namespace YACReader {
class ConcurrentQueue
{
public:
    explicit ConcurrentQueue(std::size_t threadCount)
        : jobsLeft(0),
          bailout(false)
    {
        threads.reserve(threadCount);
        for (; threadCount != 0; --threadCount)
            threads.emplace_back(&ConcurrentQueue::nextJob, this);
    }

    ~ConcurrentQueue()
    {
        joinAll();
    }

    void enqueue(std::function<void(void)> job)
    {
        {
            std::lock_guard<std::mutex> lock(jobsLeftMutex);
            ++jobsLeft;
        }

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            _queue.emplace(std::move(job));
        }

        jobAvailableVar.notify_one();
    }

    //! @brief Cancels all jobs that have not been picked up by worker threads yet.
    //! @return The number of jobs that were canceled.
    std::size_t cancelPending()
    {
        decltype(_queue) oldQueue;
        {
            const std::lock_guard<std::mutex> lock(queueMutex);
            // The mutex locking time is lower with swap() compared to assigning a
            // temporary (which destroys _queue's elements and deallocates memory).
            _queue.swap(oldQueue);
        }

        const auto size = oldQueue.size();
        if (size != 0)
            finalizeJobs(size);
        return size;
    }

    void waitAll()
    {
        std::unique_lock<std::mutex> lock(jobsLeftMutex);
        if (jobsLeft > 0) {
            _waitVar.wait(lock, [this] {
                return jobsLeft == 0;
            });
        }
    }

private:
    std::vector<std::thread> threads;
    std::queue<std::function<void(void)>> _queue;
    std::size_t jobsLeft; //!< @invariant jobsLeft >= _queue.size()
    bool bailout;
    std::condition_variable jobAvailableVar;
    std::condition_variable _waitVar;
    std::mutex jobsLeftMutex;
    std::mutex queueMutex;

    void nextJob()
    {
        while (true) {
            std::function<void(void)> job;

            {
                std::unique_lock<std::mutex> lock(queueMutex);

                if (bailout) {
                    return;
                }

                jobAvailableVar.wait(lock, [this] {
                    return _queue.size() > 0 || bailout;
                });

                if (bailout) {
                    return;
                }

                job = std::move(_queue.front());
                _queue.pop();
            }

            job();
            finalizeJobs(1);
        }
    }

    void finalizeJobs(std::size_t count)
    {
        assert(count > 0);

        std::size_t remainingJobs;
        {
            std::lock_guard<std::mutex> lock(jobsLeftMutex);
            assert(jobsLeft >= count);
            jobsLeft -= count;
            remainingJobs = jobsLeft;
        }

        if (remainingJobs == 0)
            _waitVar.notify_all();
    }

    void joinAll()
    {
        {
            std::lock_guard<std::mutex> lock(queueMutex);

            if (bailout) {
                return;
            }

            bailout = true;
        }

        jobAvailableVar.notify_all();

        for (auto &x : threads) {
            if (x.joinable()) {
                x.join();
            }
        }
    }
};

}

#endif // CONCURRENT_QUEUE_H
