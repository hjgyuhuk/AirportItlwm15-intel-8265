#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <net/if.h>
#include <errno.h>
#include <vector>
#include <HAL/IwmOperationOwner.h>
#include <HAL/ItlJoinEvent.h>
#include <Airport/SequoiaJoinEvents.h>
#include "sequoia_join_bsd_types.inc"

struct ieee80211_node {};
struct ieee80211com {
    struct { unsigned if_flags = IFF_RUNNING; } ic_if;
    ieee80211_node *ic_bss = nullptr;
    ieee80211_state ic_state = IEEE80211_S_INIT;
    ieee80211_opmode ic_opmode = IEEE80211_M_STA;
};
struct iwm_softc {
    ieee80211com sc_ic;
    ieee80211_state ns_nstate = IEEE80211_S_AUTH;
    int ns_arg = IEEE80211_FC0_SUBTYPE_AUTH, sc_generation = 7;
    unsigned sc_flags = 0;
    unsigned sc_calib_to = 0, ba_task = 0, mac_ctxt_task = 0, chan_ctxt_task = 0;
    int (*sc_newstate)(ieee80211com *, ieee80211_state, int) = nullptr;
};
static int queue;
static int *systq = &queue;
static void timeout_del(unsigned *timer) { ++*timer; }
enum class Step { StopRun, Deauth, Auth, Run, BSD };
enum class Fault { None, Error, Reset, Detach, Shutdown, Supersede, Hardware, Rfkill, Transport };
class ItlIwm {
public:
    iwm_softc com;
    ieee80211_node node;
    bool nativeDetached = false, trial = false, commandTransportReady = true;
    uint64_t nativeStateSerial = 12, nativeStateCompleted = 11;
    int nativeStateError = 0;
    ItlJoin::Progress nativeJoinProgress;
    void iwm_native_join_event(const ItlJoin::Event &);
    IwmOperationOwner nativeFirmwareOwner;
    struct Loop {
        bool gate = true, thread = false;
        bool inGate() const { return gate; }
        bool onThread() const { return thread; }
    } loop;
    struct Gate {
        unsigned wakeups = 0;
        void *expected = nullptr;
        void commandWakeup(void *event, bool one) {
            assert(event == expected && !one); ++wakeups;
        }
    } gate;
    class FirmwareLease {
        ItlIwm *driver;
        bool acquired;
    public:
        explicit FirmwareLease(ItlIwm *value) : driver(value),
            acquired(value->nativeFirmwareOwner.acquire(1)) {}
        ~FirmwareLease() { if (acquired) assert(driver->nativeFirmwareOwner.release(1)); }
        explicit operator bool() const { return acquired; }
    };
    Loop *getMainWorkLoop() { return &loop; }
    Gate *getMainCommandGate() { return &gate; }
    bool isHardwareTrial() const { return trial; }
    void iwm_del_task(iwm_softc *sc, int *taskQueue, unsigned *task) {
        assert(sc == &com && taskQueue == systq && nativeFirmwareOwner.owns(1)); ++*task;
    }
    std::vector<Step> steps;
    unsigned faultAt = 0;
    Fault fault = Fault::None;
    int perform(Step step) {
        assert(loop.inGate() && !loop.onThread() && nativeFirmwareOwner.owns(1));
        steps.push_back(step);
        if (steps.size() == faultAt) {
            switch (fault) {
                case Fault::None: break;
                case Fault::Error: return EIO;
                case Fault::Reset: ++com.sc_generation; break;
                case Fault::Detach: nativeDetached = true; break;
                case Fault::Shutdown: com.sc_flags |= IWM_FLAG_SHUTDOWN; break;
                case Fault::Supersede: ++nativeStateSerial; break;
                case Fault::Hardware: com.sc_flags |= IWM_FLAG_HW_ERR; break;
                case Fault::Rfkill: com.sc_flags |= IWM_FLAG_RFKILL; break;
                case Fault::Transport: commandTransportReady = false; break;
            }
        }
        return 0;
    }
    int iwm_run_stop(iwm_softc *sc) { assert(sc == &com); return perform(Step::StopRun); }
    int iwm_deauth(iwm_softc *sc) { assert(sc == &com); return perform(Step::Deauth); }
    int iwm_auth(iwm_softc *sc) { assert(sc == &com); return perform(Step::Auth); }
    int iwm_run(iwm_softc *sc) { assert(sc == &com); return perform(Step::Run); }
    void iwm_native_newstate_task();
    ItlIwm() {
        com.sc_ic.ic_bss = &node;
        gate.expected = &nativeStateCompleted;
        com.sc_newstate = [](ieee80211com *ic, ieee80211_state state, int arg) {
            auto *driver = reinterpret_cast<ItlIwm *>(ic);
            assert(arg == driver->com.ns_arg);
            const int error = driver->perform(Step::BSD);
            if (!error) ic->ic_state = state;
            return error;
        };
    }
};
#include "../itlwm/hal_iwm/native_join_state_sequoia.inc"

