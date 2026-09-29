#ifndef ITL_SCAN_CACHE_PACKET_H
#define ITL_SCAN_CACHE_PACKET_H

#include <HAL/ItlNativeScan.h>

namespace ItlScan {
// The caller supplies the kernel mbuf KPI (or its audited test boundary).
// 24G830 m_pullup rejects len > 224 before examining the chain; an external
// cluster also takes its 88-byte allocation path. It is not a whole-frame
// linearizer. Request one segment and verify the populated span instead.
inline mbuf_t copyCacheFrame(const uint8_t *frame, size_t length, CacheDiagnostic &d)
{
    mbuf_t packet = nullptr;
    unsigned chunks = 1;
    if (mbuf_allocpacket(MBUF_DONTWAIT, length, &chunks, &packet)) {
        CacheDiagnostic::bump(d.allocationFailed);
        return nullptr;
    }
    if (mbuf_copyback(packet, 0, length, frame, MBUF_DONTWAIT)) {
        CacheDiagnostic::bump(d.copyFailed);
        mbuf_freem(packet);
        return nullptr;
    }
    if (chunks != 1 || mbuf_next(packet) || mbuf_len(packet) != length) {
        CacheDiagnostic::bump(d.layoutFailed);
        mbuf_freem(packet);
        return nullptr;
    }
    return packet;
}
}
#endif
