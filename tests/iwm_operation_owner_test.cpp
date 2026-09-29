#include <HAL/IwmOperationOwner.h>
#include <cassert>
#include <cstdio>

int main()
{
    IwmOperationOwner owner;
    assert(!owner.busy() && !owner.acquire(0) && !owner.release(1));
    assert(owner.acquire(1) && owner.acquire(1));
    assert(owner.owns(1) && !owner.acquire(2) && !owner.release(2));
    assert(owner.release(1) && owner.busy());
    assert(owner.release(1) && !owner.busy());
    assert(owner.acquire(2) && !owner.acquire(1));
    owner.close();
    assert(!owner.acquire(2) && owner.busy());
    assert(owner.release(2) && !owner.busy());
    assert(!owner.acquire(1) && !owner.acquire(2));
    puts("IWM initialization ownership: nesting, exclusion and closing passed");
}
