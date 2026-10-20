#ifndef IWM_COMMAND_TEST_SUPPORT_H
#define IWM_COMMAND_TEST_SUPPORT_H

#define XYLog(...) ((void)0)

#include <HAL/IwmCommandSlots.h>
#include <HAL/IwmCommandId.h>
#include <HAL/IwmNvmDiagnostics.h>
#include <HAL/IwmFirmwareTrial.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#ifdef IWM_SCAN_TRANSPORT_INTEGRATION
#include <HAL/IwmScanTrial.h>
#define __packed __attribute__((packed))
#define ETHER_ADDR_LEN 6
#define IEEE80211_NWID_LEN 32
#define IEEE80211_ADDR_LEN 6
#define le32toh(value) (value)
using bus_addr_t = uint64_t;
struct ieee80211_frame { uint8_t bytes[24]; };
enum { EDCA_AC_BE, EDCA_AC_BK, EDCA_AC_VI, EDCA_AC_VO };
static uint32_t le32_to_cpup(const uint32_t *pointer) { uint32_t value; memcpy(&value, pointer, 4); return value; }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-variable-sized-type-not-at-end"
#pragma clang diagnostic ignored "-Wunused-function"
#include "../itlwm/hal_iwm/if_iwmreg.h"
#pragma clang diagnostic pop
#define isset(bytes, bit) ((bytes)[(bit) / 8] & (1 << ((bit) % 8)))
constexpr int IWM_AUX_STA_ID = 1, IWM_FLAG_RFKILL = 2;
#endif

#define _KASSERT(value) assert(value)
#define nitems(value) (sizeof(value) / sizeof((value)[0]))
#define htole16(value) (value)
#define le16toh(value) (value)
#define htole32(value) (value)
#define mtod(buffer, type) reinterpret_cast<type>((buffer)->bytes.data())
constexpr unsigned IWM_CMD_ASYNC = 1, IWM_CMD_WANT_RESP = 2;
constexpr size_t IWM_CMD_RESP_MAX = 4096;
constexpr size_t IWM_TX_RING_COUNT = 256;
constexpr int IWM_FLAG_SHUTDOWN = 0x100, IWM_FLAG_HW_ERR = 0x80;
constexpr int IWM_DEVICE_FAMILY_7000 = 7000, IWM_DEVICE_FAMILY_9000 = 9000, MBUF_DONTWAIT = 0;
constexpr int IWM_CMD_SEND_IN_RFKILL = 4;
constexpr int IWM_UCODE_TYPE_INIT = 1, IWM_UCODE_TYPE_REGULAR = 0;
#ifndef IWM_SCAN_TRANSPORT_INTEGRATION
constexpr size_t IWM_MAX_CMD_PAYLOAD_SIZE = 4088;
constexpr int IWM_CMD_FAILED_MSK = 0x40, IWM_HBUS_TARG_WRPTR = 0;
constexpr int IWM_NVM_ACCESS_CMD = 0x88, IWM_BT_CONFIG = 0x9b;
constexpr int IWM_NVM_READ_OPCODE = 0, IWM_NVM_DEFAULT_CHUNK_SIZE = 2048;
constexpr int IWM_REPLY_SF_CFG_CMD = 0xd1, IWM_TX_ANT_CONFIGURATION_CMD = 0x98;
constexpr int IWM_PHY_CONFIGURATION_CMD = 0x6a, IWM_SF_INIT_OFF = 3;
constexpr int IWM_BT_COEX_NW = 1, IWM_BT_COEX_HIGH_BAND_RET = 16;
constexpr int IWM_BT_COEX_SYNC2SCO_ENABLED = 4, IWM_BT_COEX_MPLUT_ENABLED = 2;
#endif
constexpr int M_DEVBUF = 0, M_NOWAIT = 0, M_ZERO = 0;
constexpr int THREAD_INTERRUPTIBLE = 1, THREAD_AWAKENED = 0;
constexpr int THREAD_TIMED_OUT = 1, THREAD_INTERRUPTED = 2;
constexpr unsigned kSecondScale = 1;
using IOReturn = int;
using bus_addr_t = uint64_t;
using AbsoluteTime = std::chrono::steady_clock::time_point;

#ifndef IWM_SCAN_TRANSPORT_INTEGRATION
static uint8_t iwm_cmd_groupid(uint32_t id) { return (id >> 8) & 0xff; }
static uint8_t iwm_cmd_opcode(uint32_t id) { return id & 0xff; }
static uint8_t iwm_cmd_version(uint32_t id) { return (id >> 16) & 0xff; }
static uint16_t iwm_get_dma_hi_addr(uint64_t value) { return (value >> 32) & 0xf; }
#endif
[[noreturn]] static inline void panic(const char *message) { fputs(message, stderr); abort(); }

