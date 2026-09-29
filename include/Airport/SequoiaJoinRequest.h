#ifndef AIRPORT_ITLWM_SEQUOIA_JOIN_REQUEST_H
#define AIRPORT_ITLWM_SEQUOIA_JOIN_REQUEST_H

#include <Airport/SequoiaWCL.h>

namespace SequoiaWCL {

// 24G830 WCLJoinRequest::checkValidationForApple80211Key and the join
// producer's 0x94-byte copy. Do not alias the older apple80211_key header.
struct JoinKey {
    uint32_t version;
    uint32_t length;
    uint32_t cipher;
    uint16_t flags;
    uint16_t index;
    uint8_t bytes[64];
    uint32_t rscLength;
    uint8_t rsc[8];
    uint8_t address[6];
    uint8_t reserved62[2];
    uint32_t kckLength;
    uint8_t kck[16];
    uint32_t kekLength;
    uint8_t kek[24]; // The stock validator accepts at most 16, despite capacity 24.
};

struct JoinCandidate {
    uint8_t saePKCapable;
    uint8_t reserved01;
    uint16_t encryptionMode;
    uint8_t bssid[6];
    uint8_t oweTransitionBssid[6];
    uint16_t channelSpec;
};

constexpr size_t kMaxJoinCandidates = 10;
constexpr size_t kMaxJoinIE = 257;

// Size comes from WCLJoinManager::handleSendCandidateToDriver's allocation
// and cmdIouc length. Opaque controls retain their offsets until audited.
struct JoinRequest {
    uint8_t control00;
    uint8_t wnmSupported;
    uint8_t reserved02[2];
    uint32_t unassociatedDwell;
    uint32_t batterySaveMode;
    uint16_t apMode;
    uint16_t reserved0e;
    uint32_t authLower;
    uint32_t authUpper;
    uint32_t authUpperExtended;
    uint32_t ssidLength;
    uint8_t ssid[32];
    JoinKey key;
    uint16_t rsnLength;
    uint8_t rsn[kMaxJoinIE];
    uint8_t mldAddress[6];
    uint8_t reserved1dd[3];
    uint32_t controls1e0;
    uint32_t controls1e4;
    uint32_t controls1e8;
    uint32_t extraKeyLength;
    uint8_t extraKey[32];
    uint32_t controls210; // 24G830 allow_auths (not the authUpper enum).
    uint32_t candidateCount;
    JoinCandidate candidates[kMaxJoinCandidates];
    uint16_t extraIELength;
    uint8_t extraIE[kMaxJoinIE];
    uint8_t reserved3cf;
    uint32_t controls3d0;
    uint32_t controls3d4;
    uint32_t controls3d8;
};

static_assert(sizeof(JoinKey) == 0x94, "24G830 key size");
static_assert(offsetof(JoinKey, bytes) == 0x10, "24G830 key bytes");
static_assert(offsetof(JoinKey, rscLength) == 0x50, "24G830 RSC length");
static_assert(offsetof(JoinKey, address) == 0x5c, "24G830 key peer");
static_assert(offsetof(JoinKey, kckLength) == 0x64, "24G830 KCK length");
static_assert(offsetof(JoinKey, kekLength) == 0x78, "24G830 KEK length");
static_assert(sizeof(JoinCandidate) == 18, "24G830 candidate stride");
static_assert(offsetof(JoinCandidate, bssid) == 4, "24G830 candidate BSSID");
static_assert(offsetof(JoinCandidate, oweTransitionBssid) == 10, "24G830 OWE peer");
static_assert(offsetof(JoinCandidate, channelSpec) == 16, "24G830 primary chanspec");
static_assert(sizeof(JoinRequest) == 0x3dc, "24G830 association request size");
static_assert(offsetof(JoinRequest, authLower) == 0x10, "24G830 lower auth");
static_assert(offsetof(JoinRequest, authUpper) == 0x14, "24G830 upper auth");
static_assert(offsetof(JoinRequest, authUpperExtended) == 0x18, "24G830 extended auth");
static_assert(offsetof(JoinRequest, ssidLength) == 0x1c, "24G830 SSID length");
static_assert(offsetof(JoinRequest, ssid) == 0x20, "24G830 SSID bytes");
static_assert(offsetof(JoinRequest, key) == 0x40, "24G830 embedded key");
static_assert(offsetof(JoinRequest, rsnLength) == 0xd4, "24G830 RSN length");
static_assert(offsetof(JoinRequest, rsn) == 0xd6, "24G830 RSN bytes");
static_assert(offsetof(JoinRequest, mldAddress) == 0x1d7, "24G830 MLD address");
static_assert(offsetof(JoinRequest, controls1e0) == 0x1e0, "24G830 join controls");
static_assert(offsetof(JoinRequest, extraKeyLength) == 0x1ec, "24G830 extra key length");
static_assert(offsetof(JoinRequest, controls210) == 0x210, "24G830 auth controls");
static_assert(offsetof(JoinRequest, candidateCount) == 0x214, "24G830 candidate count");
static_assert(offsetof(JoinRequest, candidates) == 0x218, "24G830 candidate list");
static_assert(offsetof(JoinRequest, extraIELength) == 0x2cc, "24G830 extra IE length");
static_assert(offsetof(JoinRequest, extraIE) == 0x2ce, "24G830 extra IE");
static_assert(offsetof(JoinRequest, controls3d0) == 0x3d0, "24G830 tail controls");
static_assert(offsetof(JoinRequest, controls3d8) == 0x3d8, "24G830 final control");

// WCL getAllowAuths reads ivars+0x224; fillAssocCandidatesList copies it to
// request+0x210. BCM's wsec_info type 0x107 maps bit 2 to RSN-PSK AP
// restriction. Only this exact nonzero mask is audited for our WPA2 path.
constexpr uint32_t kJoinAllowAuthRSNPSK = 0x4;
inline bool supportedJoinAllowAuths(const JoinRequest &r)
{
    // Zero preserves the baseline; HAL still validates the selected auth/RSN.
    return r.controls210 == 0 ||
        (r.controls210 == kJoinAllowAuthRSNPSK && r.authLower == 1 &&
         r.authUpper == 8 && r.authUpperExtended == 0);
}

inline void clearJoinSecret(void *memory, size_t size)
{
    auto *bytes = static_cast<volatile uint8_t *>(memory);
    while (size--) *bytes++ = 0;
}

// Structural decoding only, not authorization to associate. The HAL must
// separately validate auth/ciphers/controls, NVM channels and current state.
// Owns the complete input before validation so no deferred callback borrows
// WCL's request or key pointers. The caller stabilizes input during capture.
class JoinRequestSnapshot {
public:
    JoinRequestSnapshot() = default;
    JoinRequestSnapshot(const JoinRequestSnapshot &) = delete;
    JoinRequestSnapshot &operator=(const JoinRequestSnapshot &) = delete;
    ~JoinRequestSnapshot() { clear(); }

