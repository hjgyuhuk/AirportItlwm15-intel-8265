#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <net/if.h>
#include <errno.h>
#define IO80211FAMILY_V2 1
#define __MAC_15_0 150000
#define __IO80211_TARGET __MAC_15_0
#include "sequoia_join_bsd_types.inc"
#define XYLog(...) ((void)0)
#define IC2IFP(ic) (&(ic)->ic_if)
#define container_of(ptr, type, member) reinterpret_cast<type *>(reinterpret_cast<char *>(ptr) - offsetof(type, member))

struct ieee80211_node {};
struct _ifnet { void *if_softc = nullptr; };
struct ieee80211com {
    _ifnet ic_if;
    ieee80211_node *ic_bss = nullptr;
    ieee80211_state ic_state = IEEE80211_S_INIT;
};
struct task { unsigned added = 0, removed = 0; };
struct taskq {};
static taskq systemQueue;
static taskq *systq = &systemQueue;
struct iwm_softc {
    ieee80211com sc_ic;
    ieee80211_state ns_nstate = IEEE80211_S_INIT;
    int ns_arg = 777;
    unsigned sc_calib_to = 0, sc_flags = 0;
    task ba_task, mac_ctxt_task, chan_ctxt_task, newstate_task;
    taskq *sc_nswq = nullptr;
};
static unsigned ampduStops = 0, baDeletes = 0;
static int submitResult = 1;
static int task_add_checked(taskq *queue, task *work) {
    assert(queue);
    if (submitResult >= 0) ++work->added;
    return submitResult;
}
struct ItlIwm {
    iwm_softc com;
    bool nativeDetached = false;
    int nativeStateError = 0;
    uint64_t nativeStateSerial = 0;
    struct Loop { bool gate = true; bool inGate() const { return gate; } } loop;
    Loop *getMainWorkLoop() { return &loop; }
    static int iwm_newstate(ieee80211com *, ieee80211_state, int);
    void iwm_del_task(iwm_softc *sc, taskq *queue, task *item) {
        assert(sc == &com && queue == systq); ++item->removed;
    }
    void iwm_add_task(iwm_softc *sc, taskq *queue, task *item) {
        assert(sc == &com && queue == com.sc_nswq); ++item->added;
    }
};
#include "sequoia_join_state_function.inc"

int main()
{
    unsigned cases = 0;
    for (unsigned current = IEEE80211_S_INIT; current <= IEEE80211_S_RUN; ++current)
        for (unsigned pending = IEEE80211_S_INIT; pending <= IEEE80211_S_RUN; ++pending)
            for (unsigned requested = IEEE80211_S_INIT; requested <= IEEE80211_S_RUN; ++requested) {
                ItlIwm driver;
                auto &sc = driver.com;
                ieee80211_node node;
                taskq queue;
                sc.sc_ic.ic_if.if_softc = &sc;
                sc.sc_ic.ic_bss = &node;
                sc.sc_ic.ic_state = static_cast<ieee80211_state>(current);
                sc.ns_nstate = static_cast<ieee80211_state>(pending);
                sc.sc_nswq = &queue;
                ampduStops = baDeletes = 0;
                assert(!ItlIwm::iwm_newstate(&sc.sc_ic, static_cast<ieee80211_state>(requested), 123));
                const bool skipped = requested == pending && requested != IEEE80211_S_SCAN && requested != IEEE80211_S_AUTH;
                const bool cleanup = false; // Deferred until the worker owns firmware.
                assert(sc.newstate_task.added == !skipped);
                assert(sc.sc_calib_to == cleanup && sc.ba_task.removed == cleanup &&
                       sc.mac_ctxt_task.removed == cleanup && sc.chan_ctxt_task.removed == cleanup);
                assert(ampduStops == (cleanup && requested == IEEE80211_S_SCAN) && baDeletes == ampduStops);
                assert(sc.ns_nstate == (skipped ? pending : requested) && sc.ns_arg == (skipped ? 777 : 123));
                assert(sc.sc_ic.ic_state == current);
                assert(driver.nativeStateSerial == !skipped);
                ++cases;
            }
    printf("Actual iwm_newstate: %u current/pending/requested transitions passed; task scheduling mocked\n", cases);
    for (unsigned fault = 0; fault < 7; ++fault) {
        ItlIwm driver;
        auto &sc = driver.com;
        taskq queue;
        sc.sc_nswq = &queue;
        sc.sc_ic.ic_if.if_softc = &sc;
        switch (fault) {
            case 0: driver.nativeDetached = true; break;
            case 1: driver.loop.gate = false; break;
            case 2: sc.sc_flags = IWM_FLAG_SHUTDOWN; break;
            case 3: submitResult = -1; break;
            case 4: submitResult = 0; break;
            case 5: driver.nativeStateError = EIO; break;
            case 6: driver.nativeStateSerial = UINT64_MAX; break;
        }
        const int error = ItlIwm::iwm_newstate(&sc.sc_ic, IEEE80211_S_AUTH, 123);
        if (fault == 4) assert(!error && sc.ns_nstate == IEEE80211_S_AUTH && sc.ns_arg == 123);
        else assert(error == (fault == 5 ? EIO : fault == 6 ? EOVERFLOW : ENXIO) &&
                    sc.ns_nstate == IEEE80211_S_INIT && sc.ns_arg == 777 && !sc.newstate_task.added);
        submitResult = 1;
    }
    puts("Native state admission: stopped/detached/ungated, rejected queue rollback and pending coalescence passed");
}
