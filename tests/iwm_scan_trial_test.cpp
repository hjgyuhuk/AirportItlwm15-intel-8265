#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <HAL/IwmScanTrial.h>
#include <HAL/IwmFirmwareTrial.h>
#include <HAL/IwmNvmDiagnostics.h>
#include <cerrno>

#define __packed __attribute__((packed))
#define ETHER_ADDR_LEN 6
#define IEEE80211_NWID_LEN 32
#define IEEE80211_ADDR_LEN 6
#define htole16(value) (value)
#define htole32(value) (value)
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
static_assert(IWM_WIDE_ID(5 & ~IWM_CMD_FAILED_MSK, 0) == 0x500,
              "Masked DQA response must retain its group");
#define IWM_AUX_STA_ID 1
#include "../itlwm/hal_iwm/scan_request_sequoia.inc"

static void request()
{
    IwmTrial::PassivePlan plan;
    plan.channels[0] = 1;
    plan.channels[1] = 36;
    plan.count = 2;
    plan.capacity = 40;
    plan.source[0] = 2;
    plan.txAnt = plan.rxAnt = 3;
    const size_t size = IwmTrial::passiveRequestSize(plan);
    assert(size && size <= IWM_MAX_CMD_PAYLOAD_SIZE);
    std::vector<uint8_t> bytes(size + 2, 0xa5);
    assert(IwmTrial::buildPassiveRequest(plan, bytes.data() + 1, size));
    assert(bytes.front() == 0xa5 && bytes.back() == 0xa5);
    const auto *command = reinterpret_cast<const iwm_scan_req_lmac *>(bytes.data() + 1);
    assert(command->n_channels == 2 && command->scan_flags == 0x8b);
    assert(command->iter_num == 1 && command->schedule[0].iterations == 1);
    assert(command->max_out_time == 0 && command->suspend_time == 0);
    assert(command->tx_cmd[0].sta_id == IWM_AUX_STA_ID);
    const auto *channels = reinterpret_cast<const iwm_scan_channel_cfg_lmac *>(command->data);
    assert(channels[0].channel_num == 1 && channels[1].channel_num == 36);
    assert(channels[0].flags == IWM_UNIFIED_SCAN_CHANNEL_PARTIAL && channels[1].iter_count == 1);
    for (size_t i = 2; i < 40; ++i) assert(channels[i].channel_num == 0 && channels[i].flags == 0);
    const auto *probe = reinterpret_cast<const iwm_scan_probe_req_v1 *>(command->data + 40 * sizeof(*channels));
    assert(probe->mac_header.len == 26 && probe->buf[0] == 0x40 && probe->buf[10] == 2);
    assert(probe->band_data[0].offset == 26 && probe->band_data[0].len == 16);
    assert(probe->band_data[1].offset == 42 && probe->band_data[1].len == 10);
    assert(probe->common_data.offset == 52 && !probe->common_data.len);
    const auto good = bytes;
    for (unsigned fault = 0; fault < 10; ++fault) {
        auto bad = plan;
        if (fault == 0) bad.count = 0;
        if (fault == 1) bad.capacity = 1;
        if (fault == 2) bad.capacity = ItlScan::IwmPassiveTrial::MaxChannels + 1;
        if (fault == 3) bad.channels[1] = 1;
        if (fault == 4) bad.channels[1] = 144;
        if (fault == 5) bad.channels[1] = 0;
        if (fault == 6) bad.channels[1] = 197;
        if (fault == 7) bad.source[0] = 1;
        if (fault == 8) bad.txAnt = 0;
        if (fault == 9) bad.rxAnt = 8;
        assert(!IwmTrial::passiveRequestSize(bad));
        assert(!IwmTrial::buildPassiveRequest(bad, bytes.data() + 1, size));
        assert(bytes == good);
    }
    assert(!IwmTrial::buildPassiveRequest(plan, bytes.data() + 1, size - 1) && bytes == good);
    SequoiaWCL::ScanRequest nativeRequest = {};
    nativeRequest.version = 1;
    nativeRequest.scanType = 2;
    nativeRequest.passiveDwell = 90;
    nativeRequest.privateMAC = 1;
    const SequoiaWCL::Channel inventory[] = {{1, 1, 0xa}, {1, 36, 0x12}};
    const uint8_t entropy[] = {0x80, 1, 2, 3, 4, 5};
    SequoiaWCL::ScanPlan native;
    assert(native.prepare(&nativeRequest, sizeof(nativeRequest), inventory, 2, plan.source,
                          entropy, 6, false) == SequoiaWCL::ScanPlanStatus::Success);
    assert(IwmTrial::buildNativeScanRequest(native, 40, 3, 3, false, bytes.data() + 1, size));
    assert(command->passive_dwell == 45 && command->scan_flags == 0x8b);
    assert(probe->buf[10] == 0x82 && !memcmp(probe->buf + 10, native.source, 6));
    assert(plan.source[0] == 2);
}

