#ifndef ITL_EAPOL_DIAGNOSTIC_H
#define ITL_EAPOL_DIAGNOSTIC_H
#include <stdint.h>

namespace ItlEapol {
// Counts and public protocol state only. Never retain packet bytes, addresses,
// nonces, MICs, replay counters or key material in the diagnostic interface.
struct Counters {
    uint32_t rx = 0, rxM1 = 0, rxM3 = 0, rxGroup = 0;
    uint32_t txM2 = 0, txM4 = 0, txGroup = 0, txError = 0;
    uint32_t pairCalls = 0, pairResult = UINT32_MAX;
    uint32_t groupCalls = 0, groupResult = UINT32_MAX;
    uint32_t m3Stage = 0, deauthReason = 0;
};
struct Snapshot {
    Counters eapol;
    uint32_t state = 0, supplicant = 0, port = 0, ready = 0;
    uint32_t nodeFlags = 0, ifFlags = 0, linkState = 0;
    uint32_t pairFlags = 0, pairPresent = 0, groupMask = 0;
    uint32_t rxKeys = 0, replay = 0, badMic = 0, noMbuf = 0;
    uint32_t wrongBss = 0, wrongDir = 0, decap = 0, unencrypted = 0, decrypt = 0;
};
template<class IC, class IF> Snapshot capture(const IC *ic, const IF *ifp, bool ready)
{
    Snapshot s;
    if (!ic) return s;
    s.eapol = ic->ic_eapol_diagnostic;
    s.state = ic->ic_state; s.ready = ready;
    if (ifp) { s.ifFlags = ifp->if_flags; s.linkState = ifp->if_link_state; }
    const auto *ni = ic->ic_bss;
    if (ni) {
        s.supplicant = ni->ni_rsn_supp_state; s.port = ni->ni_port_valid;
        s.nodeFlags = ni->ni_flags; s.pairFlags = ni->ni_pairwise_key.k_flags;
        s.pairPresent = ni->ni_pairwise_key.k_priv != nullptr;
    }
    for (unsigned i = 0; i < 4; ++i)
        if (ic->ic_nw_keys[i].k_priv) s.groupMask |= 1u << i;
    const auto &d = ic->ic_stats;
    s.rxKeys = d.is_rx_eapol_key; s.replay = d.is_rx_eapol_replay;
    s.badMic = d.is_rx_eapol_badmic; s.noMbuf = d.is_rx_nombuf;
    s.wrongBss = d.is_rx_wrongbss; s.wrongDir = d.is_rx_wrongdir;
    s.decap = d.is_rx_decap; s.unencrypted = d.is_rx_unencrypted;
    s.decrypt = d.is_rx_wepfail;
    return s;
}
}
#endif
