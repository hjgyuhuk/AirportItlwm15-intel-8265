#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sys/queue.h>
#include <arpa/inet.h>
#include "../MacKernelSDK/Headers/net/if_llc.h"
#include "host_crypto.h"
#include <crypto/aes.h>
#include <crypto/sha1.h>
#include <crypto/hmac.h>
#include <crypto/cmac.h>
#include <crypto/arc4.h>
#include <crypto/key_wrap.h>
#include "HAL/ItlJoinEvent.h"
#include "HAL/ItlEapolDiagnostic.h"

#include "sequoia_eapol_types.inc"
#include "Airport/SequoiaSecurity.h"
#ifdef SEQUOIA_TEST_IWX
// Share the test scenario names, but execute the real IWX key/port methods.
#define ItlIwm ItlIwx
#include "sequoia_eapol_iwx_flags.inc"
#undef IWM_FLAG_SHUTDOWN
#undef IWM_FLAG_HW_ERR
#undef IWM_FLAG_RFKILL
#define IWM_FLAG_SHUTDOWN IWX_FLAG_SHUTDOWN
#define IWM_FLAG_HW_ERR IWX_FLAG_HW_ERR
#define IWM_FLAG_RFKILL IWX_FLAG_RFKILL
static uint64_t airport_up_time() { return 1; }
#endif
#define XYLog(...) ((void)0)
#define DPRINTF(...) ((void)0)
#define MCLBYTES 2048
#define MBUF_DONTWAIT 0
#define MT_DATA 1
#define ETHERTYPE_PAE 0x888e
#define IEEE80211_SEND_MGMT(ic, ni, subtype, reason) sendMgmt(reason)
#define IO80211FAMILY_V2
#define __IO80211_TARGET __MAC_15_0
#define NBPFILTER 1
#define BPF_DIRECTION_IN 1
using mbuf_tag_id_t = unsigned;
constexpr unsigned DLT_IEEE802_11 = 105, DLT_IEEE802_11_RADIO = 127;