static void activeRequest()
{
    using namespace SequoiaWCL;
    const Channel inventory[] = {{1,1,Width20MHz | Band2GHz | ActiveScan},
                                 {1,36,Width20MHz | Band5GHz},
                                 {1,52,Width20MHz | Band5GHz | ActiveScan | DFS}};
    const uint8_t mac[] = {0x88,1,2,3,4,5}, entropy[] = {2,2,2,2,2,2};
    for (unsigned count = 0; count <= kMaxScanSSIDs; ++count) {
        ScanRequest request = {}; request.version = 1; request.scanType = 1;
        request.activeDwell = 40; request.passiveDwell = 120; request.privateMAC = 1;
        request.ssidCount = count; request.bssid[0] = 2;
        for (unsigned i = 0; i < count; ++i) {
            request.ssids[i].version = 1; request.ssids[i].length = 3;
            request.ssids[i].bytes[0] = 'n'; request.ssids[i].bytes[2] = i;
        }
        ScanPlan native;
        assert(native.prepare(&request, sizeof(request), inventory, 3, mac, entropy, 6, false) == ScanPlanStatus::Success);
        IwmTrial::PassivePlan wire; wire.count = 3; wire.capacity = 40; wire.txAnt = wire.rxAnt = 3;
        wire.channels[0] = 1; wire.channels[1] = 36; wire.channels[2] = 52;
        memcpy(wire.source, native.source, 6);
        const auto size = IwmTrial::passiveRequestSize(wire);
        std::vector<uint8_t> bytes(size + 2, 0xa5);
        assert(IwmTrial::buildNativeScanRequest(native, 40, 3, 3, false, bytes.data() + 1, size));
        assert(bytes.front() == 0xa5 && bytes.back() == 0xa5);
        const auto *command = reinterpret_cast<const iwm_scan_req_lmac *>(bytes.data() + 1);
            assert(command->active_dwell == 20 && command->passive_dwell == 45);
        assert(!(command->scan_flags & (IWM_LMAC_SCAN_FLAG_PASSIVE | IWM_LMAC_SCAN_FLAG_EXTENDED_DWELL)));
        assert(bool(command->scan_flags & IWM_LMAC_SCAN_FLAG_PRE_CONNECTION) == (count == 1));
        const auto *channels = reinterpret_cast<const iwm_scan_channel_cfg_lmac *>(command->data);
        const unsigned selected = ((1u << (count ? count : 1)) - 1) << 1;
        assert(channels[0].flags == (IWM_UNIFIED_SCAN_CHANNEL_PARTIAL | selected));
        assert(channels[1].flags == IWM_UNIFIED_SCAN_CHANNEL_PARTIAL);
        assert(channels[2].flags == IWM_UNIFIED_SCAN_CHANNEL_PARTIAL);
        for (unsigned i = 0; i < count; ++i) {
            assert(command->direct_scan[i].id == 0 && command->direct_scan[i].len == 3);
            assert(!memcmp(command->direct_scan[i].ssid, request.ssids[i].bytes, 3));
        }
        if (!count) assert(!command->direct_scan[0].len);
        const auto *probe = reinterpret_cast<const iwm_scan_probe_req_v1 *>(command->data + 40 * sizeof(*channels));
        assert(!memcmp(probe->buf + 10, native.source, 6) && memcmp(probe->buf + 10, mac, 6));
        assert(!memcmp(probe->buf + 16, request.bssid, 6));
        const auto saved = bytes;
        assert(IwmTrial::buildNativeScanRequest(native, 40, 3, 3, false, bytes.data() + 1, size) && bytes == saved);
        // Both a passive-only non-DFS channel and a DFS channel must remain
        // fully passive when requested alone, even with a directed SSID list.
        for (unsigned channel = 1; channel < 3; ++channel) {
            request.channelCount = 1;
            request.channels[0] = inventory[channel];
            assert(native.prepare(&request, sizeof(request), inventory, 3, mac, entropy, 6, false) == ScanPlanStatus::Success);
            assert(native.passiveFallback && !native.active() && !native.probeSSIDCount());
            assert(IwmTrial::buildNativeScanRequest(native, 40, 3, 3, false, bytes.data() + 1, size));
            assert(command->n_channels == 1 && command->passive_dwell == 45);
            assert(command->scan_flags & IWM_LMAC_SCAN_FLAG_PASSIVE);
            assert(!(command->scan_flags & IWM_LMAC_SCAN_FLAG_PRE_CONNECTION));
            assert(channels[0].flags == IWM_UNIFIED_SCAN_CHANNEL_PARTIAL);
            for (const auto &ssid : command->direct_scan) assert(!ssid.len);
        }
    }
}

