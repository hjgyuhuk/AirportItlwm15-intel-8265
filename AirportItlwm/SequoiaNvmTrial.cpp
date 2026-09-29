#include <IOKit/network/IOEthernetController.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/version.h>
#include <pexpert/pexpert.h>
#include <HAL/SequoiaTrialPolicy.h>
#include "ItlIwm.hpp"

// IOEthernetController supplies the HAL's mbuf allocator only. No interface
// is attached or registered; the only hardware operation is INIT + NVM read.
class AirportItlwmSequoiaNvmTrial : public IOEthernetController {
    OSDeclareDefaultStructors(AirportItlwmSequoiaNvmTrial)
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
    IOWorkLoop *loop_ = nullptr;
    IOCommandGate *gate_ = nullptr;
    IOPCIDevice *device_ = nullptr;
    ItlIwm *hal_ = nullptr;
    bool opened_ = false;
    bool attached_ = false;
    bool gateAdded_ = false;
    uint16_t pciCommand_ = 0;
};

#define super IOEthernetController
OSDefineMetaClassAndStructors(AirportItlwmSequoiaNvmTrial, IOEthernetController)

static bool nvmTrialAllowed(IOService *provider)
{
    int mode = 0;
    auto *device = OSDynamicCast(IOPCIDevice, provider);
    return PE_parse_boot_argn("itlwm-sequoia-nvm", &mode, sizeof(mode)) && device &&
        SequoiaTrial::allowed(mode, version_major, version_minor, version,
            device->configRead16(kIOPCIConfigVendorID), device->configRead16(kIOPCIConfigDeviceID));
}

static bool recordNvmDiagnostics(IOService *service, const IwmNvmDiagnostics &data)
{
    bool recorded = service->setProperty("SequoiaTrialDiagnosticSchema", 1, 32);
    recorded = service->setProperty("SequoiaTrialPhase", data.phase) && recorded;
    recorded = service->setProperty("SequoiaTrialNvmCheck", data.nvmCheck) && recorded;
    const struct { const char *name; uint32_t value; } fields[] = {
        {"SequoiaTrialAliveBytes", data.aliveBytes},
        {"SequoiaTrialAliveStatus", data.aliveStatus},
        {"SequoiaTrialLastCommand", data.lastCommand},
        {"SequoiaTrialAckResult", data.ackResult},
        {"SequoiaTrialAckFlags", data.ackFlags},
        {"SequoiaTrialAckBytes", data.ackBytes},
        {"SequoiaTrialNvmSection", data.section},
        {"SequoiaTrialNvmOffset", data.offset},
        {"SequoiaTrialNvmRequested", data.requested},
        {"SequoiaTrialNvmResponseStatus", data.responseStatus},
        {"SequoiaTrialNvmResponseOffset", data.responseOffset},
        {"SequoiaTrialNvmResponseType", data.responseType},
        {"SequoiaTrialNvmResponseLength", data.responseLength},
        {"SequoiaTrialNvmPayloadBytes", data.payloadBytes},
        {"SequoiaTrialNvmReadError", data.nvmError},
        {"SequoiaTrialNvmBytesRead", data.bytesRead},
        {"SequoiaTrialNvmSectionsRead", data.sectionsRead},
    };
    for (const auto &field : fields)
        recorded = service->setProperty(field.name, field.value, 32) && recorded;
    return recorded;
}

IOService *AirportItlwmSequoiaNvmTrial::probe(IOService *provider, SInt32 *score)
{
    return nvmTrialAllowed(provider) ? super::probe(provider, score) : nullptr;
}

bool AirportItlwmSequoiaNvmTrial::createWorkLoop()
{
    if (!loop_) loop_ = IOWorkLoop::workLoop();
    return loop_ != nullptr;
}

bool AirportItlwmSequoiaNvmTrial::start(IOService *provider)
{
    if (!nvmTrialAllowed(provider) || !super::start(provider)) return false;
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
    if (!setProperty("SequoiaTrialStage", "nvm-initializing") ||
        !setProperty("SequoiaTrialExpectedBuild", "24G830") ||
        !setProperty("SequoiaTrialKernel", version) ||
        !setProperty("SequoiaTrialNativeWiFi", kOSBooleanFalse) ||
        !setProperty("SequoiaTrialNetworkPublished", kOSBooleanFalse)) goto fail;
    IOLog("AirportItlwmSequoiaNvmTrial: INIT/NVM begin pci=8086:3166; no scan/join\n");
    attached_ = true;
    {
        const bool passed = hal_->attachForNvmTrial(device_);
        const int result = hal_->nativeNvmTrialError;
        bool recorded = setProperty("SequoiaTrialNvmResult", static_cast<uint64_t>(result), 32);
        IwmNvmDiagnostics diagnostics;
        const bool snapshot = loop_->runAction([](OSObject *owner, void *output, void *, void *, void *) -> IOReturn {
            *static_cast<IwmNvmDiagnostics *>(output) = static_cast<ItlIwm *>(owner)->nativeNvmDiagnostics;
            return kIOReturnSuccess;
        }, hal_, &diagnostics) == kIOReturnSuccess;
        recorded = (snapshot && recordNvmDiagnostics(this, diagnostics)) && recorded;
        if (passed) {
            unsigned channels = 0;
            const auto *ic = hal_->get80211Controller();
            for (const auto &channel : ic->ic_channels)
                if (channel.ic_freq && channel.ic_flags) ++channels;
            recorded = setProperty("SequoiaTrialFirmware", hal_->getFirmwareVersion()) && recorded;
            recorded = setProperty("SequoiaTrialChannelCount", channels, 32) && recorded;
        }
        releaseHardware();
        const bool stopped = !(device_->configRead16(kIOPCIConfigCommand) & kIOPCICommandBusMaster);
        recorded = setProperty("SequoiaTrialHardwareStopped", stopped ? kOSBooleanTrue : kOSBooleanFalse) && recorded;
        recorded = setProperty("SequoiaTrialStage", passed ? "nvm-read-and-detached" : "nvm-failed-and-detached") && recorded;
        IOLog("AirportItlwmSequoiaNvmTrial: NVM result=%d, hardware detached; native-wifi=off\n", result);
        if (!recorded) goto fail;
    }
    registerService();
    return true;
fail:
    releaseHardware();
    super::stop(provider);
    return false;
}

void AirportItlwmSequoiaNvmTrial::releaseHardware()
{
    if (hal_) {
        if (attached_) hal_->detach(device_);
        attached_ = false;
        hal_->release();
        hal_ = nullptr;
    }
    if (opened_) {
        // Never leave bus mastering enabled without its DMA mappings.
        device_->setBusMasterEnable(false);
        device_->configWrite16(kIOPCIConfigCommand, pciCommand_ & ~kIOPCICommandBusMaster);
        device_->close(this);
        opened_ = false;
    }
}

void AirportItlwmSequoiaNvmTrial::stop(IOService *provider)
{
    releaseHardware();
    super::stop(provider);
}

void AirportItlwmSequoiaNvmTrial::free()
{
    if (gate_) {
        if (gateAdded_) loop_->removeEventSource(gate_);
        gate_->release();
        gate_ = nullptr;
    }
    if (loop_) { loop_->release(); loop_ = nullptr; }
    super::free();
}
