#include <Airport/SequoiaJoinRequest.h>
#include <Airport/SequoiaSecurity.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/types.h>
#include <net/if.h>
#include <initializer_list>
#include "sequoia_join_bsd_types.inc"

#define nitems(array) (sizeof(array) / sizeof((array)[0]))

struct ieee80211_key { ieee80211_cipher k_cipher = IEEE80211_CIPHER_NONE; };
struct ieee80211_node { ieee80211_key ni_pairwise_key; };
// Only the kernel/hardware boundary is mocked. Enums, macros, WPA/RSN
// layouts and functions above/below are captured from the production sources.
struct ieee80211com {
    int ic_state = 0, ic_opmode = IEEE80211_M_STA;
    bool ic_native_pmf = false;
    SequoiaWCL::TkipCountermeasures ic_native_tkip;
    uint32_t ic_caps = IEEE80211_C_RSN, ic_flags = 0;
    uint32_t ic_rsnprotos = 0, ic_rsnakms = 0, ic_rsnciphers = 0;
    ieee80211_cipher ic_rsngroupcipher = IEEE80211_CIPHER_NONE;
    uint8_t ic_psk[32] = {};
    ieee80211_key ic_nw_keys[4];
    ieee80211_node *ic_bss = nullptr;
    struct { unsigned is_rx_elem_toosmall = 0; } ic_stats;
};
#include "sequoia_join_bsd_functions.inc"

#ifdef SEQUOIA_TEST_IWX
static uint64_t securityNow = 0;
static uint64_t airport_up_time() { return securityNow; }
#define ItlIwm ItlIwx
#define iwm_native_join_security iwx_native_join_security
#endif

class ItlIwm {
public:
    struct Loop {
        bool gate = true, thread = false;
        bool inGate() const { return gate; }
        bool onThread() const { return thread; }
    } loop;
    Loop *getMainWorkLoop() { return &loop; }
    bool leased = false, denied = false, trial = false, nativeNet80211Attached = true;
    bool nativeDetached = false;
    bool isHardwareTrial() const { return trial; }
    class FirmwareLease {
        ItlIwm *owner;
        bool acquired;
    public:
        explicit FirmwareLease(ItlIwm *driver) : owner(driver), acquired(!driver->denied && !driver->leased)
        { if (acquired) owner->leased = true; }
        ~FirmwareLease() { if (acquired) owner->leased = false; }
        explicit operator bool() const { return acquired; }
    };
    struct { bool active = false; bool valid() const { return active; } } nativeScanSink;
    struct { ieee80211com sc_ic; } com;
    ieee80211_node node;
    ItlIwm() { com.sc_ic.ic_bss = &node; }
    int iwm_native_join_security(const SequoiaWCL::JoinRequestSnapshot &);
};

#ifdef SEQUOIA_TEST_IWX
#include "sequoia_iwx_join_security.inc"
#else
#include "../itlwm/hal_iwm/native_join_security_sequoia.inc"
#endif

