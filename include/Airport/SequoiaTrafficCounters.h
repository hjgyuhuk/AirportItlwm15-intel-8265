#ifndef SEQUOIA_TRAFFIC_COUNTERS_H
#define SEQUOIA_TRAFFIC_COUNTERS_H

#include <stdint.h>

// Interface-lifetime counters for the native generic best-effort Skywalk
// queue. All access is serialized by the controller workloop. Count accepted
// TX handoffs and delivered RX packets, not attempts, bytes, or firmware ACKs.
// Keep them across joins, radio cycles and firmware generations: WCL subtracts
// consecutive samples and interprets a reset as a very large traffic burst.
struct SequoiaTrafficCounters {
    uint64_t tx = 0, rx = 0;
    void transmitted() { ++tx; }
    void received() { ++rx; }
    void snapshot(uint64_t (&out)[7], uint64_t continuousNs) const {
        // The generic queue carries no VI/VO/AWDL/NAN accounting. Like BCM's
        // Skywalk updateRxCounter, classify its deliveries as best effort.
        out[0] = out[1] = out[4] = out[5] = 0;
        out[2] = tx;
        out[3] = rx;
        out[6] = continuousNs;
    }
};
#endif
