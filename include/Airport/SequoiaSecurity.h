#ifndef ITL_SEQUOIA_SECURITY_H
#define ITL_SEQUOIA_SECURITY_H

#include <stdint.h>
#include <stddef.h>

namespace SequoiaWCL {

// Selected association IE, not the AP's list of alternatives. Keep the
// selected suite explicit; never silently downgrade an SAE request to PSK.
struct RsnSelection {
    uint8_t pairwise = 0, group = 0, akm = 0;
    bool pmf = false, requirePmf = false;
};

inline bool selectedRsn(const uint8_t *ie, size_t length, RsnSelection &out)
{
    if (!ie || (length != 20 && length != 22 && length != 24 && length != 28) ||
        ie[0] != 48 || size_t(ie[1]) + 2 != length || ie[2] != 1 || ie[3] ||
        ie[8] != 1 || ie[9] || ie[14] != 1 || ie[15]) return false;
    const unsigned offsets[] = {4, 10, 16};
    for (unsigned offset : offsets)
        if (ie[offset] || ie[offset + 1] != 15 || ie[offset + 2] != 172) return false;
    RsnSelection value;
    value.group = ie[7]; value.pairwise = ie[13]; value.akm = ie[19];
    if ((value.group != 2 && value.group != 4) ||
        (value.pairwise != 2 && value.pairwise != 4) ||
        (value.akm != 2 && value.akm != 6)) return false;
    const uint16_t caps = length >= 22 ? uint16_t(ie[20] | uint16_t(ie[21]) << 8) : 0;
    // Replay-counter capabilities and PMF only. No preauth/PBAC/PeerKey.
    if (caps & ~0xfc) return false;
    value.pmf = (caps & 0x80) != 0;
    value.requirePmf = (caps & 0x40) != 0;
    if (value.requirePmf && !value.pmf) return false;
    if (value.pmf && (value.pairwise != 4 || value.group != 4)) return false;
    if (value.akm == 6 && !value.pmf) return false;
    // PMKSA reuse is not implemented. An omitted management cipher means
    // BIP-CMAC-128; an explicit selector must select exactly that cipher.
    if (length >= 24 && (ie[22] || ie[23])) return false;
    if (length == 28 && (!value.pmf || ie[24] || ie[25] != 15 ||
                         ie[26] != 172 || ie[27] != 6)) return false;
    out = value;
    return true;
}

// Millisecond monotonic clock, gate-owned, independent of a firmware epoch.
// Ordinary leave/reset must not erase the 60-second TKIP exclusion interval.
struct TkipCountermeasures {
    uint64_t firstMs = 0, blockedAtMs = 0;
    bool firstSeen = false, blocked = false;

    bool active(uint64_t now) const
    {
        return blocked && (now < blockedAtMs || now - blockedAtMs < 60000);
    }
    bool failure(uint64_t now)
    {
        if (active(now)) return false;
        if (blocked) { blocked = false; firstSeen = false; }
        if (!firstSeen || now < firstMs || now - firstMs >= 60000) {
            firstMs = now; firstSeen = true;
            return false;
        }
        blockedAtMs = now; blocked = true; firstSeen = false;
        return true;
    }
};

} // namespace SequoiaWCL
#endif
