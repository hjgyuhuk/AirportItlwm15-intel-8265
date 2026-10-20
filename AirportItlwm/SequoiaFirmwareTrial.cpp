#include <IOKit/network/IOEthernetController.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/version.h>
#include <pexpert/pexpert.h>
#include <HAL/SequoiaTrialPolicy.h>
#include "ItlIwm.hpp"

// The superclass supplies mbuf allocation only. This service never creates a
// network interface and releases all hardware before publishing its evidence.
class AirportItlwmSequoiaFirmwareTrial : public IOEthernetController {
    OSDeclareDefaultStructors(AirportItlwmSequoiaFirmwareTrial)
public:
    IOService *probe(IOService *, SInt32 *) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
    bool createWorkLoop() override;
    IOWorkLoop *getWorkLoop() const override { return loop_; }
    IOOutputQueue *createOutputQueue() override { return nullptr; }
    IOReturn getHardwareAddress(IOEthernetAddress *) override { return kIOReturnNotReady; }
private:
    void releaseHardware();
    bool recordDiagnostics();
    IOWorkLoop *loop_ = nullptr;
    IOCommandGate *gate_ = nullptr;
    IOPCIDevice *device_ = nullptr;
    ItlIwm *hal_ = nullptr;
    bool opened_ = false, attached_ = false, gateAdded_ = false;
    uint16_t pciCommand_ = 0;
};

#define super IOEthernetController
OSDefineMetaClassAndStructors(AirportItlwmSequoiaFirmwareTrial, IOEthernetController)

static bool firmwareTrialAllowed(IOService *provider)
{
    int mode = 0, nvm = 0, probe = 0;
    auto *device = OSDynamicCast(IOPCIDevice, provider);
    PE_parse_boot_argn("itlwm-sequoia-nvm", &nvm, sizeof(nvm));
    PE_parse_boot_argn("itlwm-sequoia-probe", &probe, sizeof(probe));
    return !nvm && !probe && PE_parse_boot_argn("itlwm-sequoia-firmware", &mode, sizeof(mode)) && device &&
        SequoiaTrial::allowed(mode, version_major, version_minor, version,
            device->configRead16(kIOPCIConfigVendorID), device->configRead16(kIOPCIConfigDeviceID));
}

IOService *AirportItlwmSequoiaFirmwareTrial::probe(IOService *provider, SInt32 *score)
{
    return firmwareTrialAllowed(provider) ? super::probe(provider, score) : nullptr;
}

bool AirportItlwmSequoiaFirmwareTrial::createWorkLoop()
{
    if (!loop_) loop_ = IOWorkLoop::workLoop();
    return loop_ != nullptr;
}

