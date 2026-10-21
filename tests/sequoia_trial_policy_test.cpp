// The kernel version check decides whether the driver loads at all. It
// exists because the driver reads the private IO80211Family by fixed
// offsets, so a layout it was not written against can make the kernel
// read a pointer that is not a pointer.
//
// The default is the exact kernel string. The other levels exist for a
// user who understands the risk; they are opt-in and none of them is the
// default.
#include <HAL/SequoiaTrialPolicy.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using SequoiaTrial::KernelMatch;

static const char kExact[] =
    "Darwin Kernel Version 24.6.0: Sun Aug 23 20:30:56 PDT 2026; "
    "root:xnu-11417.140.69.712.69~1/RELEASE_X86_64";
// The same release, a different build: a security update.
static const char kSameRelease[] =
    "Darwin Kernel Version 24.6.0: Tue Oct  1 09:00:00 PDT 2026; "
    "root:xnu-11417.140.99.99~1/RELEASE_X86_64";
// The next release of the same generation.
static const char kNextRelease[] =
    "Darwin Kernel Version 24.7.0: Mon Nov  3 09:00:00 PDT 2026; "
    "root:xnu-11417.200.1~1/RELEASE_X86_64";
// The next generation.
static const char kNextGeneration[] =
    "Darwin Kernel Version 25.0.0: Mon Sep  8 09:00:00 PDT 2026; "
    "root:xnu-12345.1.2~1/RELEASE_X86_64";

static bool matches(KernelMatch match, int major, int minor, const char *version)
{
    return SequoiaTrial::kernelVersionMatches(match, major, minor, version);
}

int main()
{
    // The default is the exact string and nothing else.
    if (SequoiaTrial::kernelMatch(nullptr) != KernelMatch::Exact) return 1;
    if (SequoiaTrial::kernelMatch("") != KernelMatch::Exact) return 1;
    // An argument that is not one of the levels falls back to the strict one
    // rather than to a looser one.
    if (SequoiaTrial::kernelMatch("off") != KernelMatch::Off) return 1;
    if (SequoiaTrial::kernelMatch("minor") != KernelMatch::Minor) return 1;
    if (SequoiaTrial::kernelMatch("major") != KernelMatch::Major) return 1;
    if (SequoiaTrial::kernelMatch("nonsense") != KernelMatch::Exact) return 1;

    // Exact: only the string it was written against.
    if (!matches(KernelMatch::Exact, 24, 6, kExact)) return 1;
    if (matches(KernelMatch::Exact, 24, 6, kSameRelease)) return 1;
    if (matches(KernelMatch::Exact, 24, 7, kNextRelease)) return 1;

    // Minor: the same release, any build of it. A security update is a
    // different build of the same release and nothing else.
    if (!matches(KernelMatch::Minor, 24, 6, kExact)) return 1;
    if (!matches(KernelMatch::Minor, 24, 6, kSameRelease)) return 1;
    if (matches(KernelMatch::Minor, 24, 7, kNextRelease)) return 1;
    if (matches(KernelMatch::Minor, 25, 0, kNextGeneration)) return 1;

    // Major: any release of the same kernel generation.
    if (!matches(KernelMatch::Major, 24, 6, kExact)) return 1;
    if (!matches(KernelMatch::Major, 24, 7, kNextRelease)) return 1;
    if (matches(KernelMatch::Major, 25, 0, kNextGeneration)) return 1;

    // Off: no version check at all.
    if (!matches(KernelMatch::Off, 24, 6, kExact)) return 1;
    if (!matches(KernelMatch::Off, 25, 0, kNextGeneration)) return 1;

    // The device and the opt-in are checked whatever the version level is.
    if (SequoiaTrial::allowed(0, 24, 6, kExact, 0x8086, 0x24FD)) return 1;
    if (SequoiaTrial::allowed(1, 24, 6, kExact, 0x1234, 0x24FD)) return 1;
    if (SequoiaTrial::allowed(1, 24, 6, kExact, 0x8086, 0x1234)) return 1;
    if (!SequoiaTrial::allowed(1, 24, 6, kExact, 0x8086, 0x24FD)) return 1;
    if (!SequoiaTrial::allowed(1, 24, 6, kSameRelease, 0x8086, 0x24FD,
                               KernelMatch::Minor)) return 1;
    if (SequoiaTrial::allowed(1, 24, 6, kSameRelease, 0x8086, 0x24FD)) return 1;

    // The gates the fork's own test states: the opt-in, the exact kernel
    // string, and the exact PCI identity.
    if (!SequoiaTrial::allowed(1, 24, 6, kExact, 0x8086, 0x3166)) return 1;
    if (!SequoiaTrial::allowed(1, 24, 6, kExact, 0x8086, 0x2725)) return 1;
    for (int mode : {0, 2, -1})
        if (SequoiaTrial::allowed(mode, 24, 6, kExact, 0x8086, 0x3166)) return 1;
    if (SequoiaTrial::allowed(1, 23, 6, kExact, 0x8086, 0x3166)) return 1;
    if (SequoiaTrial::allowed(1, 24, 5, kExact, 0x8086, 0x3166)) return 1;
    if (SequoiaTrial::allowed(1, 24, 6, nullptr, 0x8086, 0x3166)) return 1;
    if (SequoiaTrial::allowed(1, 24, 6, "Darwin Kernel Version 24.6.0",
                              0x8086, 0x3166)) return 1;
    if (SequoiaTrial::allowed(1, 24, 6, kSameRelease, 0x8086, 0x2725)) return 1;
    if (SequoiaTrial::allowed(1, 24, 6, kExact, 0x8086, 0x3165)) return 1;
    if (SequoiaTrial::allowed(1, 24, 6, kExact, 0x8086, 0x2726)) return 1;
    if (SequoiaTrial::allowed(1, 24, 6, kExact, 0x8086, 0x24f3)) return 1;
    if (SequoiaTrial::allowed(1, 24, 6, kExact, 0xffff, 0x3166)) return 1;

    puts("Kernel version policy: exact by default, three opt-in levels, "
         "device and opt-in checked at every level");
    return 0;
}
