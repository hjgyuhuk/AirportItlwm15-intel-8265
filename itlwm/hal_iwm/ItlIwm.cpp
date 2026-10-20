/*
* Copyright (C) 2020  钟先耀
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/

#include "ItlIwm.hpp"

#define super ItlHalService
OSDefineMetaClassAndStructors(ItlIwm, ItlHalService)

#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
// Single definition for the boot-loop HAL progress marker declared in
// ItlHalService.hpp. Only the Sequoia native target ever reads it.
ItlBootProgress gItlwmHalProgress;
#endif

#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
bool ItlIwm::attachForNvmTrial(IOPCIDevice *device)
{
    // 0x24FD is the Intel Wireless 8265 (IWM 8000 family, firmware 36). The
    // upstream trial only accepted the 7000-family AX211; the probe needs to
    // reach the 8000 HAL to see how far the firmware dialogue actually gets.
    const uint16_t deviceId = device ? device->configRead16(kIOPCIConfigDeviceID) : 0;
    if (!device || device->configRead16(kIOPCIConfigVendorID) != 0x8086 ||
        (deviceId != 0x3166 && deviceId != 0x24FD) || pci.pa_tag || nativeDetached)
        return false;
    nativeNvmTrial = true;
    // Probe build: the 8000-family firmware answers with its own section id
    // space. The diagnostic run records the mapping instead of rejecting the
    // answer, so one boot can recover the whole table.
    nativeNvmDiagnostics.allowSectionTypeMismatch = true;
    return attach(device);
}

bool ItlIwm::attachForFirmwareTrial(IOPCIDevice *device)
{
    // 0x24FD is the Intel Wireless 8265 (IWM 8000 family, firmware 36). The
    // upstream trial only accepted the 7000-family AX211; the probe needs to
    // reach the 8000 HAL to see how far the firmware dialogue actually gets.
    const uint16_t deviceId = device ? device->configRead16(kIOPCIConfigDeviceID) : 0;
    if (!device || device->configRead16(kIOPCIConfigVendorID) != 0x8086 ||
        (deviceId != 0x3166 && deviceId != 0x24FD) || pci.pa_tag ||
        nativeDetached || nativeNvmTrial)
        return false;
    nativeFirmwareTrial = true;
    return attach(device);
}
#endif

void ItlIwm::
detach(IOPCIDevice *device)
{
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
    if (!getMainWorkLoop()->inGate()) {
        getMainWorkLoop()->runAction([](OSObject *owner, void *provider, void *, void *, void *) -> IOReturn {
            static_cast<ItlIwm *>(owner)->detach(static_cast<IOPCIDevice *>(provider));
            return kIOReturnSuccess;
        }, this, device);
        return;
    }
    if (nativeDetached)
        return;
    nativeDetached = true;
    nativeCalibrationActive = false;
    nativeFirmwareOwner.close();
    commandTransportReady = false;
    com.sc_flags |= IWM_FLAG_SHUTDOWN;
    ++com.sc_generation;
    if (com.sc_ih) {
        com.sc_ih->disable();
        getMainWorkLoop()->removeEventSource(com.sc_ih);
        com.sc_ih->release();
        com.sc_ih = nullptr;
    }
    if (com.sc_sh) {
        if (nativeHardwareStarted)
            iwm_stop_device(&com);
        iwm_disable_interrupts(&com);
        IWM_WRITE(&com, IWM_CSR_RESET, IWM_CSR_RESET_REG_FLAG_SW_RESET);
        DELAY(5000);
        device->setBusMasterEnable(false);
    }
    // Cancel timer entry points before opening the gate to drain workers.
    if (com.sc_calib_to) timeout_del(&com.sc_calib_to);
    if (com.sc_led_blink_to) timeout_del(&com.sc_led_blink_to);
    if (nativeNet80211Attached)
        timeout_del(&com.sc_ic.ic_bgscan_timeout);
    for (auto &rxba : com.sc_rxba_data) {
        if (rxba.session_timer) timeout_del(&rxba.session_timer);
        if (rxba.reorder_buf.reorder_timer) timeout_del(&rxba.reorder_buf.reorder_timer);
    }
    wakeupOn(&com.sc_ic.ic_state);
    if (nativeSystemQueue) taskq_quiesce(systq);
    taskq_quiesce(com.sc_nswq);
    // These waits open the bound workloop gate for already-running workers.
    taskq_destroy(com.sc_nswq);
    com.sc_nswq = nullptr;
    if (nativeSystemQueue) {
        taskq_destroy(systq);
        nativeSystemQueue = false;
    }
    while (nativeFirmwareOwner.busy())
        getMainCommandGate()->commandSleep(&nativeFirmwareOwner, THREAD_UNINT);
    if (nativeNet80211Attached) {
        for (auto &rxba : com.sc_rxba_data)
            iwm_clear_reorder_buffer(&com, &rxba);
    }
#endif
    struct _ifnet *ifp = &com.sc_ic.ic_ac.ac_if;
    struct iwm_softc *sc = &com;
    
    for (int txq_i = 0; txq_i < nitems(sc->txq); txq_i++)
        iwm_free_tx_ring(sc, &sc->txq[txq_i]);
    iwm_rs_free(sc);
    iwm_free_rx_ring(sc, &sc->rxq);
    iwm_dma_contig_free(&sc->ict_dma);
    iwm_dma_contig_free(&sc->kw_dma);
    iwm_dma_contig_free(&sc->sched_dma);
    iwm_dma_contig_free(&sc->fw_dma);
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
    iwm_free_fw_paging(sc);
    iwm_fw_info_free(&sc->sc_fw);
    iwm_phy_db_clear(sc);
    if (nativeNet80211Attached) {
        ieee80211_ifdetach(ifp);
        nativeNet80211Attached = false;
    }
    if (sc->sc_st) {
        sc->sc_st->release();
        sc->sc_st = nullptr;
        sc->sc_sh = nullptr;
    }
#else
    ieee80211_ifdetach(ifp);
    taskq_destroy(systq);
    taskq_destroy(com.sc_nswq);
#endif
    releaseAll();
}

bool ItlIwm::
attach(IOPCIDevice *device)
{
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
    if (!getMainWorkLoop()->inGate()) {
        ITLWM_HAL_STAGE("attach:dispatch-to-workloop");
        return getMainWorkLoop()->runAction([](OSObject *owner, void *provider, void *, void *, void *) -> IOReturn {
            return static_cast<ItlIwm *>(owner)->attach(static_cast<IOPCIDevice *>(provider)) ?
                kIOReturnSuccess : kIOReturnError;
        }, this, device) == kIOReturnSuccess;
    }
    if (getMainWorkLoop()->onThread() || nativeDetached || pci.pa_tag)
        return false;
#endif
    pci.pa_tag = device;
    pci.workloop = getMainWorkLoop();
    bool attached;
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
    {
        ITLWM_HAL_STAGE("attach:before-firmware-lease");
        FirmwareLease operation(this);
        ITLWM_HAL_STAGE("attach:before-iwm_attach");
        attached = operation && iwm_attach(&com, &pci);
        ITLWM_HAL_STAGE("attach:after-iwm_attach");
    }
#else
    attached = iwm_attach(&com, &pci);
#endif
    if (!attached) {
        detach(device);
        releaseAll();
        return false;
    }
    return true;
}

void ItlIwm::
free()
{
    XYLog("%s\n", __FUNCTION__);
    super::free();
}

void ItlIwm::
releaseAll()
{
    XYLog("%s\n", __FUNCTION__);
    pci_intr_handle *intrHandler = com.ih;
    if (com.sc_calib_to) {
        timeout_del(&com.sc_calib_to);
        timeout_free(&com.sc_calib_to);
    }
    if (com.sc_led_blink_to) {
        timeout_del(&com.sc_led_blink_to);
        timeout_free(&com.sc_led_blink_to);
    }
    if (intrHandler) {
        if (intrHandler->intr && intrHandler->workloop) {
//            intrHandler->intr->disable();
            intrHandler->workloop->removeEventSource(intrHandler->intr);
            intrHandler->intr->release();
        }
        intrHandler->intr = NULL;
        intrHandler->workloop = NULL;
        intrHandler->arg = NULL;
        intrHandler->dev = NULL;
        intrHandler->func = NULL;
        intrHandler->release();
        com.ih = NULL;
    }
    pci.pa_tag = NULL;
    pci.workloop = NULL;
}

IOReturn ItlIwm::
enable(IONetworkInterface *netif)
{
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
    if (!getMainWorkLoop()->inGate())
        return getMainWorkLoop()->runAction([](OSObject *owner, void *interface, void *, void *, void *) -> IOReturn {
            return static_cast<ItlIwm *>(owner)->enable(static_cast<IONetworkInterface *>(interface));
        }, this, netif);
    if (getMainWorkLoop()->onThread()) return kIOReturnCannotLock;
    if (nativeDetached || isHardwareTrial() || !nativeNet80211Attached)
        return kIOReturnNotReady;
    FirmwareLease operation(this);
    if (!operation) return kIOReturnBusy;
    auto *ifp = &com.sc_ic.ic_ac.ac_if;
    // A fault only reset hardware. An explicit WCL request owns the complete
    // worker/key cleanup before reusing BSD state or restarting the device.
    if (nativeStateError) iwm_stop(ifp);
    if (ifp->if_flags & IFF_RUNNING) return kIOReturnSuccess;
    const int generation = com.sc_generation;
    int error = iwm_resume(&com);
    if (!error && (com.sc_flags & IWM_FLAG_RFKILL)) return kIOReturnNotReady;
    if (!error) {
        ifp->if_flags |= IFF_UP;
        error = iwm_init(ifp); // Native init leaves the radio idle; WCL owns scanning.
    }
    if (error) {
        ifp->if_flags &= ~IFF_UP;
        if (!nativeDetached && generation == com.sc_generation) iwm_stop_device(&com);
        return error == ETIMEDOUT ? kIOReturnTimeout : kIOReturnIOError;
    }
    return kIOReturnSuccess;
#else
    XYLog("%s\n", __PRETTY_FUNCTION__);
    struct _ifnet *ifp = &com.sc_ic.ic_ac.ac_if;
    if (ifp->if_flags & IFF_UP) {
        XYLog("%s already in activating state\n", __FUNCTION__);
        return kIOReturnSuccess;
    }
    ifp->if_flags |= IFF_UP;
    iwm_activate(&com, DVACT_RESUME);
    iwm_activate(&com, DVACT_WAKEUP);
    return kIOReturnSuccess;
#endif
}

IOReturn ItlIwm::
disable(IONetworkInterface *netif)
{
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
    if (!getMainWorkLoop()->inGate())
        return getMainWorkLoop()->runAction([](OSObject *owner, void *interface, void *, void *, void *) -> IOReturn {
            return static_cast<ItlIwm *>(owner)->disable(static_cast<IONetworkInterface *>(interface));
        }, this, netif);
    if (getMainWorkLoop()->onThread()) return kIOReturnCannotLock;
    if (nativeDetached || isHardwareTrial() || !nativeNet80211Attached)
        return kIOReturnNotReady;
    FirmwareLease operation(this);
    if (!operation) return kIOReturnBusy;
    com.sc_ic.ic_ac.ac_if.if_flags &= ~IFF_UP;
    iwm_stop(&com.sc_ic.ic_ac.ac_if);
    return kIOReturnSuccess;
#else
    struct _ifnet *ifp = &com.sc_ic.ic_ac.ac_if;
    if (!(ifp->if_flags & IFF_UP)) {
        XYLog("%s already in diactivating state\n", __FUNCTION__);
        return kIOReturnSuccess;
    }
    ifp->if_flags &= ~IFF_UP;
    iwm_activate(&com, DVACT_QUIESCE);
    return kIOReturnSuccess;
#endif
}

struct ieee80211com *ItlIwm::
get80211Controller()
{
    return &com.sc_ic;
}

ItlDriverInfo *ItlIwm::
getDriverInfo()
{
    return this;
}

ItlDriverController *ItlIwm::
getDriverController()
{
    return this;
}

void ItlIwm::
clearScanningFlags()
{
    com.sc_flags &= ~(IWM_FLAG_SCANNING | IWM_FLAG_BGSCAN);
}

IOReturn ItlIwm::
setMulticastList(IOEthernetAddress *addr, int count)
{
    struct ieee80211com *ic = &com.sc_ic;
    struct iwm_mcast_filter_cmd *cmd;
    int len;
    uint8_t addr_count;
    int err;
    
    if (ic->ic_state != IEEE80211_S_RUN || ic->ic_bss == NULL)
        return kIOReturnError;
    addr_count = count;
    if (count > IWM_MAX_MCAST_FILTERING_ADDRESSES)
        addr_count = 0;
    if (addr == NULL)
        addr_count = 0;
    len = roundup(sizeof(struct iwm_mcast_filter_cmd) + addr_count * ETHER_ADDR_LEN, 4);
    XYLog("%s multicast count=%d bssid=%s\n", __FUNCTION__, count, ether_sprintf(ic->ic_bss->ni_bssid));
    cmd = (struct iwm_mcast_filter_cmd *)malloc(len, 0, 0);
    if (!cmd)
        return kIOReturnError;
    cmd->pass_all = addr_count == 0;
    cmd->count = addr_count;
    cmd->port_id = 0;
    IEEE80211_ADDR_COPY(cmd->bssid, ic->ic_bss->ni_bssid);
    if (addr_count > 0)
        memcpy(cmd->addr_list,
               addr->bytes, ETHER_ADDR_LEN * cmd->count);
    err = iwm_send_cmd_pdu(&com, IWM_MCAST_FILTER_CMD, IWM_CMD_ASYNC, len,
                     cmd);
    ::free(cmd);
    return err ? kIOReturnError : kIOReturnSuccess;
}

const char *ItlIwm::
getFirmwareVersion()
{
    return com.sc_fwver;
}

const char *ItlIwm::
getFirmwareName()
{
    return com.sc_fwname;
}

UInt32 ItlIwm::
supportedFeatures()
{
    return kIONetworkFeatureMultiPages;
}

const char *ItlIwm::
getFirmwareCountryCode()
{
    return com.sc_fw_mcc;
}

uint32_t ItlIwm::
getTxQueueSize()
{
    return IWM_TX_RING_COUNT;
}

int16_t ItlIwm::
getBSSNoise()
{
    return com.sc_noise;
}

bool ItlIwm::
is5GBandSupport()
{
    return com.sc_nvm.sku_cap_band_52GHz_enable;
}

int ItlIwm::
getTxNSS()
{
    return iwm_mimo_enabled(&com) &&
    (com.sc_ic.ic_bss != NULL && com.sc_ic.ic_bss->ni_rx_nss > 1) ?
    2 : 1;
}
