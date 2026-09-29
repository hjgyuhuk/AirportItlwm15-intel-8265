#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <map>
#include <Airport/SequoiaSecurity.h>
#include "arc4.h"
#include "michael.h"
#include "aes.h"
#include "cmac.h"
#include "aes.h"
#include "HAL/ItlJoinEvent.h"

#define letoh16(x) (x)
#define letoh32(x) (x)
#define IO80211FAMILY_V2 1
#define __MAC_15_0 150000
#define __IO80211_TARGET __MAC_15_0
#define EAPOL_KEY_KEYMIC 0x100
#define EAPOL_KEY_ERROR 0x400
#define EAPOL_KEY_SECURE 0x200
#define IEEE80211_REASON_MIC_FAILURE 14
#define IEEE80211_ELEMID_MMIE 76
#define ticks 100
#define hz 100
#define _KASSERT(x) assert(x)
#include "sequoia_crypto_types.inc"

struct ieee80211_node {
    uint8_t ni_rsn_group_ack_pending = 0, ni_rsn_group_ack_mic[16] = {};
    unsigned ni_port_valid = 0, ni_flags = 0;
    uint8_t ni_ptk[64] = {}, ni_pmk[32] = {}, ni_nonce[32] = {};
    uint8_t ni_bssid[6] = {2, 3, 4, 5, 6, 7};
    unsigned ni_rsnprotos = IEEE80211_PROTO_RSN, ni_rsnakms = IEEE80211_AKM_PSK;
    ieee80211_cipher ni_rsncipher = IEEE80211_CIPHER_CCMP, ni_rsngroupcipher = IEEE80211_CIPHER_CCMP;
    ieee80211_key ni_pairwise_key = {};
};
struct ieee80211com {
    ieee80211_node *ic_bss = nullptr;
    ieee80211_key ic_nw_keys[IEEE80211_GROUP_NKID] = {};
    ieee80211_state ic_state = IEEE80211_S_RUN;
    ieee80211_opmode ic_opmode = IEEE80211_M_STA;
    unsigned ic_flags = IEEE80211_F_RSNON | IEEE80211_F_PSK, ic_xflags = 0;
    SequoiaWCL::TkipCountermeasures ic_native_tkip;
    uint64_t ic_tkip_micfail_last_tsc = 0;
    int ic_tkip_micfail = 0;
    uint8_t ic_psk[32] = {}, ic_nonce[32] = {};
    struct { unsigned if_flags = IFF_RUNNING; int if_link_state = LINK_STATE_UP; } ic_if;
    struct { unsigned is_tx_nombuf = 0, is_rx_nombuf = 0;
        unsigned is_ccmp_replays = 0, is_ccmp_dec_errs = 0;
        unsigned is_tkip_replays = 0, is_tkip_icv_errs = 0, is_tkip_mic_errs = 0, is_rx_locmicfail = 0,
            is_cmac_replays = 0, is_cmac_icv_errs = 0; } ic_stats;
};
struct Mbuf {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(256, 0xa5);
    size_t len = 0, total = 0;
    bool ext = false;
    Mbuf *next = nullptr;
};
using mbuf_t = Mbuf *;
static unsigned liveMbufs, liveKeys, allocations, failAt;
static std::map<void *, size_t> keySizes;
static bool allocationFails() { return ++allocations == failAt; }
static void *keyAlloc(size_t size) {
    if (allocationFails()) return nullptr;
    ++liveKeys;
    auto *p = std::malloc(size);
    keySizes[p] = size;
    memset(p, 0xa5, size);
    return p;
}
static void keyFree(void *ptr) {
    assert(ptr && liveKeys);
    const auto *bytes = static_cast<const uint8_t *>(ptr);
    for (size_t i = 0; i < keySizes.at(ptr); ++i) assert(bytes[i] == 0);
    --liveKeys;
    keySizes.erase(ptr);
    std::free(ptr);
}
static void clearSecret(void *ptr, size_t length) {
    auto *bytes = static_cast<volatile uint8_t *>(ptr);
    while (length--) *bytes++ = 0;
}
static void mbuf_freem(mbuf_t m) {
    while (m) { auto *next = m->next; assert(liveMbufs); --liveMbufs; delete m; m = next; }
}
static int mbuf_get(int, int, mbuf_t *out) {
    if (allocationFails()) return ENOMEM; // Error does not initialize caller storage.
    *out = new Mbuf;
    ++liveMbufs;
    return 0;
}
static size_t mbuf_len(mbuf_t m) { assert(m); return m->len; }
static size_t mbuf_pkthdr_len(mbuf_t m) { assert(m); return m->total; }
static mbuf_t mbuf_next(mbuf_t m) { assert(m); return m->next; }
static int mbuf_type(mbuf_t) { return 1; }
static void mbuf_setlen(mbuf_t m, size_t size) { assert(size <= m->bytes.size()); m->len = size; }
static void mbuf_pkthdr_setlen(mbuf_t m, size_t size) { m->total = size; }
static void mbuf_setnext(mbuf_t m, mbuf_t next) { assert(!m->next); m->next = next; }
static size_t mbuf_get_mhlen() { return 256; }
static size_t mbuf_get_mlen() { return 256; }
static size_t mbuf_get_minclsize() { return 256; }
static size_t mbuf_trailingspace(mbuf_t m) { return m->bytes.size() - m->len; }
static unsigned mbuf_flags(mbuf_t m) { return m->ext ? 1 : 0; }
static int m_dup_pkthdr(mbuf_t out, mbuf_t in, int) {
    if (allocationFails()) return ENOMEM;
    out->total = in->total;
    return 0;
}
static int mbuf_mclget(int, int, mbuf_t *m) {
    if (allocationFails()) return ENOMEM;
    (*m)->bytes.resize(2048, 0xa5); (*m)->ext = true;
    return 0;
}
static int mbuf_copydata(mbuf_t m, size_t offset, size_t length, void *out) {
    auto *dst = static_cast<uint8_t *>(out);
    while (m && offset >= m->len) { offset -= m->len; m = m->next; }
    while (length && m) {
        size_t count = std::min(length, m->len - offset);
        memcpy(dst, m->bytes.data() + offset, count);
        dst += count; length -= count; m = m->next; offset = 0;
    }
    return length ? EINVAL : 0;
}
static int mbuf_pullup(mbuf_t *m, size_t length) {
    if (allocationFails() || length > (*m)->total) {
        mbuf_freem(*m); *m = nullptr; return ENOMEM;
    }
    auto *n = new Mbuf; ++liveMbufs;
    n->total = n->len = (*m)->total; n->bytes.resize(n->len);
    assert(!mbuf_copydata(*m, 0, n->len, n->bytes.data()));
    mbuf_freem(*m); *m = n;
    return 0;
}
#define MBUF_DONTWAIT 0
#define MBUF_EXT 1
#define MCLBYTES 2048
#define mtod(m, type) reinterpret_cast<type>((m)->bytes.data())
#define malloc(size, pool, flags) keyAlloc(size)
#define free(ptr) keyFree(ptr)
#define explicit_bzero(ptr, size) clearSecret(ptr, size)
#define min(a, b) std::min((a), (b))
#define XYLog(...) ((void)0)
static int ieee80211_wep_set_key(ieee80211com *, ieee80211_key *) { assert(false); return EINVAL; }
static void ieee80211_wep_delete_key(ieee80211com *, ieee80211_key *) { assert(false); }
static uint64_t securityNow;
static unsigned micReports, disconnects, legacyScans;
static uint64_t airport_up_time() { return securityNow; }
static int ieee80211_send_eapol_key_req(ieee80211com *, ieee80211_node *, unsigned, uint64_t) {
    ++micReports; return 0;
}
static void ieee80211_set_link_state(ieee80211com *ic, int state) { ic->ic_if.if_link_state = state; }
static void ieee80211_notify_native_join(ieee80211com *, ieee80211_node *, ItlJoin::Stage stage, unsigned reason) {
    assert(stage == ItlJoin::Stage::Deauth && reason == IEEE80211_REASON_MIC_FAILURE); ++disconnects;
}
#undef IEEE80211_SEND_MGMT
#define IEEE80211_SEND_MGMT(...) ((void)0)
static void ieee80211_new_state(ieee80211com *, int, int) { ++legacyScans; }
static uint32_t ether_crc32_le_update(uint32_t crc, const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8; ++b) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return crc;
}
static void mbuf_adj(mbuf_t m, int amount) {
    assert(amount <= 0 && m->next == nullptr && size_t(-amount) <= m->len);
    m->len += amount; m->total += amount;
}
#define __packed __attribute__((packed))
static inline int ieee80211_has_qos(const ieee80211_frame *);
static inline uint16_t ieee80211_get_qos(const ieee80211_frame *);
static uint16_t swap16(uint16_t v) { return uint16_t(v << 8 | v >> 8); }
static uint64_t readLE(const uint8_t *p, unsigned n) {
    uint64_t v = 0; for (unsigned i = 0; i < n; ++i) v |= uint64_t(p[i]) << (8 * i); return v;
}
static void writeLE(uint8_t *p, uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) p[i] = uint8_t(v >> (8 * i));
}
#define LE_READ_6(p) readLE(p, 6)
#define LE_WRITE_6(p, v) writeLE(p, v, 6)
#define LE_WRITE_2(p, v) writeLE(p, v, 2)
unsigned ieee80211_get_hdrlen(const ieee80211_frame *);
void ieee80211_michael_mic_failure(ieee80211com *, uint64_t);
#include "sequoia_tkip.inc"
#include "sequoia_bip.inc"
#include "sequoia_crypto_functions.inc"
#undef malloc
#undef free
#undef min

