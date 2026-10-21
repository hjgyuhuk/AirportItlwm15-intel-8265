#ifndef SEQUOIA_TRIAL_POLICY_H
#define SEQUOIA_TRIAL_POLICY_H

#include <stdint.h>
#include <string.h>

namespace SequoiaTrial {
static const char kKernelVersion[] =
    "Darwin Kernel Version 24.6.0: Sun Aug 23 20:30:56 PDT 2026; "
    "root:xnu-11417.140.69.712.69~1/RELEASE_X86_64";

// The driver reads the private IO80211Family by fixed offsets (the glue
// chain at +0x8, +0x120 and +0xb08 of the interface's ivars) and calls
// its private methods. A build whose layout differs still loads, and then
// the driver reads a pointer that is not a pointer. The exact kernel
// string is the only statement about the layout that can be made from
// outside the kernel.
//
// The levels below let a user who understands that risk relax the check;
// the default is the exact one.
enum class KernelMatch {
    Exact,  // the whole kernel string, byte for byte
    Minor,  // the same Darwin release, any build of it
    Major,  // any Darwin release of the same kernel generation
    Off,    // no version check at all
};

inline KernelMatch kernelMatch(const char *setting)
{
    if (!setting) return KernelMatch::Exact;
    if (!strcmp(setting, "minor")) return KernelMatch::Minor;
    if (!strcmp(setting, "major")) return KernelMatch::Major;
    if (!strcmp(setting, "off")) return KernelMatch::Off;
    return KernelMatch::Exact;
}

inline bool kernelVersionMatches(KernelMatch match, int major, int minor,
                                 const char *kernelVersion)
{
    switch (match) {
    // A different build of the same release changes the kernel string and
    // nothing the driver reads. A security update is exactly that, and
    // this is the level that keeps working across one.
    case KernelMatch::Minor:
        return major == 24 && minor == 6;
    // A different release of the same generation can move the private
    // layout. This one is for a user who has a reason to try it.
    case KernelMatch::Major:
        return major == 24;
    case KernelMatch::Off:
        return true;
    case KernelMatch::Exact:
        return major == 24 && minor == 6 && kernelVersion &&
               strcmp(kernelVersion, kKernelVersion) == 0;
    }
    return false;
}

inline bool allowed(int mode, int major, int minor, const char *kernelVersion,
                    uint16_t vendor, uint16_t device,
                    KernelMatch match = KernelMatch::Exact)
{
    // 0x24FD is the Intel Wireless 8265/9070 (IWM) function, which the IWM HAL
    // already carries a native Sequoia path for. Upstream only validated the
    // AX210, so this entry is an untested opt-in, not a supported adapter.
    return mode == 1 && kernelVersion &&
           kernelVersionMatches(match, major, minor, kernelVersion) &&
           vendor == 0x8086 &&
           (device == 0x3166 || device == 0x2725 || device == 0x24FD);
}
}
#endif