int main()
{
    using namespace SequoiaWCL;
    JoinRequest request = {};
    request.authLower = APPLE80211_AUTHTYPE_OPEN;
    request.authUpper = APPLE80211_AUTHTYPE_WPA2_PSK;
    request.key.version = 1;
    request.key.cipher = APPLE80211_CIPHER_PMK;
    request.key.length = 32;
    memset(request.key.bytes, 0xa7, 32);
    const uint8_t rsn[] = {48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
    memcpy(request.rsn, rsn, sizeof(rsn));
    request.rsnLength = sizeof(rsn);
    JoinRequestSnapshot snapshot;
    ItlIwm driver;
    auto &ic = driver.com.sc_ic;
    auto configure = [&](const JoinRequest &wire) {
        assert(snapshot.capture(&wire, sizeof(wire)));
        const int result = driver.iwm_native_join_security(snapshot);
        assert(!driver.leased);
        return result;
    };
    assert(configure(request) == 0);
    assert(ic.ic_rsnprotos == IEEE80211_PROTO_RSN);
    assert(ic.ic_rsnakms == IEEE80211_AKM_PSK && ic.ic_rsnciphers == IEEE80211_CIPHER_CCMP);
    assert(ic.ic_rsngroupcipher == IEEE80211_CIPHER_CCMP);
    assert((ic.ic_flags & (IEEE80211_F_RSNON | IEEE80211_F_PSK)) == (IEEE80211_F_RSNON | IEEE80211_F_PSK));
    assert(!memcmp(ic.ic_psk, request.key.bytes, 32));
    // Match the actual 24G830 request, retaining exact RSN+PSK+CCMP policy.
    request.controls210 = 4;
    assert(configure(request) == 0);
    assert(ic.ic_rsnprotos == IEEE80211_PROTO_RSN);
    assert(ic.ic_rsnakms == IEEE80211_AKM_PSK && ic.ic_rsnciphers == IEEE80211_CIPHER_CCMP);
    assert(!memcmp(ic.ic_psk, request.key.bytes, 32));
#ifdef SEQUOIA_TEST_IWX
    for (unsigned pair : {2u,4u}) for (unsigned group : {2u,4u}) {
        auto wire = request; wire.rsn[7] = group; wire.rsn[13] = pair;
        assert(configure(wire) == 0);
        assert(ic.ic_rsnciphers == (pair == 4 ? IEEE80211_CIPHER_CCMP : IEEE80211_CIPHER_TKIP));
        assert(ic.ic_rsngroupcipher == (group == 4 ? IEEE80211_CIPHER_CCMP : IEEE80211_CIPHER_TKIP));
        ic.ic_native_tkip.failure(0); ic.ic_native_tkip.failure(1); securityNow = 60000;
        assert((configure(wire) == 0) == (pair == 4 && group == 4));
        securityNow = 60001; assert(configure(wire) == 0); ic.ic_native_tkip = {};
    }
    ic.ic_caps |= IEEE80211_C_MFP;
    for (unsigned akm : {2u,6u}) for (unsigned caps : {0x80u,0xc0u}) for (unsigned len : {22u,24u,28u}) {
        auto wire = request; wire.rsn[19] = akm; wire.rsn[20] = caps;
        wire.rsnLength = len; wire.rsn[1] = len - 2;
        if (len == 28) { wire.rsn[25] = 15; wire.rsn[26] = 172; wire.rsn[27] = 6; }
        assert(configure(wire) == 0 && ic.ic_native_pmf);
        assert(bool(ic.ic_flags & IEEE80211_F_MFPR) == bool(caps & 0x40));
        auto invalid = wire; invalid.rsn[7] = 2; assert(configure(invalid) != 0);
        invalid = wire; invalid.rsn[13] = 2; assert(configure(invalid) != 0);
        invalid = wire; invalid.rsn[19] = 8; assert(configure(invalid) != 0); // no SAE downgrade
        if (len == 28) { invalid = wire; invalid.rsn[27] = 11; assert(configure(invalid) != 0); }
        if (len >= 24) { invalid = wire; invalid.rsn[22] = 1; assert(configure(invalid) != 0); }
    }
    ic.ic_caps &= ~IEEE80211_C_MFP;
    assert(configure(request) == 0 && !ic.ic_native_pmf);
#endif
    const auto before = ic;
    unsigned rejected = 0;
    auto reject = [&](const JoinRequest &wire) {
        assert(configure(wire) != 0);
        assert(ic.ic_flags == before.ic_flags && ic.ic_rsnprotos == before.ic_rsnprotos);
        assert(ic.ic_rsnakms == before.ic_rsnakms && ic.ic_rsnciphers == before.ic_rsnciphers);
        assert(ic.ic_rsngroupcipher == before.ic_rsngroupcipher);
        assert(!memcmp(ic.ic_psk, before.ic_psk, 32));
        ++rejected;
    };
    for (unsigned bit = 0; bit < 32; ++bit) {
        if (bit == 2) continue;
        auto bad = request; bad.controls210 |= 1u << bit; reject(bad);
        bad.controls210 = 1u << bit; reject(bad);
    }
    auto wrongAuth = request;
    wrongAuth.authLower = APPLE80211_AUTHTYPE_SHARED; reject(wrongAuth);
    wrongAuth = request; wrongAuth.authUpper = 0;
    wrongAuth.key = {}; wrongAuth.rsnLength = 0; reject(wrongAuth);
    for (unsigned length = 0; length <= 64; ++length) {
        if (length == 32) continue;
        auto bad = request; bad.key.length = length; reject(bad);
    }
    for (unsigned length = 0; length <= 257; ++length) {
        if (length == 22) continue;
        auto bad = request; bad.rsnLength = length; reject(bad);
    }
    for (unsigned upper = 0; upper < 17; ++upper) {
        if ((1u << upper) == APPLE80211_AUTHTYPE_WPA2_PSK) continue;
        auto bad = request; bad.authUpper = 1u << upper; reject(bad);
    }
    for (unsigned offset : {4u, 10u, 16u}) {
        auto bad = request;
        memcpy(bad.rsn + offset, MICROSOFT_OUI, 3);
        reject(bad);
    }
    for (unsigned offset : {2u, 7u, 8u, 9u, 13u, 14u, 15u, 19u}) {
        auto bad = request; bad.rsn[offset] ^= 0xff; reject(bad);
    }
    for (unsigned bit = 0; bit < 16; ++bit) {
        if ((1u << bit) & 0x3c) continue;
        auto bad = request;
        bad.rsn[20] = (1u << bit) & 0xff;
        bad.rsn[21] = (1u << bit) >> 8;
        reject(bad);
    }
    for (unsigned which = 0; which < 8; ++which) {
        auto bad = request;
        switch (which) {
            case 0: bad.authUpperExtended = 1; break;
            case 1: bad.key.flags = 1; break;
            case 2: bad.key.index = 1; break;
            case 3: bad.key.rscLength = 6; break;
            case 4: bad.key.kckLength = 16; break;
            case 5: bad.key.kekLength = 16; break;
            case 6: bad.extraKeyLength = 32; break;
            case 7: bad.key.cipher = APPLE80211_CIPHER_PWD; break;
        }
        reject(bad);
    }
    for (unsigned which = 0; which < 9; ++which) {
        switch (which) {
            case 0: driver.loop.gate = false; break;
            case 1: driver.loop.thread = true; break;
            case 2: driver.denied = true; break;
            case 3: driver.nativeScanSink.active = true; break;
            case 4: driver.nativeNet80211Attached = false; break;
            case 5:
#ifdef SEQUOIA_TEST_IWX
                driver.nativeDetached = true;
#else
                driver.trial = true;
#endif
                break;
            case 6: ic.ic_state = 4; break;
            case 7: ic.ic_nw_keys[0].k_cipher = IEEE80211_CIPHER_CCMP; break;
            case 8: driver.node.ni_pairwise_key.k_cipher = IEEE80211_CIPHER_CCMP; break;
        }
        reject(request);
        driver.loop = {}; driver.denied = driver.trial = false;
        driver.nativeDetached = false;
        driver.nativeNet80211Attached = true; driver.nativeScanSink.active = false;
        ic.ic_state = 0; ic.ic_nw_keys[0] = {}; driver.node.ni_pairwise_key = {};
    }
    auto compact = request;
    compact.rsnLength = 20; compact.rsn[1] = 18;
    assert(configure(compact) == 0);
    JoinRequest open = {};
    open.authLower = APPLE80211_AUTHTYPE_OPEN;
    assert(configure(open) == 0);
    assert(!(ic.ic_flags & (IEEE80211_F_PSK | IEEE80211_F_RSNON)));
    assert(!ic.ic_rsnprotos && !ic.ic_rsnakms && !ic.ic_rsnciphers);
    for (auto byte : ic.ic_psk) assert(!byte);
    snapshot.clear();
    assert(driver.iwm_native_join_security(snapshot) != 0);
    printf("Native join security: actual BSD parser/configuration, open/WPA2 and %u atomic rejections passed; no association or radio I/O\n", rejected);
}