class ItlIwm {
public:
    struct { ieee80211com sc_ic; unsigned sc_flags = 0; int sc_generation = 3; } com;
    struct { bool gated = true; bool inGate() const { return gated; } } loop;
    struct { bool held = false; bool busy() const { return held; } } nativeFirmwareOwner;
    bool trial = false, nativeDetached = false, nativeNet80211Attached = true, commandTransportReady = true;
    int nativeStateError = 0;
    uint64_t nativeStateSerial = 3, nativeStateCompleted = 3;
    ItlJoin::Progress nativeJoinProgress;
    auto getMainWorkLoop() -> decltype(&loop) { return &loop; }
    bool isHardwareTrial() const { return trial; }
    int iwm_native_set_key(ieee80211com *, ieee80211_node *, ieee80211_key *);
    bool iwm_native_port_ready();
    void iwm_native_clear_keys();
};
#include "../itlwm/hal_iwm/native_key_sequoia.inc"

struct Vector { uint64_t pn; unsigned kid, tid; const char *plain, *wire; };
static const Vector vectors[] = {
#include "sequoia_crypto_vectors.inc"
};
static std::vector<uint8_t> unhex(const char *s) {
    std::vector<uint8_t> out;
    assert(strlen(s) % 2 == 0);
    while (*s) { unsigned byte; assert(sscanf(s, "%2x", &byte) == 1); out.push_back(byte); s += 2; }
    return out;
}
static mbuf_t packet(const std::vector<uint8_t> &bytes, size_t split) {
    assert(split <= bytes.size());
    auto *m = new Mbuf, *empty = new Mbuf, *tail = new Mbuf;
    liveMbufs += 3;
    m->total = bytes.size(); m->len = split;
    m->bytes.assign(bytes.begin(), bytes.begin() + split);
    m->next = empty; empty->next = tail;
    tail->len = bytes.size() - split;
    tail->bytes.assign(bytes.begin() + split, bytes.end());
    return m;
}
static std::vector<uint8_t> flatten(mbuf_t m) {
    assert(m);
    std::vector<uint8_t> bytes(m->total);
    assert(!mbuf_copydata(m, 0, bytes.size(), bytes.data()));
    size_t sum = 0;
    for (auto *part = m; part; part = part->next) sum += part->len;
    assert(sum == bytes.size());
    return bytes;
}
static ieee80211_key makeKey(ieee80211com &ic, unsigned id) {
    ieee80211_key key = {};
    key.k_len = 16; key.k_id = id; key.k_cipher = IEEE80211_CIPHER_CCMP;
    for (size_t i = 0; i < 16; ++i) key.k_key[i] = i;
    assert(!ieee80211_ccmp_set_key(&ic, &key));
    return key;
}