struct ether_header { uint8_t ether_dhost[6], ether_shost[6]; uint16_t ether_type; };
struct Mbuf { std::vector<uint8_t> data; size_t offset = 0, len = 0, total = 0; Mbuf *next = nullptr; bool external = false; };
using mbuf_t = Mbuf *;
static unsigned liveMbufs, liveKeys, allocations, failAt, keyInstalls, deauths, linkUps, nativeDisconnects;
static int queueError, installError;
static std::vector<std::vector<uint8_t>> sent;
static std::vector<std::vector<uint8_t>> sent80211;
static bool routeEnqueue;
static std::vector<uint8_t> testNonce, testRSN, testAPRSN;
static bool failAllocation() { return ++allocations == failAt; }
static mbuf_t allocate(size_t capacity) {
    if (failAllocation()) return nullptr;
    auto *m = new Mbuf; m->data.resize(capacity); ++liveMbufs; return m;
}
static void mbuf_freem(mbuf_t m) {
    while (m) { auto *n = m->next; delete m; assert(liveMbufs); --liveMbufs; m = n; }
}
static size_t mbuf_len(mbuf_t m) { return m->len; }
static size_t mbuf_pkthdr_len(mbuf_t m) { return m->total; }
static void *mbuf_data(mbuf_t m) { return m->data.data() + m->offset; }
static mbuf_t mbuf_next(mbuf_t m) { return m->next; }
static int mbuf_allocpacket(int, size_t n, unsigned *chunks, mbuf_t *out) {
    assert(*chunks == 1); *out = allocate(n); if (!*out) return ENOMEM;
    (*out)->external = n > 88; return 0;
}
static int mbuf_setdata(mbuf_t m, void *p, size_t n) {
    const auto offset = static_cast<uint8_t *>(p) - m->data.data();
    // Reject underflow/overflow; never let the model hide invalid lengths.
    if (offset < 0 || size_t(offset) > m->data.size() || n > m->data.size() - offset) return EINVAL;
    m->offset = offset; m->len = n; return 0;
}
static void mbuf_setlen(mbuf_t m, size_t n) { assert(n <= m->data.size() - m->offset); m->len = n; }
static void mbuf_pkthdr_setlen(mbuf_t m, size_t n) { m->total = n; }
static std::vector<uint8_t> flatten(mbuf_t m) {
    const size_t total = m->total;
    std::vector<uint8_t> bytes;
    for (; m; m = m->next) bytes.insert(bytes.end(), m->data.begin() + m->offset, m->data.begin() + m->offset + m->len);
    assert(bytes.size() == total); return bytes;
}
static int mbuf_pullup(mbuf_t *m, size_t n) {
    assert(n <= (*m)->total);
    // Actual 24G830 rejects these sizes even if the chain is contiguous.
    if (n > 224 || ((*m)->external && n > 88)) {
        mbuf_freem(*m); *m = nullptr; return ENOMEM;
    }
    auto bytes = flatten(*m);
    auto *replacement = allocate(bytes.size());
    mbuf_freem(*m); *m = replacement;
    if (!replacement) return ENOMEM;
    replacement->data = bytes; replacement->len = replacement->total = bytes.size(); return 0;
}
static int mbuf_copydata(mbuf_t m, size_t off, size_t n, void *out) {
    auto bytes = flatten(m);
    if (off > bytes.size() || n > bytes.size() - off) return EINVAL;
    memcpy(out, bytes.data() + off, n); return 0;
}
static void mbuf_adj(mbuf_t m, size_t n) {
    assert(n <= m->total); m->total -= n;
    for (; n && m; m = m->next) {
        const auto count = std::min(n, m->len);
        m->offset += count; m->len -= count; n -= count;
    }
    assert(!n);
}
static void mbuf_prepend(mbuf_t *m, size_t n, int) {
    if ((*m)->offset >= n) {
        (*m)->offset -= n; (*m)->len += n; (*m)->total += n;
        return;
    }
    auto *head = allocate(n);
    if (!head) { mbuf_freem(*m); *m = nullptr; return; }
    head->len = n; head->total = (*m)->total + n; head->next = *m; *m = head;
}
#define mtod(m, type) reinterpret_cast<type>(mbuf_data(m))
struct mbuf_list { std::vector<mbuf_t> packets; };
static void ml_enqueue(mbuf_list *list, mbuf_t packet) { list->packets.push_back(packet); }
struct UnexpectedAppleSupplicant {
    void inputPacket(mbuf_t, size_t) { assert(false); }
};
static void mbuf_dup(mbuf_t, unsigned, mbuf_t *) { assert(false); }
static void bpf_mtap(void *, mbuf_t, unsigned) { assert(false); }
struct _ifnet {
    struct Stats { unsigned inputPackets = 0; } stats;
    Stats *netStat = nullptr;
    UnexpectedAppleSupplicant *iface = nullptr;
    void *if_bpf = nullptr;
    uint64_t if_ibytes = 0, if_imcasts = 0;
    unsigned if_flags = IFF_RUNNING;
    int if_snd = 0, if_link_state = 0;
    void (*if_start)(_ifnet *) = [](_ifnet *) {};
};
struct ieee80211_tx_ba { unsigned ba_state = 0; };
struct ieee80211_node {
    unsigned ni_refcnt = 1, ni_inact = 0;
    uint16_t ni_txseq = 0, ni_qos_txseqs[16] = {};
    ieee80211_tx_ba ni_tx_ba[16];
    unsigned ni_flags = 0, ni_port_valid = 0, ni_assoc_fail = 0;
    unsigned ni_rsn_supp_state = RSNA_SUPP_PTKSTART;
    unsigned ni_rsnprotos = IEEE80211_PROTO_RSN, ni_rsnakms = IEEE80211_AKM_PSK;
    unsigned ni_rsnciphers = IEEE80211_CIPHER_CCMP;
    unsigned ni_rsncaps = 0;
    uint8_t ni_pmkid[16] = {};
    ieee80211_cipher ni_rsncipher = IEEE80211_CIPHER_CCMP, ni_rsngroupcipher = IEEE80211_CIPHER_CCMP;
    ieee80211_cipher ni_rsngroupmgmtcipher = IEEE80211_CIPHER_BIP;
    uint64_t ni_replaycnt = 0;
    int ni_replaycnt_ok = 0;
    uint8_t ni_rsn_group_ack_pending = 0, ni_rsn_group_ack_mic[16] = {};
    uint8_t ni_macaddr[6] = {2, 3, 4, 5, 6, 7}, ni_pmk[32] = {}, ni_nonce[32] = {};
    uint8_t ni_bssid[6] = {2, 3, 4, 5, 6, 7};
    ieee80211_ptk ni_ptk = {};
    ieee80211_key ni_pairwise_key = {};
    uint8_t *ni_rsnie = nullptr;
};
struct ieee80211com;
class ItlIwm;
int ieee80211_set_key(ieee80211com *, ieee80211_node *, ieee80211_key *);
static int installNativeKey(ieee80211com *, ieee80211_node *, ieee80211_key *);
struct ieee80211com {
    _ifnet ic_if;
    ItlIwm *owner = nullptr;
    ieee80211_node *ic_bss = nullptr;
    unsigned ic_flags = IEEE80211_F_RSNON | IEEE80211_F_PSK;
    unsigned ic_xflags = 0;
    int ic_mgt_timer = 0;
    unsigned ic_caps = 0, ic_tid_noack = 0;
    bool ic_native_pmf = false;
    SequoiaWCL::TkipCountermeasures ic_native_tkip;
    ieee80211_cipher ic_rsngroupmgmtcipher = IEEE80211_CIPHER_BIP;
    ieee80211_opmode ic_opmode = IEEE80211_M_STA;
    ieee80211_state ic_state = IEEE80211_S_RUN;
    uint8_t ic_myaddr[6] = {2, 8, 9, 10, 11, 12}, ic_psk[32] = {}, ic_nonce[32] = {};
    ieee80211_key ic_nw_keys[IEEE80211_GROUP_NKID] = {};
    unsigned ic_rsnciphers = IEEE80211_CIPHER_CCMP;
    ieee80211_cipher ic_rsngroupcipher = IEEE80211_CIPHER_CCMP;
    struct { unsigned is_rx_nombuf = 0, is_rx_eapol_key = 0, is_rx_eapol_replay = 0, is_rx_eapol_badmic = 0,
        is_rx_wrongbss = 0, is_rx_wrongdir = 0, is_rx_decap = 0, is_rx_unencrypted = 0, is_rx_wepfail = 0,
        is_rx_elem_toosmall = 0, is_rx_unauth = 0,
        is_tx_nombuf = 0, is_tx_nonode = 0, is_tx_noauth = 0; } ic_stats;
    ItlEapol::Counters ic_eapol_diagnostic;
    int (*ic_set_key)(ieee80211com *, ieee80211_node *, ieee80211_key *) =
        installNativeKey;
};
static ieee80211com *encapOwner;
mbuf_t ieee80211_encap(_ifnet *, mbuf_t, ieee80211_node **);
static ieee80211_node *ieee80211_ref_node(ieee80211_node *ni) { assert(ni); ++ni->ni_refcnt; return ni; }
static void ieee80211_release_node(ieee80211com *, ieee80211_node *ni) { assert(ni && ni->ni_refcnt > 1); --ni->ni_refcnt; }
static ieee80211_node *ieee80211_find_txnode(ieee80211com *ic, const uint8_t *) { return ieee80211_ref_node(ic->ic_bss); }
static int ieee80211_classify(ieee80211com *, mbuf_t) { return 0; }
static int ieee80211_can_use_ampdu(ieee80211com *, ieee80211_node *) { return 0; }
static void ieee80211_node_trigger_addba_req(ieee80211_node *, int) { assert(false); }
static int ifq_enqueue(int *, mbuf_t m) {
    if (!queueError) sent.push_back(flatten(m));
    if (!queueError && routeEnqueue) {
        ieee80211_node *node = nullptr;
        m = ieee80211_encap(&encapOwner->ic_if, m, &node);
        if (m) { assert(node); sent80211.push_back(flatten(m)); ieee80211_release_node(encapOwner, node); }
    }
    mbuf_freem(m); return queueError;
}
static void randomNonce(void *out, size_t n) { assert(n == testNonce.size()); memcpy(out, testNonce.data(), n); }
#define arc4random_buf(out, n) randomNonce(out, n)
static void panic(const char *) { assert(false); }
static uint8_t *ieee80211_add_wpa(uint8_t *, ieee80211com *, ieee80211_node *) { assert(false); return nullptr; }
static ieee80211_pmk *ieee80211_pmksa_find(ieee80211com *, ieee80211_node *, const uint8_t *) { assert(false); return nullptr; }
static void ieee80211_set_link_state(ieee80211com *ic, int state) { ic->ic_if.if_link_state = state; if (state == LINK_STATE_UP) ++linkUps; }
static void ieee80211_new_state(ieee80211com *ic, ieee80211_state state, int) { ic->ic_state = state; }
static void ieee80211_notify_native_join(ieee80211com *ic, ieee80211_node *ni,
    ItlJoin::Stage stage, unsigned reason) {
    assert(stage == ItlJoin::Stage::Deauth && reason && !ni->ni_port_valid);
    assert(ic->ic_if.if_link_state == LINK_STATE_DOWN);
    assert(ic->ic_xflags & IEEE80211_F_TX_MGMT_ONLY);
    ++nativeDisconnects;
}
static void sendMgmt(unsigned reason) { assert(reason); ++deauths; }
union ANY_CTX { HMAC_MD5_CTX md5; HMAC_SHA1_CTX sha1; AES_CMAC_CTX cmac; };
static int ieee80211_wep_set_key(ieee80211com *, ieee80211_key *) { assert(false); return EINVAL; }
static int ieee80211_tkip_set_key(ieee80211com *, ieee80211_key *) { assert(false); return EINVAL; }
static int ieee80211_bip_set_key(ieee80211com *, ieee80211_key *) { assert(false); return EINVAL; }
static void ieee80211_wep_delete_key(ieee80211com *, ieee80211_key *) { assert(false); }
static void ieee80211_tkip_delete_key(ieee80211com *, ieee80211_key *) { assert(false); }
static void ieee80211_bip_delete_key(ieee80211com *, ieee80211_key *) { assert(false); }
static void *keyAlloc(size_t n) {
    if (failAllocation()) return nullptr;
    ++liveKeys; return std::malloc(n);
}
static void keyFree(void *p) {
    assert(liveKeys); --liveKeys;
    auto *bytes = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < sizeof(ieee80211_ccmp_ctx); ++i) assert(!bytes[i]);
    std::free(p);
}
static void clearSecret(void *p, size_t n) {
    auto *bytes = static_cast<volatile uint8_t *>(p);
    while (n--) *bytes++ = 0;
}
#define explicit_bzero(p, n) clearSecret(p, n)
#define malloc(n, pool, flags) keyAlloc(n)
#define free(p) keyFree(p)
// The upstream encap function retains an unreachable if(0) raw-tag branch.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wint-to-pointer-cast"
#include "sequoia_eapol_functions.inc"
#pragma clang diagnostic pop
#undef malloc
#undef free

