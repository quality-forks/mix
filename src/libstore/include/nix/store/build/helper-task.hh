#pragma once
///@file

#include <functional>
#include <memory>

namespace nix {

struct HelperTaskState;

/**
 * Work for a goal, run on one of a set of threads that never exit, and
 * waited for when destroyed as a `std::jthread` is joined.
 *
 * A thread that starts or exits holds mimalloc's heap lock for a moment,
 * and a fork in that moment leaves the lock held in the child. A builtin
 * builder's child then hangs on its first thread, as `builtin:fetchurl`'s
 * curl thread did. The threads here start on the goal thread, the one
 * that forks builders, and the task that starts one waits until mimalloc
 * set it up.
 */
class HelperTask
{
    std::shared_ptr<HelperTaskState> state;

    void wait() noexcept;

public:
    HelperTask() = default;
    explicit HelperTask(std::move_only_function<void()> work);
    HelperTask(HelperTask &&) = default;
    HelperTask & operator=(HelperTask && other) noexcept;
    ~HelperTask();
};

} // namespace nix
