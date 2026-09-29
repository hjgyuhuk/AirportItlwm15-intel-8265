#ifndef SEQUOIA_PCI_OWNER_H
#define SEQUOIA_PCI_OWNER_H

#include <stdint.h>

// Only used by the native wrapper. Release follows HAL interrupt/DMA teardown.
// Device and owner are borrowed and must outlive this object.
template <class Device, class Owner> class SequoiaPCIOwner {
public:
    SequoiaPCIOwner() = default;
    SequoiaPCIOwner(const SequoiaPCIOwner &) = delete;
    SequoiaPCIOwner &operator=(const SequoiaPCIOwner &) = delete;

    bool acquire(Device *device, Owner *owner)
    {
        if (device_ || !device || !owner || !device->open(owner)) return false;
        device_ = device;
        owner_ = owner;
        command_ = device_->configRead16(4);
        device_->findPCICapability(0x05, &msi_);
        device_->findPCICapability(0x11, &msix_);
        // This backend installs an MSI handler, not an MSI-X table.
        if (!msi_) { release(); return false; }
        msiControl_ = device_->configRead16(msi_ + 2);
        if (msix_) msixControl_ = device_->configRead16(msix_ + 2);
        configured_ = true;
        device_->setBusMasterEnable(false);
        device_->setMemoryEnable(true);
        if (msix_) device_->configWrite16(msix_ + 2, msixControl_ & ~0x8000);
        device_->configWrite16(msi_ + 2, msiControl_ | 1);
        if (!(device_->configRead16(4) & 2) ||
            !(device_->configRead16(msi_ + 2) & 1) ||
            (msix_ && (device_->configRead16(msix_ + 2) & 0x8000))) {
            release();
            return false;
        }
        return true;
    }

    void release()
    {
        if (!device_) return;
        if (configured_) {
            device_->setBusMasterEnable(false);
            device_->configWrite16(msi_ + 2, msiControl_);
            if (msix_) device_->configWrite16(msix_ + 2, msixControl_);
            // Never restore DMA enable after the HAL freed its buffers.
            device_->configWrite16(4, command_ & ~4);
        }
        device_->close(owner_);
        device_ = nullptr;
        owner_ = nullptr;
        configured_ = false;
        msi_ = msix_ = 0;
    }

private:
    Device *device_ = nullptr;
    Owner *owner_ = nullptr;
    uint16_t command_ = 0, msiControl_ = 0, msixControl_ = 0;
    uint8_t msi_ = 0, msix_ = 0;
    bool configured_ = false;
};
#endif
