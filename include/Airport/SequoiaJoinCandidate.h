#ifndef SEQUOIA_JOIN_CANDIDATE_H
#define SEQUOIA_JOIN_CANDIDATE_H

#include <Airport/SequoiaJoinRequest.h>

namespace SequoiaWCL {
// Nonsecret evidence only. No SSID, address, IE or credential bytes.
struct JoinCandidateDiagnostic {
    // 0=not reached, 1=candidate, 2=PHY mode, 3=security, 4=BSD matcher,
    // 5=peer ownership, 6=BSD join, 7=AUTH queued (not authenticated).
    uint32_t stage = 0;
    uint32_t mask = UINT32_MAX;
    uint32_t nodes = 0, ssidLength = 0, channel = 0;
    uint64_t ageMs = UINT64_MAX;
};

enum CandidateFailure : uint32_t {
    CandidateAbsent = 1u << 0, CandidateIsBss = 1u << 1,
    CandidateNotCached = 1u << 2, CandidateChannel = 1u << 3,
    CandidateBssid = 1u << 4, CandidateSsidLength = 1u << 5,
    CandidateSsidBytes = 1u << 6, CandidateAge = 1u << 7,
    CandidateCapability = 1u << 8, CandidateInterval = 1u << 9,
    CandidateRates = 1u << 10,
};

// Caller serializes the BSD tree and supplies the CACHE state from its real
// BSD headers. This preserves admission checks while exposing every failure.
template <typename Controller, typename Node, typename Channel>
JoinCandidateDiagnostic inspectJoinCandidate(const Controller *ic, const Node *node,
    const Channel *channel, const JoinRequest &request, size_t index,
    uint64_t now, unsigned cacheState)
{
    JoinCandidateDiagnostic d;
    d.stage = 1;
    d.nodes = ic->ic_nnodes;
    d.channel = request.candidates[index].channelSpec & 0xff;
    if (!node) { d.mask = CandidateAbsent; return d; }
    d.mask = 0;
    d.ssidLength = node->ni_esslen;
    if (node->ni_age_ts && node->ni_age_ts <= now) d.ageMs = now - node->ni_age_ts;
    if (node == ic->ic_bss) d.mask |= CandidateIsBss;
    if (node->ni_state != cacheState) d.mask |= CandidateNotCached;
    if (node->ni_chan != channel) d.mask |= CandidateChannel;
    if (memcmp(node->ni_bssid, request.candidates[index].bssid, 6)) d.mask |= CandidateBssid;
    if (node->ni_esslen != request.ssidLength) d.mask |= CandidateSsidLength;
    // Bound this independently, including when the request has not been captured.
    if (request.ssidLength > sizeof(node->ni_essid) ||
        request.ssidLength > sizeof(request.ssid) ||
        memcmp(node->ni_essid, request.ssid, request.ssidLength)) d.mask |= CandidateSsidBytes;
    if (d.ageMs > 30000) d.mask |= CandidateAge;
    if ((node->ni_capinfo & 3) != 1) d.mask |= CandidateCapability;
    if (!node->ni_intval) d.mask |= CandidateInterval;
    if (!node->ni_rates.rs_nrates) d.mask |= CandidateRates;
    return d;
}
} // namespace SequoiaWCL
#endif
