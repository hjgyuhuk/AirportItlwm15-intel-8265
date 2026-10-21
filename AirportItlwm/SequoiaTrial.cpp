#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/version.h>
#include <pexpert/pexpert.h>
#include <HAL/SequoiaTrialPolicy.h>

// This personality does not instantiate AirportItlwm, attach a HAL, open PCI,
// program registers, allocate DMA, publish a network interface or transmit.
// The full driver remains in the image so KXLD must resolve its dependencies.
class AirportItlwmSequoiaProbe : public IOService {
    OSDeclareDefaultStructors(AirportItlwmSequoiaProbe)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
};

#define super IOService
OSDefineMetaClassAndStructors(AirportItlwmSequoiaProbe, IOService)

static bool trialAllowed(IOService *provider)
{
    int mode = 0;
    if (!PE_parse_boot_argn("itlwm-sequoia-probe", &mode, sizeof(mode)))
        return false;
    auto *device = OSDynamicCast(IOPCIDevice, provider);
    // osversion is present in BootKC but is NOT exported to third-party kexts.
    // Match the signed KDK's exact kernel; the host preflight also checks 24G830.
    return device && SequoiaTrial::allowed(mode, version_major, version_minor, version,
        device->configRead16(kIOPCIConfigVendorID), device->configRead16(kIOPCIConfigDeviceID));
}

IOService *AirportItlwmSequoiaProbe::probe(IOService *provider, SInt32 *score)
{
    if (!trialAllowed(provider))
        return nullptr;
    return super::probe(provider, score);
}

bool AirportItlwmSequoiaProbe::start(IOService *provider)
{
    if (!trialAllowed(provider) || !super::start(provider))
        return false;
    if (!setProperty("SequoiaTrialStage", "linked-and-pci-matched") ||
        !setProperty("SequoiaTrialExpectedBuild", "24G830") ||
        !setProperty("SequoiaTrialKernel", version) ||
        !setProperty("SequoiaTrialNativeWiFi", kOSBooleanFalse) ||
        !setProperty("SequoiaTrialRadioEnabled", kOSBooleanFalse)) {
        super::stop(provider);
        return false;
    }
    IOLog("AirportItlwmSequoiaProbe: stage=linked-and-pci-matched build=24G830 pci=8086:3166 radio=off native-wifi=off\n");
    registerService();
    return true;
}

void AirportItlwmSequoiaProbe::stop(IOService *provider)
{
    IOLog("AirportItlwmSequoiaProbe: stopped; no HAL or radio was started\n");
    super::stop(provider);
}
