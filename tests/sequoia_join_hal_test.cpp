#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <net/if.h>
#include <functional>
#include <vector>
#include <HAL/IwmOperationOwner.h>
#include "sequoia_join_bsd_types.inc"

#define IO80211FAMILY_V2 1
#define __MAC_15_0 150000
#define __IO80211_TARGET __MAC_15_0
#define __packed __attribute__((packed))
#define ETHER_ADDR_LEN 6
#define le32toh(value) (value)
#define htole32(value) (value)
#define htole16(value) (value)
using bus_addr_t = uint64_t;
enum { EDCA_AC_BE, EDCA_AC_BK, EDCA_AC_VI, EDCA_AC_VO, EDCA_NUM_AC };
static uint32_t le32_to_cpup(const uint32_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-variable-sized-type-not-at-end"
#pragma clang diagnostic ignored "-Wunused-function"
#include "../itlwm/hal_iwm/if_iwmreg.h"
#pragma clang diagnostic pop
#define XYLog(...) ((void)0)
#define splassert(...) ((void)0)
#define DELAY(...) ((void)0)
#define nitems(a) (sizeof(a) / sizeof((a)[0]))
#define isset(a, bit) ((a)[(bit) / 8] & (1 << ((bit) % 8)))
#define container_of(pointer, type, member) reinterpret_cast<type *>(pointer)

enum nl80211_band { NL80211_BAND_2GHZ, NL80211_BAND_5GHZ };
struct ieee80211_channel { unsigned ic_flags = IEEE80211_CHAN_2GHZ; };
struct ieee80211_node {
    ieee80211_channel *ni_chan;
    unsigned ni_chw = IEEE80211_CHAN_WIDTH_20, ni_flags = 0, ni_intval = 100;
    int ni_txrate = 0, ni_txmcs = 0;
    uint8_t ni_macaddr[6] = {2,1,2,3,4,5};
};
struct iwm_node {
    ieee80211_node in_ni;
    unsigned *in_phyctxt = nullptr, in_amn = 0;
    uint8_t in_macaddr[6] = {};
    unsigned in_id = 0, in_color = 0;
};
struct ieee80211com {
    ieee80211_node *ic_bss = nullptr;
    ieee80211_opmode ic_opmode = IEEE80211_M_STA;
    ieee80211_channel ic_channels[256], *ic_ibss_chan = &ic_channels[1];
    unsigned ic_vht_sup_mcs[2] = {};
};
struct iwm_rxba_data { unsigned baid = IWM_RX_REORDER_DATA_INVALID_BAID, tid = 0; };
struct iwm_tx_ring { unsigned cur = 0; };
struct TestSpin { bool held = false; unsigned locks = 0, unlocks = 0; };
static void IOSimpleLockLock(TestSpin *s) { assert(s && !s->held); s->held = true; ++s->locks; }
static void IOSimpleLockUnlock(TestSpin *s) { assert(s && s->held); s->held = false; ++s->unlocks; }
struct iwm_softc {
    ieee80211com sc_ic;
    unsigned sc_flags = 0, sc_phyctxt[1] = {}, sc_tx_ant = 0, sc_mgmt_last_antenna_idx = 0;
    int sc_generation = 7, sc_rx_ba_sessions = 0;
    unsigned sc_time_event_uid = 0x1234;
    uint8_t sc_enabled_capa[256] = {};
    unsigned sc_amrr = 0, sc_calib_to = 0, agg_queue_mask = 0;
    struct { void *wn = nullptr; } sc_tx_ba[16];
    struct { unsigned start_tidmask = 0, stop_tidmask = 0; } ba_rx, ba_tx;
    iwm_rxba_data sc_rxba_data[2];
    iwm_tx_ring txq[32];
    struct { struct { struct { TestSpin *lock = nullptr; } pers; iwm_lq_cmd lq = {}; } rs_drv; } lq_sta;
};
static void timeout_add_msec(unsigned *timer, unsigned ms) { assert(ms == 500); ++*timer; }
static void ieee80211_amrr_node_init(unsigned *, unsigned *) {}
static void ieee80211_ba_del(ieee80211_node *) {}
static void rs_drv_alloc_sta(iwm_softc *, ieee80211_node *) {}
static void rs_drv_free_sta(iwm_softc *, ieee80211_node *) {}
int iwl_mvm_rs_rate_init(iwm_softc *, ieee80211_node *, nl80211_band, bool);

enum class Call { Phy, Mac, Bind, Sta, Protect, Rate, Sf, Mcast, DevicePower, MacPower,
                  Quota, RemoveSta, Flush, RxAgg, TxAgg, BeaconFilter };
class ItlIwm {
public:
    iwm_softc com;
    iwm_node node;
    TestSpin spin;
    bool nativeDetached = false, commandTransportReady = true;
    int nativeStateError = 0, prepareError = 0;
    IwmOperationOwner nativeFirmwareOwner;
    struct Loop {
        bool gate = true, thread = false;
        bool inGate() const { return gate; }
        bool onThread() const { return thread; }
    } loop;
    Loop *getMainWorkLoop() { return &loop; }
    class FirmwareLease {
        ItlIwm *driver;
        bool acquired;
    public:
        explicit FirmwareLease(ItlIwm *d) : driver(d), acquired(d->nativeFirmwareOwner.acquire(1)) {}
        ~FirmwareLease() { if (acquired) assert(driver->nativeFirmwareOwner.release(1)); }
        explicit operator bool() const { return acquired; }
    };
    std::vector<Call> calls;
    unsigned failAt = 0, leds = 0, cleared = 0;
    std::function<void(Call)> after;
    int invoke(Call call) {
        calls.push_back(call);
        if (after) after(call);
        return calls.size() == failAt ? EIO : 0;
    }
    int iwm_auth(iwm_softc *);
    int iwm_run(iwm_softc *);
    int iwm_deauth(iwm_softc *);
    int iwm_run_stop(iwm_softc *);
    int iwm_protect_session(iwm_softc *, iwm_node *, uint32_t, uint32_t);
    int iwm_unprotect_session(iwm_softc *, iwm_node *);
    int iwm_phy_ctxt_update(iwm_softc *, unsigned *, ieee80211_channel *, int, int, int) { return invoke(Call::Phy); }
    int iwm_mac_ctxt_cmd(iwm_softc *, iwm_node *, int, int) { return invoke(Call::Mac); }
    int iwm_binding_cmd(iwm_softc *, iwm_node *, int) { return invoke(Call::Bind); }
    int iwm_add_sta_cmd(iwm_softc *sc, iwm_node *, int, int) {
        int error = invoke(Call::Sta); if (!error) sc->sc_flags |= IWM_FLAG_STA_ACTIVE; return error;
    }
    int iwm_rm_sta_cmd(iwm_softc *sc, iwm_node *) {
        int error = invoke(Call::RemoveSta); if (!error) sc->sc_flags &= ~IWM_FLAG_STA_ACTIVE; return error;
    }
    void iwm_toggle_tx_ant(iwm_softc *, unsigned *) {}
    int iwm_send_time_event_cmd(iwm_softc *, const iwm_time_event_cmd *cmd) {
        assert(cmd->action == IWM_FW_CTXT_ACTION_ADD || cmd->action == IWM_FW_CTXT_ACTION_REMOVE);
        if (cmd->action == IWM_FW_CTXT_ACTION_ADD) {
            assert(cmd->id == IWM_TE_BSS_STA_AGGRESSIVE_ASSOC && cmd->duration == 200 && cmd->max_delay == 50);
        } else assert(cmd->id == com.sc_time_event_uid);
        return invoke(Call::Protect);
    }
    int iwm_send_cmd_pdu(iwm_softc *sc, unsigned id, unsigned flags, unsigned size, const void *data) {
        assert(sc == &com && id == IWM_LQ_CMD && !flags && size == sizeof(iwm_lq_cmd));
        assert(data != &com.lq_sta.rs_drv.lq && !spin.held && nativeFirmwareOwner.owns(1));
        const auto copy = *static_cast<const iwm_lq_cmd *>(data);
        assert(!memcmp(data, &com.lq_sta.rs_drv.lq, size));
        int error = invoke(Call::Rate);
        // A concurrent TX-status mutation must not alter the command snapshot.
        memset(&com.lq_sta.rs_drv.lq, 0x5a, sizeof(com.lq_sta.rs_drv.lq));
        assert(!memcmp(data, &copy, size));
        return error;
    }
    int iwm_mimo_enabled(iwm_softc *) { return 1; }
    int iwm_sf_config(iwm_softc *, int) { return invoke(Call::Sf); }
    int iwm_allow_mcast(iwm_softc *) { return invoke(Call::Mcast); }
    int iwm_power_update_device(iwm_softc *) { return invoke(Call::DevicePower); }
    int iwm_power_mac_update_mode(iwm_softc *, iwm_node *) { return invoke(Call::MacPower); }
    int iwm_update_quotas(iwm_softc *, iwm_node *, int) { return invoke(Call::Quota); }
    int iwm_disable_beacon_filter(iwm_softc *) { return invoke(Call::BeaconFilter); }
    int iwm_flush_tx_path(iwm_softc *, int) { return invoke(Call::Flush); }
    int iwm_sta_rx_agg(iwm_softc *, ieee80211_node *, unsigned, int, int, int, int) { return invoke(Call::RxAgg); }
    int iwm_sta_tx_agg(iwm_softc *, ieee80211_node *, unsigned, int, int, int) { return invoke(Call::TxAgg); }
    void iwm_clear_reorder_buffer(iwm_softc *, iwm_rxba_data *) { ++cleared; }
    void iwm_ampdu_txq_advance(iwm_softc *, iwm_tx_ring *, unsigned) {}
    void iwm_clear_oactive(iwm_softc *, iwm_tx_ring *) {}
    void iwm_led_blink_start(iwm_softc *) { ++leds; }
    void iwm_led_blink_stop(iwm_softc *) { ++leds; }
    void iwm_led_enable(iwm_softc *) { ++leds; }
    ItlIwm() {
        com.sc_ic.ic_bss = &node.in_ni;
        node.in_ni.ni_chan = &com.sc_ic.ic_channels[1];
        com.lq_sta.rs_drv.pers.lock = &spin;
    }
};
static int rs_drv_rate_init(iwm_softc *sc, ieee80211_node *, nl80211_band, bool submit = true)
{
    auto *driver = reinterpret_cast<ItlIwm *>(sc);
    assert(driver->spin.held && !submit);
    memset(&sc->lq_sta.rs_drv.lq, 0xa5, sizeof(sc->lq_sta.rs_drv.lq));
    return driver->prepareError;
}
#include "sequoia_join_hal_functions.inc"

int main()
{
    unsigned traces = 0;
    for (unsigned mode = 0; mode < 4; ++mode) {
        auto run = [mode](ItlIwm &d) {
            switch (mode) {
                case 0: return d.iwm_auth(&d.com);
                case 1: return d.iwm_run(&d.com);
                case 2: return d.iwm_deauth(&d.com);
                default: return d.iwm_run_stop(&d.com);
            }
        };
        auto setup = [mode](ItlIwm &d) {
            if (mode != 0) {
                d.com.sc_flags |= IWM_FLAG_TE_ACTIVE | IWM_FLAG_STA_ACTIVE | IWM_FLAG_MAC_ACTIVE | IWM_FLAG_BINDING_ACTIVE;
                d.node.in_ni.ni_flags = IEEE80211_NODE_HT | IEEE80211_NODE_VHT;
                d.com.sc_rxba_data[0].baid = 0;
                d.com.sc_rx_ba_sessions = 1;
                d.com.agg_queue_mask = 1 << IWM_FIRST_AGG_TX_QUEUE;
            }
        };
        ItlIwm baseline; setup(baseline);
        assert(!run(baseline));
        for (unsigned point = 1; point <= baseline.calls.size(); ++point) {
            ItlIwm driver; setup(driver); driver.failAt = point;
            assert(run(driver) == EIO);
            assert(driver.calls.size() >= point);
            for (unsigned i = 0; i < point; ++i) assert(driver.calls[i] == baseline.calls[i]);
            // AUTH's existing rollback removes bindings/MAC after add failures.
            if (mode || baseline.calls[point - 1] == Call::Protect || baseline.calls[point - 1] == Call::Rate)
                assert(driver.calls.size() == point);
            assert(!driver.spin.held && !driver.nativeFirmwareOwner.busy());
            if (mode == 1) assert(!driver.com.sc_calib_to && !driver.leds);
            if (mode == 3 && point == 1) assert(!driver.cleared && driver.com.sc_rx_ba_sessions == 1);
            ++traces;
        }
        if (mode == 1) assert(baseline.com.sc_calib_to == 1 && baseline.leds == 1);
        ++traces;
    }
    for (unsigned removal = 0; removal < 2; ++removal)
        for (unsigned reset = 0; reset < 2; ++reset) {
            ItlIwm d;
            d.com.sc_flags = removal ? IWM_FLAG_TE_ACTIVE : 0;
            if (reset) d.after = [&](Call) { ++d.com.sc_generation; d.com.sc_flags = 0; };
            int error = removal ? d.iwm_unprotect_session(&d.com, &d.node) : d.iwm_protect_session(&d.com, &d.node, 200, 50);
            assert(error == (reset ? ECANCELED : 0));
            assert(bool(d.com.sc_flags & IWM_FLAG_TE_ACTIVE) == (!removal && !reset));
            ++traces;
        }
    for (unsigned fault = 0; fault < 17; ++fault) {
        ItlIwm d;
        int expected = 0;
        switch (fault) {
            case 1: d.com.lq_sta.rs_drv.pers.lock = nullptr; expected = ENOMEM; break;
            case 2: d.loop.gate = false; expected = EWOULDBLOCK; break;
            case 3: d.loop.thread = true; expected = EWOULDBLOCK; break;
            case 4: assert(d.nativeFirmwareOwner.acquire(2)); expected = EBUSY; break;
            case 5: d.nativeDetached = true; expected = ENXIO; break;
            case 6: d.nativeStateError = EIO; expected = ENXIO; break;
            case 7: d.commandTransportReady = false; expected = ENXIO; break;
            case 8: d.com.sc_flags = IWM_FLAG_SHUTDOWN; expected = ENXIO; break;
            case 9: d.prepareError = EINVAL; expected = EINVAL; break;
            case 10: d.failAt = 1; expected = EIO; break;
            default: if (fault > 10) {
                expected = ECANCELED;
                d.after = [&](Call) {
                    switch (fault) {
                        case 11: ++d.com.sc_generation; break;
                        case 12: d.nativeDetached = true; break;
                        case 13: d.nativeStateError = EIO; break;
                        case 14: d.commandTransportReady = false; break;
                        case 15: d.com.sc_flags = IWM_FLAG_HW_ERR; break;
                        case 16: d.com.sc_flags = IWM_FLAG_RFKILL; break;
                    }
                };
            }
        }
        assert(iwl_mvm_rs_rate_init(&d.com, &d.node.in_ni, NL80211_BAND_2GHZ, false) == expected);
        assert(d.calls.size() == (fault == 0 || fault >= 10 ? 1 : 0));
        assert(!d.spin.held && d.spin.locks == d.spin.unlocks);
        if (fault == 4) assert(d.nativeFirmwareOwner.release(2));
        assert(!d.nativeFirmwareOwner.busy());
        ++traces;
    }
    printf("Actual HAL auth/RUN/teardown/protection/rate wrapper: %u error/admission/reset traces; synchronous owned rate snapshot outside spinlock; firmware and rate builder mocked\n", traces);
}