static void securityTests() {
    unsigned count = 0;
    // IEEE 802.11i TKIP key-mixing example: TK 00..0f, TA 10:22:33:44:55:66, TSC 0.
    uint8_t tk[16], ta[6] = {0x10,0x22,0x33,0x44,0x55,0x66};
    for (unsigned i = 0; i < 16; ++i) tk[i] = i;
    uint16_t p1[5], seed[8];
    Phase1(p1, tk, ta, 0);
    const uint16_t expected[] = {0x3dd2,0x016e,0x76f4,0x8697,0xb2e8};
    assert(!memcmp(p1, expected, sizeof(p1)));
    Phase2(reinterpret_cast<uint8_t *>(seed), tk, p1, 0);
    const auto expectedSeed = unhex("00200033ea8d2f60ca6d1374234a660b");
    assert(!memcmp(seed, expectedSeed.data(), 16)); count += 2;
    // RFC 4493 examples 1-4 (AES-CMAC); independent expected tags.
    const auto cmacKey = unhex("2b7e151628aed2a6abf7158809cf4f3c");
    const auto message = unhex("6bc1bee22e409f96e93d7e117393172a" "ae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52ef" "f69f2445df4f9b17ad2b417be66c3710");
    const size_t lengths[] = {0,16,40,64};
    const char *tags[] = {"bb1d6929e95937287fa37d129b756746", "070a16b46b4d4144f79bdd9dd04a287c",
        "dfa66747de9ae63030ca32611497c827", "51f0bebf7e3b9d92fc49741779363cfe"};
    for (unsigned i = 0; i < 4; ++i) for (size_t split = 0; split <= lengths[i]; ++split) {
        AES_CMAC_CTX ctx; uint8_t tag[16];
        AES_CMAC_SetKey(&ctx, cmacKey.data()); AES_CMAC_Init(&ctx);
        AES_CMAC_Update(&ctx, message.data(), split);
        AES_CMAC_Update(&ctx, message.data() + split, lengths[i] - split);
        AES_CMAC_Final(tag, &ctx); assert(!memcmp(tag, unhex(tags[i]).data(), 16)); ++count;
    }
    // Run the actual TKIP frames with both STA MIC directions. AP input is
    // synthesized by swapping the directional Michael subkeys, never live keys.
    for (unsigned mode = 0; mode < 4; ++mode) for (unsigned length : {0u,1u,15u,16u,239u,511u,1500u}) {
        ieee80211com ic; ieee80211_key tx = {}, rx = {};
        tx.k_cipher = rx.k_cipher = IEEE80211_CIPHER_TKIP; tx.k_len = rx.k_len = 32;
        for (unsigned i = 0; i < 32; ++i) tx.k_key[i] = i;
        memcpy(rx.k_key, tx.k_key, 16); memcpy(rx.k_key + 16, tx.k_key + 24, 8);
        memcpy(rx.k_key + 24, tx.k_key + 16, 8);
        assert(!ieee80211_set_key(&ic,nullptr,&tx)); assert(!ieee80211_set_key(&ic,nullptr,&rx));
        size_t hdr = 24 + ((mode & 1) ? 2 : 0);
        std::vector<uint8_t> plain(hdr + length);
        plain[0] = (mode & 1) ? 0x88 : 0x08; plain[1] = 0x40 | ((mode & 2) ? 2 : 1);
        for (size_t i = 4; i < 22; ++i) plain[i] = uint8_t(i * 7);
        if (mode & 1) plain[24] = 5;
        for (size_t i = hdr; i < plain.size(); ++i) plain[i] = uint8_t(i * 13);
        auto wirePacket = ieee80211_tkip_encrypt(&ic,packet(plain,hdr),&tx);
        assert(wirePacket); auto wire = flatten(wirePacket); mbuf_freem(wirePacket);
        plain[1] &= ~0x40;
        for (size_t split = 0; split <= wire.size(); ++split) {
            memset(rx.k_rsc,0,sizeof(rx.k_rsc));
            auto result = ieee80211_tkip_decrypt(&ic,packet(wire,split),&rx);
            assert(result && flatten(result) == plain); mbuf_freem(result);
            assert(!ieee80211_tkip_decrypt(&ic,packet(wire,wire.size()),&rx)); ++count;
        }
        // Corrupt ciphertext, including MIC/ICV; failed authentication must
        // not advance receive sequence state.
        for (size_t i = hdr + 8; i < wire.size(); ++i) {
            auto bad = wire; bad[i] ^= 0x80; memset(rx.k_rsc,0,sizeof(rx.k_rsc));
            assert(!ieee80211_tkip_decrypt(&ic,packet(bad,bad.size()),&rx));
            for (auto rsc : rx.k_rsc) assert(!rsc); ++count;
        }
        tx.k_tsc = 0xffffffffffffULL; plain[1] |= 0x40;
        assert(!ieee80211_tkip_encrypt(&ic,packet(plain,hdr),&tx));
        ieee80211_reset_key(&ic,&tx); ieee80211_reset_key(&ic,&rx);
        assert(!liveKeys && !liveMbufs);
    }
    for (unsigned length : {2u,4u,120u,240u,400u}) {
        ieee80211com ic; ieee80211_key k = {};
        k.k_cipher = IEEE80211_CIPHER_BIP; k.k_id = 4; k.k_len = 16;
        memcpy(k.k_key, cmacKey.data(),16); assert(!ieee80211_set_key(&ic,nullptr,&k));
        std::vector<uint8_t> plain(24 + length); plain[0] = 0xc0; plain[4] = 0xff;
        for (size_t i = 10; i < plain.size(); ++i) plain[i] = uint8_t(i * 7);
        auto m = ieee80211_bip_encap(&ic,packet(plain,plain.size()),&k);
        assert(m); auto wire = flatten(m); mbuf_freem(m); assert(readLE(wire.data()+wire.size()-14,6) == 1);
        auto contiguous = [&](const std::vector<uint8_t> &v) {
            auto *p = packet(v,v.size()); assert(!mbuf_pullup(&p,v.size())); return p;
        };
        for (size_t i = 24; i < wire.size(); ++i) {
            auto bad = wire; bad[i] ^= 1; k.k_mgmt_rsc = 0;
            assert(!ieee80211_bip_decap(&ic,contiguous(bad),&k)); assert(!k.k_mgmt_rsc); ++count;
        }
        k.k_mgmt_rsc = 0; m = ieee80211_bip_decap(&ic,contiguous(wire),&k);
        assert(m && flatten(m) == plain); mbuf_freem(m); assert(k.k_mgmt_rsc == 1);
        assert(!ieee80211_bip_decap(&ic,contiguous(wire),&k));
        k.k_tsc = 0xffffffffffffULL;
        assert(!ieee80211_bip_encap(&ic,contiguous(plain),&k));
        ieee80211_reset_key(&ic,&k); assert(!liveKeys && !liveMbufs); count += 3;
    }
    ieee80211com ic; ieee80211_node ni; ic.ic_bss = &ni;
    ni.ni_port_valid = 1; ni.ni_rsngroupcipher = IEEE80211_CIPHER_TKIP;
    securityNow = 0; ieee80211_michael_mic_failure(&ic,1);
    assert(ni.ni_port_valid && !disconnects);
    securityNow = 59999; ieee80211_michael_mic_failure(&ic,2);
    assert(!ni.ni_port_valid && disconnects == 1 && micReports == 2 && !legacyScans);
    assert(ic.ic_native_tkip.active(119998) && !ic.ic_native_tkip.active(119999));
    ni.ni_port_valid = 1; securityNow = 119999; ieee80211_michael_mic_failure(&ic,3);
    assert(ni.ni_port_valid && disconnects == 1);
    securityNow += 60000; ieee80211_michael_mic_failure(&ic,4);
    assert(ni.ni_port_valid && disconnects == 1);
    ++securityNow; ieee80211_michael_mic_failure(&ic,5);
    assert(!ni.ni_port_valid && disconnects == 2 && !legacyScans); count += 6;
    printf("TKIP/BIP real cipher and MIC lifecycle: %u cases passed\n",count);
}

