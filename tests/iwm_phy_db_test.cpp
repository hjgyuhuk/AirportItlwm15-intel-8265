#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>
#include <HAL/IwmNvmDiagnostics.h>
#include <HAL/IwmFirmwareTrial.h>

#define le16toh(value) (value)
#define htole16(value) (value)
#define nitems(value) (sizeof(value) / sizeof((value)[0]))
constexpr unsigned IWM_PHY_DB_CFG = 1, IWM_PHY_DB_CALIB_NCH = 2;
constexpr unsigned IWM_PHY_DB_CALIB_CHG_PAPD = 4, IWM_PHY_DB_CALIB_CHG_TXP = 5;
constexpr unsigned IWM_NUM_PAPD_CH_GROUPS = 9, IWM_NUM_TXP_CH_GROUPS = 9;
constexpr unsigned IWM_UCODE_TYPE_INIT = 1, IWM_UCODE_TYPE_REGULAR = 0;
constexpr int IWM_INIT_COMPLETE = 1, IWM_CALIB_COMPLETE = 2, M_DEVBUF = 0, M_NOWAIT = 0;
struct iwm_calib_res_notif_phy_db { uint16_t type, length; uint8_t data[0]; } __attribute__((packed));
struct iwm_phy_db_chg_txp { uint32_t space; uint16_t max_channel_idx; } __attribute__((packed));
struct iwm_phy_db_entry { uint16_t size = 0; uint8_t *data = nullptr; };
constexpr unsigned IWM_PHY_DB_CMD = 0x6c, IWM_MAX_CMD_PAYLOAD_SIZE = 4088;
struct iwm_phy_db_cmd { uint16_t type, length; };
struct iwm_host_cmd { unsigned id = 0, flags = 0; const void *data[2] = {}; uint16_t len[2] = {}; };
struct iwm_phy_db {
    iwm_phy_db_entry &cfg, &calib_nch;
    iwm_phy_db_entry (&calib_ch_group_papd)[IWM_NUM_PAPD_CH_GROUPS];
    iwm_phy_db_entry (&calib_ch_group_txp)[IWM_NUM_TXP_CH_GROUPS];
};
struct iwm_softc {
    unsigned sc_uc_current = IWM_UCODE_TYPE_INIT;
    int sc_init_complete = 0;
    iwm_phy_db_entry cfg, nch, papd[IWM_NUM_PAPD_CH_GROUPS], txp[IWM_NUM_TXP_CH_GROUPS];
    iwm_phy_db sc_phy_db{cfg, nch, papd, txp};
    struct { struct { unsigned ic_freq = 0, ic_flags = 0; } ic_channels[256]; } sc_ic;
};
static bool failAllocation = false;
static std::set<void *> allocations;
static unsigned allocationCalls = 0;
static void *testAllocate(size_t bytes, int, int)
{
    ++allocationCalls;
    if (failAllocation) return nullptr;
    void *pointer = malloc(bytes);
    assert(pointer && allocations.insert(pointer).second);
    return pointer;
}
static void testFree(void *pointer)
{
    if (!pointer) return;
    assert(allocations.erase(pointer) == 1);
    free(pointer);
}
struct ItlIwm {
    bool nativeFirmwareTrial = true, gated = true, eventThread = false;
    IwmFirmwareDiagnostics nativeFirmwareDiagnostics;
    IwmNvmDiagnostics nativeNvmDiagnostics;
    bool isHardwareTrial() const { return nativeFirmwareTrial; }
    unsigned replaySubmissions = 0, replayFault = 0;
    int replayError = EIO;
    ItlIwm *getMainWorkLoop() { return this; }
    bool inGate() const { return gated; }
    bool onThread() const { return eventThread; }
    int iwm_replay_phy_db(iwm_softc *);
    int iwm_send_cmd(iwm_softc *sc, iwm_host_cmd *command) {
        assert(sc == &state && command->id == IWM_PHY_DB_CMD && command->flags == 0);
        assert(command->len[0] == sizeof(iwm_phy_db_cmd));
        const auto *header = static_cast<const iwm_phy_db_cmd *>(command->data[0]);
        const auto &result = nativeFirmwareDiagnostics;
        auto *entry = iwm_phy_db_get_section(sc, result.replayLastType, result.replayLastGroup);
        assert(entry && entry->data == command->data[1] && entry->size == command->len[1]);
        assert(header->type == result.replayLastType && header->length == entry->size);
        return ++replaySubmissions == replayFault ? replayError : 0;
    }
    bool nativeCalibrationActive = false;
    int nativeCalibrationError = 0;
    unsigned wakeups = 0;
    iwm_softc state;

