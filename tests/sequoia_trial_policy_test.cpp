#include <HAL/SequoiaTrialPolicy.h>
#include <assert.h>
#include <stdio.h>

int main()
{
    using namespace SequoiaTrial;
    assert(allowed(1, 24, 6, kKernelVersion, 0x8086, 0x3166));
    assert(allowed(1, 24, 6, kKernelVersion, 0x8086, 0x2725));
    const int modes[] = {0, 2, -1};
    for (int mode : modes)
        assert(!allowed(mode, 24, 6, kKernelVersion, 0x8086, 0x3166));
    assert(!allowed(1, 23, 6, kKernelVersion, 0x8086, 0x3166));
    assert(!allowed(1, 24, 5, kKernelVersion, 0x8086, 0x3166));
    assert(!allowed(1, 24, 6, nullptr, 0x8086, 0x3166));
    assert(!allowed(1, 24, 6, "Darwin Kernel Version 24.6.0", 0x8086, 0x3166));
    assert(!allowed(1, 24, 6,
        "Darwin Kernel Version 24.6.0: Tue Jul 21 20:50:07 PDT 2026; "
        "root:xnu-11417.140.69.711.44~1/RELEASE_X86_64", 0x8086, 0x2725));
    assert(!allowed(1, 24, 6, kKernelVersion, 0x8086, 0x3165));
    assert(!allowed(1, 24, 6, kKernelVersion, 0x8086, 0x2726));
    assert(!allowed(1, 24, 6, kKernelVersion, 0xffff, 0x3166));
    puts("Sequoia trial: opt-in, exact kernel version and exact PCI identity gates passed");
}
