#ifndef ITL_NATIVE_SCAN_H
#define ITL_NATIVE_SCAN_H

#include <HAL/ItlScan.h>

namespace ItlScan {
// Workloop-owned cumulative cache evidence; contains no frame or identity bytes.
struct CacheDiagnostic {
    uint32_t frames = 0, eligibilityRejected = 0, frameRejected = 0;
    uint32_t filterRejected = 0, channelRejected = 0;
    uint32_t allocationFailed = 0, copyFailed = 0, layoutFailed = 0;
    uint32_t parserCalls = 0, nodeHits = 0, nodesAtScanEnd = 0;
    uint32_t nodeLimit = 0, lastFrameLength = 0;
    // Associated bgscan passthrough: RX_PHY/RX_MPDU notifications handed back
    // to the legacy data path so traffic keeps flowing between the firmware's
    // service-channel dwells.
    uint32_t passthroughFrames = 0;
    // r225: whether the last scan's wire carried IWM_LMAC_SCAN_FLAG_FRAGMENTED
    // (1 = fragmented bgscan, 0 = plain one-pass or API bit absent).
    uint32_t assocScanFragmented = 0;
    static void bump(uint32_t &value) { if (value != UINT32_MAX) ++value; }
};

// Borrowed for the entire synchronous scan worker, including all commandSleep
// intervals. Every callback runs under the same HAL/controller workloop gate.
// Callbacks must not sleep, stop hardware, or release their context.
struct NativeSink {
    void *context = nullptr;
    void (*submitted)(void *, Submission) = nullptr;
    void (*notification)(void *, uint16_t, const void *, size_t) = nullptr;
    void (*failed)(void *, Outcome) = nullptr;
    void (*stopped)(void *) = nullptr;
    bool valid() const { return context && submitted && notification && failed && stopped; }
};
} // namespace ItlScan
#endif
