#include <HAL/ItlJoinEvent.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <functional>

enum { IEEE80211_S_INIT, IEEE80211_S_AUTH, IEEE80211_S_ASSOC, IEEE80211_S_RUN };
enum { IEEE80211_M_STA = 1, IFF_RUNNING = 1, IWM_FLAG_SHUTDOWN = 1, IWM_FLAG_HW_ERR = 2,
       IWM_FLAG_RFKILL = 4, IEEE80211_FC0_SUBTYPE_DEAUTH = 0xc0, IEEE80211_FC0_SUBTYPE_DISASSOC = 0xa0,
       kMillisecondScale = 1000000, THREAD_UNINT = 0 };
#define nitems(array) (sizeof(array) / sizeof((array)[0]))
using AbsoluteTime = uint64_t;
static uint64_t testClock;
static uint64_t airport_up_time() { return testClock; }
static void clock_interval_to_deadline(unsigned count, unsigned scale, AbsoluteTime *deadline) {
    assert(count == 1 && scale == kMillisecondScale); *deadline = testClock + 1;
}
struct Queue { bool full = false, empty = true; };
static bool mq_full(Queue *queue) { return queue->full; }
static bool mq_empty(Queue *queue) { return queue->empty; }
struct Stats { unsigned outputErrors = 0; };
struct Ifnet { unsigned if_flags = IFF_RUNNING; Stats *netStat = nullptr; };
struct Node { uint8_t ni_bssid[6] = {2,0,0,0,0,0}; };
struct Com {
    int ic_state = IEEE80211_S_RUN, ic_opmode = IEEE80211_M_STA;
    Ifnet ic_if;
    Node *ic_bss = nullptr;
    Queue ic_mgtq;
};
static int sendError;
static unsigned sends, sentType, sentReason;
static std::function<void()> afterSend;
static int ieee80211_send_mgmt(Com *, Node *, unsigned type, unsigned reason, unsigned arg) {
    assert(!arg); ++sends; sentType = type; sentReason = reason;
    if (afterSend) afterSend(); return sendError;
}
struct ItlIwm {
    struct Loop {
        bool gated = true, on = false;
        bool inGate() { return gated; }
        bool onThread() { return on; }
    } loop;
    struct Gate {
        unsigned waits = 0;
        std::function<void()> afterWait;
        void commandSleep(void *, AbsoluteTime deadline, int) {
            ++waits; testClock = deadline;
            if (afterWait) afterWait();
        }
    } gate;
    struct {
        Com sc_ic;
        int sc_generation = 1;
        unsigned sc_flags = 0, cmdqid = 3;
        struct { unsigned queued = 0; } txq[8];
    } com;
    bool nativeDetached = false, commandTransportReady = true, leaseAvailable = true;
    int nativeStateError = 0;
    unsigned leases = 0, kicks = 0;
    Node node; Stats stats;
    ItlJoin::Progress nativeJoinProgress;
    struct FirmwareLease {
        ItlIwm *driver;
        bool acquired;
        FirmwareLease(ItlIwm *d) : driver(d), acquired(d->leaseAvailable) { if (acquired) ++d->leases; }
        ~FirmwareLease() { if (acquired) --driver->leases; }
        explicit operator bool() const { return acquired; }
    };
    Loop *getMainWorkLoop() { return &loop; }
    Gate *getMainCommandGate() { return &gate; }
    void iwm_start(Ifnet *) { ++kicks; }
    int iwm_native_leave(bool, uint16_t, unsigned);
    ItlIwm() {
        com.sc_ic.ic_bss = &node; com.sc_ic.ic_if.netStat = &stats;
        assert(nativeJoinProgress.begin(node.ni_bssid, 1));
    }
};
#include "../itlwm/hal_iwm/native_leave_sequoia.inc"

int main()
{
    unsigned traces = 0;
    for (unsigned mode = 0; mode < 2; ++mode) {
        for (unsigned fault = 0; fault < 25; ++fault) {
            ItlIwm d;
            sends = sentType = sentReason = 0; sendError = 0; afterSend = {};
            testClock = 1000;
            unsigned reason = 3, waitMs = 10;
            int expected = 0;
            switch (fault) {
                case 1: d.loop.gated = false; expected = EWOULDBLOCK; break;
                case 2: d.loop.on = true; expected = EWOULDBLOCK; break;
                case 3: reason = 0; expected = EINVAL; break;
                case 4: waitMs = 1001; expected = EINVAL; break;
                case 5: d.leaseAvailable = false; expected = EBUSY; break;
                case 6: d.com.sc_ic.ic_state = IEEE80211_S_INIT; break;
                case 7: d.nativeDetached = true; expected = ENETDOWN; break;
                case 8: d.commandTransportReady = false; expected = ENETDOWN; break;
                case 9: d.nativeStateError = EIO; expected = ENETDOWN; break;
                case 10: d.com.sc_flags = IWM_FLAG_RFKILL; expected = ENETDOWN; break;
                case 11: d.com.sc_ic.ic_if.if_flags = 0; expected = ENETDOWN; break;
                case 12: d.com.sc_ic.ic_bss = nullptr; expected = ENETDOWN; break;
                case 13: d.node.ni_bssid[5] = 1; expected = ENETDOWN; break;
                case 14: d.com.sc_ic.ic_opmode = 3; expected = EINVAL; break;
                case 15: d.com.sc_ic.ic_if.netStat = nullptr; expected = ENXIO; break;
                case 16: d.com.sc_ic.ic_mgtq.full = true; expected = ENOBUFS; break;
                case 17: sendError = ENOMEM; expected = ENOMEM; break;
                case 18: afterSend = [&] { ++d.stats.outputErrors; }; expected = EIO; break;
                case 19: d.com.txq[d.com.cmdqid].queued = 1; break;
                case 20:
                    d.com.txq[0].queued = 1;
                    d.gate.afterWait = [&] { if (d.gate.waits == 3) d.com.txq[0].queued = 0; };
                    break;
                case 21:
                    d.com.sc_ic.ic_mgtq.empty = false;
                    d.gate.afterWait = [&] { if (d.gate.waits == 4) d.com.sc_ic.ic_mgtq.empty = true; };
                    break;
                case 22: d.com.txq[0].queued = 1; expected = ETIMEDOUT; break;
                case 23:
                    d.com.txq[0].queued = 1;
                    d.gate.afterWait = [&] { ++d.com.sc_generation; }; expected = ENETDOWN; break;
                case 24: waitMs = 0; d.com.txq[0].queued = 1; expected = ETIMEDOUT; break;
            }
            assert(d.iwm_native_leave(mode, reason, waitMs) == expected && !d.leases);
            const bool called = fault == 0 || fault >= 17;
            assert(sends == called);
            if (called) assert(sentType == (mode ? IEEE80211_FC0_SUBTYPE_DEAUTH : IEEE80211_FC0_SUBTYPE_DISASSOC) && sentReason == reason);
            if (fault == 20) assert(d.gate.waits == 3 && d.kicks == 3);
            if (fault == 21) assert(d.gate.waits == 4 && d.kicks == 4);
            if (fault == 22) assert(d.gate.waits == 10);
            if (fault == 23) assert(d.gate.waits == 1 && !d.kicks);
            if (fault == 24) assert(d.gate.waits == 100);
            ++traces;
        }
    }
    std::printf("Native leave HAL: %u actual gate/lease/management-send/drain/timeout/generation/error traces passed; hardware and TX completion mocked\n", traces);
}
