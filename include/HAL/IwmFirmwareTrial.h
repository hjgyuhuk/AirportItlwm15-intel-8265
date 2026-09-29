#ifndef IWM_FIRMWARE_TRIAL_H
#define IWM_FIRMWARE_TRIAL_H

#include <stdint.h>

enum class IwmFirmwareStage { Resources, Nvm, Calibration, Regular, Configuration, Scan, Complete };

struct IwmPhyDbSnapshot {
    uint32_t cfgBytes = 0, nchBytes = 0, papdMask = 0, txpMask = 0;
    uint32_t bytes = 0, channels = 0, coveredChannels = 0;
    uint32_t firstUncoveredChannel = UINT32_MAX;
    bool complete() const { return cfgBytes && nchBytes && channels && channels == coveredChannels; }
};

// Bounded observations, not PHY/NVM contents. All access uses the HAL gate.
struct IwmFirmwareDiagnostics {
    IwmFirmwareStage stage = IwmFirmwareStage::Resources;
    uint32_t nvmResult = UINT32_MAX, calibrationResult = UINT32_MAX, regularResult = UINT32_MAX;
    uint32_t nvmAliveBytes = 0, nvmAliveStatus = UINT32_MAX;
    uint32_t calibrationAliveBytes = 0, calibrationAliveStatus = UINT32_MAX;
    uint32_t regularAliveBytes = 0, regularAliveStatus = UINT32_MAX;
    uint32_t calibrationFlags = 0;
    uint32_t nvmChannels = 0, excludedChannels = 0, firstExcludedChannel = UINT32_MAX;
    uint32_t configurationResult = UINT32_MAX, antennaResult = UINT32_MAX;
    uint32_t replayResult = UINT32_MAX, phyConfigResult = UINT32_MAX, btResult = UINT32_MAX;
    uint32_t replayCommands = 0, replayBytes = 0, replayPapdMask = 0, replayTxpMask = 0;
    uint32_t replayLastType = UINT32_MAX, replayLastGroup = UINT32_MAX;
    uint32_t scanResult = UINT32_MAX, scanPreflightResult = UINT32_MAX, scanDqaResult = UINT32_MAX;
    uint32_t scanAuxResult = UINT32_MAX, scanSubmitResult = UINT32_MAX, scanWaitResult = UINT32_MAX;
    IwmPhyDbSnapshot phy;

    const char *stageName() const
    {
        switch (stage) {
            case IwmFirmwareStage::Resources: return "resources";
            case IwmFirmwareStage::Nvm: return "nvm";
            case IwmFirmwareStage::Calibration: return "calibration";
            case IwmFirmwareStage::Regular: return "regular";
            case IwmFirmwareStage::Configuration: return "configuration";
            case IwmFirmwareStage::Scan: return "scan";
            case IwmFirmwareStage::Complete: return "complete";
        }
        return "invalid";
    }
};

#endif