int main()
{
    unsigned traces = 0;
    for (unsigned previous = IEEE80211_S_INIT; previous <= IEEE80211_S_RUN; ++previous)
        for (unsigned requested = IEEE80211_S_INIT; requested <= IEEE80211_S_RUN; ++requested) {
            ItlIwm driver;
            driver.com.sc_ic.ic_state = static_cast<ieee80211_state>(previous);
            driver.com.ns_nstate = static_cast<ieee80211_state>(requested);
            driver.iwm_native_newstate_task();
            assert(!driver.nativeFirmwareOwner.busy() && driver.gate.wakeups == 1);
            assert(driver.nativeStateCompleted == 12);
            if (requested == IEEE80211_S_SCAN) {
                assert(driver.nativeStateError == EOPNOTSUPP && driver.steps.empty());
            } else if (previous == IEEE80211_S_SCAN ||
                (requested == IEEE80211_S_ASSOC && previous != IEEE80211_S_AUTH) ||
                (requested == IEEE80211_S_RUN && previous != IEEE80211_S_ASSOC)) {
                assert(driver.nativeStateError == EINVAL && driver.steps.empty());
            } else {
                assert(!driver.nativeStateError && driver.com.sc_ic.ic_state == requested);
                assert(driver.steps.back() == Step::BSD);
                assert(driver.com.sc_calib_to == (previous == IEEE80211_S_RUN));
                assert(driver.com.ba_task == driver.com.sc_calib_to &&
                    driver.com.mac_ctxt_task == driver.com.sc_calib_to &&
                    driver.com.chan_ctxt_task == driver.com.sc_calib_to);
            }
            ++traces;
        }
    // RUN->AUTH traverses stop/deauth/auth/BSD. Every sleeping HAL boundary
    // is faulted separately, ensuring no later step commits stale state.
    for (unsigned point = 1; point <= 4; ++point)
        for (unsigned fault = 1; fault <= static_cast<unsigned>(Fault::Transport); ++fault) {
            if (point == 4 && fault != static_cast<unsigned>(Fault::Error)) continue;
            ItlIwm driver;
            driver.com.sc_ic.ic_state = IEEE80211_S_RUN;
            driver.faultAt = point;
            driver.fault = static_cast<Fault>(fault);
            driver.iwm_native_newstate_task();
            assert(driver.steps.size() == point && driver.com.sc_ic.ic_state == IEEE80211_S_RUN);
            assert(!driver.nativeFirmwareOwner.busy());
            const bool teardown = driver.fault == Fault::Reset || driver.fault == Fault::Detach ||
                driver.fault == Fault::Shutdown;
            assert(driver.nativeStateCompleted == (teardown ? 11 : 12));
            assert(driver.gate.wakeups == !teardown);
            assert(driver.nativeStateError == (teardown ? 0 : driver.fault == Fault::Error ? EIO : ECANCELED));
            // An error is sticky: an already-queued transition cannot retry it.
            if (driver.nativeStateError) {
                driver.iwm_native_newstate_task();
                assert(driver.steps.size() == point && driver.gate.wakeups == 1);
            }
            ++traces;
        }
    for (unsigned fault = 0; fault < 13; ++fault) {
        ItlIwm driver;
        switch (fault) {
            case 0: driver.loop.gate = false; break;
            case 1: driver.loop.thread = true; break;
            case 2: driver.nativeDetached = true; break;
            case 3: driver.com.sc_flags = IWM_FLAG_SHUTDOWN; break;
            case 4: driver.nativeStateError = EIO; break;
            case 5: assert(driver.nativeFirmwareOwner.acquire(2)); break;
            case 6: driver.commandTransportReady = false; break;
            case 7: driver.com.sc_ic.ic_if.if_flags = 0; break;
            case 8: driver.com.sc_ic.ic_bss = nullptr; break;
            case 9: driver.com.sc_ic.ic_opmode = IEEE80211_M_MONITOR; break;
            case 10: driver.trial = true; break;
            case 11: driver.com.sc_flags = IWM_FLAG_HW_ERR; break;
            case 12: driver.com.sc_flags = IWM_FLAG_RFKILL; break;
        }
        driver.iwm_native_newstate_task();
        assert(driver.steps.empty());
        assert(driver.gate.wakeups == (fault >= 5));
        if (fault == 5) {
            assert(driver.nativeStateError == EBUSY && driver.nativeFirmwareOwner.owns(2));
            assert(driver.nativeFirmwareOwner.release(2));
        } else if (fault >= 6) assert(driver.nativeStateError == ENXIO);
        ++traces;
    }
    printf("Actual native state worker: %u transition/admission/fault traces; no automatic init/scan; hardware and gate mocked\n", traces);
    const uint8_t peer[6] = {2,1,2,3,4,5};
    unsigned observations = 0;
    for (auto stage : {ItlJoin::Stage::Auth, ItlJoin::Stage::Assoc, ItlJoin::Stage::Deauth, ItlJoin::Stage::Disassoc})
        for (uint16_t status : {uint16_t(0), uint16_t(1), uint16_t(17), uint16_t(65535)})
            for (unsigned fault = 0; fault < 6; ++fault) {
                ItlIwm driver;
                assert(driver.nativeJoinProgress.begin(peer, driver.com.sc_generation));
                ItlJoin::Event event = {};
                memcpy(event.bssid, peer, 6);
                if (stage == ItlJoin::Stage::Assoc) {
                    event.stage = ItlJoin::Stage::Auth;
                    driver.iwm_native_join_event(event);
                }
                driver.gate.wakeups = 0;
                event.stage = stage; event.status = status;
                switch (fault) {
                    case 1: event.bssid[5] ^= 2; break;
                    case 2: ++driver.com.sc_generation; break;
                    case 3: driver.nativeDetached = true; break;
                    case 4: driver.com.sc_flags |= IWM_FLAG_SHUTDOWN; break;
                    case 5: driver.loop.gate = false; break;
                }
                driver.iwm_native_join_event(event);
                assert(driver.gate.wakeups == !fault);
                const bool disconnect = stage == ItlJoin::Stage::Deauth || stage == ItlJoin::Stage::Disassoc;
                const bool failed = !fault && (disconnect || status);
                assert(driver.nativeJoinProgress.failed() == failed);
                assert(driver.nativeStateError == (failed ? disconnect ? ENETDOWN : ECONNREFUSED : 0));
                if (!fault) {
                    event.status ^= 0x7f;
                    const auto *copy = disconnect ? driver.nativeJoinProgress.disconnect() :
                        stage == ItlJoin::Stage::Auth ? driver.nativeJoinProgress.auth() : driver.nativeJoinProgress.assoc();
                    assert(copy && copy->status == status && !memcmp(copy->bssid, peer, 6));
                    driver.iwm_native_join_event(event); // Duplicate cannot overwrite the first result.
                    assert(driver.gate.wakeups == 1 && copy->status == status);
                    assert(!driver.nativeJoinProgress.begin(peer, driver.com.sc_generation));
                    SequoiaWCL::AssocEvent encoded;
                    memset(&encoded, 0xa5, sizeof(encoded));
                    const auto before = encoded;
                    const bool ready = stage == ItlJoin::Stage::Assoc || (stage == ItlJoin::Stage::Auth && status);
                    assert(SequoiaWCL::encodeObservedAssociation(driver.nativeJoinProgress, encoded) == ready);
                    if (!ready) assert(!memcmp(&encoded, &before, sizeof(encoded)));
                    else {
                        assert(encoded.ieeeStatus == status && encoded.joinReason == 0xffff && encoded.authResultValid);
                        assert(!memcmp(encoded.bssid, peer, 6) && !encoded.reserved0b);
                        const auto &detail = stage == ItlJoin::Stage::Auth ? encoded.auth : encoded.assoc;
                        assert(detail.status == (status ? 0xe3ff8100u : 0));
                        assert(detail.reason == (status ? status < 0x45 ? 0xe0821000u | status : 0xe3ff8100u : 0));
                    }
                }
                assert(driver.steps.empty());
                ++observations;
            }
    ItlJoin::Progress progress;
    uint8_t zero[6] = {}, multicast[6] = {1,2,3,4,5,6};
    assert(!progress.begin(nullptr, 1) && !progress.begin(zero, 1) && !progress.begin(multicast, 1));
    assert(progress.begin(peer, 1));
    ItlJoin::Event early = {}; early.stage = ItlJoin::Stage::Assoc; memcpy(early.bssid, peer, 6);
    assert(!progress.record(early, 1) && !progress.assoc());
    progress.clear(); assert(!progress.active() && !progress.auth() && !progress.failed());
    assert(!progress.record(early, 1) && progress.begin(peer, 2));
    printf("Actual HAL native event observer: %u identity/generation/admission/status/copy traces, duplicates and out-of-order association rejected\n", observations);
    for (unsigned status = 0; status <= UINT16_MAX; ++status) {
        const auto detail = SequoiaWCL::observedJoinDetail(status);
        assert(detail.status == (status ? 0xe3ff8100u : 0));
        assert(detail.reason == (status ? status < 0x45 ? 0xe0821000u | status : 0xe3ff8100u : 0));
    }
    puts("WCL association encoding: observed results only, auth-only success suppressed, 65536 IEEE status boundaries mapped without invented Broadcom transport codes");
}
