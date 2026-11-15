#include "nix/store/build/helper-task.hh"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace nix {

struct HelperTaskState
{
    boost::compat::move_only_function<void()> work;
    std::mutex lock;
    std::condition_variable finished;
    bool done = false;
};

namespace {

struct HelperThreads
{
    std::mutex lock;
    std::condition_variable queued;
    std::deque<std::shared_ptr<HelperTaskState>> tasks;
    /** Tasks queued or running. */
    size_t busy = 0;
    size_t threads = 0;
    /** Threads never exit and each holds a mimalloc heap of its own, so a
        burst of goals must not size the pool. No task waits on another, so
        a queued task only waits for a thread. */
    const size_t maxThreads = std::max<size_t>(32, 4 * std::thread::hardware_concurrency());

    void serve()
    {
        while (true) {
            std::shared_ptr<HelperTaskState> task;
            {
                std::unique_lock guard(lock);
                queued.wait(guard, [&] { return !tasks.empty(); });
                task = std::move(tasks.front());
                tasks.pop_front();
            }
            task->work();
            task->work = nullptr;
            {
                std::lock_guard guard(lock);
                busy--;
            }
            {
                std::lock_guard guard(task->lock);
                task->done = true;
            }
            task->finished.notify_all();
        }
    }

    void submit(std::shared_ptr<HelperTaskState> task)
    {
        std::unique_lock guard(lock);
        tasks.push_back(std::move(task));
        if (++busy <= threads || threads >= maxThreads) {
            queued.notify_one();
            return;
        }
        threads++;
        std::mutex readyLock;
        std::condition_variable readyChanged;
        bool ready = false;
        std::thread([&] {
            /* The first allocation sets up the thread's mimalloc heap, and
               a new-expression's allocation may be elided. */
            ::operator delete(::operator new(1));
            {
                std::lock_guard readyGuard(readyLock);
                ready = true;
                readyChanged.notify_one();
            }
            serve();
        }).detach();
        guard.unlock();
        std::unique_lock readyGuard(readyLock);
        readyChanged.wait(readyGuard, [&] { return ready; });
    }
};

/* Leaked, so exiting never destroys it under its threads. */
HelperThreads & helperThreads = *new HelperThreads;

} // namespace

HelperTask::HelperTask(boost::compat::move_only_function<void()> work)
    : state(std::make_shared<HelperTaskState>())
{
    state->work = std::move(work);
    helperThreads.submit(state);
}

void HelperTask::wait() noexcept
{
    if (!state)
        return;
    std::unique_lock guard(state->lock);
    state->finished.wait(guard, [&] { return state->done; });
}

HelperTask & HelperTask::operator=(HelperTask && other) noexcept
{
    wait();
    state = std::move(other.state);
    return *this;
}

HelperTask::~HelperTask()
{
    wait();
}

} // namespace nix