static void associatedRequest()
{
    SequoiaWCL::ScanRequest nativeRequest = {};
    nativeRequest.version = 1;
    nativeRequest.scanType = 2;
    nativeRequest.passiveDwell = 90;
    nativeRequest.privateMAC = 1;
    const SequoiaWCL::Channel inventory[] = {{1, 1, 0xa}, {1, 36, 0x12}};
    const uint8_t entropy[] = {0x80, 1, 2, 3, 4, 5};
    const uint8_t source[6] = {2};
    SequoiaWCL::ScanPlan native;
    assert(native.prepare(&nativeRequest, sizeof(nativeRequest), inventory, 2, source,
                          entropy, 6, false) == SequoiaWCL::ScanPlanStatus::Success);
    native.markAssociatedScan(1);
    assert(native.associatedScan() && native.assocChannel() == 1);
    IwmTrial::PassivePlan wire;
    wire.count = 2; wire.capacity = 40; wire.txAnt = wire.rxAnt = 3;
    wire.channels[0] = 1; wire.channels[1] = 36;
    memcpy(wire.source, source, 6);
    const auto size = IwmTrial::passiveRequestSize(wire);
    std::vector<uint8_t> bytes(size + 2, 0xa5);
    // Without the fragmented grant the associated plan stays a plain passive
    // request: no FRAGMENTED flag, no suspend/max-out fields.
    assert(IwmTrial::buildNativeScanRequest(native, 40, 3, 3, false, bytes.data() + 1, size));
    const auto *command = reinterpret_cast<const iwm_scan_req_lmac *>(bytes.data() + 1);
    assert(!(command->scan_flags & IWM_LMAC_SCAN_FLAG_FRAGMENTED));
    assert(command->max_out_time == 0 && command->suspend_time == 0 &&
           command->fragmented_dwell == 44);
    // r225 bgscan wire: fragmented dwell 95, suspend 120 TU, max off-channel
    // 600 TU, and the FRAGMENTED flag forces the passive dwells to fragment.
    assert(IwmTrial::buildNativeScanRequest(native, 40, 3, 3, true, bytes.data() + 1, size));
    assert(command->scan_flags == (0x8b | IWM_LMAC_SCAN_FLAG_FRAGMENTED));
    assert(command->fragmented_dwell == 95 && command->max_out_time == 600 &&
           command->suspend_time == 120);
    // The fragmented grant must not leak onto an unassociated plan.
    SequoiaWCL::ScanPlan plain;
    assert(plain.prepare(&nativeRequest, sizeof(nativeRequest), inventory, 2, source,
                         entropy, 6, false) == SequoiaWCL::ScanPlanStatus::Success);
    assert(IwmTrial::buildNativeScanRequest(plain, 40, 3, 3, true, bytes.data() + 1, size));
    assert(!(command->scan_flags & IWM_LMAC_SCAN_FLAG_FRAGMENTED) &&
           command->max_out_time == 0 && command->suspend_time == 0);
}