    ~ItlIwm()
    {
        iwm_phy_db_clear(&state);
        assert(allocations.empty());
    }
    iwm_phy_db_entry *iwm_phy_db_get_section(iwm_softc *sc, uint16_t type, uint16_t group)
    {
        switch (type) {
            case IWM_PHY_DB_CFG: return &sc->cfg;
            case IWM_PHY_DB_CALIB_NCH: return &sc->nch;
            case IWM_PHY_DB_CALIB_CHG_PAPD: return group < IWM_NUM_PAPD_CH_GROUPS ? &sc->papd[group] : nullptr;
            case IWM_PHY_DB_CALIB_CHG_TXP: return group < IWM_NUM_TXP_CH_GROUPS ? &sc->txp[group] : nullptr;
            default: return nullptr;
        }
    }
    void wakeupOn(void *event) { assert(event == &state.sc_init_complete); ++wakeups; }
    int iwm_phy_db_set_section(iwm_softc *, const uint8_t *, size_t);
    void iwm_rx_phy_db(iwm_softc *, const uint8_t *, size_t);
    void iwm_phy_db_clear(iwm_softc *);
    IwmPhyDbSnapshot iwm_phy_db_snapshot(iwm_softc *);
    int iwm_is_valid_channel(uint16_t);
    uint8_t iwm_ch_id_to_ch_index(uint16_t);
    uint16_t iwm_channel_id_to_papd(uint16_t);
    uint16_t iwm_channel_id_to_txp(iwm_softc *, uint16_t);
};
#define IO80211FAMILY_V2 1
#define __IO80211_TARGET 150000
#define __MAC_15_0 150000
#include "../itlwm/hal_iwm/phy_channel_map.inc"
#define malloc(bytes, type, flags) testAllocate(bytes, type, flags)
#define free(pointer) testFree(pointer)
#include "../itlwm/hal_iwm/phy_db_sequoia.inc"
#undef malloc
#undef free
#include "../itlwm/hal_iwm/phy_db_replay_sequoia.inc"

static void replayDatabase()
{
    for (unsigned fault = 0; fault <= 15; ++fault) {
        ItlIwm driver;
        driver.state.sc_ic.ic_channels[1] = {2412, 1};
        for (uint16_t type : {1, 2, 4, 5}) {
            const unsigned groups = type == 4 ? 4 : type == 5 ? 9 : 1;
            for (unsigned group = 0; group < groups; ++group) {
                uint8_t data[] = {static_cast<uint8_t>(type), 0, 8, 0, static_cast<uint8_t>(group), 0, 0, 0, 38, 0, 0, 0};
                assert(!driver.iwm_phy_db_set_section(&driver.state, data, sizeof(data)));
            }
        }
        auto &result = driver.nativeFirmwareDiagnostics;
        result.stage = IwmFirmwareStage::Configuration;
        result.calibrationResult = result.regularResult = 0;
        result.phy = driver.iwm_phy_db_snapshot(&driver.state);
        driver.state.sc_uc_current = IWM_UCODE_TYPE_REGULAR;
        driver.replayFault = fault;
        driver.replayError = fault % 2 ? ETIMEDOUT : ENXIO;
        assert(driver.iwm_replay_phy_db(&driver.state) == (fault ? driver.replayError : 0));
        assert(result.replayCommands == (fault ? fault - 1 : 15));
        assert(result.replayBytes == result.replayCommands * 8);
        assert(driver.replaySubmissions == (fault ? fault : 15));
        if (!fault) {
            assert(result.replayBytes == result.phy.bytes && result.replayPapdMask == 15 && result.replayTxpMask == 511);
            assert(result.replayLastType == 5 && result.replayLastGroup == 8);
        }
        assert(driver.iwm_replay_phy_db(&driver.state) == EPERM);
        result.replayLastType = UINT32_MAX;
        driver.state.cfg.size = 5000;
        result.phy = driver.iwm_phy_db_snapshot(&driver.state);
        const unsigned before = driver.replaySubmissions;
        assert(driver.iwm_replay_phy_db(&driver.state) == E2BIG);
        assert(driver.replaySubmissions == before);
    }
}

