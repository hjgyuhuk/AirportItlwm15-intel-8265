#ifndef AIRPORT_ITLWM_SEQUOIA_JOIN_EVENTS_H
#define AIRPORT_ITLWM_SEQUOIA_JOIN_EVENTS_H

#include <Airport/SequoiaJoinRequest.h>
#include <HAL/ItlJoinEvent.h>

namespace SequoiaWCL {

constexpr uint32_t kAssocEventMessage = 0xd3;
constexpr uint32_t kConnectionCompleteMessage = 0xd5;
constexpr uint32_t kLinkStatusMessage = 0xd8;

// 24G830 BCM NetAdapter::handleLink -> WCLNetManager::linkStatusInd.
// This driver-to-WCL event is distinct from WCL's 16-byte link request.
struct LinkStatusEvent {
    uint8_t bssid[6];
    uint8_t up;
    uint8_t debounce; // InfraInterface::isDebounceOnGoing(), not security.
    uint32_t reason; // 0xff is the stock unspecified link reason.
};
static_assert(sizeof(LinkStatusEvent) == 12, "24G830 link status event");
static_assert(offsetof(LinkStatusEvent, up) == 6, "24G830 link indication");
static_assert(offsetof(LinkStatusEvent, reason) == 8, "24G830 link reason");

// 24G830 handleAuth/handleAssoc and updateAuthAssocStatus. The 32-bit
// details are WCL-domain results, not timestamps, raw errno or Intel codes.
struct JoinResultDetail {
    uint32_t status;
    uint32_t reason;
};

struct AssocEvent {
    uint16_t ieeeStatus;
    uint16_t joinReason;
    uint8_t authResultValid;
    uint8_t bssid[6];
    uint8_t reserved0b;
    JoinResultDetail auth;
    JoinResultDetail assoc;
};

struct ConnectionCandidateResult {
    uint8_t bssid[6];
    uint8_t reserved06[2];
    JoinResultDetail result;
};

// sendConnectComplete sends 0xa4 bytes, even though the currently audited
// JoinRequest consumer reads only the header and first result. Preserve all
// ten records and zero unused entries, not a truncated 20-byte lookalike.
struct ConnectionCompleteEvent {
    uint16_t ieeeStatus;
    uint16_t joinReason;
    ConnectionCandidateResult candidates[kMaxJoinCandidates];
};

static_assert(sizeof(JoinResultDetail) == 8, "24G830 result pair");
static_assert(sizeof(AssocEvent) == 0x1c, "24G830 association event");
static_assert(offsetof(AssocEvent, authResultValid) == 4, "24G830 auth validity");
static_assert(offsetof(AssocEvent, bssid) == 5, "24G830 event BSSID");
static_assert(offsetof(AssocEvent, auth) == 0xc, "24G830 authentication details");
static_assert(offsetof(AssocEvent, assoc) == 0x14, "24G830 association details");
static_assert(sizeof(ConnectionCandidateResult) == 0x10, "24G830 completion stride");
static_assert(offsetof(ConnectionCandidateResult, result) == 8, "24G830 connection details");
static_assert(sizeof(ConnectionCompleteEvent) == 0xa4, "24G830 completion event");
static_assert(offsetof(ConnectionCompleteEvent, candidates) == 4, "24G830 completion list");

// handleAuth/handleAssoc use this IEEE-result domain for the detail reason.
// The transport-status field is deliberately generic on failure: Intel has
// no Broadcom event status, and inventing one would misclassify diagnostics.
inline JoinResultDetail observedJoinDetail(uint16_t ieeeStatus)
{
    if (!ieeeStatus) return {};
    return {0xe3ff8100u, ieeeStatus < 0x45 ? 0xe0821000u | ieeeStatus : 0xe3ff8100u};
}

// Auth success alone produces no 0xd3 event, matching stock handleAuth.
// This encodes only observed AP results, never RUN/key/connection success.
// The workqueue owner retains this snapshot through postMessage retries.
inline bool encodeObservedAssociation(const ItlJoin::Progress &progress, AssocEvent &output)
{
    const auto *auth = progress.auth();
    const auto *assoc = progress.assoc();
    if (!progress.active() || !auth || (!auth->status && !assoc)) return false;
    const auto *result = auth->status ? auth : assoc;
    AssocEvent event = {};
    event.ieeeStatus = result->status;
    event.joinReason = 0xffff; // Stock unspecified reason, not an invented timeout/retry cause.
    event.authResultValid = 1;
    memcpy(event.bssid, result->bssid, sizeof(event.bssid));
    event.auth = observedJoinDetail(auth->status);
    if (assoc) event.assoc = observedJoinDetail(assoc->status);
    output = event;
    return true;
}

} // namespace SequoiaWCL
#endif
