#ifndef ITL_IWM_OPERATION_OWNER_H
#define ITL_IWM_OPERATION_OWNER_H

#include <stdint.h>

// Workloop-gated ownership survives commandSleep opening that gate. Nested
// helpers on the same thread may join; another initializer must return busy.
class IwmOperationOwner {
public:
    bool acquire(uintptr_t thread)
    {
        if (!thread || closing_ || (depth_ && owner_ != thread) || depth_ == UINT32_MAX)
            return false;
        owner_ = thread;
        ++depth_;
        return true;
    }
    bool release(uintptr_t thread)
    {
        if (!owns(thread)) return false;
        if (!--depth_) owner_ = 0;
        return true;
    }
    bool owns(uintptr_t thread) const { return depth_ && owner_ == thread; }
    bool busy() const { return depth_ != 0; }
    void close() { closing_ = true; }

private:
    uintptr_t owner_ = 0;
    uint32_t depth_ = 0;
    bool closing_ = false;
};

#endif
