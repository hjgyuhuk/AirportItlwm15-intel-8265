#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include "HAL/ItlBootProgress.h"

#define htole32(x) (x)
#define XYLog(...) ((void)0)
#include "iwx_command_types.inc"
static ItlBootProgress progress;
#define ITLWM_HAL_STAGE(label) progress.record(label)
#define ITLWM_HAL_RESULT(value) progress.recordResult(value)
#define ITLWM_IWX_DETAIL(key, value) progress.iwxRecord(ItlBootProgress::key, (value))
#define IEEE80211_ADDR_EQ(a, b) (!memcmp((a), (b), 6))
#define IEEE80211_ADDR_COPY(a, b) memcpy((a), (b), 6)
static const uint8_t etheranyaddr[6] = {};
static uint64_t SEC_TO_NSEC(unsigned value) { return uint64_t(value) * 1000000000; }
enum { IWX_CMD_SEND_IN_RFKILL = 4 };
struct iwx_softc {
    unsigned sc_flags = 0, sc_init_complete = 0;
    bool sc_tx_with_siso_diversity = false;
    struct { uint8_t ic_myaddr[6] = {}; } sc_ic;
    struct { uint8_t hw_addr[6] = {2, 1, 2, 3, 4, 5}; } sc_nvm;
};
class ItlIwx {
public:
    int firmwareError = 0, commandError = 0, sleepError = 0, nvmError = 0;
    unsigned commands = 0, sleeps = 0, nvmReads = 0, failCommand = 0;
    bool earlyComplete = false, completeOnWake = true;
    int iwx_load_ucode_wait_alive(iwx_softc *) { return firmwareError; }
    int iwx_send_cmd_pdu(iwx_softc *sc, uint32_t id, unsigned, unsigned, const void *) {
        ++commands;
        assert(id == (commands == 1 ? IWX_WIDE_ID(IWX_SYSTEM_GROUP, IWX_INIT_EXTENDED_CFG_CMD) :
                      IWX_WIDE_ID(IWX_REGULATORY_AND_NVM_GROUP, IWX_NVM_ACCESS_COMPLETE)));
        if (commands == failCommand) return commandError;
        if (commands == 2 && earlyComplete) sc->sc_init_complete |= IWX_INIT_COMPLETE;
        return 0;
    }
    int tsleep_nsec(void *event, int, const char *, uint64_t) {
        ++sleeps;
        if (completeOnWake && !sleepError) *static_cast<unsigned *>(event) |= IWX_INIT_COMPLETE;
        return sleepError;
    }
    int iwx_nvm_get(iwx_softc *) { ++nvmReads; return nvmError; }
    int iwx_run_init_mvm_ucode(iwx_softc *, int);
};
#include "iwx_init_method.inc"

int main() {
    // INIT_COMPLETE already arrived while the NVM command awaited its ACK.
    { ItlIwx d; iwx_softc sc; d.earlyComplete = true; d.sleepError = ETIMEDOUT;
      assert(!d.iwx_run_init_mvm_ucode(&sc, 1));
      assert(d.commands == 2 && d.sleeps == 0 && d.nvmReads == 1);
      assert(!memcmp(sc.sc_ic.ic_myaddr, sc.sc_nvm.hw_addr, 6)); }
    { ItlIwx d; iwx_softc sc;
      assert(!d.iwx_run_init_mvm_ucode(&sc, 1) && d.sleeps == 1 && d.nvmReads == 1); }
    for (int error : {ETIMEDOUT, EINTR}) {
        ItlIwx d; iwx_softc sc; d.sleepError = error;
        assert(d.iwx_run_init_mvm_ucode(&sc, 1) == error && !d.nvmReads);
    }
    { ItlIwx d; iwx_softc sc; d.completeOnWake = false;
      assert(d.iwx_run_init_mvm_ucode(&sc, 1) == EIO && !d.nvmReads); }
    for (unsigned command : {1U, 2U}) {
        ItlIwx d; iwx_softc sc; d.failCommand = command; d.commandError = ETIMEDOUT;
        assert(d.iwx_run_init_mvm_ucode(&sc, 1) == ETIMEDOUT);
        assert(d.commands == command && !d.sleeps && !d.nvmReads);
    }
    { ItlIwx d; iwx_softc sc; d.firmwareError = EIO;
      assert(d.iwx_run_init_mvm_ucode(&sc, 1) == EIO && !d.commands); }
    { ItlIwx d; iwx_softc sc; d.nvmError = EIO;
      assert(d.iwx_run_init_mvm_ucode(&sc, 1) == EIO && d.nvmReads == 1); }
    { ItlIwx d; iwx_softc sc;
      assert(!d.iwx_run_init_mvm_ucode(&sc, 0) && !d.nvmReads); }
    puts("IWX init: early notification, normal wake, timeout, interruption, false wake and firmware/command/NVM failures passed; hardware mocked");
}
