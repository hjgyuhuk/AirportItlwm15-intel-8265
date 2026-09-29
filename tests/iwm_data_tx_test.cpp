#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <memory>

#define IO80211FAMILY_V2
#define __MAC_15_0 150000
#define __IO80211_TARGET __MAC_15_0
#define __packed __attribute__((packed))
#define htole16(x) (x)
#define htole32(x) (x)
#define le16toh(x) (x)
#define letoh16(x) (x)
#define le32toh(x) (x)
#define le16_to_cpu(x) (x)
#define le32_to_cpu(x) (x)
#define LE_READ_2(p) (uint16_t((p)[0]) | (uint16_t((p)[1]) << 8))
#define _ALIGN(x, y) (((x) + (y) - 1) & ~((y) - 1))
#define MIN(x, y) std::min<size_t>(x, y)
#define XYLog(...) ((void)0)
#define DPRINTFN(...) ((void)0)
#define DPRINTF(...) ((void)0)
#define KASSERT(value, message) assert(value)
#define ETHER_ADDR_LEN 6
using bus_addr_t = uint64_t;
using u8 = uint8_t;
using s8 = int8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using s32 = int32_t;
using __le32 = uint32_t;
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BUILD_BUG_ON(test) static_assert(!(test), #test)
#define WARN_ON(test) ((void)(test))
#define WARN_ON_ONCE(test) WARN_ON(test)
enum { EDCA_AC_BE, EDCA_AC_BK, EDCA_AC_VI, EDCA_AC_VO };
static uint32_t le32_to_cpup(const uint32_t *p) { uint32_t n; memcpy(&n, p, 4); return n; }
#include "iwm_data_tx_types.inc"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-variable-sized-type-not-at-end"
#pragma clang diagnostic ignored "-Wunused-function"
#include "../itlwm/hal_iwm/if_iwmreg.h"
#pragma clang diagnostic pop
#define isset(bytes, bit) ((bytes)[(bit) / 8] & (1 << ((bit) % 8)))
constexpr int IFF_UP = 1, NL80211_BAND_2GHZ = 0, NL80211_BAND_5GHZ = 1;
constexpr int IEEE80211_CIPHER_CCMP = 8;
struct ieee80211_channel { unsigned ic_flags = IEEE80211_CHAN_2GHZ; };
struct ieee80211_key {
    unsigned k_flags = IEEE80211_KEY_SWCRYPTO, k_cipher = IEEE80211_CIPHER_CCMP, k_len = 16, k_id = 0;
    uint64_t k_tsc = 1;
    uint8_t k_key[16] = {};
};
struct ieee80211_tx_ba { int ba_state = 0; uint16_t ba_winstart = 0; uint64_t ba_bitmap = 0; };
struct ieee80211_node {
    unsigned ni_refcnt = 1;
    ieee80211_tx_ba ni_tx_ba[16];
    int ni_txmcs = 0, ni_txrate = 1;
    ieee80211_channel *ni_chan = nullptr;
};
struct iwm_node : ieee80211_node { ieee80211_node &in_ni = *this; uint8_t in_macaddr[6] = {2, 1, 2, 3, 4, 5}; };
struct ieee80211com {
    struct { int if_flags = IFF_UP; } ic_if;
    int ic_rtsthreshold = 2347;
    unsigned ic_flags = 0;
    int ic_state = 4;
    int ic_fixed_mcs = -1;
    ieee80211_node *ic_bss = nullptr;
};
constexpr int IEEE80211_S_RUN = 4;
static unsigned bars = 0, windowMoves = 0, statuses = 0;
static void ieee80211_tx_compressed_bar(ieee80211com *, ieee80211_node *node, int, uint16_t) {
    assert(node && node->ni_refcnt); ++bars;
}
static void ieee80211_output_ba_move_window(ieee80211com *, ieee80211_node *node, int tid, uint16_t ssn) {
    assert(node && node->ni_refcnt); node->ni_tx_ba[tid].ba_winstart = ssn; ++windowMoves;
}
static bool ieee80211_is_mgmt(uint16_t fc) { return (fc & 0x0c) == 0; }
static bool ieee80211_is_data_qos(uint16_t fc) { return (fc & 0x8c) == 0x88; }
static __attribute__((unused)) ieee80211_node *ieee80211_ref_node(ieee80211_node *node) { assert(node); ++node->ni_refcnt; return node; }
static void ieee80211_release_node(ieee80211com *, ieee80211_node *node) {
    assert(node && node->ni_refcnt); --node->ni_refcnt;
}
struct Buffer { std::vector<uint8_t> bytes; size_t offset = 0; };
using mbuf_t = Buffer *;
static unsigned liveMbufs = 0, encryptions = 0;
static bool missingKey = false, encryptionFail = false;
static ieee80211_key key;
#define mtod(m, type) reinterpret_cast<type>((m)->bytes.data() + (m)->offset)
static size_t mbuf_pkthdr_len(mbuf_t m) { return m->bytes.size() - m->offset; }
static void mbuf_adj(mbuf_t m, size_t n) { assert(n <= mbuf_pkthdr_len(m)); m->offset += n; }
static void mbuf_freem(mbuf_t m) { assert(m && liveMbufs); --liveMbufs; delete m; }
static unsigned ieee80211_get_hdrlen(const ieee80211_frame *frame) { return ieee80211_has_qos(frame) ? 26 : 24; }
static ieee80211_key *ieee80211_get_txkey(ieee80211com *, ieee80211_frame *, ieee80211_node *) {
    return missingKey ? nullptr : &key;
}
static mbuf_t ieee80211_encrypt(ieee80211com *, mbuf_t m, ieee80211_key *) {
    ++encryptions;
    if (encryptionFail) { mbuf_freem(m); return nullptr; }
    // Crypto is separately vector-tested. Force relocation here to check that
    // Intel serialization reacquires the header after the cipher returns.
    auto *copy = new Buffer; ++liveMbufs; copy->bytes = m->bytes;
    const auto hdrlen = ieee80211_get_hdrlen(mtod(m, ieee80211_frame *));
    copy->bytes.insert(copy->bytes.begin() + hdrlen, 8, 0x55);
    copy->bytes.insert(copy->bytes.end(), 8, 0x66);
    mbuf_freem(m); return copy;
}
static int ieee80211_up_to_ac(ieee80211com *, int tid) { return tid & 3; }
struct IOPhysicalSegment { uint64_t location, length; };
struct Cursor {
    int count = 1;
    bool relocate = false;
    uint64_t address = 0x123456789ULL;
    int lengthAdjustment = 0;
    int getPhysicalSegmentsWithCoalesce(mbuf_t m, IOPhysicalSegment *segments, int capacity) {
        assert(capacity >= count);
        if (!count) return 0;
        if (relocate) {
            auto copy = m->bytes; m->bytes.swap(copy);
        }
        segments[0] = {address, mbuf_pkthdr_len(m) + lengthAdjustment};
        return count;
    }
};
struct Map { Cursor *cursor; };
static void bus_dmamap_destroy(void *, Map *) {}
constexpr unsigned BUS_DMASYNC_POSTREAD = 1, IWM_RBUF_SIZE = 4096;
static void bus_dmamap_sync(void *, Map *, size_t, size_t, unsigned) {}
struct iwm_rx_data { Map *map = nullptr; };
struct iwm_dma_info { void *vaddr = nullptr; size_t size = 0; };
struct iwm_tx_data {
    Map *map = nullptr;
    uint64_t scratch_paddr = 0, cmd_paddr = 0;
    mbuf_t m = nullptr;
    iwm_node *in = nullptr;
    int txmcs = 0, txrate = 0, totlen = 0;
    uint16_t fc = 0;
    ieee80211_tx_info info;
};
struct iwm_tx_ring {
    iwm_dma_info desc_dma, cmd_dma;
    iwm_tfd *desc = nullptr;
    iwm_device_cmd *cmd = nullptr;
    iwm_tx_data data[IWM_TX_RING_COUNT];
    int cur = 0, queued = 0, qid = 0, tail = 0;
};
struct iwm_tx_ba { iwm_node *wn = nullptr; unsigned lq_color = 0, tx_time = 0, rate_n_flags = 0; };
struct iwm_softc {
    void *sc_dmat = nullptr;
    int cmdqid = IWM_DQA_CMD_QUEUE, sc_device_family = 7000, sc_nic_locks = 0;
    ieee80211com sc_ic;
    uint8_t sc_enabled_capa[32] = {};
    iwm_tx_ba sc_tx_ba[16];
    iwm_tx_ring txq[IWM_MAX_QUEUES];
    struct { void *vaddr; } sched_dma;
    unsigned qfullmsk = 0;
    int sc_tx_timer[IWM_MAX_QUEUES] = {};
    uint8_t sc_mgmt_last_antenna_idx = 0, sc_tx_ant = 0;
};
static void iwl_mvm_hwrate_to_tx_status(uint32_t, ieee80211_tx_info *) {}
static void ieee80211_tx_status(iwm_softc *, ieee80211_tx_info *, int, uint16_t, int) { ++statuses; }
static void iwl_mvm_rs_tx_status(iwm_softc *, ieee80211_node *, int, ieee80211_tx_info *, bool) { ++statuses; }
struct iwl_rs_rate_info {};
struct ItlIwm {
    unsigned kicks = 0, lastKick = 0;
    int iwm_tx(iwm_softc *, mbuf_t, ieee80211_node *, int);
    void iwm_update_sched(iwm_softc *, int, int, uint8_t, uint16_t);
    void iwm_reset_tx_ring(iwm_softc *, iwm_tx_ring *);
    void iwm_free_tx_ring(iwm_softc *, iwm_tx_ring *);
    void iwm_txd_done(iwm_softc *, iwm_tx_data *);
    void iwm_rx_tx_cmd(iwm_softc *, iwm_rx_packet *, iwm_rx_data *);
    void iwm_rx_tx_cmd_single(iwm_softc *, iwm_tx_resp *, int, int);
    void iwm_rx_tx_ba_notif(iwm_softc *, iwm_rx_packet *, iwm_rx_data *);
    void iwm_ampdu_txq_advance(iwm_softc *, iwm_tx_ring *, int);
    void iwm_ampdu_rate_control(iwm_softc *, ieee80211_node *, iwm_tx_ring *,
        uint16_t, uint16_t, ieee80211_tx_info *, int, uint32_t);
    void iwm_ampdu_tx_done(iwm_softc *, iwm_cmd_header *, iwm_node *, iwm_tx_ring *,
        uint32_t, uint8_t, uint8_t, uint16_t, int, iwm_agg_tx_status *);
    void iwm_reset_sched(iwm_softc *, int, int, uint8_t) {}
    void iwm_toggle_tx_ant(iwm_softc *, uint8_t *ant) { *ant ^= 1; }
    void iwm_clear_oactive(iwm_softc *, iwm_tx_ring *) {}
    void iwm_dma_contig_free(iwm_dma_info *dma) { dma->vaddr = nullptr; dma->size = 0; }
    void iwm_nic_unlock(iwm_softc *sc) { assert(sc->sc_nic_locks); --sc->sc_nic_locks; }
    const iwl_rs_rate_info *iwm_tx_fill_cmd(iwm_softc *, iwm_node *, ieee80211_frame *, iwm_tx_cmd *) {
        return nullptr;
    }
    unsigned iwm_coex_tx_prio(iwm_softc *, ieee80211_frame *, int) { return 0; }
    void write(unsigned reg, unsigned value) { assert(reg == IWM_HBUS_TARG_WRPTR); ++kicks; lastKick = value; }
};
#undef IWM_WRITE
#define IWM_WRITE(sc, reg, value) write(reg, value)
constexpr int IWM_DEVICE_FAMILY_7000 = 7000;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "iwm_data_tx_methods.inc"
#pragma clang diagnostic pop

struct Fixture {
    std::unique_ptr<iwm_softc> storage{new iwm_softc};
    iwm_softc &sc = *storage;
    ItlIwm driver;
    Cursor cursor;
    Map map = {&cursor};
    iwm_node peer, foreign;
    ieee80211_channel channel;
    std::vector<iwm_agn_scd_bc_tbl> schedule{IWM_MAX_QUEUES};
    Fixture() {
        sc.cmdqid = IWM_CMD_QUEUE;
        sc.sc_ic.ic_bss = &peer;
        peer.ni_chan = &channel;
        sc.sched_dma.vaddr = schedule.data();
        for (int q = 0; q < IWM_MAX_QUEUES; ++q) {
            auto &ring = sc.txq[q]; ring.qid = q;
            ring.desc = new iwm_tfd[IWM_TX_RING_COUNT]();
            ring.cmd = new iwm_device_cmd[IWM_TX_RING_COUNT]();
            ring.desc_dma = {ring.desc, sizeof(*ring.desc) * IWM_TX_RING_COUNT};
            ring.cmd_dma = {ring.cmd, sizeof(*ring.cmd) * IWM_TX_RING_COUNT};
            for (auto &slot : ring.data) {
                slot.map = &map; slot.cmd_paddr = 0x234560000ULL; slot.scratch_paddr = 0x234560010ULL;
            }
        }
    }
    ~Fixture() {
        for (auto &ring : sc.txq) {
            for (auto &slot : ring.data) if (slot.m) mbuf_freem(slot.m);
            delete[] ring.desc; delete[] ring.cmd;
        }
        assert(!liveMbufs);
    }
};
static mbuf_t frame(bool qos, unsigned tid, bool protectedFrame = false) {
    auto *m = new Buffer; ++liveMbufs; m->bytes.resize((qos ? 26 : 24) + 64, 0x42);
    auto *wh = mtod(m, ieee80211_frame *); memset(wh, 0, 24);
    wh->i_fc[0] = qos ? 0x88 : 0x08; wh->i_fc[1] = protectedFrame ? 0x41 : 1;
    wh->i_addr1[0] = 2;
    if (qos) { m->bytes[24] = tid; m->bytes[25] = 0; }
    return m;
}
static void completion(Fixture &f, int qid, unsigned index, unsigned count, uint16_t ssn,
                       unsigned status, int truncate = 0) {
    const size_t payload = sizeof(iwm_tx_resp) + count * sizeof(iwm_agg_tx_status) + sizeof(uint32_t);
    std::vector<uint8_t> bytes(sizeof(iwm_rx_packet) + payload - truncate);
    auto *packet = reinterpret_cast<iwm_rx_packet *>(bytes.data());
    packet->len_n_flags = sizeof(packet->hdr) + payload - truncate;
    packet->hdr.qid = qid; packet->hdr.idx = index;
    auto *response = reinterpret_cast<iwm_tx_resp *>(packet->data);
    response->frame_count = count;
    response->ra_tid = qid >= IWM_FIRST_AGG_TX_QUEUE ? qid - IWM_FIRST_AGG_TX_QUEUE : IWM_TID_NON_QOS;
    if (count) response->status[0].status = status;
    if (truncate == 0) { uint32_t value = ssn; memcpy(response->status + count, &value, sizeof(value)); }
    iwm_rx_data data;
    f.driver.iwm_rx_tx_cmd(&f.sc, packet, &data);
}
static void blockAck(Fixture &f, uint16_t ssn, unsigned sent = 1, unsigned acked = 1, unsigned fault = 0) {
    std::vector<uint8_t> bytes(sizeof(iwm_rx_packet) + sizeof(iwm_ba_notif));
    auto *packet = reinterpret_cast<iwm_rx_packet *>(bytes.data());
    packet->len_n_flags = sizeof(packet->hdr) + sizeof(iwm_ba_notif);
    auto *notification = reinterpret_cast<iwm_ba_notif *>(packet->data);
    notification->sta_id = IWM_STATION_ID;
    memcpy(notification->sta_addr, f.peer.in_macaddr, 6);
    notification->scd_flow = IWM_FIRST_AGG_TX_QUEUE;
    notification->scd_ssn = ssn; notification->txed = sent; notification->txed_2_done = acked;
    if (fault == 1) notification->tid = 1;
    if (fault == 2) notification->sta_id++;
    if (fault == 3) notification->sta_addr[5] ^= 1;
    if (fault == 4) notification->scd_flow = IWM_LAST_AGG_TX_QUEUE + 1;
    iwm_rx_data data;
    f.driver.iwm_rx_tx_ba_notif(&f.sc, packet, &data);
}
int main()
{
    unsigned cases = 0;
    if (std::getenv("IWM_BA_REPRO")) {
        Fixture f;
        const int qid = IWM_FIRST_AGG_TX_QUEUE;
        auto &ring = f.sc.txq[qid];
        f.peer.ni_tx_ba[0].ba_state = IEEE80211_BA_AGREED;
        f.sc.sc_tx_ba[0].wn = &f.peer; f.peer.ni_refcnt = 2;
        assert(f.driver.iwm_tx(&f.sc, frame(true, 0), &f.peer, 0) == 0);
        blockAck(f, 2);
        assert(ring.queued == 1 && ring.tail == 0 && f.peer.ni_refcnt == 2);
        return 0;
    }
    if (const char *repro = std::getenv("IWM_COMPLETION_REPRO")) {
        Fixture f;
        const bool aggregation = std::strcmp(repro, "node") == 0;
        const int qid = aggregation ? IWM_FIRST_AGG_TX_QUEUE : 0;
        auto &ring = f.sc.txq[qid];
        ring.data[0].m = frame(aggregation, 0); ring.data[0].in = &f.peer;
        ring.queued = 1; ring.cur = 1; f.peer.ni_refcnt = 2;
        f.peer.ni_tx_ba[0].ba_state = IEEE80211_BA_AGREED;
        f.sc.sc_tx_ba[0].wn = &f.peer;
        completion(f, qid, 0, 1, 1, IWM_TX_STATUS_SUCCESS, aggregation ? 0 : 4);
        if (aggregation) assert(windowMoves == 1 && f.peer.ni_refcnt == 1);
        else assert(ring.queued == 1 && ring.tail == 0);
        return 0;
    }
    // The byte-count table must inspect this slot's TX payload, not the
    // command header at slot zero. Include wrap and duplicated-table slots.
    {
        Fixture f;
        for (int index : {0, 1, 63, 64, 255}) {
            for (unsigned cipher : {0u, unsigned(IWM_TX_CMD_SEC_CCM), unsigned(IWM_TX_CMD_SEC_TKIP), unsigned(IWM_TX_CMD_SEC_WEP)}) {
                auto &ring = f.sc.txq[5];
                memset(ring.cmd, 0xa5, sizeof(*ring.cmd) * IWM_TX_RING_COUNT);
                auto *tx = reinterpret_cast<iwm_tx_cmd *>(ring.cmd[index].data); tx->sec_ctl = cipher;
                f.driver.iwm_update_sched(&f.sc, 5, index, 0, 100);
                const unsigned extra = cipher == IWM_TX_CMD_SEC_CCM ? 8 : cipher == IWM_TX_CMD_SEC_TKIP ? 4 : cipher == IWM_TX_CMD_SEC_WEP ? 8 : 0;
                const unsigned expected = ((100 + 8 + extra + 3) / 4) | (IWM_STATION_ID << 12);
                assert(f.schedule[5].tfd_offset[index] == expected);
                if (index < IWM_TFD_QUEUE_SIZE_BC_DUP)
                    assert(f.schedule[5].tfd_offset[IWM_TFD_QUEUE_SIZE_MAX + index] == expected);
                ++cases;
            }
        }
    }
    for (bool dqa : {false, true}) {
        for (unsigned tid = 0; tid < 16; ++tid) {
            for (bool belongs : {false, true}) {
                Fixture f;
                if (dqa) f.sc.sc_enabled_capa[IWM_UCODE_TLV_CAPA_DQA_SUPPORT / 8] |= 1 << (IWM_UCODE_TLV_CAPA_DQA_SUPPORT % 8);
                f.sc.sc_tx_ba[tid].wn = belongs ? &f.peer : &f.foreign;
                f.peer.ni_tx_ba[tid].ba_state = IEEE80211_BA_AGREED;
                assert(f.driver.iwm_tx(&f.sc, frame(true, tid), &f.peer, EDCA_AC_BE) == 0);
                const bool aggregated = belongs && tid < 8;
                const unsigned qid = aggregated ? IWM_FIRST_AGG_TX_QUEUE + tid : dqa ? IWM_DQA_MIN_MGMT_QUEUE : EDCA_AC_BE;
                assert(f.driver.kicks == 1 && (f.driver.lastKick >> 8) == qid);
                const auto *tx = reinterpret_cast<const iwm_tx_cmd *>(f.sc.txq[qid].cmd[0].data);
                assert(tx->tid_tspec == (aggregated ? tid : IWM_TID_NON_QOS));
                assert(tx->len == 90 && tx->sec_ctl == 0 && f.sc.txq[qid].data[0].m);
                ++cases;
            }
        }
    }
    for (unsigned fault = 0; fault < 4; ++fault) {
        Fixture f; missingKey = fault == 1; encryptionFail = fault == 2;
        if (fault == 3) f.cursor.count = 0;
        const unsigned old = encryptions;
        const int result = f.driver.iwm_tx(&f.sc, frame(false, 0, true), &f.peer, 0);
        assert(result == (fault == 1 ? EINVAL : fault == 2 ? ENOBUFS : fault == 3 ? ENOMEM : 0));
        assert(f.driver.kicks == !fault && encryptions - old == (fault != 1));
        if (!fault) {
            const auto *tx = reinterpret_cast<const iwm_tx_cmd *>(f.sc.txq[0].cmd[0].data);
            assert(tx->len == 104 && tx->sec_ctl == 0 && f.sc.txq[0].data[0].fc == 0x4108);
        } else assert(!liveMbufs && !f.sc.txq[0].data[0].m);
        ++cases;
    }
    missingKey = encryptionFail = false;
    {
        Fixture f; f.cursor.relocate = true;
        assert(f.driver.iwm_tx(&f.sc, frame(false, 0), &f.peer, 0) == 0);
        assert(f.sc.txq[0].data[0].fc == 0x0108);
        ++cases;
    }
    for (unsigned fault = 0; fault < 8; ++fault) {
        Fixture f;
        if (fault == 0) f.cursor.address = 1ULL << 36;
        if (fault == 1) f.cursor.address = (1ULL << 36) - 63;
        if (fault == 2) f.cursor.lengthAdjustment = -1;
        if (fault == 3) f.cursor.lengthAdjustment = 1;
        if (fault == 4) f.cursor.lengthAdjustment = 4096;
        if (fault == 5) f.cursor.lengthAdjustment = -64;
        if (fault == 6) f.sc.txq[0].data[0].cmd_paddr = (1ULL << 36) - 20;
        if (fault == 7) f.sc.txq[0].data[0].scratch_paddr = 1ULL << 36;
        assert(f.driver.iwm_tx(&f.sc, frame(false, 0), &f.peer, 0) == EFBIG);
        assert(!f.driver.kicks && !liveMbufs && !f.sc.txq[0].data[0].m);
        ++cases;
    }
    {
        Fixture f; f.sc.txq[0].data[0].cmd_paddr = 0xfffffff0ULL;
        assert(f.driver.iwm_tx(&f.sc, frame(false, 0), &f.peer, 0) == 0);
        const auto &descriptor = f.sc.txq[0].desc[0];
        assert((descriptor.tbs[0].hi_n_len & 15) == 0);
        assert((descriptor.tbs[1].hi_n_len & 15) == 1 && descriptor.tbs[1].lo == 4);
        ++cases;
    }
    for (unsigned fault = 0; fault < 5; ++fault) {
        Fixture f;
        auto &ring = f.sc.txq[0];
        if (fault == 0) ring.cur = -1;
        if (fault == 1) ring.cur = IWM_TX_RING_COUNT;
        if (fault == 2) ring.data[0].m = frame(false, 0);
        if (fault == 3) ring.data[0].map = nullptr;
        if (fault == 4) f.map.cursor = nullptr;
        assert(f.driver.iwm_tx(&f.sc, frame(false, 0), &f.peer, 0) == ENOBUFS);
        assert(!f.driver.kicks && liveMbufs == (fault == 2));
        ++cases;
    }
    for (bool freeRing : {false, true}) {
        Fixture f;
        auto &ring = f.sc.txq[1];
        f.peer.ni_refcnt = 3; // One base reference and two queued TX references.
        assert(f.driver.iwm_tx(&f.sc, frame(false, 0), &f.peer, 1) == 0);
        assert(f.driver.iwm_tx(&f.sc, frame(false, 0), &f.peer, 1) == 0);
        if (freeRing) f.driver.iwm_free_tx_ring(&f.sc, &ring);
        else f.driver.iwm_reset_tx_ring(&f.sc, &ring);
        assert(f.peer.ni_refcnt == 1 && !liveMbufs && !ring.data[0].in && !ring.data[1].in);
        if (freeRing) f.driver.iwm_free_tx_ring(&f.sc, &ring);
        else f.driver.iwm_reset_tx_ring(&f.sc, &ring);
        assert(f.peer.ni_refcnt == 1);
        ++cases;
    }
    for (bool aggregation : {false, true}) {
        for (int start : {0, 1, 254, 255}) {
            for (unsigned status : {unsigned(IWM_TX_STATUS_SUCCESS), unsigned(IWM_TX_STATUS_DIRECT_DONE),
                 unsigned(IWM_TX_STATUS_FAIL_FIFO_FLUSHED), unsigned(IWM_TX_STATUS_FAIL_LONG_LIMIT)}) {
                Fixture f;
                const int qid = aggregation ? IWM_FIRST_AGG_TX_QUEUE : 0;
                auto &ring = f.sc.txq[qid]; ring.cur = ring.tail = start;
                f.sc.sc_tx_ba[0].wn = &f.peer;
                f.peer.ni_tx_ba[0].ba_state = aggregation ? IEEE80211_BA_AGREED : 0;
                f.peer.ni_tx_ba[0].ba_winstart = start;
                f.peer.ni_refcnt = 4;
                for (int i = 0; i < 3; ++i)
                    assert(f.driver.iwm_tx(&f.sc, frame(aggregation, 0), &f.peer, 0) == 0);
                const auto oldStatuses = statuses, oldWindows = windowMoves, oldBars = bars;
                completion(f, qid, start, 1, start + 1, status);
                assert(ring.queued == 2 && ring.tail == (start + 1) % IWM_TX_RING_COUNT);
                assert(f.peer.ni_refcnt == 3 && liveMbufs == 2 && statuses == oldStatuses + 1);
                assert(f.sc.sc_tx_timer[qid] == 15 && windowMoves == oldWindows + aggregation);
                assert(bars == oldBars + (aggregation && status != IWM_TX_STATUS_SUCCESS && status != IWM_TX_STATUS_DIRECT_DONE));
                completion(f, qid, (start + 1) % IWM_TX_RING_COUNT, 1, start + 3, status);
                assert(!ring.queued && ring.tail == ring.cur && f.peer.ni_refcnt == 1 && !liveMbufs);
                assert(!f.sc.sc_tx_timer[qid] && statuses == oldStatuses + 3);
                const auto oldMoves = windowMoves;
                completion(f, qid, (start + 1) % IWM_TX_RING_COUNT, 1, start + 3, status);
                assert(!ring.queued && f.peer.ni_refcnt == 1 && windowMoves == oldMoves);
                ++cases;
            }
        }
    }
    for (unsigned fault = 0; fault < 15; ++fault) {
        Fixture f;
        auto &ring = f.sc.txq[0];
        f.peer.ni_refcnt = 2;
        assert(f.driver.iwm_tx(&f.sc, frame(false, 0), &f.peer, 0) == 0);
        f.sc.sc_tx_timer[0] = 7;
        if (fault == 0) completion(f, 0, 0, 0, 1, IWM_TX_STATUS_SUCCESS);
        if (fault >= 1 && fault <= 4) completion(f, 0, 0, 1, 1, IWM_TX_STATUS_SUCCESS, fault);
        if (fault == 5) completion(f, 0, 0, 1, 2, IWM_TX_STATUS_SUCCESS);
        if (fault == 6) completion(f, 0, 0, 1, 0, IWM_TX_STATUS_SUCCESS);
        if (fault == 7) completion(f, 0, 1, 1, 1, IWM_TX_STATUS_SUCCESS);
        if (fault == 8) completion(f, 0, 0, 2, 1, IWM_TX_STATUS_SUCCESS);
        if (fault == 9) completion(f, IWM_LAST_AGG_TX_QUEUE + 1, 0, 1, 1, IWM_TX_STATUS_SUCCESS);
        if (fault == 10) { f.sc.cmdqid = 0; completion(f, 0, 0, 1, 1, IWM_TX_STATUS_SUCCESS); }
        if (fault == 11) { ring.tail = -1; completion(f, 0, 0, 1, 1, IWM_TX_STATUS_SUCCESS); ring.tail = 0; }
        if (fault == 12) { ring.cur = 2; completion(f, 0, 0, 1, 1, IWM_TX_STATUS_SUCCESS); ring.cur = 1; }
        if (fault == 13) { ring.data[0].in = nullptr; completion(f, 0, 0, 1, 1, IWM_TX_STATUS_SUCCESS); ring.data[0].in = &f.peer; }
        if (fault == 14) { auto *desc = ring.desc; ring.desc = nullptr; completion(f, 0, 0, 1, 1, IWM_TX_STATUS_SUCCESS); ring.desc = desc; }
        assert(ring.queued == 1 && ring.tail == 0 && f.peer.ni_refcnt == 2 && f.sc.sc_tx_timer[0] == 7);
        f.driver.iwm_reset_tx_ring(&f.sc, &ring);
        ++cases;
    }
    for (unsigned size = 0; size < sizeof(iwm_tx_resp); ++size) {
        Fixture f;
        std::vector<uint8_t> bytes(sizeof(iwm_rx_packet) + size);
        auto *packet = reinterpret_cast<iwm_rx_packet *>(bytes.data());
        packet->len_n_flags = sizeof(packet->hdr) + size;
        iwm_rx_data data;
        f.driver.iwm_rx_tx_cmd(&f.sc, packet, &data);
        ++cases;
    }
    for (unsigned fault = 0; fault < 3; ++fault) {
        Fixture f;
        const int qid = IWM_FIRST_AGG_TX_QUEUE;
        auto &ring = f.sc.txq[qid];
        f.sc.sc_tx_ba[0].wn = &f.peer;
        f.peer.ni_tx_ba[0].ba_state = IEEE80211_BA_AGREED;
        f.peer.ni_refcnt = 3;
        for (int i = 0; i < 2; ++i) assert(f.driver.iwm_tx(&f.sc, frame(true, 0), &f.peer, 0) == 0);
        f.sc.sc_tx_timer[qid] = 7;
        if (fault == 1) f.sc.sc_tx_ba[0].wn = &f.foreign;
        completion(f, qid, 0, fault == 2 ? 3 : 2, 2, IWM_TX_STATUS_SUCCESS);
        assert(ring.queued == 2 && ring.tail == 0 && f.peer.ni_refcnt == 3 && f.sc.sc_tx_timer[qid] == 7);
        f.driver.iwm_reset_tx_ring(&f.sc, &ring);
        ++cases;
    }
    for (unsigned start : {0u, 254u, 255u, 4094u, 4095u}) {
        Fixture f;
        const int qid = IWM_FIRST_AGG_TX_QUEUE;
        auto &ring = f.sc.txq[qid]; ring.tail = ring.cur = start % IWM_TX_RING_COUNT;
        f.peer.ni_tx_ba[0].ba_state = IEEE80211_BA_AGREED;
        f.peer.ni_tx_ba[0].ba_winstart = start;
        f.sc.sc_tx_ba[0].wn = &f.peer; f.peer.ni_refcnt = 4;
        for (unsigned i = 0; i < 3; ++i) assert(f.driver.iwm_tx(&f.sc, frame(true, 0), &f.peer, 0) == 0);
        auto oldStatuses = statuses;
        blockAck(f, (start + 1) & 0xfff);
        assert(ring.queued == 2 && f.peer.ni_refcnt == 3 && f.sc.sc_tx_timer[qid] == 15);
        assert(ring.tail == int((start + 1) % IWM_TX_RING_COUNT) && statuses == oldStatuses + 1);
        f.sc.sc_tx_timer[qid] = 7;
        blockAck(f, (start + 1) & 0xfff, 1, 0);
        assert(ring.queued == 2 && f.peer.ni_refcnt == 3 && f.sc.sc_tx_timer[qid] == 7);
        oldStatuses = statuses;
        blockAck(f, (start + 3) & 0xfff, 2, 2);
        assert(!ring.queued && f.peer.ni_refcnt == 1 && !f.sc.sc_tx_timer[qid] && !liveMbufs);
        assert(ring.tail == ring.cur && statuses == oldStatuses + 2);
        ++cases;
    }
    for (unsigned fault = 0; fault < 13; ++fault) {
        Fixture f;
        const int qid = IWM_FIRST_AGG_TX_QUEUE;
        auto &ring = f.sc.txq[qid];
        f.peer.ni_tx_ba[0].ba_state = IEEE80211_BA_AGREED;
        f.sc.sc_tx_ba[0].wn = &f.peer; f.peer.ni_refcnt = 2;
        assert(f.driver.iwm_tx(&f.sc, frame(true, 0), &f.peer, 0) == 0);
        f.sc.sc_tx_timer[qid] = 7;
        const auto oldMoves = windowMoves, oldStatuses = statuses;
        if (fault == 0) blockAck(f, 2);
        if (fault == 1) blockAck(f, 4095);
        if (fault == 2) blockAck(f, 4096);
        if (fault == 3) blockAck(f, 257);
        if (fault == 4) blockAck(f, 1, 1, 2);
        if (fault >= 5 && fault <= 8) blockAck(f, 1, 1, 1, fault - 4);
        if (fault == 9) { f.sc.sc_tx_ba[0].wn = &f.foreign; blockAck(f, 1); }
        if (fault == 10) { f.sc.sc_ic.ic_bss = nullptr; blockAck(f, 1); }
        if (fault == 11) { f.peer.ni_tx_ba[0].ba_winstart = 1; blockAck(f, 1); }
        if (fault == 12) { ring.data[0].in = &f.foreign; blockAck(f, 1); ring.data[0].in = &f.peer; }
        assert(ring.queued == 1 && ring.tail == 0 && f.peer.ni_refcnt == 2 && f.sc.sc_tx_timer[qid] == 7);
        assert(windowMoves == oldMoves && statuses == oldStatuses);
        f.driver.iwm_reset_tx_ring(&f.sc, &ring);
        ++cases;
    }
    for (unsigned size = 0; size < sizeof(iwm_ba_notif); ++size) {
        Fixture f;
        std::vector<uint8_t> bytes(sizeof(iwm_rx_packet) + size);
        auto *packet = reinterpret_cast<iwm_rx_packet *>(bytes.data());
        packet->len_n_flags = sizeof(packet->hdr) + size;
        iwm_rx_data data;
        f.driver.iwm_rx_tx_ba_notif(&f.sc, packet, &data);
        ++cases;
    }
    std::printf("Actual Intel data TX: %u queue/TID/byte-count/software-crypto/failure cases passed; DMA and cipher boundary mocked\n", cases);
}
