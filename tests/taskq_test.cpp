#define ITL_TASKQ_TEST 1
#include "../itl80211/openbsd/sys/_task.cpp"
#include <chrono>
#include <functional>

static void waitUntil(const std::function<bool()> &condition)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!condition()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

struct Blocker {
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic<bool> entered{false};
    bool released = false;

    static void run(void *argument)
    {
        auto *blocker = static_cast<Blocker *>(argument);
        std::unique_lock<std::mutex> lock(blocker->mutex);
        blocker->entered = true;
        blocker->condition.wait(lock, [&] { return blocker->released; });
    }
    void release()
    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        condition.notify_all();
    }
};

static void increment(void *argument)
{
    ++*static_cast<std::atomic<unsigned> *>(argument);
}

static void allocationAndNames()
{
    assert(!taskq_create("zero", 0, 0, 0));
    failAllocation = true;
    assert(!taskq_create("alloc", 1, 0, 0));
    failAllocation = false;
    failLock = true;
    assert(!taskq_create("lock", 1, 0, 0));
    failLock = false;
    threadBudget = 0;
    assert(!taskq_create("thread", 1, 0, 0));
    threadBudget = 1;
    auto *queue = taskq_create("partial", 4, 0, 0);
    assert(queue);
    threadBudget = -1;
    std::atomic<unsigned> count{0};
    struct task work = {};
    const char tiny[] = "a"; // The old memcpy(name, 256) overread this input.
    task_set(&work, increment, &count, tiny);
    assert(strcmp(work.name, "a") == 0);
    assert(task_add(queue, &work));
    taskq_barrier(queue);
    assert(count == 1);
    char longName[512];
    memset(longName, 'b', sizeof(longName));
    longName[511] = 0;
    task_set(&work, increment, &count, longName);
    assert(strlen(work.name) == 255);
    task_set(&work, increment, &count, nullptr);
    assert(work.name[0] == 0);
    taskq_destroy(queue);
    joinTestThreads();
}

static void cancellationAndDestroy()
{
    auto *queue = taskq_create("cancel", 1, 0, 0);
    Blocker blocker;
    struct task active = {}, pending = {};
    std::atomic<unsigned> count{0};
    task_set(&active, Blocker::run, &blocker, "active");
    task_set(&pending, increment, &count, "cancel");
    assert(task_add(queue, &active));
    waitUntil([&] { return blocker.entered.load(); });
    assert(task_add(queue, &pending));
    assert(!task_add(queue, &pending));
    assert(task_add_checked(queue, &pending) == 0);
    assert(task_del(queue, &pending));
    assert(!task_del(queue, &pending));
    assert(!task_del(queue, &active));

    std::atomic<bool> deleted{false};
    std::thread deleter([&] { taskq_del_barrier(queue, &active); deleted = true; });
    waitUntil([&] { return queue->lock->sleepers.load() != 0; });
    assert(!deleted);
    blocker.release();
    deleter.join();
    assert(deleted && count == 0);
    assert(task_add(queue, &pending));
    taskq_destroy(queue); // Already queued work drains before destruction.
    joinTestThreads();
    assert(count == 1);

    queue = taskq_create("stopping", 1, 0, 0);
    Blocker stopBlocker;
    task_set(&active, Blocker::run, &stopBlocker, "stop-active");
    assert(task_add(queue, &active));
    waitUntil([&] { return stopBlocker.entered.load(); });
    std::atomic<bool> destroyed{false};
    std::thread destroyer([&] { taskq_destroy(queue); destroyed = true; });
    waitUntil([&] {
        IORecursiveLockLock(queue->lock);
        const bool stopping = queue->state == TaskQueueState::Stopping;
        IORecursiveLockUnlock(queue->lock);
        return stopping;
    });
    assert(!destroyed && !task_add(queue, &pending));
    assert(task_add_checked(queue, &pending) == -1);
    stopBlocker.release();
    destroyer.join();
    joinTestThreads();
}