static std::vector<uint8_t> iteration()
{
    std::vector<uint8_t> bytes(28);
    bytes[0] = 2; bytes[1] = 1;
    bytes[12] = 1; bytes[13] = 1;
    bytes[20] = 36;
    return bytes;
}

static void lifecycle()
{
    const uint16_t channels[] = {1, 36};
    uint8_t terminal[12] = {}; terminal[2] = 1;
    auto result = iteration();
    for (bool early : {false, true}) {
        ItlScan::IwmPassiveTrial scan;
        const auto token = scan.begin(channels, 2);
        assert(token && !scan.begin(channels, 2));
        ItlScan::Completion completion;
        if (!early) assert(scan.submitted(token, ItlScan::Submission::Accepted));
        assert(!scan.notification(0xe7, result.data(), result.size()));
        assert(!scan.completion(completion));
        assert(scan.notification(0x6d, terminal, sizeof(terminal)));
        if (early) {
            assert(!scan.completion(completion));
            assert(scan.submitted(token, ItlScan::Submission::Accepted));
        }
        assert(scan.completeCoverage() && scan.drained());
        assert(scan.completion(completion) && completion.accepted && completion.outcome == ItlScan::Outcome::Success);
        assert(!scan.notification(0x6d, terminal, sizeof(terminal)));
        assert(scan.acknowledge(token) && !scan.begin(channels, 2));
    }
    for (unsigned fault = 0; fault < 7; ++fault) {
        ItlScan::IwmPassiveTrial scan;
        const auto token = scan.begin(channels, 2);
        assert(scan.submitted(token, ItlScan::Submission::Accepted));
        auto bad = result;
        if (fault == 0) bad.resize(27);
        if (fault == 1) bad[20] = 1;
        if (fault == 2) bad[20] = 144;
        if (fault == 3) bad[13] = 0;
        if (fault == 4) bad[1] = 9;
        if (fault == 5) bad[0] = 3;
        if (fault == 6) bad.resize(11);
        assert(!scan.notification(0xe7, bad.data(), bad.size()));
        assert(scan.evidence.malformed == 1 && !scan.evidence.iterations);
        assert(scan.timeout(token) && !scan.drained());
        scan.hardwareStopped();
        ItlScan::Completion completion;
        assert(scan.drained() && scan.completion(completion) && completion.outcome == ItlScan::Outcome::TimedOut);
        assert(!scan.completeCoverage());
    }
    ItlScan::IwmPassiveTrial scan;
    const auto token = scan.begin(channels, 2);
    assert(scan.submitted(token, ItlScan::Submission::Indeterminate));
    assert(!scan.drained());
    scan.hardwareStopped();
    ItlScan::Completion completion;
    assert(scan.completion(completion) && !completion.accepted && completion.outcome == ItlScan::Outcome::TransportError);
}

static void receive()
{
    static_assert(sizeof(iwm_rx_phy_info) == 68 && offsetof(iwm_rx_phy_info, channel) == 22, "PHY ABI");
    static_assert(sizeof(iwm_rx_mpdu_res_start) == 4, "MPDU ABI");
    ItlScan::IwmPassiveTrial scan;
    const uint16_t channels[] = {1};
    assert(scan.begin(channels, 1));
    uint8_t phy[68] = {}; phy[22] = 1;
    scan.rxPhy(phy, sizeof(phy));
    assert(scan.evidence.phy == 1);
    uint8_t frame[46] = {}; frame[0] = 38; frame[4] = 0x80; frame[20] = 2; frame[42] = 3;
    scan.rxMpdu(frame, sizeof(frame));
    assert(scan.evidence.beacons == 1 && scan.evidence.mpdu == 1);
    frame[4] = 0x50;
    scan.rxMpdu(frame, sizeof(frame));
    assert(scan.evidence.probeResponses == 1);
    frame[42] = 0;
    scan.rxMpdu(frame, sizeof(frame));
    assert(scan.evidence.badCrc == 1 && scan.evidence.mpdu == 2);
    frame[42] = 3; frame[41] = 33;
    scan.rxMpdu(frame, sizeof(frame));
    assert(scan.evidence.malformed == 1 && scan.evidence.probeResponses == 1);
    for (size_t size = 0; size < sizeof(frame); ++size) scan.rxMpdu(frame, size);
    for (size_t size = 0; size < sizeof(phy); ++size) scan.rxPhy(phy, size);
    scan.rxPhy(nullptr, 100);
    scan.rxMpdu(nullptr, 100);
    scan.hardwareStopped();
    const auto count = scan.evidence.malformed;
    scan.rxMpdu(frame, sizeof(frame));
    assert(scan.evidence.malformed == count);
}

