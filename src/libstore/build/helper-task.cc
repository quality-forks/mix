#include "nix/store/build/helper-task.hh"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace nix {

struct HelperTaskState
{
    std::move_only_function<void()> work;
    std::mutex lock;
    std::condition_variable finished;
    bool done = false;
};

namespace {

struct Pool
{
    std::mutex lock;
    std::condition_variable queued;
    std::deque<std::shared_ptr<HelperTaskState>> tasks;
    /** Tasks queued or running, never more than `threads`. */
    size_t busy = 0;
    size_t threads = 0;

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
        if (++busy <= threads) {
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
Pool & pool = *new Pool;

} // namespace

HelperTask::HelperTask(std::move_only_function<void()> work)
    : state(std::make_shared<HelperTaskState>())
{
    state->work = std::move(work);
    pool.submit(state);
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