struct OSObject {};
class TestLoop;
static thread_local TestLoop *currentLoop = nullptr;
static thread_local unsigned gateDepth = 0;
static thread_local bool eventThread = false;

class TestLoop {
public:
    using Action = IOReturn (*)(OSObject *, void *, void *, void *, void *);
    std::recursive_mutex mutex;
    std::condition_variable_any condition;
    std::atomic<unsigned> sleeps{0};
    int interrupt = 0;

    bool inGate() const { return currentLoop == this; }
    bool onThread() const { return eventThread; }
    IOReturn runAction(Action action, OSObject *owner, void *a = nullptr, void *b = nullptr,
                       void *c = nullptr, void *d = nullptr) {
        std::lock_guard<std::recursive_mutex> guard(mutex);
        assert(currentLoop == nullptr || currentLoop == this);
        currentLoop = this;
        ++gateDepth;
        auto result = action(owner, a, b, c, d);
        if (--gateDepth == 0) currentLoop = nullptr;
        return result;
    }
    int commandSleep(void *, AbsoluteTime deadline, int) {
        assert(inGate() && gateDepth);
        const unsigned depth = gateDepth;
        for (unsigned i = 1; i < depth; ++i) mutex.unlock();
        currentLoop = nullptr;
        gateDepth = 0;
        ++sleeps;
        const auto status = condition.wait_until(mutex, deadline);
        currentLoop = this;
        gateDepth = depth;
        for (unsigned i = 1; i < depth; ++i) mutex.lock();
        if (interrupt) { const auto value = interrupt; interrupt = 0; return value; }
        return status == std::cv_status::timeout ? THREAD_TIMED_OUT : THREAD_AWAKENED;
    }
    void commandWakeup(void *, bool) { assert(inGate()); condition.notify_all(); }
};

static void clock_interval_to_deadline(unsigned seconds, unsigned, AbsoluteTime *deadline)
{
    assert(seconds == 1 || seconds == 2 || seconds == 10);
    // Shorter test clock; production uses a real two-second absolute deadline.
    *deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
}

#ifndef IWM_SCAN_TRANSPORT_INTEGRATION
struct iwm_cmd_header { uint8_t code, flags, idx, qid; };
struct iwm_cmd_header_wide { uint8_t opcode, group_id, idx, qid; uint16_t length; uint8_t reserved, version; };
struct iwm_device_cmd {
    union {
        struct { iwm_cmd_header hdr; uint8_t data[320]; };
        struct { iwm_cmd_header_wide hdr_wide; uint8_t data_wide[316]; };
    };
};
struct iwm_rx_packet { uint32_t len_n_flags; iwm_cmd_header hdr; };
struct iwm_tfd { struct { uint32_t lo; uint16_t hi_n_len; } tbs[1]; uint8_t num_tbs; };
#endif
struct iwm_host_cmd {
    const void *data[2] = {};
    iwm_rx_packet *resp_pkt = nullptr;
    size_t resp_pkt_len = 0;
    uint32_t flags = 0;
    uint16_t len[2] = {};
    uint32_t id = 0;
};
struct TestMbuf { std::vector<uint8_t> bytes; };
using mbuf_t = TestMbuf *;
static std::atomic<int> liveMbufs{0};
static bool failMbuf = false, failResponse = false;
static std::set<void *> responseAllocations;
static void *testResponseAllocate(size_t bytes, int, int) {
    if (failResponse) return nullptr;
    auto *pointer = calloc(1, bytes);
    assert(pointer); responseAllocations.insert(pointer);
    return pointer;
}
static void testResponseFree(void *pointer) {
    if (!pointer) return;
    assert(responseAllocations.erase(pointer) == 1);
    free(pointer);
}
static int mbuf_allocpacket(int, size_t bytes, unsigned *, mbuf_t *output) {
    if (failMbuf) { *output = nullptr; return ENOMEM; }
    *output = new TestMbuf{std::vector<uint8_t>(bytes)}; ++liveMbufs; return 0;
}
static void mbuf_freem(mbuf_t buffer) { assert(buffer); delete buffer; --liveMbufs; }
static void mbuf_setlen(mbuf_t buffer, size_t bytes) { assert(buffer->bytes.size() == bytes); }
static void mbuf_pkthdr_setlen(mbuf_t buffer, size_t bytes) { assert(buffer->bytes.size() == bytes); }
struct IOPhysicalSegment { uint64_t location, length; };
struct TestCursor {
    unsigned segments = 1;
    uint64_t address = 0x20000;
    bool shortSegment = false;
    unsigned getPhysicalSegmentsWithCoalesce(mbuf_t buffer, IOPhysicalSegment *segment, unsigned) {
        *segment = {address, buffer->bytes.size() - (shortSegment ? 1 : 0)};
        return segments;
    }
};
struct TestMap { TestCursor *cursor; unsigned dm_nsegs; };
struct iwm_tx_data { mbuf_t m = nullptr; TestMap *map = nullptr; uint64_t cmd_paddr = 0x10000; };
struct iwm_tx_ring {
    iwm_tfd *desc;
    iwm_device_cmd *cmd;
    iwm_tx_data data[IWM_TX_RING_COUNT];
    int qid = 0, cur = 0, queued = 0, read = 0, tail = 0;
};
#ifndef IWM_SCAN_TRANSPORT_INTEGRATION
constexpr unsigned IWM_PHY_DB_CMD = 0x6c, IWM_PHY_DB_CFG = 1, IWM_PHY_DB_CALIB_NCH = 2;
constexpr unsigned IWM_PHY_DB_CALIB_CHG_PAPD = 4, IWM_PHY_DB_CALIB_CHG_TXP = 5;
struct iwm_phy_db_cmd { uint16_t type, length; };
struct iwm_phy_db_chg_txp { uint32_t space; uint16_t max_channel_idx; } __attribute__((packed));
#endif
struct iwm_phy_db_entry { uint16_t size = 0; uint8_t *data = nullptr; };
struct iwm_softc {
    iwm_phy_db_entry phySections[6][9];
    int sc_uc_current = IWM_UCODE_TYPE_INIT;
    uint32_t sc_fw_phy_config = 0x1234;
    struct { uint32_t event_trigger = 0x12, flow_trigger = 0x34; } sc_default_calib[2];
    int sc_fw = 0, sc_uc = 0, sc_init_complete = 0;
    int cmdqid = 0, sc_flags = 0, sc_device_family = IWM_DEVICE_FAMILY_7000;
    iwm_tx_ring txq[2];
#ifdef IWM_SCAN_TRANSPORT_INTEGRATION
    int sc_mqrx_supported = 0;
    unsigned sc_capa_n_scan_channels = 40, agg_queue_mask = 0, sched_base = 0;
    uint8_t sc_enabled_capa[16] = {0, 0x10}, sc_ucode_api[16] = {};
    struct { uint8_t hw_addr[6] = {2}; } sc_nvm;
    struct { struct { unsigned ic_freq = 0, ic_flags = 0; } ic_channels[256]; } sc_ic;
#endif
    uint8_t *sc_cmd_resp_pkt[IWM_TX_RING_COUNT] = {};
    size_t sc_cmd_resp_len[IWM_TX_RING_COUNT] = {};
};

