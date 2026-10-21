//
//  AirportItlwmEthernetInterface.cpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#include "AirportItlwmEthernetInterface.hpp"
#include <HAL/ItlDiagnostics.h>

#include <sys/_if_ether.h>
#include <net80211/ieee80211_var.h>
#if __IO80211_TARGET >= __MAC_15_0
#include "AirportItlwmV2.hpp"
#endif

#define super IOEthernetInterface
OSDefineMetaClassAndStructors(AirportItlwmEthernetInterface, IOEthernetInterface);

bool AirportItlwmEthernetInterface::
initWithSkywalkInterfaceAndProvider(IONetworkController *controller, IO80211SkywalkInterface *interface)
{
    bool ret = super::init(controller);
    if (ret)
        this->interface = interface;
    this->isAttach = false;
    return ret;
}

#if __IO80211_TARGET >= __MAC_15_0
IOReturn AirportItlwmEthernetInterface::recordNativeBSDResult(const char *stage, IOReturn result)
{
    // The provider survives a failed native controller start within this boot.
    auto *owner = getController();
    auto *evidence = owner ? owner->getProvider() : nullptr;
    IOService *targets[] = {owner, evidence};
    for (auto *target : targets) {
        if (!target) continue;
        ITLWM_PUBLISH_DIAGNOSTIC(target, "ItlwmNativeBSDStage", stage);
        ITLWM_PUBLISH_DIAGNOSTIC(target, "ItlwmNativeBSDResult", static_cast<UInt64>(result), 32);
    }
    return result;
}
#endif

IOReturn AirportItlwmEthernetInterface::
attachToDataLinkLayer( IOOptionBits options, void *parameter )
{
#if __IO80211_TARGET >= __MAC_15_0
    if (!interface || isAttach || nativeBSDPrepared)
        return recordNativeBSDResult("invalid-or-duplicate-attach", kIOReturnNotReady);
    auto *owner = OSDynamicCast(AirportItlwm, getController());
    // nativeAvailable acquires the startup/stop admission lock. Successful
    // admission observes completed registration/start; their resources remain
    // valid until stop's synchronous detach has drained this callback.
    if (!owner || !owner->nativeAvailable())
        return recordNativeBSDResult("controller-not-ready", kIOReturnNotReady);
    if (owner->fNetIf != interface || owner->bsdInterface != this ||
        !owner->nativeInterfaceStarted || !owner->nativeRegistration.prepared())
        return recordNativeBSDResult("interface-not-ready", kIOReturnNotReady);
    const auto *network = static_cast<IOSkywalkNetworkInterface *>(interface);
    if (!network->mExpansionData || !network->mExpansionData->fRegistrationInfo)
        return recordNativeBSDResult("registration-storage-missing", kIOReturnNotReady);
    // 24G830 IO80211's _getIfListCopy admits SIOCGIFTYPE type=IFT_ETHER
    // only with subfamily=3 (Wi-Fi). The Ethernet backing defaults to 0,
    // which leaves a visible enN but no discoverable airportd interface.
    // IONetworkInterface copies this into ifnet_init_eparams at allocation;
    // its setter rejects changes after BSD attachment. Do not change if_type.
    constexpr uint32_t nativeWiFiSubfamily = 3;
    if (!setInterfaceSubType(nativeWiFiSubfamily))
        return recordNativeBSDResult("bsd-wifi-subtype-failed", kIOReturnNotReady);
    recordNativeBSDResult("bsd-attach-in-progress", kIOReturnNotReady);
    IOReturn result = super::attachToDataLinkLayer(options, parameter);
    if (result != kIOReturnSuccess) return recordNativeBSDResult("bsd-attach-failed", result);
    if (!getIfnet()) {
        super::detachFromDataLinkLayer(options, parameter);
        return recordNativeBSDResult("bsd-ifnet-missing", kIOReturnNotReady);
    }
    char name[IFNAMSIZ];
    UInt8 builtIn = 0;
    snprintf(name, sizeof(name), "%s%u", ifnet_name(getIfnet()), ifnet_unit(getIfnet()));
    // Both objects describe the same BSD interface, but IOInterfaceName is
    // the Apple80211 control-service lookup key, not the BSD child's name.
    // 24G830 _Apple80211FindService takes the first property-only match and
    // _isInfraInterface reads its role without searching parents. Publishing
    // IOInterfaceName on this child shadows the real Skywalk control service.
    // Keep BSD Name/unit/prefix on both; only Skywalk owns IOInterfaceName.
    auto publishBSDProperties = [&](auto *target) {
        return target &&
            target->setProperty("built-in", &builtIn, sizeof(builtIn)) &&
            target->setProperty("BSD Name", name) &&
            target->setProperty(kIOInterfaceUnit, ifnet_unit(getIfnet()), 32) &&
            target->setProperty(kIOInterfaceNamePrefix, ifnet_name(getIfnet()));
    };
    if (!publishBSDProperties(this) || !publishBSDProperties(interface) ||
        !interface->setProperty("IOInterfaceName", name)) {
        super::detachFromDataLinkLayer(options, parameter);
        return recordNativeBSDResult("bsd-properties-failed", kIOReturnNoMemory);
    }
    recordNativeBSDResult("skywalk-prepare-in-progress", kIOReturnNotReady);
    result = owner->prepareNativeBSDInterface(interface, getIfnet());
    if (result != kIOReturnSuccess) {
        // The base stores ifnet before it can report a later preparation error.
        interface->finalizeBSDInterface(getIfnet(), 0);
        super::detachFromDataLinkLayer(options, parameter);
        return recordNativeBSDResult("skywalk-prepare-failed", result);
    }
    nativeBSDPrepared = true;
    isAttach = true;
    // This callback publishes the BSD child.  The parent Skywalk interface is
    // published by startNative() only after this callback returns; registering
    // `interface` here leaves AirportItlwmEthernetInterface !registered and
    // produces an un-named enN shell with no CoreWLAN service.
    registerService();
    return recordNativeBSDResult("bsd-prepared-and-published", kIOReturnSuccess);
#else
    XYLog("%s\n", __FUNCTION__);
    char infName[IFNAMSIZ];
    IOReturn ret = super::attachToDataLinkLayer(options, parameter);
    if (ret == kIOReturnSuccess && interface) {
        UInt8 builtIn = 0;
        IOEthernetAddress addr;
        interface->setProperty("built-in", OSData::withBytes(&builtIn, sizeof(builtIn)));
        snprintf(infName, sizeof(infName), "%s%u", ifnet_name(getIfnet()), ifnet_unit(getIfnet()));
        interface->setProperty("IOInterfaceName", OSString::withCString(infName));
        interface->setProperty(kIOInterfaceUnit, OSNumber::withNumber(ifnet_unit(getIfnet()), 8));
        interface->setProperty(kIOInterfaceNamePrefix, OSString::withCString(ifnet_name(getIfnet())));
        if (OSDynamicCast(IOEthernetController, getController())->getHardwareAddress(&addr) == kIOReturnSuccess)
            setProperty(kIOMACAddress,  (void *) &addr,
                        kIOEthernetAddressSize);
        interface->registerService();
        interface->prepareBSDInterface(getIfnet(), 0);
//        ret = bpf_attach(getIfnet(), DLT_RAW, 0x48, &AirportItlwmEthernetInterface::bpfOutputPacket, &AirportItlwmEthernetInterface::bpfTap);
    }
    isAttach = true;
    return ret;
#endif
}

