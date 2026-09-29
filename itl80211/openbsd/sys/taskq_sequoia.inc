#ifdef ITL_TASKQ_TEST
#include "taskq_test_support.h"
#else
#include <sys/_task.h>
#include <sys/proc.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOWorkLoop.h>
#endif

enum class TaskQueueState { Running, Stopping };
struct taskq_worker {
    TAILQ_ENTRY(taskq_worker) entry;
    struct task work;
    struct task *source;
    thread_t thread;
};
TAILQ_HEAD(taskq_active, taskq_worker);

struct taskq {
    TaskQueueState state;
    unsigned running;
    uint64_t issued;
    IORecursiveLock *lock;
    struct task_list pending;
    struct taskq_active active;
    IOCommandGate *gate;
    IOWorkLoop *workloop;
};

static struct taskq taskq_system = {};
struct taskq *const systq = &taskq_system;

static bool taskq_current_worker(struct taskq *tq)
{
    struct taskq_worker *worker;
    TAILQ_FOREACH(worker, &tq->active, entry) {
        if (worker->thread == current_thread())
            return true;
    }
    return false;
}

static void taskq_wait_locked(struct taskq *tq)
{
    if (tq->gate && tq->workloop->inGate()) {
        // The completer takes this same gate before waking us. Registration
        // and gate release are atomic, so completion cannot slip past sleep.
        IORecursiveLockUnlock(tq->lock);
        tq->gate->commandSleep(tq, THREAD_UNINT);
        IORecursiveLockLock(tq->lock);
    } else {
        IORecursiveLockSleep(tq->lock, tq, THREAD_UNINT);
    }
}

static void taskq_notify_gate(IOCommandGate *gate, void *event)
{
    if (gate) {
        gate->runAction([](OSObject *, void *source, void *token, void *, void *) -> IOReturn {
            static_cast<IOCommandGate *>(source)->commandWakeup(token, false);
            return kIOReturnSuccess;
        }, gate, event);
    }
}

static void taskq_worker_main(void *argument)
{
    auto *tq = static_cast<struct taskq *>(argument);
    struct taskq_worker worker = {};
    worker.thread = current_thread();
    IORecursiveLockLock(tq->lock);
    for (;;) {
        auto *next = TAILQ_FIRST(&tq->pending);
        if (!next) {
            if (tq->state == TaskQueueState::Stopping)
                break;
            IORecursiveLockSleep(tq->lock, tq, THREAD_UNINT);
            continue;
        }
        TAILQ_REMOVE(&tq->pending, next, t_entry);
        CLR(next->t_flags, TASK_ONQUEUE);
        worker.work = *next;
        worker.source = next;
        TAILQ_INSERT_TAIL(&tq->active, &worker, entry);
        auto *jobGate = tq->gate;
        IORecursiveLockUnlock(tq->lock);

        if (jobGate) {
            jobGate->runAction([](OSObject *, void *item, void *, void *, void *) -> IOReturn {
                auto *work = static_cast<task *>(item);
                work->t_func(work->t_arg);
                return kIOReturnSuccess;
            }, &worker.work);
        } else {
            worker.work.t_func(worker.work.t_arg);
        }

        IORecursiveLockLock(tq->lock);
        TAILQ_REMOVE(&tq->active, &worker, entry);
        IORecursiveLockWakeup(tq->lock, tq, false);
        jobGate = tq->gate;
        // Never acquire the workloop gate while holding the queue lock.
        IORecursiveLockUnlock(tq->lock);
        taskq_notify_gate(jobGate, tq);
        IORecursiveLockLock(tq->lock);
    }
    auto *gate = tq->gate;
    auto *workloop = tq->workloop;
    if (gate) gate->retain();
    if (workloop) workloop->retain();
    if (gate) {
        IORecursiveLockUnlock(tq->lock);
        gate->runAction([](OSObject *, void *source, void *queue, void *, void *) -> IOReturn {
            auto *q = static_cast<taskq *>(queue);
            IORecursiveLockLock(q->lock);
            --q->running;
            IORecursiveLockWakeup(q->lock, q, false);
            IORecursiveLockUnlock(q->lock);
            // No dereference of q after the count reaches zero and unlock.
            static_cast<IOCommandGate *>(source)->commandWakeup(queue, false);
            return kIOReturnSuccess;
        }, gate, tq);
    } else {
        --tq->running;
        IORecursiveLockWakeup(tq->lock, tq, false);
        IORecursiveLockUnlock(tq->lock);
    }
    if (gate) gate->release();
    if (workloop) workloop->release();
    thread_terminate(current_thread());
}

