#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>

#define IO80211FAMILY_V2
#define __MAC_15_0 150000
#define __IO80211_TARGET __MAC_15_0
#define __packed __attribute__((packed))
#define htole16(x) (x)
#define le16toh(x) (x)
#define le32toh(x) (x)
#define le64toh(x) (x)
#define letoh16(x) (x)
#define letoh32(x) (x)
#define MIN(x, y) std::min<int>(x, y)
#define ETHER_ADDR_LEN 6
#define XYLog(...) ((void)0)
#define nitems(a) (sizeof(a) / sizeof((a)[0]))
#define _KASSERT(test) assert(test)
using bus_addr_t = uint64_t;
enum { EDCA_AC_BE, EDCA_AC_BK, EDCA_AC_VI, EDCA_AC_VO };
static uint32_t le32_to_cpup(const uint32_t *p) { uint32_t value; memcpy(&value, p, 4); return value; }
#include "iwm_data_rx_types.inc"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-variable-sized-type-not-at-end"
#pragma clang diagnostic ignored "-Wunused-function"
#include "../itlwm/hal_iwm/if_iwmreg.h"
#pragma clang diagnostic pop
constexpr int IEEE80211_M_STA = 1, IEEE80211_M_MONITOR = 6;
struct Statistics { unsigned inputErrors = 0; };
struct ieee80211com {
    struct { Statistics *netStat; } ic_if;
    struct { unsigned is_rx_tooshort = 0; } ic_stats;
    int ic_opmode = IEEE80211_M_STA, ic_max_rssi = 100;
};
#define IC2IFP(ic) (&(ic)->ic_if)
struct iwm_softc { ieee80211com sc_ic; iwm_rx_phy_info sc_last_phy_info = {}; };
struct ieee80211_rx_status {};
struct iwm_rx_data {};
struct mbuf_list {};
struct Buffer {
    std::vector<uint8_t> bytes;
    uint8_t *data;
    size_t length, packetLength;
    explicit Buffer(size_t size) : bytes(size), data(bytes.data()), length(size), packetLength(size) {}
};
using mbuf_t = Buffer *;
static unsigned liveMbufs = 0, received = 0;
static std::vector<uint8_t> lastFrame;
static ieee80211_rxinfo lastInfo;
static bool decryptFail = false, duplicate = false, reorder = false, setDataFail = false;
#define mtod(m, type) reinterpret_cast<type>((m)->data)
static size_t mbuf_len(mbuf_t m) { return m->length; }
static void *mbuf_data(mbuf_t m) { return m->data; }
static void mbuf_freem(mbuf_t m) { assert(m && liveMbufs); --liveMbufs; delete m; }
static int mbuf_setdata(mbuf_t m, void *pointer, size_t length) {
    if (setDataFail) return 1;
    auto *bytes = static_cast<uint8_t *>(pointer);
    assert(bytes >= m->bytes.data() && bytes <= m->bytes.data() + m->bytes.size());
    assert(length <= size_t(m->bytes.data() + m->bytes.size() - bytes));
    m->data = bytes; m->length = length; return 0;
}
static void mbuf_pkthdr_setlen(mbuf_t m, size_t length) { m->packetLength = length; }
static void mbuf_setlen(mbuf_t m, size_t length) { assert(length == m->length); }
static void mbuf_adj(mbuf_t m, size_t length) {
    assert(length <= m->length); m->data += length; m->length -= length; m->packetLength -= length;
}
static void rs_update_last_rssi(iwm_softc *, ieee80211_rx_status *) {}
struct ItlIwm {
    void iwm_rx_rx_phy_cmd(iwm_softc *, iwm_rx_packet *, iwm_rx_data *);
    void iwm_rx_mpdu(iwm_softc *, mbuf_t, void *, size_t, mbuf_list *);
    void iwm_rx_mpdu_mq(iwm_softc *, mbuf_t, void *, size_t, mbuf_list *);
    void iwm_flip_address(uint8_t *);
    int iwm_get_signal_strength(iwm_softc *, ieee80211_rx_status *, iwm_rx_phy_info *) { return -50; }
    int iwm_rxmq_get_signal_strength(iwm_softc *, ieee80211_rx_status *, uint32_t, iwm_rx_mpdu_desc *) { return -50; }
    int iwm_rx_hwdecrypt(iwm_softc *, mbuf_t, uint32_t, ieee80211_rxinfo *) { return decryptFail; }
    bool iwm_detect_duplicate(iwm_softc *, mbuf_t, iwm_rx_mpdu_desc *, ieee80211_rxinfo *) { return duplicate; }
    bool iwm_rx_reorder(iwm_softc *, mbuf_t m, int, iwm_rx_mpdu_desc *, int, int, uint32_t,
                       ieee80211_rxinfo *, mbuf_list *) {
        if (reorder) mbuf_freem(m);
        return reorder;
    }
    void iwm_rx_frame(iwm_softc *, mbuf_t m, int, uint32_t, int, int, uint32_t,
                      ieee80211_rxinfo *rxi, mbuf_list *) {
        assert(m->length == m->packetLength);
        lastFrame.assign(m->data, m->data + m->length); lastInfo = *rxi; ++received;
        mbuf_freem(m);
    }
};
#include "iwm_data_rx_methods.inc"
struct Fixture {
    Statistics stats;
    iwm_softc sc;
    ItlIwm driver;
    Fixture() { sc.sc_ic.ic_if.netStat = &stats; sc.sc_last_phy_info.channel = 1; }
    ~Fixture() { assert(!liveMbufs); }
};
static mbuf_t legacy(unsigned length, unsigned trailer = 4) {
    auto *m = new Buffer(sizeof(iwm_rx_mpdu_res_start) + length + trailer); ++liveMbufs;
    auto *header = reinterpret_cast<iwm_rx_mpdu_res_start *>(m->data); header->byte_count = length;
    if (length) m->data[sizeof(*header)] = 0x08;
    if (trailer == 4) {
        uint32_t status = IWM_RX_MPDU_RES_STATUS_CRC_OK | IWM_RX_MPDU_RES_STATUS_OVERRUN_OK;
        memcpy(m->data + sizeof(*header) + length, &status, sizeof(status));
    }
    return m;
}
static mbuf_t mq(unsigned length, bool padded = false, bool ccmp = false) {
    auto *m = new Buffer(sizeof(iwm_rx_mpdu_desc) + length); ++liveMbufs;
    auto *desc = reinterpret_cast<iwm_rx_mpdu_desc *>(m->data);
    desc->mpdu_len = length;
    desc->status = IWM_RX_MPDU_RES_STATUS_CRC_OK | IWM_RX_MPDU_RES_STATUS_OVERRUN_OK;
    if (ccmp) desc->status |= IWM_RX_MPDU_RES_STATUS_SEC_CCM_ENC;
    if (padded) desc->mac_flags2 |= IWM_RX_MPDU_MFLG2_PAD;
    desc->v1.channel = 1;
    if (length) m->data[sizeof(*desc)] = 0x88;
    return m;
}
int main() {
    unsigned cases = 0;
    if (const char *repro = std::getenv("IWM_RX_REPRO")) {
        Fixture f;
        auto *m = std::strcmp(repro, "padding") == 0 ? mq(24, true, true) : legacy(24, 0);
        if (std::strcmp(repro, "padding") == 0) f.driver.iwm_rx_mpdu_mq(&f.sc, m, m->data, m->length, nullptr);
        else f.driver.iwm_rx_mpdu(&f.sc, m, m->data, m->length, nullptr);
        assert(!received);
        return 0;
    }
    for (bool multi : {false, true}) {
        for (unsigned length : {24u, 25u, 26u, 31u, 32u, 64u, 1514u}) {
            Fixture f; const unsigned old = received;
            auto *m = multi ? mq(length) : legacy(length);
            if (multi) f.driver.iwm_rx_mpdu_mq(&f.sc, m, m->data, m->length, nullptr);
            else f.driver.iwm_rx_mpdu(&f.sc, m, m->data, m->length, nullptr);
            const bool accepted = !multi || length >= 26;
            assert(received == old + accepted);
            if (accepted) assert(lastFrame.size() == length && lastInfo.rxi_chan == 1);
            ++cases;
        }
    }
    for (unsigned trailer = 0; trailer < 4; ++trailer) {
        Fixture f; const auto old = received;
        auto *m = legacy(24, trailer); f.driver.iwm_rx_mpdu(&f.sc, m, m->data, m->length, nullptr);
        assert(received == old); ++cases;
    }
    for (bool multi : {false, true}) {
        const size_t header = multi ? sizeof(iwm_rx_mpdu_desc) : sizeof(iwm_rx_mpdu_res_start) + 4;
        for (size_t length = 0; length < header; ++length) {
            Fixture f; const auto old = received;
            auto *m = new Buffer(length); ++liveMbufs;
            if (multi) f.driver.iwm_rx_mpdu_mq(&f.sc, m, m->data, m->length, nullptr);
            else f.driver.iwm_rx_mpdu(&f.sc, m, m->data, m->length, nullptr);
            assert(received == old); ++cases;
        }
    }
    for (bool ccmp : {false, true}) {
        for (bool address4 : {false, true}) {
            for (bool htc : {false, true}) {
                const unsigned header = 26 + (address4 ? 6 : 0) + (htc ? 4 : 0) + (ccmp ? 8 : 0);
                for (unsigned length = 24; length <= header + 4; ++length) {
                    Fixture f; const auto old = received;
                    auto *m = mq(length, true, ccmp);
                    auto *bytes = m->data + sizeof(iwm_rx_mpdu_desc);
                    bytes[1] = (address4 ? 3 : 0) | (htc ? IEEE80211_FC1_ORDER : 0);
                    std::vector<uint8_t> expected;
                    const bool accepted = length >= header + 2;
                    if (accepted) {
                        expected.assign(bytes, bytes + header);
                        expected.insert(expected.end(), bytes + header + 2, bytes + length);
                    }
                    f.driver.iwm_rx_mpdu_mq(&f.sc, m, m->data, m->length, nullptr);
                    assert(received == old + accepted);
                    if (accepted) assert(lastFrame == expected);
                    ++cases;
                }
            }
        }
    }
    for (bool multi : {false, true}) {
        for (unsigned fault = 0; fault < 7; ++fault) {
            Fixture f; const auto old = received;
            auto *m = multi ? mq(64) : legacy(64);
            if (fault == 0) {
                if (multi) reinterpret_cast<iwm_rx_mpdu_desc *>(m->data)->status = 0;
                else memset(m->data + m->length - 4, 0, 4);
            }
            if (fault == 1) {
                if (multi) reinterpret_cast<iwm_rx_mpdu_desc *>(m->data)->mpdu_len = 65;
                else reinterpret_cast<iwm_rx_mpdu_res_start *>(m->data)->byte_count = 65;
            }
            decryptFail = fault == 2; duplicate = fault == 3; reorder = fault == 4;
            setDataFail = fault == 6;
            if (fault == 5) f.sc.sc_last_phy_info.channel = 0;
            if (multi) f.driver.iwm_rx_mpdu_mq(&f.sc, m, m->data, m->length, nullptr);
            else f.driver.iwm_rx_mpdu(&f.sc, m, m->data, m->length, nullptr);
            const bool accepted = multi ? fault == 5 : fault == 3 || fault == 4;
            assert(received == old + accepted); ++cases;
        }
    }
    decryptFail = duplicate = reorder = setDataFail = false;
    for (size_t length = 0; length <= sizeof(iwm_rx_phy_info) + 4; ++length) {
        Fixture f;
        std::vector<uint8_t> bytes(sizeof(iwm_rx_packet) + length);
        auto *packet = reinterpret_cast<iwm_rx_packet *>(bytes.data());
        packet->len_n_flags = sizeof(packet->hdr) + length;
        if (length >= sizeof(iwm_rx_phy_info)) reinterpret_cast<iwm_rx_phy_info *>(packet->data)->channel = 36;
        iwm_rx_data data; f.driver.iwm_rx_rx_phy_cmd(&f.sc, packet, &data);
        assert(f.sc.sc_last_phy_info.channel == (length >= sizeof(iwm_rx_phy_info) ? 36 : 0));
        auto *m = legacy(64); const auto old = received;
        f.driver.iwm_rx_mpdu(&f.sc, m, m->data, m->length, nullptr);
        assert(received == old + (length >= sizeof(iwm_rx_phy_info))); ++cases;
    }
    for (unsigned config : {0u, 20u, 21u, 255u}) {
        for (unsigned channel : {0u, 1u, 36u, 196u, 197u, 65535u}) {
            Fixture f;
            f.sc.sc_last_phy_info.channel = channel;
            f.sc.sc_last_phy_info.cfg_phy_cnt = config;
            auto *m = legacy(64); const auto old = received;
            f.driver.iwm_rx_mpdu(&f.sc, m, m->data, m->length, nullptr);
            assert(received == old + (config <= 20 && channel >= 1 && channel <= 196)); ++cases;
        }
    }
    std::printf("Actual IWM PHY/legacy/MQ RX: %u cases passed; mbuf/crypto/reorder/BSD boundaries mocked\n", cases);
}