void AirportItlwmEthernetInterface::
detachFromDataLinkLayer(IOOptionBits options, void *parameter)
{
#if __IO80211_TARGET >= __MAC_15_0
    if (nativeBSDPrepared) {
        interface->finalizeBSDInterface(getIfnet(), 0);
        nativeBSDPrepared = false;
    }
    isAttach = false;
#endif
    super::detachFromDataLinkLayer(options, parameter);
    isAttach = false;
#if __IO80211_TARGET >= __MAC_15_0
    recordNativeBSDResult("bsd-detached", kIOReturnSuccess);
#endif
}

/**
 Add another hack to fake that the provider is IOSkywalkNetworkInterface, to avoid skywalkfamily instance cast panic.
 */
IOService *AirportItlwmEthernetInterface::
getProvider() const
{
    return isAttach ? this->interface : super::getProvider();
}

errno_t AirportItlwmEthernetInterface::
bpfOutputPacket(ifnet_t interface, u_int32_t data_link_type, mbuf_t packet)
{
    XYLog("%s data_link_type: %d\n", __FUNCTION__, data_link_type);
    AirportItlwmEthernetInterface *networkInterface = (AirportItlwmEthernetInterface *)ifnet_softc(interface);
    return networkInterface->enqueueOutputPacket(packet);
}

errno_t AirportItlwmEthernetInterface::
bpfTap(ifnet_t interface, u_int32_t data_link_type, bpf_tap_mode direction)
{
    XYLog("%s data_link_type: %d direction: %d\n", __FUNCTION__, data_link_type, direction);
    return 0;
}

bool AirportItlwmEthernetInterface::
setLinkState(IO80211LinkState state)
{
    if (state == kIO80211NetworkLinkUp) {
        ifnet_set_flags(getIfnet(), ifnet_flags(getIfnet()) | (IFF_UP | IFF_RUNNING), (IFF_UP | IFF_RUNNING));
    } else {
        ifnet_set_flags(getIfnet(), ifnet_flags(getIfnet()) & ~(IFF_UP | IFF_RUNNING), 0);
    }
    return true;
}

extern const char* hexdump(uint8_t *buf, size_t len);

UInt32 AirportItlwmEthernetInterface::
inputPacket(mbuf_t packet, UInt32 length, IOOptionBits options, void *param)
{
#if __IO80211_TARGET >= __MAC_15_0
    auto *owner = OSDynamicCast(AirportItlwm, getController());
    if (owner && owner->nativeReceivePacket(packet)) return 1;
    // Skywalk delivery is not ready (the RX queue has no netif_queue wired to it),
    // so the packet would otherwise be dropped. Hand it to the standard
    // IOEthernetInterface input path, which delivers it to the BSD protocol stack;
    // this is the route DHCP and ordinary traffic take. nativeReceivePacket leaves
    // the mbuf owned by the caller when it does not deliver.
    return IOEthernetInterface::inputPacket(packet, length, options, param);
#else
    ether_header_t *eh;
    size_t len = mbuf_len(packet);
    
    eh = (ether_header_t *)mbuf_data(packet);
    if (len >= sizeof(ether_header_t) && eh->ether_type == htons(ETHERTYPE_PAE)) { // EAPOL packet
        const char* dump = hexdump((uint8_t*)mbuf_data(packet), len);
        XYLog("input EAPOL packet, len: %zu, data: %s\n", len, dump ? dump : "Failed to allocate memory");
        if (dump)
            IOFree((void*)dump, 3 * len + 1);
    }
    return IOEthernetInterface::inputPacket(packet, length, options, param);
#endif
}