static std::vector<uint8_t> packet(unsigned type, size_t bytes, unsigned group = 0)
{
    std::vector<uint8_t> data(4 + bytes, 0xa5);
    data[0] = type & 0xff; data[1] = type >> 8;
    data[2] = bytes & 0xff; data[3] = bytes >> 8;
    if (bytes >= 2) { data[4] = group & 0xff; data[5] = group >> 8; }
    if (type == IWM_PHY_DB_CALIB_CHG_TXP && bytes >= 6) {
        const uint16_t maximum = group == 0 ? 13 : group == 1 ? 21 : 38;
        data[8] = maximum & 0xff; data[9] = maximum >> 8;
    }
    return data;
}

static void malformedPackets()
{
    ItlIwm driver;
    const unsigned before = allocationCalls;
    auto valid = packet(IWM_PHY_DB_CFG, 8);
    assert(driver.iwm_phy_db_set_section(nullptr, valid.data(), valid.size()) == EINVAL);
    assert(driver.iwm_phy_db_set_section(&driver.state, nullptr, valid.size()) == EINVAL);
    for (size_t bytes = 0; bytes < valid.size(); ++bytes) {
        // Exact-size buffers let ASan catch even one-byte header/data overreads.
        std::vector<uint8_t> truncated(valid.begin(), valid.begin() + bytes);
        assert(driver.iwm_phy_db_set_section(&driver.state, truncated.data(), bytes) == EINVAL);
    }
    for (unsigned type = 0; type <= 7; ++type) {
        const unsigned minimum = type == IWM_PHY_DB_CALIB_CHG_TXP ? 6 :
            (type == IWM_PHY_DB_CALIB_CHG_PAPD ? 2 : 1);
        for (unsigned bytes = 0; bytes < minimum; ++bytes) {
            auto data = packet(type, bytes);
            assert(driver.iwm_phy_db_set_section(&driver.state, data.data(), data.size()) == EINVAL);
        }
        if (type == 0 || type == 3 || type >= 6) {
            auto data = packet(type, 8);
            assert(driver.iwm_phy_db_set_section(&driver.state, data.data(), data.size()) == EINVAL);
        }
    }
    for (unsigned type : {IWM_PHY_DB_CALIB_CHG_PAPD, IWM_PHY_DB_CALIB_CHG_TXP}) {
        for (unsigned group : {9, 256, 65535}) {
            auto data = packet(type, 8, group);
            assert(driver.iwm_phy_db_set_section(&driver.state, data.data(), data.size()) == EINVAL);
        }
    }
    valid[2] = valid[3] = 0xff;
    assert(driver.iwm_phy_db_set_section(&driver.state, valid.data(), valid.size()) == EINVAL);
    assert(allocationCalls == before && allocations.empty());
}

static void ownedReplacement()
{
    ItlIwm driver;
    for (unsigned type : {IWM_PHY_DB_CFG, IWM_PHY_DB_CALIB_NCH,
                          IWM_PHY_DB_CALIB_CHG_PAPD, IWM_PHY_DB_CALIB_CHG_TXP}) {
        auto data = packet(type, 16, 8);
        data.insert(data.begin(), 0); // Deliberately unaligned firmware header.
        assert(!driver.iwm_phy_db_set_section(&driver.state, data.data() + 1, data.size() - 1));
        auto *entry = driver.iwm_phy_db_get_section(&driver.state, type, 8);
        assert(entry && entry->size == 16 && !memcmp(entry->data, data.data() + 5, 16));
        auto *saved = entry->data;
        const size_t count = allocations.size();
        failAllocation = true;
        assert(driver.iwm_phy_db_set_section(&driver.state, data.data() + 1, data.size() - 1) == ENOMEM);
        failAllocation = false;
        assert(entry->data == saved && entry->size == 16 && allocations.size() == count);
        auto replacement = packet(type, 8, 8);
        replacement.insert(replacement.end(), 8, 0x77); // RX padding is not stored.
        assert(!driver.iwm_phy_db_set_section(&driver.state, replacement.data(), replacement.size()));
        assert(entry->size == 8 && entry->data != saved && allocations.size() == count);
        assert(!memcmp(entry->data, replacement.data() + 4, 8));
    }
}

