#ifndef SEQUOIA_TRIAL_POLICY_H
#define SEQUOIA_TRIAL_POLICY_H

#include <stdint.h>
#include <string.h>

namespace SequoiaTrial {
static const char kKernelVersion[] =
    "Darwin Kernel Version 24.6.0: Sun Aug 23 20:30:56 PDT 2026; "
    "root:xnu-11417.140.69.712.69~1/RELEASE_X86_64";

inline bool allowed(int mode, int major, int minor, const char *kernelVersion,
                    uint16_t vendor, uint16_t device)
{
    // 0x24FD is the Intel Wireless 8265/9070 (IWM) function, which the IWM HAL
    // already carries a native Sequoia path for. Upstream only validated the
    // AX210, so this entry is an untested opt-in, not a supported adapter.
    return mode == 1 && major == 24 && minor == 6 && kernelVersion &&
           strcmp(kernelVersion, kKernelVersion) == 0 && vendor == 0x8086 &&
           (device == 0x3166 || device == 0x2725 || device == 0x24FD);
}
}
#endif