#ifndef IWM_SCAN_TRANSPORT_INTEGRATION
struct iwm_nvm_access_cmd { uint8_t op_code, target; uint16_t type, offset, length; };
struct iwm_bt_coex_cmd { uint32_t mode, enabled_modules; };
struct iwm_sf_cfg_cmd { uint32_t state, rest[22]; };
struct iwm_tx_ant_cfg_cmd { uint32_t valid; };
struct iwm_phy_cfg_cmd { uint32_t phy_cfg; struct { uint32_t event_trigger, flow_trigger; } calib_control; };
#endif

class ItlIwm : public OSObject {
public:
    TestLoop loop;
    IwmCommand::Slots<IWM_TX_RING_COUNT> commandSlots;
    bool commandTransportReady = true;
    bool nativeNvmTrial = false;
    bool nativeFirmwareTrial = false;
    bool isHardwareTrial() const { return nativeNvmTrial || nativeFirmwareTrial; }
    IwmFirmwareDiagnostics nativeFirmwareDiagnostics;
    bool nativeCalibrationActive = false;
    IwmNvmDiagnostics nativeNvmDiagnostics;
    uint64_t commandEpoch = 0;
    iwm_softc sc = {};
    iwm_tfd descriptors[IWM_TX_RING_COUNT] = {};
    iwm_device_cmd commands[IWM_TX_RING_COUNT] = {};
    TestCursor cursor;
    TestMap mapping{&cursor, 0};
    bool nicAvailable = true;
    unsigned nicLocks = 0, submissions = 0;
    std::function<void()> onKick;