class ItlIwm {
public:
    struct { ieee80211com sc_ic; unsigned sc_flags = 0; int sc_generation = 3; int sc_tx_timer[31] = {}; } com;
    struct { bool gated = true; bool inGate() const { return gated; } } loop;
    struct { bool held = false; bool busy() const { return held; } } nativeFirmwareOwner;
    bool trial = false, nativeDetached = false, nativeNet80211Attached = true, commandTransportReady = true;
    int nativeStateError = 0;
    uint64_t nativeWatchdogLastMs = 0;
    bool nativeWatchdogActive = false;
    int iwm_native_watchdog(uint64_t);
    uint64_t nativeStateSerial = 3, nativeStateCompleted = 3;
    ItlJoin::Progress nativeJoinProgress;
    auto getMainWorkLoop() -> decltype(&loop) { return &loop; }
    bool isHardwareTrial() const { return trial; }
#ifdef SEQUOIA_TEST_IWX
    bool nativeJoinMode = true;
    int iwx_native_set_key(ieee80211com *, ieee80211_node *, ieee80211_key *);
    bool iwx_native_port_ready();
    void iwx_native_clear_keys();
    bool iwx_native_data_operation_ready() const { return !nativeFirmwareOwner.busy(); }
    int iwx_set_key(ieee80211com *, ieee80211_node *, ieee80211_key *) { assert(false); return EIO; }
    int iwm_native_set_key(ieee80211com *ic, ieee80211_node *ni, ieee80211_key *k) { return iwx_native_set_key(ic, ni, k); }
    bool iwm_native_port_ready() { return iwx_native_port_ready(); }
    void iwm_native_clear_keys() { iwx_native_clear_keys(); }
#else
    int iwm_native_set_key(ieee80211com *, ieee80211_node *, ieee80211_key *);
    bool iwm_native_port_ready();
    void iwm_native_clear_keys();
#endif
};
#ifdef SEQUOIA_TEST_IWX
#define nativeError nativeStateError
#define nativeCommandTransport commandTransportReady
#include "sequoia_eapol_iwx_methods.inc"
#undef nativeError
#undef nativeCommandTransport
#else
#include "../itlwm/hal_iwm/native_key_sequoia.inc"
#include "../itlwm/hal_iwm/native_watchdog_sequoia.inc"
#endif
static int installNativeKey(ieee80211com *ic, ieee80211_node *ni, ieee80211_key *k) {
    ++keyInstalls;
    if (installError) return installError;
    return ic->owner->iwm_native_set_key(ic, ni, k);
}