static void calibrationErrorPropagation()
{
    ItlIwm driver;
    auto data = packet(IWM_PHY_DB_CFG, 8);
    driver.iwm_rx_phy_db(&driver.state, data.data(), data.size());
    assert(driver.wakeups == 0 && !driver.state.cfg.data);
    driver.nativeCalibrationActive = true;
    driver.state.sc_uc_current = IWM_UCODE_TYPE_REGULAR;
    driver.iwm_rx_phy_db(&driver.state, data.data(), data.size());
    assert(driver.wakeups == 0 && !driver.state.cfg.data);
    driver.state.sc_uc_current = IWM_UCODE_TYPE_INIT;
    driver.iwm_rx_phy_db(&driver.state, data.data(), data.size());
    assert(driver.wakeups == 1 && driver.state.sc_init_complete == IWM_CALIB_COMPLETE);
    assert(driver.nativeCalibrationError == 0 && driver.state.cfg.data);
    // An error before the INIT terminal must remain visible after a valid record.
    driver.iwm_rx_phy_db(&driver.state, nullptr, 0);
    assert(driver.wakeups == 2 && driver.nativeCalibrationError == EINVAL);
    auto *saved = driver.state.cfg.data;
    driver.iwm_rx_phy_db(&driver.state, data.data(), data.size());
    assert(driver.wakeups == 2 && driver.nativeCalibrationError == EINVAL && driver.state.cfg.data == saved);

    driver.nativeCalibrationError = 0;
    driver.state.sc_init_complete = 0;
    failAllocation = true;
    driver.iwm_rx_phy_db(&driver.state, data.data(), data.size());
    failAllocation = false;
    assert(driver.nativeCalibrationError == ENOMEM && driver.state.sc_init_complete == 0);
    assert(driver.state.cfg.data == saved && driver.wakeups == 3);
    driver.nativeCalibrationActive = false;
    driver.iwm_rx_phy_db(&driver.state, data.data(), data.size());
    assert(driver.wakeups == 3 && driver.nativeCalibrationError == ENOMEM);

    driver.nativeCalibrationActive = true;
    driver.nativeCalibrationError = 0;
    driver.state.sc_init_complete = IWM_INIT_COMPLETE | IWM_CALIB_COMPLETE;
    driver.iwm_rx_phy_db(&driver.state, nullptr, 0);
    assert(driver.wakeups == 3 && !driver.nativeCalibrationError);
    assert(driver.state.cfg.data == saved); // The terminal freezes this calibration database.
}

