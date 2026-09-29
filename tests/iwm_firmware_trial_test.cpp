#include "iwm_command_test_support.h"

static void commandPolicy()
{
    ItlIwm driver;
    driver.nativeFirmwareTrial = true;
    auto &stage = driver.nativeFirmwareDiagnostics.stage;
    iwm_host_cmd command;
    iwm_nvm_access_cmd nvm{0, 0, 1, 0, 2048};
    command.id = IWM_NVM_ACCESS_CMD;
    command.data[0] = &nvm;
    command.len[0] = sizeof(nvm);
    command.flags = IWM_CMD_WANT_RESP | IWM_CMD_SEND_IN_RFKILL;
    command.resp_pkt_len = 16;
    assert(driver.iwm_send_cmd(&driver.sc, &command) == EPERM);
    stage = IwmFirmwareStage::Nvm;
    assert(driver.iwm_firmware_trial_command_allowed(&driver.sc, &command));
    for (unsigned fault = 0; fault < 5; ++fault) {
        auto bad = nvm;
        if (fault == 0) bad.op_code = 1;
        if (fault == 1) bad.target = 1;
        if (fault == 2) bad.length = 0;
        if (fault == 3) bad.length = 2049;
        if (fault == 4) bad.offset = 65535;
        command.data[0] = &bad;
        assert(driver.iwm_send_cmd(&driver.sc, &command) == EPERM);
    }
    command.data[0] = &nvm;
    stage = IwmFirmwareStage::Calibration;
    assert(!driver.iwm_firmware_trial_command_allowed(&driver.sc, &command));

    iwm_sf_cfg_cmd sf = {};
    sf.state = IWM_SF_INIT_OFF;
    iwm_tx_ant_cfg_cmd antenna{3};
    iwm_phy_cfg_cmd phy{driver.sc.sc_fw_phy_config, {0x12, 0x34}};
    iwm_bt_coex_cmd bt{IWM_BT_COEX_NW, IWM_BT_COEX_HIGH_BAND_RET | IWM_BT_COEX_SYNC2SCO_ENABLED};
    const struct { uint32_t id; const void *data; uint16_t size; } allowed[] = {
        {IWM_REPLY_SF_CFG_CMD, &sf, sizeof(sf)}, {IWM_TX_ANT_CONFIGURATION_CMD, &antenna, sizeof(antenna)},
        {IWM_PHY_CONFIGURATION_CMD, &phy, sizeof(phy)}, {IWM_BT_CONFIG, &bt, sizeof(bt)}};
    for (const auto &entry : allowed) {
        command = {};
        command.id = entry.id;
        command.data[0] = entry.data;
        command.len[0] = entry.size;
        assert(driver.iwm_firmware_trial_command_allowed(&driver.sc, &command));
        const auto before = driver.submissions;
        driver.onKick = [&] { driver.acknowledge(before, command.id, EIO); };
        assert(driver.iwm_send_cmd(&driver.sc, &command) == EIO); // Real failed ACK is not success.
        assert(driver.submissions == before + 1);
        for (auto other : {IwmFirmwareStage::Resources, IwmFirmwareStage::Regular, IwmFirmwareStage::Complete}) {
            stage = other;
            assert(driver.iwm_send_cmd(&driver.sc, &command) == EPERM);
        }
        stage = IwmFirmwareStage::Calibration;
        command.flags = IWM_CMD_ASYNC;
        assert(driver.iwm_send_cmd(&driver.sc, &command) == EPERM);
        command.flags = 0;
        --command.len[0];
        assert(driver.iwm_send_cmd(&driver.sc, &command) == EPERM);
    }
    command = {};
    command.data[0] = &sf;
    command.len[0] = sizeof(sf);
    for (uint32_t id : {0x80U, 0x81U, 0x8bU, 0x2bU, 0x1cU, 0x1dU, 0x4fU, 0x136U}) {
        command.id = id;
        assert(driver.iwm_send_cmd(&driver.sc, &command) == EPERM);
    }
    command.id = IWM_REPLY_SF_CFG_CMD;
    sf.state = 1;
    assert(driver.iwm_send_cmd(&driver.sc, &command) == EPERM);
    sf.state = IWM_SF_INIT_OFF;
    driver.sc.sc_uc_current = IWM_UCODE_TYPE_REGULAR;
    assert(driver.iwm_send_cmd(&driver.sc, &command) == EPERM);
    assert(driver.submissions == 4);
}