bool AirportItlwmSequoiaFirmwareTrial::recordDiagnostics()
{
    struct Snapshot { IwmFirmwareDiagnostics firmware; IwmNvmDiagnostics detail; ItlScan::IwmPassiveTrial::Evidence scan; } snapshot;
    if (loop_->runAction([](OSObject *owner, void *output, void *, void *, void *) -> IOReturn {
        auto *driver = static_cast<ItlIwm *>(owner);
        auto *value = static_cast<Snapshot *>(output);
        value->firmware = driver->nativeFirmwareDiagnostics;
        value->detail = driver->nativeNvmDiagnostics;
        value->scan = driver->nativePassiveScan.evidence;
        return kIOReturnSuccess;
    }, hal_, &snapshot) != kIOReturnSuccess) return false;
    const auto &fw = snapshot.firmware;
    const auto &detail = snapshot.detail;
    bool recorded = setProperty("SequoiaFirmwareSchema", 6, 32);
    recorded = setProperty("SequoiaFirmwareStage", fw.stageName()) && recorded;
    recorded = setProperty("SequoiaFirmwarePhase", detail.phase) && recorded;
    const struct { const char *name; uint32_t value; } fields[] = {
        {"SequoiaFirmwareScanResult", fw.scanResult},
        {"SequoiaFirmwareScanPreflightResult", fw.scanPreflightResult},
        {"SequoiaFirmwareScanDqaResult", fw.scanDqaResult},
        {"SequoiaFirmwareScanAuxResult", fw.scanAuxResult},
        {"SequoiaFirmwareScanSubmitResult", fw.scanSubmitResult},
        {"SequoiaFirmwareScanWaitResult", fw.scanWaitResult},
        {"SequoiaFirmwareScanRequested", snapshot.scan.requested},
        {"SequoiaFirmwareScanIterations", snapshot.scan.iterations},
        {"SequoiaFirmwareScanChannels", snapshot.scan.scanned},
        {"SequoiaFirmwareScanTerminalStatus", snapshot.scan.terminalStatus},
        {"SequoiaFirmwareScanIterationStatus", snapshot.scan.iterationStatus},
        {"SequoiaFirmwareScanPhy", snapshot.scan.phy},
        {"SequoiaFirmwareScanMpdu", snapshot.scan.mpdu},
        {"SequoiaFirmwareScanBeacons", snapshot.scan.beacons},
        {"SequoiaFirmwareScanProbeResponses", snapshot.scan.probeResponses},
        {"SequoiaFirmwareScanMalformed", snapshot.scan.malformed},
        {"SequoiaFirmwareScanBadCrc", snapshot.scan.badCrc},
        {"SequoiaFirmwareScanIgnored", snapshot.scan.ignored},
        {"SequoiaFirmwareScanPhySeen", snapshot.scan.phySeen},
        {"SequoiaFirmwareScanPhyLastBytes", snapshot.scan.phyLastBytes},
        {"SequoiaFirmwareScanPhyLastCfg", snapshot.scan.phyLastCfg},
        {"SequoiaFirmwareScanPhyLastChannel", snapshot.scan.phyLastChannel},
        {"SequoiaFirmwareScanPhyBadSize", snapshot.scan.phyBadSize},
        {"SequoiaFirmwareScanPhyBadConfig", snapshot.scan.phyBadConfig},
        {"SequoiaFirmwareScanPhyBadChannel", snapshot.scan.phyBadChannel},
        {"SequoiaFirmwareScanMissingPhy", snapshot.scan.missingPhy},
        {"SequoiaFirmwareNvmResult", fw.nvmResult},
        {"SequoiaFirmwareCalibrationResult", fw.calibrationResult},
        {"SequoiaFirmwareRegularResult", fw.regularResult},
        {"SequoiaFirmwareConfigurationResult", fw.configurationResult},
        {"SequoiaFirmwareAntennaResult", fw.antennaResult},
        {"SequoiaFirmwareReplayResult", fw.replayResult},
        {"SequoiaFirmwarePhyConfigResult", fw.phyConfigResult},
        {"SequoiaFirmwareBtResult", fw.btResult},
        {"SequoiaFirmwareReplayCommands", fw.replayCommands},
        {"SequoiaFirmwareReplayBytes", fw.replayBytes},
        {"SequoiaFirmwareReplayPapdMask", fw.replayPapdMask},
        {"SequoiaFirmwareReplayTxpMask", fw.replayTxpMask},
        {"SequoiaFirmwareReplayLastType", fw.replayLastType},
        {"SequoiaFirmwareReplayLastGroup", fw.replayLastGroup},
        {"SequoiaFirmwareNvmAliveBytes", fw.nvmAliveBytes},
        {"SequoiaFirmwareNvmAliveStatus", fw.nvmAliveStatus},
        {"SequoiaFirmwareCalibrationAliveBytes", fw.calibrationAliveBytes},
        {"SequoiaFirmwareCalibrationAliveStatus", fw.calibrationAliveStatus},
        {"SequoiaFirmwareRegularAliveBytes", fw.regularAliveBytes},
        {"SequoiaFirmwareRegularAliveStatus", fw.regularAliveStatus},
        {"SequoiaFirmwareCalibrationFlags", fw.calibrationFlags},
        {"SequoiaFirmwarePhyCfgBytes", fw.phy.cfgBytes},
        {"SequoiaFirmwarePhyNchBytes", fw.phy.nchBytes},
        {"SequoiaFirmwarePhyPapdMask", fw.phy.papdMask},
        {"SequoiaFirmwarePhyTxpMask", fw.phy.txpMask},
        {"SequoiaFirmwarePhyBytes", fw.phy.bytes},
        {"SequoiaFirmwarePhyChannels", fw.phy.channels},
        {"SequoiaFirmwarePhyCoveredChannels", fw.phy.coveredChannels},
        {"SequoiaFirmwarePhyFirstUncoveredChannel", fw.phy.firstUncoveredChannel},
        {"SequoiaFirmwareNvmChannelCount", fw.nvmChannels},
        {"SequoiaFirmwareExcludedChannels", fw.excludedChannels},
        {"SequoiaFirmwareFirstExcludedChannel", fw.firstExcludedChannel},
        {"SequoiaFirmwareNvmBytes", detail.bytesRead},
        {"SequoiaFirmwareNvmSections", detail.sectionsRead},
        {"SequoiaFirmwareChannelsAfterParse", detail.channelsAfterParse},
        {"SequoiaFirmwareLastCommand", detail.lastCommand},
        {"SequoiaFirmwareRegulatoryValidFlags", detail.regulatoryValidFlags},
        {"SequoiaFirmwareSkuValue", detail.skuValue},
        {"SequoiaFirmwareAckResult", detail.ackResult},
        {"SequoiaFirmwareAckFlags", detail.ackFlags},
        {"SequoiaFirmwareAckBytes", detail.ackBytes},
        {"SequoiaFirmwareCommandQueue", detail.commandQueue},
        {"SequoiaFirmwareCommandIndex", detail.commandIndex},
        {"SequoiaFirmwareCommandBytes", detail.commandBytes},
        {"SequoiaFirmwareRxPackets", detail.rxPackets},
        {"SequoiaFirmwareRxCommand", detail.rxCommand},
        {"SequoiaFirmwareRxSequence", detail.rxSequence},
        {"SequoiaFirmwareAckSeen", detail.ackSeen},
        {"SequoiaFirmwareAckRejected", detail.ackRejected},
        {"SequoiaFirmwareRawAckCommand", detail.rawAckCommand},
        {"SequoiaFirmwareRawAckSequence", detail.rawAckSequence},
        {"SequoiaFirmwareErrors", detail.firmwareErrors},
        {"SequoiaFirmwareErrorType", detail.firmwareErrorType},
        {"SequoiaFirmwareErrorCommand", detail.firmwareErrorCommand},
        {"SequoiaFirmwareErrorSequence", detail.firmwareErrorSequence},
        {"SequoiaFirmwareTimeoutFlags", detail.timeoutFlags},
        {"SequoiaFirmwareTimeoutQueued", detail.timeoutQueued},
    };
    for (const auto &field : fields) recorded = setProperty(field.name, field.value, 32) && recorded;
    recorded = setProperty("SequoiaFirmwareRegulatoryWords", detail.regulatoryWords) && recorded;
    recorded = setProperty("SequoiaFirmwarePhySkuWords", detail.phySkuWords) && recorded;
    recorded = setProperty("SequoiaFirmwareRegulatoryNonZero", detail.regulatoryNonZero) && recorded;
    recorded = setProperty("SequoiaFirmwareTxpGroupMaxIndex", detail.txpGroupMaxIndex) && recorded;
    recorded = setProperty("SequoiaFirmwareChannelGroups", detail.channelGroups) && recorded;
    recorded = setProperty("SequoiaFirmwarePapdGroupBytes", detail.papdGroupBytes) && recorded;
    recorded = setProperty("SequoiaFirmwareTxpGroupBytes", detail.txpGroupBytes) && recorded;
    recorded = setProperty("SequoiaFirmwareUcodeApiBits", detail.ucodeApiBits) && recorded;
    recorded = setProperty("SequoiaFirmwareEnabledCapaBits", detail.enabledCapaBits) && recorded;
    recorded = setProperty("SequoiaFirmwareScanPreflightMask", detail.scanPreflightMask, 32) && recorded;
    // Probe build: publish the firmware's own section id for every requested
    // section, so the generation-specific mapping can be recovered from one
    // run instead of being guessed at.
    OSDictionary *types = OSDictionary::withCapacity(16);
    OSDictionary *lengths = OSDictionary::withCapacity(16);
    if (types && lengths) {
        for (unsigned i = 0; i < 16; ++i) {
            if (!(detail.sectionTypeMapValid & (uint32_t(1) << i))) continue;
            char key[8];
            snprintf(key, sizeof(key), "%u", i);
            types->setObject(OSString::withCString(key),
                OSNumber::withNumber(detail.sectionTypeMap[i], 32));
            lengths->setObject(OSString::withCString(key),
                OSNumber::withNumber(detail.sectionLengthMap[i], 32));
        }
        recorded = setProperty("SequoiaFirmwareNvmTypeMap", types) && recorded;
        recorded = setProperty("SequoiaFirmwareNvmLengthMap", lengths) && recorded;
        types->release();
        lengths->release();
    }
    if (fw.nvmResult == 0) {
        unsigned channels = 0;
        for (const auto &channel : hal_->get80211Controller()->ic_channels)
            if (channel.ic_freq && channel.ic_flags) ++channels;
        recorded = setProperty("SequoiaFirmwareVersion", hal_->getFirmwareVersion()) && recorded;
        recorded = setProperty("SequoiaFirmwareChannelCount", channels, 32) && recorded;
    }
    return recorded;
}