struct Vector {
    const char *pmk, *anonce, *snonce, *ptk, *rsn, *aprsn, *gtk, *msg1, *msg2, *msg3, *msg4, *group1, *group2;
    const char *rotate1, *rotate2, *rekey_anonce, *rekey_snonce, *rekey_gtk, *rekey_ptk, *rekey1, *rekey2, *rekey3, *rekey4;
};
static const Vector vectors[] = {
#include "sequoia_eapol_vectors.inc"
};
static std::vector<uint8_t> unhex(const char *s) {
    std::vector<uint8_t> out;
    while (*s) { unsigned b; assert(sscanf(s, "%2x", &b) == 1); out.push_back(b); s += 2; } return out;
}
static mbuf_t packet(const std::vector<uint8_t> &bytes, size_t split) {
    assert(split <= bytes.size());
    auto *m = new Mbuf, *tail = new Mbuf; liveMbufs += 2;
    m->data.assign(bytes.begin(), bytes.begin() + split); m->len = split; m->total = bytes.size();
    tail->data.assign(bytes.begin() + split, bytes.end()); tail->len = bytes.size() - split;
    m->next = tail; return m;
}
static void clearKeys(ieee80211com &ic, ieee80211_node &ni) {
    for (auto &k : ic.ic_nw_keys) ieee80211_reset_key(&ic, &k);
    ieee80211_reset_key(&ic, &ni.ni_pairwise_key);
    assert(!liveKeys);
}
static void setup(const Vector &v, ieee80211com &ic, ieee80211_node &ni) {
    clearKeys(ic, ni);
    assert(!liveMbufs);
    auto *owner = ic.owner;
    assert(owner);
    *owner = ItlIwm(); ni = {}; ic.owner = owner; ic.ic_bss = &ni;
    ic.ic_if.netStat = &ic.ic_if.stats;
    assert(owner->nativeJoinProgress.begin(ni.ni_bssid, owner->com.sc_generation));
    ItlJoin::Event event = {ItlJoin::Stage::Auth, {}, 0};
    memcpy(event.bssid, ni.ni_bssid, 6);
    assert(owner->nativeJoinProgress.record(event, owner->com.sc_generation));
    event.stage = ItlJoin::Stage::Assoc;
    assert(owner->nativeJoinProgress.record(event, owner->com.sc_generation));
    testNonce = unhex(v.snonce); testRSN = unhex(v.rsn); testAPRSN = unhex(v.aprsn);
    ni.ni_rsnie = testAPRSN.data();
    ni.ni_rsncaps = testAPRSN[testAPRSN.size() - 2];
    auto pmk = unhex(v.pmk); memcpy(ic.ic_psk, pmk.data(), pmk.size());
    allocations = failAt = keyInstalls = deauths = linkUps = nativeDisconnects = 0; queueError = installError = 0; sent.clear();
    sent80211.clear(); routeEnqueue = false; encapOwner = &ic;
}
static void input(ieee80211com &ic, ieee80211_node &ni, const std::vector<uint8_t> &bytes, size_t split) {
    ieee80211_eapol_key_input(&ic, packet(bytes, split), &ni); assert(!liveMbufs);
}
static void inputEthernet(ieee80211com &ic, ieee80211_node &ni,
    const std::vector<uint8_t> &bytes, size_t split, mbuf_list &list) {
    assert(split >= sizeof(ether_header)); // decap supplies a contiguous Ethernet header.
    ieee80211_enqueue_data(&ic, packet(bytes, split), &ni, 0, &list);
}
static ieee80211_eapol_key *keyOf(std::vector<uint8_t> &bytes) {
    assert(bytes.size() >= 14 + sizeof(ieee80211_eapol_key));
    return reinterpret_cast<ieee80211_eapol_key *>(bytes.data() + 14);
}
static void sign(std::vector<uint8_t> &bytes, const std::vector<uint8_t> &ptk) {
    auto *key = keyOf(bytes); memset(key->mic, 0, sizeof(key->mic)); ieee80211_eapol_key_mic(key, ptk.data());
}
static std::vector<uint8_t> withPayload(std::vector<uint8_t> wire, std::vector<uint8_t> payload,
    const std::vector<uint8_t> &ptk, bool encrypted) {
    if (encrypted) {
        const auto length = std::max(size_t(16), (payload.size() + 7) & ~size_t(7));
        if (length != payload.size()) { payload.push_back(0xdd); payload.resize(length); }
        payload.resize(length + 8);
        aes_key_wrap_ctx ctx;
        aes_key_wrap_set_key_wrap_only(&ctx, ptk.data() + 16, 16);
        aes_key_wrap(&ctx, payload.data(), length / 8, payload.data());
    }
    wire.resize(14 + sizeof(ieee80211_eapol_key));
    wire.insert(wire.end(), payload.begin(), payload.end());
    auto *key = keyOf(wire);
    BE_WRITE_2(key->paylen, payload.size()); BE_WRITE_2(key->len, wire.size() - 18);
    if (BE_READ_2(key->info) & EAPOL_KEY_KEYMIC) sign(wire, ptk);
    return wire;
}