int main() {
    securityTests();
    ieee80211com ic;
    unsigned cases = 0;
    for (const auto &v : vectors) {
        auto plain = unhex(v.plain), wire = unhex(v.wire);
        auto clear = plain; clear[1] &= ~IEEE80211_FC1_PROTECTED;
        auto key = makeKey(ic, v.kid);
        const size_t hdr = ieee80211_get_hdrlen(reinterpret_cast<const ieee80211_frame *>(plain.data()));
        for (size_t split : {size_t(0), size_t(1), size_t(23), size_t(24), hdr, plain.size() / 2, plain.size()}) {
            key.k_tsc = v.pn - 1;
            auto *encrypted = ieee80211_ccmp_encrypt(&ic, packet(plain, split), &key);
            assert(flatten(encrypted) == wire && key.k_tsc == v.pn);
            mbuf_freem(encrypted); assert(liveMbufs == 0); ++cases;
        }
        for (size_t split : {size_t(0), size_t(1), size_t(23), hdr, hdr + 7, hdr + 8, wire.size() - 8, wire.size() - 1, wire.size()}) {
            key.k_rsc[v.tid] = 0;
            auto *decrypted = ieee80211_ccmp_decrypt(&ic, packet(wire, split), &key);
            assert(flatten(decrypted) == clear && key.k_rsc[v.tid] == v.pn);
            mbuf_freem(decrypted);
            assert(!ieee80211_ccmp_decrypt(&ic, packet(wire, split), &key));
            assert(key.k_rsc[v.tid] == v.pn && liveMbufs == 0); cases += 2;
        }
        for (size_t offset : {size_t(4), size_t(10), hdr, hdr + 2, hdr + 3, wire.size() - 1}) {
            auto corrupt = wire; corrupt[offset] ^= 1;
            key.k_rsc[v.tid] = 0;
            assert(!ieee80211_ccmp_decrypt(&ic, packet(corrupt, 1), &key));
            assert(key.k_rsc[v.tid] == 0 && liveMbufs == 0); ++cases;
        }
        for (unsigned failure = 1; failure <= 30; ++failure) {
            allocations = 0; failAt = failure; key.k_tsc = v.pn - 1;
            auto *encrypted = ieee80211_ccmp_encrypt(&ic, packet(plain, 1), &key);
            if (encrypted) assert(flatten(encrypted) == wire);
            mbuf_freem(encrypted); assert(!liveMbufs);
            allocations = 0; key.k_rsc[v.tid] = 0;
            auto *decrypted = ieee80211_ccmp_decrypt(&ic, packet(wire, 1), &key);
            if (decrypted) assert(flatten(decrypted) == clear && key.k_rsc[v.tid] == v.pn);
            else assert(key.k_rsc[v.tid] == 0);
            mbuf_freem(decrypted); assert(!liveMbufs);
            failAt = 0; cases += 2;
        }
        key.k_tsc = 0xffffffffffffULL;
        assert(!ieee80211_ccmp_encrypt(&ic, packet(plain, 1), &key));
        assert(key.k_tsc == 0xffffffffffffULL && !liveMbufs); ++cases;
        auto *old = key.k_priv;
        assert(ieee80211_ccmp_set_key(&ic, &key) == EINVAL && key.k_priv == old);
        ieee80211_ccmp_delete_key(&ic, &key);
        ieee80211_ccmp_delete_key(&ic, &key);
        assert(!liveKeys); ++cases;
    }
    auto key = makeKey(ic, 0);
    for (size_t length = 0; length < 40; ++length) {
        std::vector<uint8_t> bytes(length, 0);
        if (length > 1) { bytes[0] = 0x88; bytes[1] = 0xc3; }
        assert(!ieee80211_ccmp_decrypt(&ic, packet(bytes, 0), &key)); ++cases;
    }
    auto plain = unhex(vectors[0].plain);
    for (int mismatch : {-1, 1}) {
        auto *m = packet(plain, 1); m->total += mismatch;
        assert(!ieee80211_ccmp_encrypt(&ic, m, &key)); ++cases;
    }
    for (uint8_t fc : {uint8_t(0x04), uint8_t(0x0c), uint8_t(0x09)}) {
        auto bad = plain; bad[0] = fc;
        assert(!ieee80211_ccmp_encrypt(&ic, packet(bad, 1), &key)); ++cases;
    }
    ieee80211_ccmp_delete_key(&ic, &key);
    for (unsigned length : {0u, 15u, 17u, 32u}) {
        key.k_len = length;
        assert(ieee80211_ccmp_set_key(&ic, &key) == EINVAL); ++cases;
    }
    key.k_len = 16; allocations = 0; failAt = 1;
    assert(ieee80211_ccmp_set_key(&ic, &key) == ENOMEM && !key.k_priv);
    assert(!liveKeys && !liveMbufs);
    failAt = 0; ++cases;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        for (unsigned kid = 0; kid < 4; ++kid) {
            auto &group = ic.ic_nw_keys[kid];
            ieee80211_reset_key(&ic, &group);
            group.k_len = 16; group.k_id = kid; group.k_cipher = IEEE80211_CIPHER_CCMP;
            group.k_flags = IEEE80211_KEY_GROUP;
            memset(group.k_key, attempt, 16);
            assert(!ieee80211_set_key(&ic, nullptr, &group));
            group.k_tsc = 17; group.k_rsc[0] = 42;
            auto *ctx = group.k_priv;
            assert(!ieee80211_must_update_group_key(&group, group.k_key, 16));
            assert(group.k_priv == ctx && group.k_tsc == 17 && group.k_rsc[0] == 42);
            auto next = group; next.k_key[0] ^= 1;
            assert(ieee80211_must_update_group_key(&group, next.k_key, 16)); ++cases;
        }
        assert(liveKeys == 4);
    }
    for (auto &group : ic.ic_nw_keys) ieee80211_reset_key(&ic, &group);
    assert(!liveKeys);
    for (unsigned scenario = 0; scenario < 38; ++scenario) {
        ItlIwm driver;
        ieee80211_node peer;
        auto *state = &driver.com.sc_ic;
        state->ic_bss = &peer;
        auto *candidate = &peer.ni_pairwise_key;
        candidate->k_cipher = IEEE80211_CIPHER_CCMP; candidate->k_len = 16;
        auto event = ItlJoin::Event{ItlJoin::Stage::Auth, {}, 0};
        memcpy(event.bssid, peer.ni_bssid, 6);
        assert(driver.nativeJoinProgress.begin(peer.ni_bssid, 3));
        assert(driver.nativeJoinProgress.record(event, 3));
        event.stage = ItlJoin::Stage::Assoc;
        assert(driver.nativeJoinProgress.record(event, 3));
        ieee80211com foreign;
        switch (scenario) {
            case 0: break;
            case 1: driver.loop.gated = false; break;
            case 2: state = &foreign; break;
            case 3: state->ic_bss = nullptr; break;
            case 4: driver.trial = true; break;
            case 5: driver.nativeDetached = true; break;
            case 6: driver.nativeNet80211Attached = false; break;
            case 7: driver.commandTransportReady = false; break;
            case 8: driver.nativeStateError = EIO; break;
            case 9: ++driver.nativeStateSerial; break;
            case 10: driver.nativeFirmwareOwner.held = true; break;
            case 11: driver.com.sc_flags = IWM_FLAG_SHUTDOWN; break;
            case 12: driver.com.sc_flags = IWM_FLAG_HW_ERR; break;
            case 13: driver.com.sc_flags = IWM_FLAG_RFKILL; break;
            case 14: state->ic_if.if_flags = 0; break;
            case 15: state->ic_state = IEEE80211_S_ASSOC; break;
            case 16: state->ic_opmode = IEEE80211_M_MONITOR; break;
            case 17: ++driver.com.sc_generation; break;
            case 18: ++peer.ni_bssid[5]; break;
            case 19: driver.nativeJoinProgress.clear(); break;
            case 20: state->ic_flags &= ~IEEE80211_F_RSNON; break;
            case 21: state->ic_flags &= ~IEEE80211_F_PSK; break;
            case 22: peer.ni_rsnprotos = IEEE80211_PROTO_WPA; break;
            case 23: peer.ni_rsnakms = IEEE80211_AKM_8021X; break;
            case 24: peer.ni_rsncipher = IEEE80211_CIPHER_TKIP; break;
            case 25: peer.ni_rsngroupcipher = IEEE80211_CIPHER_TKIP; break;
            case 26: candidate->k_cipher = IEEE80211_CIPHER_TKIP; break;
            case 27: candidate->k_len = 15; break;
            case 28: candidate->k_id = 4; break;
            case 29: candidate->k_flags = IEEE80211_KEY_SWCRYPTO; break;
            case 30: candidate->k_id = 1; break;
            case 31: candidate->k_flags = IEEE80211_KEY_GROUP; break;
            case 32: candidate->k_flags = IEEE80211_KEY_TX; break;
            case 33: candidate = &state->ic_nw_keys[2]; candidate->k_flags = IEEE80211_KEY_GROUP;
                candidate->k_id = 2; candidate->k_len = 16; candidate->k_cipher = IEEE80211_CIPHER_CCMP; break;
            case 34: allocations = 0; failAt = 1; break;
            case 35: event.stage = ItlJoin::Stage::Deauth; event.status = 3;
                assert(driver.nativeJoinProgress.record(event, 3)); break;
            case 36: driver.nativeJoinProgress.clear();
                assert(driver.nativeJoinProgress.begin(peer.ni_bssid, 3)); break;
            case 37: assert(!ieee80211_ccmp_set_key(state, candidate)); break;
        }
        const auto before = *candidate;
        const int result = driver.iwm_native_set_key(state, &peer, candidate);
        if (scenario == 0 || scenario == 33) {
            assert(result == 0 && candidate->k_priv && (candidate->k_flags & IEEE80211_KEY_SWCRYPTO));
            ieee80211_reset_key(state, candidate);
        } else {
            assert(result != 0 && !memcmp(&before, candidate, sizeof(before)));
            if (scenario == 37) ieee80211_ccmp_delete_key(state, candidate);
        }
        if (scenario == 0) {
            assert(!driver.iwm_native_port_ready());
            candidate->k_cipher = IEEE80211_CIPHER_CCMP; candidate->k_len = 16;
            assert(!driver.iwm_native_set_key(state, &peer, candidate));
            peer.ni_flags = IEEE80211_NODE_TXRXPROT | IEEE80211_NODE_PMK;
            peer.ni_port_valid = 1;
            assert(!driver.iwm_native_port_ready());
            auto &gtk = state->ic_nw_keys[1];
            gtk.k_cipher = IEEE80211_CIPHER_CCMP; gtk.k_len = 16; gtk.k_id = 1; gtk.k_flags = IEEE80211_KEY_GROUP;
            assert(!driver.iwm_native_set_key(state, &peer, &gtk));
            assert(driver.iwm_native_port_ready());
            peer.ni_port_valid = 0; assert(!driver.iwm_native_port_ready()); peer.ni_port_valid = 1;
            ++driver.com.sc_generation; assert(!driver.iwm_native_port_ready()); --driver.com.sc_generation;
            peer.ni_flags &= ~IEEE80211_NODE_TXPROT; assert(!driver.iwm_native_port_ready()); peer.ni_flags |= IEEE80211_NODE_TXPROT;
            candidate->k_flags &= ~IEEE80211_KEY_SWCRYPTO; assert(!driver.iwm_native_port_ready()); candidate->k_flags |= IEEE80211_KEY_SWCRYPTO;
            gtk.k_id = 2; assert(!driver.iwm_native_port_ready()); gtk.k_id = 1;
            state->ic_if.if_link_state = 0; assert(!driver.iwm_native_port_ready()); state->ic_if.if_link_state = LINK_STATE_UP;
            memset(peer.ni_ptk, 0x5a, sizeof(peer.ni_ptk)); memset(peer.ni_pmk, 0x5a, sizeof(peer.ni_pmk));
            memset(peer.ni_nonce, 0x5a, sizeof(peer.ni_nonce)); memset(state->ic_psk, 0x5a, sizeof(state->ic_psk));
            memset(state->ic_nonce, 0x5a, sizeof(state->ic_nonce));
            driver.iwm_native_clear_keys(); driver.iwm_native_clear_keys();
            assert(!peer.ni_port_valid && !(peer.ni_flags & (IEEE80211_NODE_TXRXPROT | IEEE80211_NODE_PMK)));
            assert(!peer.ni_pairwise_key.k_priv && peer.ni_pairwise_key.k_cipher == IEEE80211_CIPHER_NONE);
            for (auto byte : peer.ni_ptk) assert(!byte);
            for (auto byte : peer.ni_pmk) assert(!byte);
            for (auto byte : peer.ni_nonce) assert(!byte);
            for (auto byte : state->ic_psk) assert(!byte);
            for (auto byte : state->ic_nonce) assert(!byte);
            state->ic_flags = 0;
            assert(driver.iwm_native_port_ready());
            state->ic_flags = IEEE80211_F_WEPON; assert(!driver.iwm_native_port_ready());
            cases += 12;
        }
        failAt = 0;
        assert(!liveKeys && !liveMbufs); ++cases;
    }
    printf("BSD CCMP + actual AES/key admission: %u cases, 120 independent OpenSSL vectors passed\n", cases);
}