static bool taskq_start(struct taskq *tq, unsigned count)
{
    if (!count)
        return false;
    tq->lock = IORecursiveLockAlloc();
    if (!tq->lock)
        return false;
    tq->state = TaskQueueState::Running;
    tq->running = 0;
    tq->issued = 0;
    tq->gate = nullptr;
    tq->workloop = nullptr;
    TAILQ_INIT(&tq->pending);
    TAILQ_INIT(&tq->active);
    IORecursiveLockLock(tq->lock);
    for (unsigned i = 0; i < count; ++i) {
        thread_t thread = THREAD_NULL;
        // Reserve the worker reference before it can acquire the queue lock.
        ++tq->running;
        const auto error = kernel_thread_start((thread_continue_t)taskq_worker_main, tq, &thread);
        if (error != KERN_SUCCESS) {
            --tq->running;
            break;
        }
        thread_deallocate(thread);
    }
    const bool started = tq->running != 0;
    IORecursiveLockUnlock(tq->lock);
    if (!started) {
        IORecursiveLockFree(tq->lock);
        tq->lock = nullptr;
    }
    return started;
}

void taskq_init()
{
    // Driver startup/teardown owns this singleton; concurrent init is unsupported.
    if (!systq->lock && !taskq_start(systq, 1))
        IOLog("itlwm: failed to start system task queue\n");
}

struct taskq *taskq_create(const char *, unsigned count, int, unsigned)
{
    auto *tq = static_cast<struct taskq *>(IOMalloc(sizeof(struct taskq)));
    if (!tq)
        return nullptr;
    bzero(tq, sizeof(*tq));
    if (!taskq_start(tq, count)) {
        IOFree(tq, sizeof(*tq));
        return nullptr;
    }
    return tq;
}

bool taskq_bind_workloop(struct taskq *tq, IOWorkLoop *workloop)
{
    if (!tq || !tq->lock || !workloop)
        return false;
    auto *gate = IOCommandGate::commandGate(workloop);
    if (!gate)
        return false;
    if (workloop->addEventSource(gate) != kIOReturnSuccess) {
        gate->release();
        return false;
    }
    IORecursiveLockLock(tq->lock);
    bool allowed = tq->state == TaskQueueState::Running && !tq->gate &&
        TAILQ_EMPTY(&tq->pending) && TAILQ_EMPTY(&tq->active);
    if (allowed) {
        workloop->retain();
        tq->workloop = workloop;
        tq->gate = gate;
    }
    IORecursiveLockUnlock(tq->lock);
    if (!allowed) {
        workloop->removeEventSource(gate);
        gate->release();
    }
    return allowed;
}

bool taskq_init_workloop(IOWorkLoop *workloop)
{
    // Native HAL attach owns the singleton; never take over another owner's queue.
    if (systq->lock || !taskq_start(systq, 1))
        return false;
    if (!taskq_bind_workloop(systq, workloop)) {
        taskq_destroy(systq);
        return false;
    }
    return true;
}

bool taskq_is_current(struct taskq *tq, struct task *work)
{
    if (!tq || !tq->lock || !work)
        return false;
    IORecursiveLockLock(tq->lock);
    bool found = false;
    struct taskq_worker *worker;
    TAILQ_FOREACH(worker, &tq->active, entry) {
        if (worker->source == work && worker->thread == current_thread()) {
            found = true;
            break;
        }
    }
    IORecursiveLockUnlock(tq->lock);
    return found;
}

void taskq_quiesce(struct taskq *tq)
{
    if (!tq || !tq->lock)
        return;
    IORecursiveLockLock(tq->lock);
    tq->state = TaskQueueState::Stopping;
    while (auto *work = TAILQ_FIRST(&tq->pending)) {
        TAILQ_REMOVE(&tq->pending, work, t_entry);
        CLR(work->t_flags, TASK_ONQUEUE);
    }
    IORecursiveLockWakeup(tq->lock, tq, false);
    IORecursiveLockUnlock(tq->lock);
}