    ItlIwm() {
        sc.txq[0].desc = descriptors; sc.txq[0].cmd = commands;
        for (auto &data : sc.txq[0].data) data.map = &mapping;
    }
    ~ItlIwm() {
        gated([&] { reset(); });
        assert(liveMbufs == 0 && responseAllocations.empty());
    }
    TestLoop *getMainWorkLoop() { return &loop; }
    TestLoop *getMainCommandGate() { return &loop; }
    int iwm_send_cmd(iwm_softc *, iwm_host_cmd *);
    bool iwm_firmware_trial_command_allowed(iwm_softc *, const iwm_host_cmd *);
#ifdef IWM_SCAN_TRANSPORT_INTEGRATION
    bool iwm_scan_trial_command_allowed(iwm_softc *, const iwm_host_cmd *);
    bool nativeNet80211Attached = false;
    const void *nativePassiveRequest = nullptr;
    size_t nativePassiveRequestBytes = 0;
    ItlScan::IwmPassiveTrial nativePassiveScan;
    int iwm_passive_scan_trial(iwm_softc *);
    void iwm_scan_trial_rx(uint16_t, const void *, size_t);
    int iwm_send_dqa_cmd(iwm_softc *);
    int iwm_add_aux_sta(iwm_softc *);
    int iwm_enable_txq(iwm_softc *, int, int, int, int, int, int);
    int iwm_send_cmd_pdu(iwm_softc *, uint32_t, uint32_t, uint16_t, const void *);
    int iwm_send_cmd_status(iwm_softc *, iwm_host_cmd *, uint32_t *);
    int iwm_send_cmd_pdu_status(iwm_softc *, uint32_t, uint16_t, const void *, uint32_t *);
    void iwm_free_resp(iwm_softc *, iwm_host_cmd *);
    int iwm_is_valid_channel(unsigned channel) { return channel != 144; }
    uint8_t iwm_fw_valid_rx_ant(iwm_softc *) { return 3; }
    int iwm_enable_ac_txq(iwm_softc *, int, int) { assert(false); return ENOTSUP; }
    void iwm_nic_assert_locked(iwm_softc *) { assert(nicLocks); }
    void iwm_write_prph(iwm_softc *, uint32_t, uint32_t) { assert(nicLocks); }
    void iwm_write_mem32(iwm_softc *, uint32_t, uint32_t) { assert(nicLocks); }
    uint32_t iwm_read_prph(iwm_softc *, uint32_t) { assert(nicLocks); return 0; }
    void writeRegister(uint32_t reg, uint32_t value) {
        assert(reg == IWM_HBUS_TARG_WRPTR && nicLocks);
        if ((value >> 8) == unsigned(sc.cmdqid)) kick();
        else assert((value >> 8) == IWM_DQA_AUX_QUEUE);
    }
#else
    bool iwm_scan_trial_command_allowed(iwm_softc *, const iwm_host_cmd *) { return false; }
    void writeRegister(uint32_t, uint32_t) { kick(); }
#endif
    int nativePassiveDone = 0;
    int nativeScanDone = 0;
    iwm_phy_db_entry *iwm_phy_db_get_section(iwm_softc *state, uint16_t type, uint16_t group) {
        return type < 6 && group < 9 ? &state->phySections[type][group] : nullptr;
    }
    uint8_t iwm_fw_valid_tx_ant(iwm_softc *) { return 3; }
    void iwm_command_ack(iwm_softc *, const iwm_rx_packet *, size_t);
    void iwm_command_reset(iwm_softc *);
    int iwm_wait_status(const int *, int, void *, unsigned, const int * = nullptr);
    bool iwm_nic_lock(iwm_softc *) { if (!nicAvailable) return false; ++nicLocks; return true; }
    void iwm_nic_unlock(iwm_softc *) { assert(nicLocks > 0); --nicLocks; }
    void iwm_update_sched(iwm_softc *, int, int, int, int) { assert(loop.inGate()); }
    void kick() { ++submissions; if (onKick) onKick(); }
    void gated(const std::function<void()> &action) {
        loop.runAction([](OSObject *, void *fn, void *, void *, void *) -> IOReturn {
            (*static_cast<const std::function<void()> *>(fn))(); return 0;
        }, this, const_cast<std::function<void()> *>(&action));
    }
    void acknowledge(unsigned index, uint16_t command, int error = 0, size_t size = sizeof(iwm_rx_packet)) {
        std::vector<uint8_t> bytes(size < sizeof(iwm_rx_packet) ? sizeof(iwm_rx_packet) : size, 0xa5);
        auto *packet = reinterpret_cast<iwm_rx_packet *>(bytes.data());
        packet->hdr = {iwm_cmd_opcode(command), static_cast<uint8_t>(iwm_cmd_groupid(command) |
                       (error ? IWM_CMD_FAILED_MSK : 0)), static_cast<uint8_t>(index), 0};
        iwm_command_ack(&sc, packet, size);
    }
    void reset() {
        for (auto &data : sc.txq[0].data) if (data.m) { mbuf_freem(data.m); data.m = nullptr; }
        sc.txq[0].queued = sc.txq[0].cur = 0; nicLocks = 0;
        iwm_command_reset(&sc);
    }
};

#undef IWM_WRITE
#define IWM_WRITE(sc, reg, value) writeRegister(reg, value)
#define malloc(bytes, type, flags) testResponseAllocate(bytes, type, flags)
#define free(pointer) testResponseFree(pointer)
#include "../itlwm/hal_iwm/command_sequoia.inc"
#undef malloc
#undef free
#endif