static void databaseFreshnessAndCoverage()
{
    ItlIwm driver;
    driver.state.sc_ic.ic_channels[1] = {2412, 1};
    driver.state.sc_ic.ic_channels[36] = {5180, 1};
    const auto add = [&](unsigned type, unsigned group) {
        auto data = packet(type, 8, group);
        assert(!driver.iwm_phy_db_set_section(&driver.state, data.data(), data.size()));
    };
    add(IWM_PHY_DB_CFG, 0);
    add(IWM_PHY_DB_CALIB_NCH, 0);
    add(IWM_PHY_DB_CALIB_CHG_PAPD, 0);
    add(IWM_PHY_DB_CALIB_CHG_TXP, 0);
    auto snapshot = driver.iwm_phy_db_snapshot(&driver.state);
    assert(snapshot.channels == 2 && snapshot.coveredChannels == 1 && !snapshot.complete());
    assert(snapshot.firstUncoveredChannel == 36);
    add(IWM_PHY_DB_CALIB_CHG_PAPD, 1);
    add(IWM_PHY_DB_CALIB_CHG_TXP, 1);
    snapshot = driver.iwm_phy_db_snapshot(&driver.state);
    assert(snapshot.complete() && snapshot.bytes == 48 && snapshot.papdMask == 3 && snapshot.txpMask == 3);
    assert(snapshot.firstUncoveredChannel == UINT32_MAX);
    driver.iwm_phy_db_clear(&driver.state);
    driver.iwm_phy_db_clear(&driver.state);
    assert(allocations.empty());
    snapshot = driver.iwm_phy_db_snapshot(&driver.state);
    assert(!snapshot.complete() && snapshot.bytes == 0 && snapshot.coveredChannels == 0);
    add(IWM_PHY_DB_CFG, 0); // A later incomplete attempt cannot reuse old groups.
    snapshot = driver.iwm_phy_db_snapshot(&driver.state);
    assert(!snapshot.complete() && snapshot.bytes == 8 && !snapshot.nchBytes && !snapshot.txpMask);
}

static void realLegacyChannelMapCoverage()
{
    ItlIwm driver;
    // Synthetic complete group data, but the production mapping and snapshot.
    for (auto type : {IWM_PHY_DB_CFG, IWM_PHY_DB_CALIB_NCH, IWM_PHY_DB_CALIB_CHG_PAPD, IWM_PHY_DB_CALIB_CHG_TXP}) {
        for (unsigned group = 0; group < (type == IWM_PHY_DB_CALIB_CHG_PAPD ? 4 : type == IWM_PHY_DB_CALIB_CHG_TXP ? 9 : 1); ++group) {
            auto data = packet(type, 8, group);
            assert(!driver.iwm_phy_db_set_section(&driver.state, data.data(), data.size()));
        }
    }
    const uint16_t channels[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
        36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
        149, 153, 157, 161, 165};
    for (auto channel : channels) driver.state.sc_ic.ic_channels[channel] = {1, 1};
    auto snapshot = driver.iwm_phy_db_snapshot(&driver.state);
    assert(snapshot.channels == 38 && snapshot.coveredChannels == 37 && snapshot.firstUncoveredChannel == 144);
    assert(!snapshot.complete());
    assert(driver.iwm_ch_id_to_ch_index(140) == 32 && driver.iwm_channel_id_to_papd(140) == 2);
    assert(driver.iwm_ch_id_to_ch_index(144) == 0xff && driver.iwm_channel_id_to_papd(144) == 0xff);
    assert(driver.iwm_ch_id_to_ch_index(149) == 34 && driver.iwm_channel_id_to_papd(149) == 3);
    assert(driver.iwm_ch_id_to_ch_index(165) == 38 && driver.iwm_channel_id_to_papd(165) == 3);
    for (auto invalid : {0, 15, 35, 65, 99, 144, 160, 166, 65535})
        assert(driver.iwm_channel_id_to_txp(&driver.state, invalid) == 0xff);
    driver.state.sc_ic.ic_channels[144] = {};
    snapshot = driver.iwm_phy_db_snapshot(&driver.state);
    assert(snapshot.complete() && snapshot.channels == 37 && snapshot.firstUncoveredChannel == UINT32_MAX);
    // Real TXP lookup must reject incomplete/short predecessors and uncovered maxima.
    auto &txp0 = driver.state.txp[0];
    txp0.size = 5;
    assert(driver.iwm_channel_id_to_txp(&driver.state, 1) == 0xff);
    txp0.size = 8;
    for (auto &entry : driver.state.txp) { entry.data[4] = 0; entry.data[5] = 0; }
    snapshot = driver.iwm_phy_db_snapshot(&driver.state);
    assert(!snapshot.complete() && snapshot.coveredChannels == 1 && snapshot.firstUncoveredChannel == 2);
}

int main()
{
    replayDatabase();
    malformedPackets();
    ownedReplacement();
    calibrationErrorPropagation();
    databaseFreshnessAndCoverage();
    realLegacyChannelMapCoverage();
    puts("Production IWM PHY DB: bounds, unaligned fields, allocation failure and sticky calibration errors passed");
}