// Exercise the production stage orchestrator with hardware operations replaced
// by a fault-injecting shim. No firmware image or PCI access occurs here.
namespace Sequence {
constexpr int IWM_DEVICE_FAMILY_7000 = 7000, IWM_FLAG_RFKILL = 1, IWM_UCODE_TYPE_REGULAR = 0;
struct iwm_softc {
    int sc_device_family = 7000, sc_init_complete = 0, sc_flags = 0;
    struct { struct { unsigned ic_freq = 0, ic_flags = 0; } ic_channels[256]; } sc_ic;
};
struct ItlIwm {
    bool nativeFirmwareTrial = true, nativeNvmTrial = false, nativeNet80211Attached = false;
    bool nativeHardwareStarted = false, owned = true, gated = true, eventThread = false;
    IwmFirmwareDiagnostics nativeFirmwareDiagnostics;
    IwmNvmDiagnostics nativeNvmDiagnostics;
    IwmPhyDbSnapshot database;
    unsigned step = 0, fault = 0, stops = 0, active = 0;
    struct FirmwareLease {
        bool acquired;
        explicit FirmwareLease(ItlIwm *driver) : acquired(driver->owned) {}
        explicit operator bool() const { return acquired; }
    };
    ItlIwm *getMainWorkLoop() { return this; }
    bool inGate() const { return gated; }
    bool onThread() const { return eventThread; }
    int action() { return ++step == fault ? EIO : 0; }
    int iwm_is_valid_channel(uint16_t channel) { return channel != 144; }
    int iwm_start_hw(iwm_softc *) { ++active; return action(); }
    void iwm_stop_device(iwm_softc *) { assert(active); active = 0; ++stops; }
    int iwm_run_init_mvm_ucode(iwm_softc *sc, int nvm)
    {
        assert(active == 1);
        const auto expected = nvm ? IwmFirmwareStage::Nvm : IwmFirmwareStage::Calibration;
        assert(nativeFirmwareDiagnostics.stage == expected);
        nativeNvmDiagnostics.aliveBytes = 68;
        nativeNvmDiagnostics.aliveStatus = 0xcafe;
        const int error = action();
        if (!error && !nvm) {
            sc->sc_init_complete = 3;
            database.cfgBytes = database.nchBytes = 8;
            database.papdMask = database.txpMask = 1;
            database.channels = database.coveredChannels = 38;
            database.bytes = 32;
        }
        return error;
    }
    IwmPhyDbSnapshot iwm_phy_db_snapshot(iwm_softc *)
    {
        if (fault == 12) database.coveredChannels = 0;
        return database;
    }
    int iwm_load_ucode_wait_alive(iwm_softc *, int type)
    {
        assert(type == IWM_UCODE_TYPE_REGULAR && active == 1 && nativeFirmwareDiagnostics.stage == IwmFirmwareStage::Regular);
        nativeNvmDiagnostics.aliveBytes = 68;
        nativeNvmDiagnostics.aliveStatus = 0xcafe;
        return action();
    }
    int iwm_run_firmware_trial(iwm_softc *);
    struct { void hardwareStopped() {} } nativePassiveScan;
    int iwm_passive_scan_trial(iwm_softc *) {
        assert(nativeFirmwareDiagnostics.stage == IwmFirmwareStage::Scan && active == 1);
        return action();
    }
    unsigned iwm_fw_valid_tx_ant(iwm_softc *) { return 3; }
    int iwm_send_tx_ant_cfg(iwm_softc *, unsigned mask) { assert(mask == 3); return setup("regular-antennas"); }
    int iwm_replay_phy_db(iwm_softc *) { return setup("replay-phy-db"); }
    int iwm_send_phy_cfg_cmd(iwm_softc *) { return setup("regular-phy-config"); }
    int iwm_send_bt_init_conf(iwm_softc *) { return setup("regular-bt-config"); }
    int setup(const char *phase) {
        assert(nativeFirmwareDiagnostics.stage == IwmFirmwareStage::Configuration && active == 1);
        assert(!strcmp(nativeNvmDiagnostics.phase, phase));
        return action();
    }
};
#include "../itlwm/hal_iwm/firmware_trial_sequoia.inc"

static void stagesAndFailures()
{
    for (unsigned fault = 0; fault <= 12; ++fault) {
        ItlIwm driver;
        iwm_softc state;
        driver.fault = fault;
        state.sc_ic.ic_channels[1] = {2412, 1};
        state.sc_ic.ic_channels[144] = {5720, 1};
        state.sc_ic.ic_channels[160] = {5800, 1}; // Unknown entries are not silently dropped.
        assert(driver.iwm_run_firmware_trial(&state) == (fault == 12 ? ENODATA : fault ? EIO : 0));
        assert(driver.active == 0 && driver.nativeHardwareStarted);
        auto &result = driver.nativeFirmwareDiagnostics;
        if (result.nvmResult == 0) {
            assert(result.nvmChannels == 3 && result.excludedChannels == 1 && result.firstExcludedChannel == 144);
            assert(state.sc_ic.ic_channels[144].ic_freq == 0 && state.sc_ic.ic_channels[144].ic_flags == 0);
            assert(state.sc_ic.ic_channels[1].ic_freq == 2412 && state.sc_ic.ic_channels[160].ic_freq == 5800);
        } else {
            assert(!result.nvmChannels && !result.excludedChannels && result.firstExcludedChannel == UINT32_MAX);
            assert(state.sc_ic.ic_channels[144].ic_freq == 5720);
        }
        if (!fault) {
            assert(driver.step == 11 && driver.stops == 3 && result.stage == IwmFirmwareStage::Complete && !result.scanResult);
            assert(!result.configurationResult && !result.antennaResult && !result.replayResult && !result.phyConfigResult && !result.btResult);
            assert(!result.nvmResult && !result.calibrationResult && !result.regularResult && result.phy.complete());
            assert(result.nvmAliveStatus == 0xcafe && result.calibrationAliveStatus == 0xcafe && result.regularAliveStatus == 0xcafe);
        } else if (fault <= 2) {
            assert(driver.stops == 1 && result.stage == IwmFirmwareStage::Nvm && result.calibrationResult == UINT32_MAX);
        } else if (fault <= 4 || fault == 12) {
            assert(driver.stops == 2 && result.stage == IwmFirmwareStage::Calibration && result.regularResult == UINT32_MAX);
        } else if (fault <= 6) {
            assert(driver.stops == 3 && result.stage == IwmFirmwareStage::Regular && !result.calibrationResult);
            if (fault == 5) assert(result.regularAliveStatus == UINT32_MAX);
        } else if (fault == 11) {
            assert(driver.stops == 3 && result.stage == IwmFirmwareStage::Scan && result.scanResult == EIO);
        } else {
            assert(driver.stops == 3 && result.stage == IwmFirmwareStage::Configuration && result.configurationResult == EIO);
            const uint32_t values[] = {result.antennaResult, result.replayResult, result.phyConfigResult, result.btResult};
            for (unsigned index = 0; index < 4; ++index)
                assert(values[index] == (index + 7 < fault ? 0U : index + 7 == fault ? EIO : UINT32_MAX));
            assert(driver.step == fault);
        }
        assert(driver.iwm_run_firmware_trial(&state) == EPERM); // One attempt per instance.
    }
    for (unsigned rejection = 0; rejection < 6; ++rejection) {
        ItlIwm driver;
        iwm_softc state;
        if (rejection == 0) driver.gated = false;
        if (rejection == 1) driver.eventThread = true;
        if (rejection == 2) driver.nativeNvmTrial = true;
        if (rejection == 3) driver.nativeNet80211Attached = true;
        if (rejection == 4) state.sc_device_family = 8000;
        if (rejection == 5) driver.owned = false;
        assert(driver.iwm_run_firmware_trial(&state) == (rejection == 5 ? EBUSY : EPERM));
        assert(!driver.step && !driver.stops && !driver.nativeHardwareStarted);
    }
}
}

