#include <HAL/IwmNicLease.h>
#include <cassert>
#include <cstdio>

struct Softc { int locks = 0; };
struct Driver {
    bool canLock = true;
    unsigned unlocks = 0;
    int iwm_nic_lock(Softc *sc) { if (!canLock) return 0; ++sc->locks; return 1; }
    void iwm_nic_unlock(Softc *sc) { assert(sc->locks > 0); --sc->locks; ++unlocks; }
};

int main()
{
    uint64_t epoch = 1;
    Driver driver;
    Softc sc;
    using Lease = IwmNicLease<Driver, Softc>;
    { Lease idle(&driver, &sc, epoch); }
    assert(!sc.locks && !driver.unlocks);
    for (unsigned stage = 0; stage < 20; ++stage) {
        {
            Lease lease(&driver, &sc, epoch);
            assert(lease.acquire() && !lease.acquire());
            assert(sc.locks == 1);
            if (stage & 1) { ++epoch; sc.locks = 0; }
        }
        assert(!sc.locks);
    }
    assert(driver.unlocks == 10);
    {
        Lease old(&driver, &sc, epoch);
        assert(old.acquire());
        ++epoch;
        sc.locks = 0;
        Lease current(&driver, &sc, epoch);
        assert(current.acquire());
        old.release();
        assert(sc.locks == 1);
        current.release();
        current.release();
        assert(!sc.locks);
    }
    driver.canLock = false;
    { Lease denied(&driver, &sc, epoch); assert(!denied.acquire()); }
    assert(driver.unlocks == 11);
    puts("IWM NIC lease: failed acquisition, early returns, epoch reset and idempotent release passed");
}