void taskq_destroy(struct taskq *tq)
{
    if (!tq || !tq->lock)
        return;
    IORecursiveLockLock(tq->lock);
    if (taskq_current_worker(tq))
        panic("itlwm: task queue cannot destroy itself");
    tq->state = TaskQueueState::Stopping;
    IORecursiveLockWakeup(tq->lock, tq, false);
    while (tq->running)
        taskq_wait_locked(tq);
    IORecursiveLockUnlock(tq->lock);
    if (tq->gate) {
        tq->workloop->removeEventSource(tq->gate);
        tq->gate->release();
        tq->gate = nullptr;
        tq->workloop->release();
        tq->workloop = nullptr;
    }
    IORecursiveLockFree(tq->lock);
    tq->lock = nullptr;
    if (tq != systq)
        IOFree(tq, sizeof(*tq));
}

void task_set(struct task *work, void (*function)(void *), void *argument, const char *name)
{
    work->t_func = function;
    work->t_arg = argument;
    work->t_flags = 0;
    work->t_ticket = 0;
    strlcpy(work->name, name ? name : "", sizeof(work->name));
}

int task_add_checked(struct taskq *tq, struct task *work)
{
    if (!tq || !tq->lock || !work || !work->t_func)
        return -1;
    IORecursiveLockLock(tq->lock);
    if (tq->state != TaskQueueState::Running) {
        IORecursiveLockUnlock(tq->lock);
        return -1;
    }
    if (ISSET(work->t_flags, TASK_ONQUEUE)) {
        IORecursiveLockUnlock(tq->lock);
        return 0;
    }
    if (tq->issued == UINT64_MAX) {
        IORecursiveLockUnlock(tq->lock);
        return -1;
    }
    work->t_ticket = ++tq->issued;
    SET(work->t_flags, TASK_ONQUEUE);
    TAILQ_INSERT_TAIL(&tq->pending, work, t_entry);
    IORecursiveLockWakeup(tq->lock, tq, false);
    IORecursiveLockUnlock(tq->lock);
    return 1;
}

int task_add(struct taskq *tq, struct task *work)
{
    return task_add_checked(tq, work) == 1;
}

static int task_remove_locked(struct taskq *tq, struct task *work)
{
    if (!ISSET(work->t_flags, TASK_ONQUEUE))
        return 0;
    TAILQ_REMOVE(&tq->pending, work, t_entry);
    CLR(work->t_flags, TASK_ONQUEUE);
    IORecursiveLockWakeup(tq->lock, tq, false);
    return 1;
}

int task_del(struct taskq *tq, struct task *work)
{
    if (!tq || !tq->lock || !work)
        return 0;
    IORecursiveLockLock(tq->lock);
    int removed = task_remove_locked(tq, work);
    IORecursiveLockUnlock(tq->lock);
    return removed;
}

static bool taskq_before_fence(struct taskq *tq, uint64_t fence)
{
    struct task *work;
    TAILQ_FOREACH(work, &tq->pending, t_entry) {
        if (work->t_ticket <= fence)
            return true;
    }
    struct taskq_worker *worker;
    TAILQ_FOREACH(worker, &tq->active, entry) {
        if (worker->work.t_ticket <= fence)
            return true;
    }
    return false;
}

void taskq_barrier(struct taskq *tq)
{
    if (!tq || !tq->lock)
        return;
    IORecursiveLockLock(tq->lock);
    if (taskq_current_worker(tq))
        panic("itlwm: task queue cannot wait for itself");
    const auto fence = tq->issued;
    while (taskq_before_fence(tq, fence))
        taskq_wait_locked(tq);
    IORecursiveLockUnlock(tq->lock);
}

void taskq_del_barrier(struct taskq *tq, struct task *work)
{
    if (!tq || !tq->lock || !work)
        return;
    IORecursiveLockLock(tq->lock);
    if (taskq_is_current(tq, work))
        panic("itlwm: task queue cannot wait for its own tasks");
    for (;;) {
        task_remove_locked(tq, work);
        struct taskq_worker *worker;
        TAILQ_FOREACH(worker, &tq->active, entry) {
            if (worker->source == work)
                break;
        }
        if (!worker)
            break;
        taskq_wait_locked(tq);
    }
    IORecursiveLockUnlock(tq->lock);
}