static void replayCommandPolicy()
{
    ItlIwm driver;
    driver.nativeFirmwareTrial = true;
    auto &result = driver.nativeFirmwareDiagnostics;
    result.stage = IwmFirmwareStage::Configuration;
    result.calibrationResult = result.regularResult = 0;
    driver.sc.sc_uc_current = IWM_UCODE_TYPE_REGULAR;
    uint8_t bytes[640] = {};
    bytes[0] = 2;
    iwm_phy_db_cmd header{IWM_PHY_DB_CALIB_CHG_TXP, sizeof(bytes)};
    driver.sc.phySections[5][2] = {sizeof(bytes), bytes};
    result.replayLastType = 5;
    result.replayLastGroup = 2;
    iwm_host_cmd command = {};
    command.id = IWM_PHY_DB_CMD;
    command.data[0] = &header;
    command.len[0] = sizeof(header);
    command.data[1] = bytes;
    command.len[1] = sizeof(bytes);
    assert(driver.iwm_firmware_trial_command_allowed(&driver.sc, &command));
    for (unsigned fault = 0; fault < 12; ++fault) {
        auto bad = command;
        auto badHeader = header;
        bad.data[0] = &badHeader;
        if (fault == 0) bad.flags = IWM_CMD_ASYNC;
        if (fault == 1) bad.flags = IWM_CMD_WANT_RESP;
        if (fault == 2) --bad.len[0];
        if (fault == 3) --bad.len[1];
        if (fault == 4) badHeader.type = 3;
        if (fault == 5) ++badHeader.length;
        if (fault == 6) bad.data[1] = nullptr;
        if (fault == 7) bad.data[1] = bytes + 1;
        if (fault == 8) driver.nativeCalibrationActive = true;
        if (fault == 9) driver.sc.sc_uc_current = IWM_UCODE_TYPE_INIT;
        if (fault == 10) result.regularResult = EIO;
        if (fault == 11) result.replayLastGroup = 1;
        assert(!driver.iwm_firmware_trial_command_allowed(&driver.sc, &bad));
        driver.nativeCalibrationActive = false;
        driver.sc.sc_uc_current = IWM_UCODE_TYPE_REGULAR;
        result.regularResult = 0;
        result.replayLastGroup = 2;
    }
    driver.onKick = [&] {
        assert(driver.sc.txq[0].data[0].m && liveMbufs == 1);
        const auto *payload = driver.sc.txq[0].data[0].m->bytes.data() + sizeof(iwm_cmd_header);
        assert(!memcmp(payload, &header, sizeof(header)));
        assert(!memcmp(payload + sizeof(header), bytes, sizeof(bytes)));
        driver.acknowledge(0, IWM_PHY_DB_CMD, EIO);
    };
    assert(driver.iwm_send_cmd(&driver.sc, &command) == EIO);
    assert(driver.submissions == 1 && !liveMbufs);
}

int main()
{
    commandPolicy();
    replayCommandPolicy();
    Sequence::stagesAndFailures();
    puts("Production IWM firmware trial: phase allowlist, failed ACKs, one-shot stages and cleanup fault injection passed");
}