namespace Radio {
constexpr int IWM_UCODE_TYPE_REGULAR = 0, IWM_DEVICE_FAMILY_7000 = 7000, IWM_DEVICE_FAMILY_9000 = 9000, IWM_FLAG_RFKILL = 1;
constexpr int IWM_CMD_WANT_RESP = 2, M_DEVBUF = 0, M_NOWAIT = 0;
#define nitems(value) (sizeof(value) / sizeof((value)[0]))
#define isset(bytes, bit) ((bytes)[(bit) / 8] & (1 << ((bit) % 8)))
struct iwm_host_cmd { uint32_t id = 0, flags = 0; uint16_t len[2] = {}; const void *data[2] = {}; };
struct iwm_softc {
    int sc_device_family = 7000, sc_mqrx_supported = 0, sc_uc_current = 0, cmdqid = 0, sc_flags = 0;
    unsigned sc_capa_n_scan_channels = 40;
    uint8_t sc_enabled_capa[16] = {0, 0x10}, sc_ucode_api[16] = {};
    struct { uint8_t hw_addr[6] = {2}; } sc_nvm;
    struct { struct { unsigned ic_freq = 0, ic_flags = 0; } ic_channels[256]; } sc_ic;
};
static bool failAllocation = false;
static void *allocate(size_t size, int, int) { return failAllocation ? nullptr : malloc(size); }
struct ItlIwm {
    bool nativeFirmwareTrial = true, nativeNet80211Attached = false, nativeCalibrationActive = false;

    bool iwm_channel_is_valid(struct iwm_softc *sc, uint16_t ch_id)
    {
        if (sc && ch_id < 256 && sc->sc_ic.ic_channels[ch_id].ic_freq &&
            sc->sc_ic.ic_channels[ch_id].ic_flags) return true;
        return ch_id <= 14 || (36 <= ch_id && ch_id <= 64 && ch_id % 4 == 0) ||
            (100 <= ch_id && ch_id <= 140 && ch_id % 4 == 0) ||
            (145 <= ch_id && ch_id <= 165 && ch_id % 4 == 1);
    }