static void parallelFence()
{
    auto *queue = taskq_create("fence", 2, 0, 0);
    Blocker earlier, later;
    struct task first = {}, second = {};
    task_set(&first, Blocker::run, &earlier, "earlier");
    task_set(&second, Blocker::run, &later, "later");
    assert(task_add(queue, &first));
    waitUntil([&] { return earlier.entered.load(); });
    // One idle worker is already sleeping. Wait until the fence also sleeps.
    waitUntil([&] { return queue->lock->sleepers.load() == 1; });
    std::atomic<bool> complete{false};
    std::thread waiter([&] { taskq_barrier(queue); complete = true; });
    waitUntil([&] { return queue->lock->sleepers.load() == 2; });
    assert(!complete);
    assert(task_add(queue, &second));
    waitUntil([&] { return later.entered.load(); });
    earlier.release();
    waitUntil([&] { return complete.load(); }); // Later work must not extend the fence.
    waiter.join();
    later.release();
    taskq_destroy(queue);
    joinTestThreads();
}

static void concurrentProducers()
{
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        auto *queue = taskq_create("stress", 4, 0, 0);
        std::atomic<unsigned> count{0};
        struct task work[128] = {};
        for (auto &item : work)
            task_set(&item, increment, &count, "work");
        std::vector<std::thread> producers;
        for (unsigned producer = 0; producer < 4; ++producer) {
            producers.emplace_back([&, producer] {
                for (unsigned i = producer; i < 128; i += 4)
                    assert(task_add(queue, &work[i]));
            });
        }
        for (auto &producer : producers)
            producer.join();
        taskq_barrier(queue);
        assert(count == 128);
        taskq_destroy(queue);
        joinTestThreads();
    }
    for (unsigned i = 0; i < 50; ++i) {
        taskq_init();
        taskq_init(); // No replacing a live queue's lock.
        std::atomic<unsigned> count{0};
        struct task work = {};
        task_set(&work, increment, &count, "system");
        assert(task_add(systq, &work));
        taskq_destroy(systq);
        assert(count == 1 && !systq->lock);
        joinTestThreads();
    }
}

static void gatedQueues()
{
    auto *loop = new IOWorkLoop;
    auto *queue = taskq_create("gated", 2, 0, 0);
    failGate = true;
    assert(!taskq_bind_workloop(queue, loop));
    failGate = false; failGateAttach = true;
    assert(!taskq_bind_workloop(queue, loop));
    failGateAttach = false;
    assert(taskq_bind_workloop(queue, loop));
    assert(!taskq_bind_workloop(queue, loop));
    unsigned executed = 0;
    struct Context { IOWorkLoop *loop; unsigned *executed; } context{loop, &executed};
    struct task tasks[128] = {};
    for (auto &work : tasks) task_set(&work, [](void *argument) {
        auto *context = static_cast<Context *>(argument);
        assert(context->loop->inGate());
        ++*context->executed;
    }, &context, "gated-work");
    loop->gated([&] {
        for (auto &work : tasks) assert(task_add(queue, &work));
        taskq_barrier(queue); // Must open the workloop gate for the workers.
        assert(executed == 128);
        taskq_destroy(queue); // Also drains last-worker notification in gate.
    });
    joinTestThreads();
    loop->release();
    assert(liveGateObjects == 0);

    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        loop = new IOWorkLoop;
        queue = taskq_create("quiesce", 1, 0, 0);
        assert(taskq_bind_workloop(queue, loop));
        executed = 0;
        context = {loop, &executed};
        loop->gated([&] {
            // A popped worker may already be blocked on this gate. Its task
            // must remain alive even though pending tasks can be cancelled.
            for (auto &work : tasks) assert(task_add(queue, &work));
            taskq_quiesce(queue);
            assert(!task_add(queue, &tasks[0]));
            taskq_destroy(queue);
            assert(executed <= 1);
        });
        joinTestThreads();
        loop->release();
        assert(liveGateObjects == 0);
    }
}

int main()
{
    allocationAndNames();
    cancellationAndDestroy();
    parallelFence();
    concurrentProducers();
    gatedQueues();
    puts("taskq: allocation, cancellation, fences, shutdown and 12800 concurrent tasks passed");
}
