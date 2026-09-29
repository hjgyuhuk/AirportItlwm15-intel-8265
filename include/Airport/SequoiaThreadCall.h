#ifndef SEQUOIA_THREAD_CALL_H
#define SEQUOIA_THREAD_CALL_H

// Only public Mach thread-call KPIs are used. The separate count includes
// callbacks that have left their workloop but have not released the owner yet.
struct SequoiaThreadCall {
    thread_call_t call;
    IOLock *lock;
    OSObject *owner;
    thread_call_func_t action;
    unsigned outstanding;
    bool closed;

    static SequoiaThreadCall *allocate(thread_call_func_t action, OSObject *owner)
    {
        auto *state = static_cast<SequoiaThreadCall *>(IOMalloc(sizeof(SequoiaThreadCall)));
        if (!state) return nullptr;
        bzero(state, sizeof(*state));
        state->owner = owner;
        state->action = action;
        state->lock = IOLockAlloc();
        if (state->lock) state->call = thread_call_allocate_with_options(invoke, state,
            THREAD_CALL_PRIORITY_HIGH, THREAD_CALL_OPTIONS_ONCE);
        if (!state->call) {
            if (state->lock) IOLockFree(state->lock);
            IOFree(state, sizeof(*state));
            return nullptr;
        }
        return state;
    }

    bool enter()
    {
        IOLockLock(lock);
        if (closed || outstanding == UINT_MAX) { IOLockUnlock(lock); return false; }
        owner->retain();
        ++outstanding;
        const bool coalesced = thread_call_enter(call);
        if (coalesced) {
            owner->release();
            --outstanding;
        }
        IOLockUnlock(lock);
        return true;
    }

    bool closeAndWait()
    {
        IOLockLock(lock);
        closed = true;
        const bool canceled = thread_call_cancel(call);
        if (canceled) --outstanding;
        IOLockUnlock(lock);
        if (canceled) owner->release();
        IOLockLock(lock);
        while (outstanding) IOLockSleep(lock, this, THREAD_UNINT);
        IOLockUnlock(lock);
        return canceled;
    }

    static void destroy(SequoiaThreadCall *&state)
    {
        if (!state) return;
        state->closeAndWait();
        // Public thread_call_free permits a callback's final return to still
        // be in flight and defers the kernel allocation's release accordingly.
        if (!thread_call_free(state->call)) return;
        IOLockFree(state->lock);
        IOFree(state, sizeof(*state));
        state = nullptr;
    }

    static void invoke(thread_call_param_t context, thread_call_param_t argument)
    {
        auto *state = static_cast<SequoiaThreadCall *>(context);
        state->action(state->owner, argument);
        state->owner->release();
        IOLockLock(state->lock);
        --state->outstanding;
        IOLockWakeup(state->lock, state, false);
        IOLockUnlock(state->lock);
        // No state or owner access after unlock: stop may now destroy both.
    }
};

#endif