bool AirportItlwmSequoiaFirmwareTrial::start(IOService *provider)
{
    if (!firmwareTrialAllowed(provider) || !super::start(provider)) return false;
    device_ = OSDynamicCast(IOPCIDevice, provider);
    if (!loop_ || loop_->onThread()) goto fail;
    gate_ = IOCommandGate::commandGate(this);
    if (!gate_ || loop_->addEventSource(gate_) != kIOReturnSuccess) goto fail;
    gateAdded_ = true;
    if (!device_->open(this)) goto fail;
    opened_ = true;
    pciCommand_ = device_->configRead16(kIOPCIConfigCommand);
    device_->setMemoryEnable(true);
    hal_ = new ItlIwm;
    if (!hal_ || !hal_->initWithController(this, loop_, gate_)) goto fail;
    if (!setProperty("SequoiaTrialExpectedBuild", "24G830") ||
        !setProperty("SequoiaTrialKernel", version) ||
        !setProperty("SequoiaTrialNativeWiFi", kOSBooleanFalse) ||
        !setProperty("SequoiaTrialNetworkPublished", kOSBooleanFalse)) goto fail;
    IOLog("AirportItlwmSequoiaFirmwareTrial: NVM/calibration/configuration/passive scan begin; no network\n");
    attached_ = true;
    {
        const bool passed = hal_->attachForFirmwareTrial(device_);
        const int result = hal_->nativeFirmwareTrialError;
        bool recorded = setProperty("SequoiaFirmwareResult", static_cast<uint64_t>(result), 32);
        recorded = recordDiagnostics() && recorded;
        releaseHardware();
        const bool stopped = !(device_->configRead16(kIOPCIConfigCommand) & kIOPCICommandBusMaster);
        recorded = setProperty("SequoiaTrialHardwareStopped", stopped ? kOSBooleanTrue : kOSBooleanFalse) && recorded;
        recorded = setProperty("SequoiaTrialStage", passed ? "firmware-scanned-and-detached" : "firmware-failed-and-detached") && recorded;
        IOLog("AirportItlwmSequoiaFirmwareTrial: result=%d; detached; native-wifi=off\n", result);
        if (!recorded) goto fail;
    }
    registerService();
    return true;
fail:
    releaseHardware();
    super::stop(provider);
    return false;
}

void AirportItlwmSequoiaFirmwareTrial::releaseHardware()
{
    if (hal_) {
        if (attached_) hal_->detach(device_);
        attached_ = false;
        hal_->release();
        hal_ = nullptr;
    }
    if (opened_) {
        device_->setBusMasterEnable(false);
        device_->configWrite16(kIOPCIConfigCommand, pciCommand_ & ~kIOPCICommandBusMaster);
        device_->close(this);
        opened_ = false;
    }
}

void AirportItlwmSequoiaFirmwareTrial::stop(IOService *provider)
{
    releaseHardware();
    super::stop(provider);
}

void AirportItlwmSequoiaFirmwareTrial::free()
{
    if (gate_) {
        if (gateAdded_) loop_->removeEventSource(gate_);
        gate_->release();
        gate_ = nullptr;
    }
    if (loop_) { loop_->release(); loop_ = nullptr; }
    super::free();
}