    bool commandTransportReady = true, gated = true, eventThread = false, early = false, resetInAux = false;
    unsigned steps = 0, fault = 0, locks = 0;
    uint64_t commandEpoch = 1;
    int nativePassiveDone = 0;
    const void *nativePassiveRequest = nullptr;
    size_t nativePassiveRequestBytes = 0;
    ItlScan::IwmPassiveTrial nativePassiveScan;
    IwmFirmwareDiagnostics nativeFirmwareDiagnostics;
    IwmNvmDiagnostics nativeNvmDiagnostics;
    ItlIwm() {
        auto &r = nativeFirmwareDiagnostics;
        r.stage = IwmFirmwareStage::Scan;
        r.configurationResult = 0;
        r.phy.cfgBytes = r.phy.nchBytes = 8;
        r.phy.channels = r.phy.coveredChannels = 2;
    }
    ItlIwm *getMainWorkLoop() { return this; }
    ItlIwm *getMainCommandGate() { return this; }
    bool inGate() const { return gated; }
    bool onThread() const { return eventThread; }
    void commandWakeup(void *, bool) {}
    int iwm_is_valid_channel(unsigned channel) { return channel != 144; }
    uint8_t iwm_fw_valid_tx_ant(iwm_softc *) { return 3; }
    uint8_t iwm_fw_valid_rx_ant(iwm_softc *) { return 3; }
    bool iwm_nic_lock(iwm_softc *) { assert(!locks); ++locks; return true; }
    void iwm_nic_unlock(iwm_softc *) { assert(locks == 1); --locks; }
    int iwm_passive_scan_trial(iwm_softc *);
    bool iwm_scan_trial_command_allowed(iwm_softc *, const iwm_host_cmd *);
    void iwm_scan_trial_rx(uint16_t, const void *, size_t);
    int iwm_send_cmd(iwm_softc *sc, iwm_host_cmd *command) {
        assert(iwm_scan_trial_command_allowed(sc, command));
        for (unsigned fault = 0; fault < 5; ++fault) {
            auto bad = *command;
            if (fault == 0) bad.flags |= 1;
            if (fault == 1) --bad.len[0];
            if (fault == 2) bad.len[1] = 1;
            if (fault == 3) bad.data[0] = nullptr;
            if (fault == 4) bad.id = 0x80;
            assert(!iwm_scan_trial_command_allowed(sc, &bad));
        }
        ++steps;
        if (resetInAux && command->id == IWM_SCD_QUEUE_CFG) { ++commandEpoch; locks = 0; return ENXIO; }
        if (steps == fault) return EIO;
        if (early && command->id == IWM_SCAN_OFFLOAD_REQUEST_CMD) notify();
        return 0;
    }
    int iwm_send_dqa_cmd(iwm_softc *sc) {
        iwm_dqa_enable_cmd payload{IWM_DQA_CMD_QUEUE};
        iwm_host_cmd command;
        command.id = iwm_cmd_id(IWM_DQA_ENABLE_CMD, IWM_DATA_PATH_GROUP, 0);
        command.data[0] = &payload; command.len[0] = sizeof(payload);
        return iwm_send_cmd(sc, &command);
    }
    int iwm_add_aux_sta(iwm_softc *sc) {
        assert(locks == 1);
        iwm_scd_txq_cfg_cmd queue = {};
        queue.sta_id = IWM_AUX_STA_ID; queue.tid = IWM_MAX_TID_COUNT;
        queue.scd_queue = IWM_DQA_AUX_QUEUE; queue.enable = IWM_SCD_CFG_ENABLE_QUEUE;
        queue.tx_fifo = IWM_TX_FIFO_MCAST; queue.window = IWM_FRAME_LIMIT;
        iwm_host_cmd command;
        command.id = IWM_SCD_QUEUE_CFG; command.data[0] = &queue; command.len[0] = sizeof(queue);
        const int error = iwm_send_cmd(sc, &command);
        if (error) return error;
        iwm_add_sta_cmd_v7 station = {};
        station.sta_id = IWM_AUX_STA_ID; station.tid_disable_tx = 0xffff;
        station.mac_id_n_color = IWM_FW_CMD_ID_AND_COLOR(IWM_MAC_INDEX_AUX, 0);
        station.tfd_queue_msk = 1 << IWM_DQA_AUX_QUEUE;
        command.id = IWM_ADD_STA; command.data[0] = &station; command.len[0] = sizeof(station);
        command.flags = IWM_CMD_WANT_RESP;
        return iwm_send_cmd(sc, &command);
    }
    void notify() {
        auto results = iteration();
        iwm_scan_trial_rx(IWM_SCAN_ITERATION_COMPLETE, results.data(), results.size());
        assert(!nativePassiveDone);
        uint8_t terminal[12] = {}; terminal[2] = 1;
        iwm_scan_trial_rx(IWM_SCAN_OFFLOAD_COMPLETE, terminal, sizeof(terminal));
        assert(nativePassiveDone);
    }
    int iwm_wait_status(const int *status, int mask, void *event, unsigned seconds) {
        assert(status == &nativePassiveDone && mask == 1 && event == status && seconds == 10);
        assert(!locks);
        if (++steps == fault) return ETIMEDOUT;
        if (!early) notify();
        return 0;
    }
};
#define malloc allocate
#include "../itlwm/hal_iwm/scan_trial_sequoia.inc"
#undef malloc

static void sequence()
{
    for (bool early : {false, true}) for (unsigned fault = 0; fault <= 5; ++fault) {
        ItlIwm driver;
        iwm_softc sc;
        sc.sc_ic.ic_channels[1] = {2412, 1}; sc.sc_ic.ic_channels[36] = {5180, 1};
        driver.fault = fault; driver.early = early;
        const int error = driver.iwm_passive_scan_trial(&sc);
        assert(error == (fault == 5 ? ETIMEDOUT : fault ? EIO : 0));
        const auto &r = driver.nativeFirmwareDiagnostics;
        assert(!r.scanPreflightResult && !driver.locks && !driver.nativePassiveRequest && !driver.nativePassiveRequestBytes);
        if (!fault) assert(!r.scanDqaResult && !r.scanAuxResult && !r.scanSubmitResult && !r.scanWaitResult);
        if (fault == 1) assert(r.scanDqaResult == EIO && r.scanAuxResult == UINT32_MAX);
        if (fault == 2 || fault == 3) assert(r.scanAuxResult == EIO && r.scanSubmitResult == UINT32_MAX);
        if (fault == 4) assert(r.scanSubmitResult == EIO && r.scanWaitResult == UINT32_MAX);
        if (fault == 5) assert(r.scanWaitResult == ETIMEDOUT);
        driver.nativePassiveScan.hardwareStopped();
        assert(driver.nativePassiveScan.drained());
        assert(driver.iwm_passive_scan_trial(&sc) == EPERM);
    }
    for (unsigned fault = 0; fault < 8; ++fault) {
        ItlIwm driver;
        iwm_softc sc;
        sc.sc_ic.ic_channels[1] = {2412, 1}; sc.sc_ic.ic_channels[36] = {5180, 1};
        if (fault == 0) sc.sc_mqrx_supported = 1;
        if (fault == 1) sc.cmdqid = 1; // Not the DQA command queue
        if (fault == 2) sc.sc_enabled_capa[1] = 0; // No DQA
        if (fault == 3) sc.sc_flags = IWM_FLAG_RFKILL;
        if (fault == 4) sc.sc_capa_n_scan_channels = 1;
        if (fault == 5) sc.sc_ic.ic_channels[144] = {5720, 1};
        if (fault == 6) sc.sc_nvm.hw_addr[0] = 1;
        failAllocation = fault == 7;
        assert(driver.iwm_passive_scan_trial(&sc) != 0 && !driver.steps && !driver.locks);
        assert(driver.nativeFirmwareDiagnostics.scanDqaResult == UINT32_MAX);
        failAllocation = false;
    }
    // Station type support and UMAC scan support are extra capabilities
    // rather than a different station format, so reporting them must not
    // block the trial.
    for (unsigned capability = 0; capability < 2; ++capability) {
        ItlIwm driver;
        iwm_softc sc;
        sc.sc_ic.ic_channels[1] = {2412, 1}; sc.sc_ic.ic_channels[36] = {5180, 1};
        if (capability == 0) sc.sc_ucode_api[3] = 0x40; // STA_TYPE
        if (capability == 1) sc.sc_enabled_capa[0] = 4; // UMAC scan
        assert(driver.iwm_passive_scan_trial(&sc) == 0);
    }
    ItlIwm driver;
    iwm_softc sc;
    sc.sc_ic.ic_channels[1] = {2412, 1}; sc.sc_ic.ic_channels[36] = {5180, 1};
    driver.resetInAux = true;
    assert(driver.iwm_passive_scan_trial(&sc) == ENXIO && !driver.locks && driver.steps == 2);
}
} // namespace Radio

int main()
{
    request(); activeRequest(); associatedRequest(); lifecycle(); receive(); Radio::sequence();
    puts("IWM scan: passive trial and active SSID bitmaps, probe privacy, DFS, RX, coverage, lifecycle and fragmented bgscan wire passed");
}