    void clear()
    {
        valid_ = false;
        clearJoinSecret(&request_, sizeof(request_));
    }

    bool capture(const void *input, size_t size)
    {
        // Internal aliases are rejected without clearing before the copy.
        // uintptr_t arithmetic avoids undefined comparisons of unrelated pointers.
        const auto source = reinterpret_cast<uintptr_t>(input);
        const auto own = reinterpret_cast<uintptr_t>(this);
        if (!input || size != sizeof(JoinRequest) ||
            (source >= own ? source - own < sizeof(*this) : own - source < size)) {
            clear();
            return false;
        }
        clear();
        memcpy(&request_, input, sizeof(request_));
        if (request_.ssidLength > sizeof(request_.ssid) ||
            request_.candidateCount > kMaxJoinCandidates ||
            request_.key.length > sizeof(request_.key.bytes) ||
            request_.key.rscLength > sizeof(request_.key.rsc) ||
            request_.key.kckLength > sizeof(request_.key.kck) ||
            request_.key.kekLength > 16 ||
            request_.extraKeyLength > sizeof(request_.extraKey) ||
            request_.rsnLength > sizeof(request_.rsn) ||
            request_.extraIELength > sizeof(request_.extraIE)) {
            clear();
            return false;
        }
        valid_ = true;
        return true;
    }

    const JoinRequest *get() const { return valid_ ? &request_ : nullptr; }

private:
    JoinRequest request_ = {};
    bool valid_ = false;
};

} // namespace SequoiaWCL
#endif
