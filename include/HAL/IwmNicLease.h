#ifndef IWM_NIC_LEASE_H
#define IWM_NIC_LEASE_H

#include <stdint.h>

// A synchronous firmware wait opens the gate; reset can invalidate the count.
// Never decrement a new hardware epoch's NIC lock on an old caller's return.
template <class Driver, class Softc> class IwmNicLease {
public:
    IwmNicLease(Driver *driver, Softc *softc, const uint64_t &epoch)
        : driver_(driver), softc_(softc), epoch_(epoch) {}
    IwmNicLease(const IwmNicLease &) = delete;
    IwmNicLease &operator=(const IwmNicLease &) = delete;
    ~IwmNicLease() { release(); }
    bool acquire()
    {
        if (held_) return false;
        captured_ = epoch_;
        return held_ = driver_->iwm_nic_lock(softc_) != 0;
    }
    void release()
    {
        if (held_ && captured_ == epoch_) driver_->iwm_nic_unlock(softc_);
        held_ = false;
    }
private:
    Driver *driver_;
    Softc *softc_;
    const uint64_t &epoch_;
    uint64_t captured_ = 0;
    bool held_ = false;
};
#endif