int main() {
    unsigned cases = 0;
    static_assert(sizeof(ieee80211_eapol_key) == 99, "EAPOL wire layout");
    // Check real allocator boundaries and both audited pullup rejection paths.
    for (unsigned size : {0u, 128u, 512u, 1917u}) {
        auto *m = ieee80211_get_eapol_key(MBUF_DONTWAIT, MT_DATA, size);
        assert(m && !m->next && m->offset == 32 && !m->len);
        assert(m->data.size() - m->offset >= sizeof(ieee80211_eapol_key) + size);
        mbuf_freem(m);
    }
    for (unsigned size : {1918u, 2048u, UINT32_MAX})
        assert(!ieee80211_get_eapol_key(MBUF_DONTWAIT, MT_DATA, size));
    for (unsigned size : {99u, 225u, 347u}) {
        auto *m = allocate(size); m->external = true; m->len = m->total = size;
        assert(mbuf_pullup(&m, size) == ENOMEM && !m && !liveMbufs);
    }
    for (const auto &v : vectors) {
        auto m1 = unhex(v.msg1), m2 = unhex(v.msg2), m3 = unhex(v.msg3), m4 = unhex(v.msg4);
        auto ptk = unhex(v.ptk), gtk = unhex(v.gtk), g1 = unhex(v.group1), g2 = unhex(v.group2);
        ItlIwm driver;
        auto &ic = driver.com.sc_ic; ic.owner = &driver;
        ieee80211_node ni;
        // Initial M4 failures also permit the exact M3 retry without installing
        // a key early or making the controlled port usable.
        for (unsigned failure : {1u, 2u}) {
            setup(v, ic, ni); input(ic, ni, m1, 1);
            if (failure == 1) queueError = ENOBUFS;
            else failAt = allocations + 2;
            input(ic, ni, m3, 1);
            assert(sent.size() == 1 && !keyInstalls && !ni.ni_port_valid);
            assert(!ni.ni_replaycnt_ok && ni.ni_rsn_supp_state == RSNA_SUPP_PTKNEGOTIATING);
            queueError = 0; failAt = 0;
            input(ic, ni, m3, 1);
            assert(sent.size() == 2 && sent.back() == m4 && keyInstalls == 2);
            assert(driver.iwm_native_port_ready());
            cases += 2;
        }
        // Exercise real PTK replacement while the previous data key is live.
        // Local M4 enqueue/allocation failure must retain the old key state,
        // allowing the same authenticated M3 to be processed on retry.
        for (unsigned failure : {0u, 1u, 2u}) {
            setup(v, ic, ni); input(ic, ni, m1, 1); input(ic, ni, m3, 1);
            ni.ni_pairwise_key.k_tsc = 41;
            ic.ic_nw_keys[1].k_rsc[0] = 42;
            testNonce = unhex(v.rekey_snonce);
            auto nextPtk = unhex(v.rekey_ptk), nextGtk = unhex(v.rekey_gtk);
            auto nextM3 = unhex(v.rekey3);
            input(ic, ni, unhex(v.rekey1), 1);
            assert(sent.back() == unhex(v.rekey2) && driver.iwm_native_port_ready());
            assert(!memcmp(&ni.ni_ptk, ptk.data(), sizeof(ni.ni_ptk)));
            if (failure == 1) queueError = ENOBUFS;
            if (failure == 2) failAt = allocations + 2; // RX linear copy, then M4 allocation.
            input(ic, ni, nextM3, 1);
            if (failure) {
                assert(sent.size() == 3 && keyInstalls == 2 && driver.iwm_native_port_ready());
                assert(ni.ni_replaycnt == 2 && ni.ni_rsn_supp_state == RSNA_SUPP_PTKNEGOTIATING);
                assert(!memcmp(&ni.ni_ptk, ptk.data(), sizeof(ni.ni_ptk)));
                assert(ni.ni_pairwise_key.k_tsc == 41 && ic.ic_nw_keys[1].k_rsc[0] == 42);
                queueError = 0; failAt = 0;
                input(ic, ni, nextM3, 1); // same counter, not an artificial increment
            }
            assert(sent.size() == 4 && sent.back() == unhex(v.rekey4));
            assert(keyInstalls == 4 && ni.ni_replaycnt == 11 && driver.iwm_native_port_ready());
            assert(ni.ni_rsn_supp_state == RNSA_SUPP_PTKDONE);
            assert(!memcmp(&ni.ni_ptk, nextPtk.data(), sizeof(ni.ni_ptk)));
            assert(!memcmp(ni.ni_pairwise_key.k_key, nextPtk.data() + 32, 16));
            assert(!memcmp(ic.ic_nw_keys[2].k_key, nextGtk.data(), 16));
            assert(ic.ic_nw_keys[1].k_rsc[0] == 42); // old GTK slot remains intact
            ni.ni_pairwise_key.k_tsc = 51; ic.ic_nw_keys[2].k_rsc[0] = 52;
            auto retry = nextM3;
            BE_WRITE_8(keyOf(retry)->replaycnt, uint64_t(12)); sign(retry, nextPtk);
            input(ic, ni, retry, 1);
            assert(keyInstalls == 4 && ni.ni_pairwise_key.k_tsc == 51 && ic.ic_nw_keys[2].k_rsc[0] == 52);
            auto stale = g1;
            BE_WRITE_8(keyOf(stale)->replaycnt, uint64_t(13)); sign(stale, ptk);
            input(ic, ni, stale, 1);
            assert(keyInstalls == 4 && ni.ni_replaycnt == 12); // old KCK cannot rotate GTK
            cases += 8;
        }
        // GTK slot rotation retains the pairwise key and both receive slots.
        setup(v, ic, ni); input(ic, ni, m1, 1); input(ic, ni, m3, 1);
        ni.ni_pairwise_key.k_tsc = 61; ic.ic_nw_keys[1].k_rsc[0] = 62;
        input(ic, ni, unhex(v.rotate1), 1);
        assert(sent.back() == unhex(v.rotate2) && keyInstalls == 3 && driver.iwm_native_port_ready());
        assert(ni.ni_pairwise_key.k_tsc == 61 && ic.ic_nw_keys[1].k_rsc[0] == 62);
        assert(!memcmp(ic.ic_nw_keys[2].k_key, unhex(v.rekey_gtk).data(), 16));
        ++cases;
        for (unsigned failure : {1u, 2u}) {
            setup(v, ic, ni); input(ic, ni, m1, 1); input(ic, ni, m3, 1);
            auto rotate = unhex(v.rotate1);
            if (failure == 1) queueError = ENOBUFS;
            else failAt = allocations + 3; // RX copy, GTK context, G2 allocation
            input(ic, ni, rotate, 1);
            assert(sent.size() == 2 && keyInstalls == 3 && ni.ni_replaycnt == 4);
            assert(driver.iwm_native_port_ready());
            ic.ic_nw_keys[2].k_rsc[0] = 73; ni.ni_pairwise_key.k_tsc = 74;
            queueError = ENOBUFS; failAt = 0;
            input(ic, ni, rotate, 1);
            assert(sent.size() == 2 && keyInstalls == 3 && ni.ni_rsn_group_ack_pending);
            queueError = 0; failAt = 0;
            auto tampered = rotate; tampered.back() ^= 1;
            input(ic, ni, tampered, 1); // same recorded MIC, invalid body
            assert(sent.size() == 2 && keyInstalls == 3);
            auto different = std::vector<uint8_t>{221,22,0,15,172,1,2,0};
            different.insert(different.end(), gtk.begin(), gtk.end());
            input(ic, ni, withPayload(rotate, different, ptk, true), 1);
            assert(sent.size() == 2 && keyInstalls == 3); // valid MIC but not the pending message
            input(ic, ni, rotate, 1);
            assert(sent.size() == 3 && sent.back() == unhex(v.rotate2));
            assert(keyInstalls == 3 && ni.ni_replaycnt == 4 && driver.iwm_native_port_ready());
            assert(ic.ic_nw_keys[2].k_rsc[0] == 73 && ni.ni_pairwise_key.k_tsc == 74);
            input(ic, ni, rotate, 1);
            assert(sent.size() == 3 && keyInstalls == 3); // no pending local failure now
            cases += 7;
        }
        for (unsigned boundary = 0; boundary < 3; ++boundary) {
            setup(v, ic, ni); input(ic, ni, m1, 1); input(ic, ni, m3, 1);
            queueError = ENOBUFS;
            input(ic, ni, unhex(v.rotate1), 1);
            assert(ni.ni_rsn_group_ack_pending);
            queueError = 0;
            if (boundary == 0) {
                ni.ni_port_valid = 0;
                input(ic, ni, unhex(v.rotate1), 1);
                assert(sent.size() == 2 && keyInstalls == 3);
            } else if (boundary == 1) {
                testNonce = unhex(v.rekey_snonce);
                input(ic, ni, unhex(v.rekey1), 1);
                assert(!ni.ni_rsn_group_ack_pending && sent.size() == 3);
                input(ic, ni, unhex(v.rotate1), 1);
                assert(sent.size() == 3 && keyInstalls == 3);
            } else {
                driver.iwm_native_clear_keys();
                assert(!ni.ni_rsn_group_ack_pending && !liveKeys);
                for (auto byte : ni.ni_rsn_group_ack_mic) assert(!byte);
                input(ic, ni, unhex(v.rotate1), 1);
                assert(sent.size() == 2 && !liveKeys);
            }
            cases += 2;
        }
        // A negotiated PMF session must reject authenticated M3 without IGTK
        // before acknowledging or installing any key. Exercise native teardown.
        setup(v, ic, ni);
        ni.ni_flags |= IEEE80211_NODE_MFP;
        input(ic, ni, m1, 1);
        input(ic, ni, m3, 1);
        assert(sent.size() == 1 && !keyInstalls && !liveKeys && !ni.ni_port_valid);
        assert(deauths == 1 && nativeDisconnects == 1 && ic.ic_state == IEEE80211_S_RUN);
        ++cases;
        // Failed GTK replacement must close the old controlled port and let
        // WCL own reconnect, rather than entering the legacy scan state.
        setup(v, ic, ni);
        input(ic, ni, m1, 1); input(ic, ni, m3, 1);
        auto replacementGtk = std::vector<uint8_t>{221,22,0,15,172,1,1,0};
        replacementGtk.insert(replacementGtk.end(), gtk.begin(), gtk.end());
        replacementGtk[8] ^= 1;
        installError = EIO;
        input(ic, ni, withPayload(g1, replacementGtk, ptk, true), 1);
        assert(deauths == 1 && nativeDisconnects == 1 && !ni.ni_port_valid);
        assert(ic.ic_state == IEEE80211_S_RUN && !driver.iwm_native_port_ready());
        ++cases;
        {
            setup(v, ic, ni); routeEnqueue = true;
            ieee80211_node *txNode = nullptr;
            std::vector<uint8_t> ordinary(64, 0); ordinary[12] = 8;
            auto *blocked = ieee80211_encap(&ic.ic_if, packet(ordinary, 14), &txNode);
            assert(!blocked && !txNode && ni.ni_refcnt == 1 && !liveMbufs);
            input(ic, ni, m1, 1);
            input(ic, ni, m3, 1);
            input(ic, ni, g1, 1);
            assert(sent80211.size() == 3 && ni.ni_refcnt == 1);
            for (unsigned index = 0; index < 3; ++index) {
                const auto &wire = sent80211[index]; const auto &ethernet = sent[index];
                assert(wire.size() == 24 + LLC_SNAPFRAMELEN + ethernet.size() - 14);
                assert(wire[0] == 8 && wire[1] == (index == 2 ? 0x41 : 1));
                assert(!memcmp(wire.data() + 4, ni.ni_bssid, 6));
                assert(!memcmp(wire.data() + 10, ethernet.data() + 6, 6));
                assert(!memcmp(wire.data() + 16, ethernet.data(), 6));
                assert(wire[22] == index * 16 && wire[23] == 0);
                const uint8_t snap[] = {0xaa, 0xaa, 3, 0, 0, 0, 0x88, 0x8e};
                assert(!memcmp(wire.data() + 24, snap, sizeof(snap)));
                assert(std::equal(wire.begin() + 32, wire.end(), ethernet.begin() + 14));
                ++cases;
            }
            auto *allowed = ieee80211_encap(&ic.ic_if, packet(ordinary, 14), &txNode);
            assert(allowed && txNode == &ni && ni.ni_refcnt == 2);
            assert(flatten(allowed)[1] == 0x41);
            mbuf_freem(allowed); ieee80211_release_node(&ic, txNode);
            failAt = allocations + 1;
            allowed = ieee80211_encap(&ic.ic_if, packet(ordinary, 14), &txNode);
            assert(!allowed && !txNode && ni.ni_refcnt == 1 && !liveMbufs);
            failAt = 0; cases += 3;
        }
        for (size_t split = 14; split <= m3.size(); split += 13) {
            setup(v, ic, ni);
            mbuf_list list;
            std::vector<uint8_t> ordinary(64, 0);
            ordinary[12] = 8; // IPv4, not an EAPOL exchange.
            inputEthernet(ic, ni, ordinary, 14, list);
            assert(list.packets.empty() && !liveMbufs && ic.ic_stats.is_rx_unauth == 1);
            inputEthernet(ic, ni, m1, std::min(split, m1.size()), list);
            assert(sent.size() == 1 && sent[0] == m2 && !driver.iwm_native_port_ready());
            assert(list.packets.empty() && !liveMbufs);
            inputEthernet(ic, ni, m3, split, list);
            assert(sent.size() == 2 && sent[1] == m4 && driver.iwm_native_port_ready());
            assert(list.packets.empty() && !liveMbufs && ic.ic_if.stats.inputPackets == 2);
            inputEthernet(ic, ni, ordinary, 14, list);
            assert(list.packets.size() == 1 && liveMbufs == 2);
            mbuf_freem(list.packets[0]); list.packets.clear();
            cases += 4;
        }
        for (size_t split = 0; split <= m3.size(); ++split) {
            setup(v, ic, ni);
            input(ic, ni, m1, std::min(split, m1.size()));
            assert(sent.size() == 1 && sent[0] == m2 && !ni.ni_port_valid && !keyInstalls);
            assert(!driver.iwm_native_port_ready());
            input(ic, ni, m3, split);
            assert(sent.size() == 2 && sent[1] == m4 && ni.ni_port_valid && keyInstalls == 2 && linkUps == 1);
            assert(driver.iwm_native_port_ready());
            assert(!memcmp(&ni.ni_ptk, ptk.data(), sizeof(ni.ni_ptk)));
            assert(!memcmp(ni.ni_pairwise_key.k_key, ptk.data() + 32, 16));
            assert(!memcmp(ic.ic_nw_keys[1].k_key, gtk.data(), 16));
            ni.ni_pairwise_key.k_tsc = 41; ic.ic_nw_keys[1].k_rsc[0] = 42;
            input(ic, ni, m3, split);
            assert(sent.size() == 2 && keyInstalls == 2 && ic.ic_stats.is_rx_eapol_replay == 1);
            auto retry = m3;
            BE_WRITE_8(keyOf(retry)->replaycnt, uint64_t(3)); sign(retry, ptk);
            input(ic, ni, retry, split);
            assert(sent.size() == 3 && keyInstalls == 2);
            assert(ni.ni_pairwise_key.k_tsc == 41 && ic.ic_nw_keys[1].k_rsc[0] == 42);
            // Fresh group exchange after independent initial handshake.
            setup(v, ic, ni); input(ic, ni, m1, 1); input(ic, ni, m3, split);
            input(ic, ni, g1, std::min(split, g1.size()));
            assert(sent.size() == 3 && sent[2] == g2 && keyInstalls == 2 && ni.ni_replaycnt == 3);
            cases += 5;
        }
        for (unsigned body = 0; body < 95; ++body) {
            setup(v, ic, ni); auto malformed = m1;
            BE_WRITE_2(keyOf(malformed)->len, body);
            BE_WRITE_2(keyOf(malformed)->paylen, 65535);
            input(ic, ni, malformed, 1);
            assert(sent.empty() && !ni.ni_flags && !keyInstalls); ++cases;
        }
        for (size_t length = 0; length < m1.size(); ++length) {
            setup(v, ic, ni); auto shortFrame = m1; shortFrame.resize(length);
            input(ic, ni, shortFrame, length / 2);
            assert(sent.empty() && !ni.ni_flags && !keyInstalls); ++cases;
        }
        for (unsigned failure = 1; failure <= 6; ++failure) {
            setup(v, ic, ni); failAt = failure;
            input(ic, ni, m1, 1); assert(!ni.ni_port_valid && !keyInstalls); ++cases;
            setup(v, ic, ni); input(ic, ni, m1, 1); allocations = 0; failAt = failure;
            input(ic, ni, m3, 1);
            if (sent.size() == 1) assert(!ni.ni_port_valid && !keyInstalls);
            ++cases;
        }
        for (size_t offset : {size_t(20), size_t(31), size_t(95), m3.size() - 1}) {
            setup(v, ic, ni); input(ic, ni, m1, 1); auto bad = m3; bad[offset] ^= 1;
            input(ic, ni, bad, 1); assert(!ni.ni_port_valid && !keyInstalls && sent.size() == 1); ++cases;
        }
        setup(v, ic, ni); input(ic, ni, m1, 1); auto badWrap = m3;
        badWrap.back() ^= 1; sign(badWrap, ptk); input(ic, ni, badWrap, 1);
        assert(!ni.ni_port_valid && !keyInstalls && sent.size() == 1); ++cases;
        setup(v, ic, ni); input(ic, ni, m1, 1); queueError = ENOBUFS;
        input(ic, ni, m3, 1); assert(!ni.ni_port_valid && !keyInstalls); ++cases;
        setup(v, ic, ni); input(ic, ni, m1, 1); installError = EIO;
        input(ic, ni, m3, 1); assert(!ni.ni_port_valid && deauths == 1); ++cases;
        for (unsigned length = 0; length < 80; ++length) {
            auto bytes = m3; auto *key = keyOf(bytes); BE_WRITE_2(key->paylen, length);
            if (length < 24 || (length & 7)) {
                assert(ieee80211_eapol_key_decrypt(key, ptk.data() + 16)); ++cases;
            }
        }
        auto bytes = m3; auto *key = keyOf(bytes); const auto wrapped = BE_READ_2(key->paylen);
        assert(!ieee80211_eapol_key_decrypt(key, ptk.data() + 16));
        assert(BE_READ_2(key->paylen) == wrapped - 8);
        for (unsigned i = 0; i < 8; ++i) assert(reinterpret_cast<uint8_t *>(key + 1)[wrapped - 8 + i] == 0);
        ++cases;
        const auto *data = reinterpret_cast<const uint8_t *>(key + 1);
        std::vector<uint8_t> clear(data, data + testAPRSN.size() + 24);
        for (unsigned split : {1u, 14u, 100u, 224u}) {
            setup(v, ic, ni); input(ic, ni, m1, 1);
            auto extended = clear;
            // A valid unknown IE makes M3 exceed the kernel pullup ceiling.
            extended.insert(extended.end(), {127, 128});
            extended.resize(extended.size() + 128, 0);
            auto longM3 = withPayload(m3, extended, ptk, true);
            assert(longM3.size() > 224);
            input(ic, ni, longM3, split);
            assert(sent.size() == 2 && sent[1] == m4 && driver.iwm_native_port_ready());
            auto saved = ItlEapol::capture(&ic, &ic.ic_if, driver.iwm_native_port_ready());
            assert(saved.eapol.rxM1 == 1 && saved.eapol.rxM3 == 1);
            assert(saved.eapol.txM2 == 1 && saved.eapol.txM4 == 1 && !saved.eapol.txError);
            assert(saved.eapol.pairCalls == 1 && !saved.eapol.pairResult);
            assert(saved.eapol.groupCalls == 1 && !saved.eapol.groupResult);
            assert(saved.port && saved.ready && saved.pairPresent && saved.groupMask == 2);
            driver.iwm_native_clear_keys();
            assert(saved.port && saved.pairPresent && !ni.ni_port_valid && !liveKeys);
            ++cases;
        }
        for (unsigned variant = 0; variant < 7; ++variant) {
            setup(v, ic, ni); input(ic, ni, m1, 1);
            auto second = testRSN;
            if (variant == 0) second = {IEEE80211_ELEMID_RSN, 0};
            if (variant == 1) second[2] = 2; // Unsupported RSN version.
            if (variant == 2) second[7] = 255; // Unknown group cipher.
            if (variant == 3) second[8] = 255; // Pairwise count exceeds IE.
            if (variant == 4) second[19] = 1; // Different AKM.
            if (variant == 5) second[13] = 2; // Unsupported TKIP pairwise.
            auto payload = clear;
            payload.insert(payload.end(), second.begin(), second.end());
            input(ic, ni, withPayload(m3, payload, ptk, true), 1);
            if (variant == 6) {
                assert(sent.size() == 2 && keyInstalls == 2 && driver.iwm_native_port_ready());
            } else {
                assert(sent.size() == 1 && !keyInstalls && !ni.ni_port_valid && deauths == 1);
                assert(ni.ni_rsncipher == IEEE80211_CIPHER_CCMP && !ni.ni_replaycnt_ok);
                for (auto byte : ni.ni_ptk.kck) assert(!byte);
            }
            ++cases;
        }
        for (unsigned failure = 0; failure < 15; ++failure) {
            setup(v, ic, ni); input(ic, ni, m1, 1);
            switch (failure) {
                case 0: driver.loop.gated = false; break;
                case 1: driver.nativeDetached = true; break;
                case 2: driver.nativeNet80211Attached = false; break;
                case 3: driver.commandTransportReady = false; break;
                case 4: driver.nativeStateError = EIO; break;
                case 5: ++driver.nativeStateSerial; break;
                case 6: driver.nativeFirmwareOwner.held = true; break;
                case 7: driver.com.sc_flags |= IWM_FLAG_SHUTDOWN; break;
                case 8: driver.com.sc_flags |= IWM_FLAG_HW_ERR; break;
                case 9: driver.com.sc_flags |= IWM_FLAG_RFKILL; break;
                case 10: ++driver.com.sc_generation; break;
                case 11: driver.nativeJoinProgress.clear(); break;
                case 12: ic.ic_bss = nullptr; break;
                case 13: ni.ni_bssid[5] ^= 1; break;
                case 14: ic.ic_opmode = IEEE80211_M_MONITOR; break;
            }
            input(ic, ni, m3, 1);
            assert(!driver.iwm_native_port_ready() && !ni.ni_port_valid && !liveKeys && deauths == 1);
            ++cases;
        }
        setup(v, ic, ni); input(ic, ni, m1, 1); input(ic, ni, m3, 1);
        assert(driver.iwm_native_port_ready());
        driver.iwm_native_clear_keys();
        assert(!driver.iwm_native_port_ready() && !liveKeys && !ni.ni_port_valid);
        for (auto byte : ic.ic_psk) assert(!byte);
        for (auto byte : ni.ni_pmk) assert(!byte);
        ++cases;
#ifndef SEQUOIA_TEST_IWX
        // IWX uses one TX watchdog; its actual watchdog is covered by the
        // separate IWX scan/stop/firmware integration fixture.
        for (unsigned qid = 0; qid < 31; ++qid) {
            setup(v, ic, ni); input(ic, ni, m1, 1); input(ic, ni, m3, 1);
            driver.com.sc_tx_timer[qid] = 15;
            assert(!driver.iwm_native_watchdog(100));
            assert(!driver.iwm_native_watchdog(1099) && driver.com.sc_tx_timer[qid] == 15);
            assert(!driver.iwm_native_watchdog(14999) && driver.com.sc_tx_timer[qid] == 1);
            assert(driver.iwm_native_port_ready());
            assert(driver.iwm_native_watchdog(15100) == ETIMEDOUT);
            assert(!driver.iwm_native_port_ready() && !ni.ni_port_valid && ic.ic_if.if_link_state == LINK_STATE_DOWN);
            assert(driver.iwm_native_watchdog(16000) == ETIMEDOUT);
            cases += 5;
        }
        for (unsigned scenario = 0; scenario < 7; ++scenario) {
            setup(v, ic, ni); input(ic, ni, m1, 1); input(ic, ni, m3, 1);
            assert(!driver.iwm_native_watchdog(1000));
            driver.com.sc_tx_timer[0] = 2;
            if (scenario == 0) {
                driver.com.sc_tx_timer[0] = 0; // Firmware completion cleared the queue timer.
                assert(!driver.iwm_native_watchdog(UINT64_MAX));
                assert(driver.iwm_native_port_ready());
            } else if (scenario == 1) {
                driver.com.sc_tx_timer[0] = 0; ic.ic_mgt_timer = 2;
                assert(driver.iwm_native_watchdog(3000) == ETIMEDOUT);
                assert(!driver.iwm_native_port_ready() && !ic.ic_mgt_timer);
            } else if (scenario == 2) {
                assert(driver.iwm_native_watchdog(999) == ETIMEDOUT);
                assert(!driver.iwm_native_port_ready());
            } else if (scenario == 3) {
                driver.nativeFirmwareOwner.held = true;
                assert(!driver.iwm_native_watchdog(30000) && !driver.nativeWatchdogActive);
                driver.nativeFirmwareOwner.held = false;
                assert(!driver.iwm_native_watchdog(60000) && driver.com.sc_tx_timer[0] == 2);
                assert(driver.iwm_native_watchdog(62000) == ETIMEDOUT);
            } else if (scenario == 4) {
                driver.loop.gated = false;
                assert(driver.iwm_native_watchdog(3000) == EWOULDBLOCK);
                assert(driver.com.sc_tx_timer[0] == 2 && ni.ni_port_valid);
            } else if (scenario == 5) {
                ++driver.nativeStateSerial;
                assert(!driver.iwm_native_watchdog(30000) && !driver.nativeWatchdogActive);
                driver.nativeStateCompleted = driver.nativeStateSerial;
                assert(!driver.iwm_native_watchdog(60000) && driver.com.sc_tx_timer[0] == 2);
            } else {
                ++driver.com.sc_generation;
                assert(driver.iwm_native_watchdog(3000) == ENETDOWN);
                assert(driver.com.sc_tx_timer[0] == 2);
            }
            ++cases;
        }
#endif
        for (unsigned bad = 0; bad < 4; ++bad) {
            setup(v, ic, ni); input(ic, ni, m1, 1);
            auto malformed = clear;
            if (bad == 0) malformed.insert(malformed.end(), {1, 255});
            if (bad == 1) malformed.insert(malformed.end(), {0xdd, 255});
            if (bad == 2) malformed.insert(malformed.end(), {0xdd, 0, 1});
            if (bad == 3) malformed[testAPRSN.size() + 1] = 255;
            auto wire = withPayload(m3, malformed, ptk, true);
            input(ic, ni, wire, 1);
            assert(sent.size() == 1 && !keyInstalls && !ni.ni_port_valid && !ni.ni_replaycnt_ok);
            for (auto byte : ni.ni_ptk.kck) assert(!byte);
            ++cases;
        }
        setup(v, ic, ni); input(ic, ni, m1, 1);
        auto badLength = m3; BE_WRITE_2(keyOf(badLength)->keylen, 15); sign(badLength, ptk);
        input(ic, ni, badLength, 1);
        assert(sent.size() == 1 && !keyInstalls && !ni.ni_port_valid && deauths == 1); ++cases;
        setup(v, ic, ni);
        input(ic, ni, withPayload(m1, {0xdd, 255}, ptk, false), 1);
        assert(sent.empty() && !ni.ni_flags); ++cases;
        for (unsigned declared = 0; declared < 256; ++declared) {
            for (unsigned actual : {0u, 1u, declared, declared + 1}) {
                std::vector<uint8_t> payload = {1, uint8_t(declared)};
                payload.resize(actual + 2, 0);
                auto wire = withPayload(m1, payload, ptk, false);
                assert(bool(ieee80211_eapol_key_data_valid(keyOf(wire), 0)) == (actual == declared));
                ++cases;
            }
        }
        clearKeys(ic, ni);
    }
    printf("BSD receive routing + EAPOL/RSN/PRF/MIC/AES-wrap/native CCMP admission: %u cases, %zu independent OpenSSL initial/rekey vector sets passed; hardware/gate/mbufs/TX/allocation mocked\n", cases, sizeof(vectors) / sizeof(vectors[0]));
}
