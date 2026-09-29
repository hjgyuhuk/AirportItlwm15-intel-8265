#ifndef TASKQ_TEST_SUPPORT_H
#define TASKQ_TEST_SUPPORT_H

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <functional>
#include "../itl80211/openbsd/sys/_task.h"

struct IORecursiveLock {
    std::recursive_mutex mutex;
    std::condition_variable_any condition;
    std::atomic<unsigned> sleepers{0};
};

using thread_t = std::thread::id;
using thread_continue_t = void (*)(void *);
#define THREAD_NULL thread_t{}
#define THREAD_UNINT 0
#define KERN_SUCCESS 0

static std::atomic<int> threadBudget{-1};
static std::atomic<bool> failAllocation{false}, failLock{false};
static std::vector<std::thread> testThreads;
static std::mutex testThreadsLock;

static void *IOMalloc(size_t size) { return failAllocation ? nullptr : malloc(size); }
static void IOFree(void *pointer, size_t) { free(pointer); }
static void IOLog(const char *, ...) {}
[[noreturn]] static void panic(const char *message) { fputs(message, stderr); abort(); }
static IORecursiveLock *IORecursiveLockAlloc() { return failLock ? nullptr : new IORecursiveLock; }
static void IORecursiveLockFree(IORecursiveLock *lock) { delete lock; }
static void IORecursiveLockLock(IORecursiveLock *lock) { lock->mutex.lock(); }
static void IORecursiveLockUnlock(IORecursiveLock *lock) { lock->mutex.unlock(); }
static void IORecursiveLockSleep(IORecursiveLock *lock, void *, int)
{
    ++lock->sleepers;
    lock->condition.wait(lock->mutex);
    --lock->sleepers;
}
static void IORecursiveLockWakeup(IORecursiveLock *lock, void *, bool one)
{
    if (one)
        lock->condition.notify_one();
    else
        lock->condition.notify_all();
}
static thread_t current_thread() { return std::this_thread::get_id(); }
static void thread_terminate(thread_t) {}
static void thread_deallocate(thread_t thread) { assert(thread != THREAD_NULL); }
static int kernel_thread_start(thread_continue_t function, void *argument, thread_t *thread)
{
    if (threadBudget == 0)
        return 1;
    if (threadBudget > 0)
        --threadBudget;
    std::lock_guard<std::mutex> guard(testThreadsLock);
    testThreads.emplace_back(function, argument);
    *thread = testThreads.back().get_id();
    return KERN_SUCCESS;
}
static void joinTestThreads()
{
    for (auto &thread : testThreads)
        thread.join();
    testThreads.clear();
}

using IOReturn = int;
constexpr int kIOReturnSuccess = 0, kIOReturnNotReady = -1;
static std::atomic<unsigned> liveGateObjects{0};
static bool failGate = false, failGateAttach = false;
struct OSObject {
    std::atomic<unsigned> refs{1};
    OSObject() { ++liveGateObjects; }
    virtual ~OSObject() { --liveGateObjects; }
    void retain() { ++refs; }
    void release() { if (--refs == 0) delete this; }
};
class IOCommandGate;
static thread_local IOWorkLoop *taskCurrentLoop = nullptr;
static thread_local unsigned taskGateDepth = 0;
class IOWorkLoop : public OSObject {
public:
    using Action = IOReturn (*)(OSObject *, void *, void *, void *, void *);
    std::recursive_mutex mutex;
    std::condition_variable_any condition;
    std::atomic<unsigned> sleepers{0};
    bool inGate() const { return taskCurrentLoop == this; }
    IOReturn runAction(Action action, OSObject *owner, void *a = nullptr, void *b = nullptr,
                       void *c = nullptr, void *d = nullptr) {
        std::lock_guard<std::recursive_mutex> guard(mutex);
        assert(!taskCurrentLoop || taskCurrentLoop == this);
        taskCurrentLoop = this;
        ++taskGateDepth;
        auto result = action(owner, a, b, c, d);
        if (--taskGateDepth == 0) taskCurrentLoop = nullptr;
        return result;
    }
    void gated(const std::function<void()> &action) {
        runAction([](OSObject *, void *fn, void *, void *, void *) -> IOReturn {
            (*static_cast<const std::function<void()> *>(fn))(); return 0;
        }, this, const_cast<std::function<void()> *>(&action));
    }
    void sleep() {
        assert(inGate() && taskGateDepth);
        const unsigned depth = taskGateDepth;
        for (unsigned i = 1; i < depth; ++i) mutex.unlock();
        taskCurrentLoop = nullptr; taskGateDepth = 0;
        ++sleepers;
        condition.wait(mutex);
        --sleepers;
        taskCurrentLoop = this; taskGateDepth = depth;
        for (unsigned i = 1; i < depth; ++i) mutex.lock();
    }
    IOReturn addEventSource(IOCommandGate *);
    void removeEventSource(IOCommandGate *);
};
class IOCommandGate : public OSObject {
public:
    OSObject *owner;
    std::atomic<IOWorkLoop *> loop{nullptr};
    explicit IOCommandGate(OSObject *value) : owner(value) {}
    static IOCommandGate *commandGate(OSObject *owner) { return failGate ? nullptr : new IOCommandGate(owner); }
    IOReturn runAction(IOWorkLoop::Action action, void *a = nullptr, void *b = nullptr,
                       void *c = nullptr, void *d = nullptr) {
        auto *workloop = loop.load();
        return workloop ? workloop->runAction(action, owner, a, b, c, d) : kIOReturnNotReady;
    }
    int commandSleep(void *, int) { loop.load()->sleep(); return 0; }
    void commandWakeup(void *, bool) {
        auto *workloop = loop.load(); assert(workloop && workloop->inGate());
        workloop->condition.notify_all();
    }
};
inline IOReturn IOWorkLoop::addEventSource(IOCommandGate *gate)
{
    std::lock_guard<std::recursive_mutex> guard(mutex);
    if (failGateAttach) return kIOReturnNotReady;
    gate->retain(); gate->loop = this; return kIOReturnSuccess;
}
inline void IOWorkLoop::removeEventSource(IOCommandGate *gate)
{
    std::lock_guard<std::recursive_mutex> guard(mutex);
    gate->loop = nullptr; gate->release();
}
#endif
