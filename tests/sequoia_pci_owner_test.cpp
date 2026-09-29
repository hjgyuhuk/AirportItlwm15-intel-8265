#include <HAL/SequoiaPCIOwner.h>
#include <cassert>
#include <cstdio>
#include <type_traits>
#include <initializer_list>

struct Owner {};
struct Device {
    uint16_t config[128] = {};
    uint8_t msi = 0x50, msix = 0x70;
    bool canOpen = true, opened = false, ignoreFirstWrite = false;
    unsigned writes = 0, closes = 0;
    bool open(Owner *) { if (!canOpen || opened) return false; return opened = true; }
    void close(Owner *) { assert(opened); opened = false; ++closes; }
    uint16_t configRead16(unsigned offset) { return config[offset / 2]; }
    void configWrite16(unsigned offset, uint16_t value) {
        assert(opened);
        ++writes;
        if (ignoreFirstWrite && offset == msi + 2u) { ignoreFirstWrite = false; return; }
        config[offset / 2] = value;
    }
    void findPCICapability(unsigned id, uint8_t *offset) { *offset = id == 5 ? msi : msix; }
    void setBusMasterEnable(bool enabled) { configWrite16(4, (configRead16(4) & ~4) | (enabled ? 4 : 0)); }
    void setMemoryEnable(bool enabled) { configWrite16(4, (configRead16(4) & ~2) | (enabled ? 2 : 0)); }
};

int main()
{
    using Lease = SequoiaPCIOwner<Device, Owner>;
    static_assert(!std::is_copy_constructible<Lease>::value, "PCI ownership cannot be copied");
    Owner owner;
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        Lease lease;
        Device denied;
        denied.canOpen = false;
        assert(!lease.acquire(nullptr, &owner));
        assert(!lease.acquire(&denied, nullptr));
        assert(!lease.acquire(&denied, &owner));
        lease.release();
        assert(!denied.writes && !denied.closes);
        Device noMsi;
        noMsi.msi = 0;
        assert(!lease.acquire(&noMsi, &owner));
        assert(!noMsi.writes && noMsi.closes == 1);
        for (bool hasMsix : {false, true}) {
            Device device;
            device.msix = hasMsix ? 0x70 : 0;
            device.config[2] = 0x147;
            device.config[0x52 / 2] = 0x80;
            device.config[0x72 / 2] = 0x8012;
            assert(lease.acquire(&device, &owner));
            assert(device.opened && device.configRead16(4) == 0x143);
            assert(device.configRead16(0x52) == 0x81);
            if (hasMsix) assert(device.configRead16(0x72) == 0x12);
            assert(!lease.acquire(&device, &owner));
            device.setBusMasterEnable(true);
            lease.release();
            assert(!device.opened && device.closes == 1);
            assert(device.configRead16(4) == 0x143);
            assert(device.configRead16(0x52) == 0x80);
            assert(device.configRead16(0x72) == 0x8012);
            auto writes = device.writes;
            lease.release();
            assert(device.writes == writes && device.closes == 1);
        }
        Device rejectedWrite;
        rejectedWrite.ignoreFirstWrite = true;
        assert(!lease.acquire(&rejectedWrite, &owner));
        assert(!rejectedWrite.opened && rejectedWrite.closes == 1);
        assert(!rejectedWrite.configRead16(4));
    }
    puts("sequoia PCI ownership: exclusivity, MSI/MSI-X, failed readback, restore, reuse passed");
}
