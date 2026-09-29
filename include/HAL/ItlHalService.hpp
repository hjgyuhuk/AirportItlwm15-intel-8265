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

#ifndef ItlHalService_hpp
#define ItlHalService_hpp

#include <libkern/c++/OSObject.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>

#include "ItlDriverInfo.hpp"
#include "ItlDriverController.hpp"

#include <net80211/ieee80211_var.h>

class ItlNativeHal;

// Boot-loop diagnosis. The HAL attach path (ItlIwm::attach -> iwm_attach ->
// iwm_preinit -> firmware INIT) executes on the IOWorkLoop thread, so the
// startNative() stage label cannot advance while the IOKit configuration thread
// is parked inside fHalService->attach(). These markers record how far the HAL
// actually got; the native stage reporter echoes the value next to its own
// label, which localizes a stall inside attach() in one boot. They are no-ops
// for every non-native target, where the native reporter does not exist.
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
#include <IOKit/IOLib.h>
#include "ItlBootProgress.h"
extern ItlBootProgress gItlwmHalProgress;
// The recorder is lock free and allocation free, but a stalled HAL attach leaves
// nothing in IORegistry and the reporter's first tick is two seconds away, so
// each marker is also printed in place during the start region: the frozen
// console is then the record of how far the HAL actually got.
#define ITLWM_HAL_STAGE(label) do { \
    gItlwmHalProgress.record(label); \
    IOLog("itlwm-native: hal %s\n", (label)); \
} while (0)
#define ITLWM_HAL_RESULT(value) gItlwmHalProgress.recordResult(value)
#define ITLWM_IWX_DETAIL(key, value) gItlwmHalProgress.iwxRecord(ItlBootProgress::key, (value))
#define ITLWM_IWX_BUMP(key) gItlwmHalProgress.iwxBump(ItlBootProgress::key)
#else
#define ITLWM_HAL_STAGE(label) do { } while (0)
#define ITLWM_HAL_RESULT(value) do { } while (0)
#define ITLWM_IWX_DETAIL(key, value) do { } while (0)
#define ITLWM_IWX_BUMP(key) do { } while (0)
#endif

class ItlHalService : public OSObject {
    OSDeclareAbstractStructors(ItlHalService)
    
public:
    
    virtual bool attach(IOPCIDevice *device) = 0;
    
    virtual void detach(IOPCIDevice *device) = 0;
    
    virtual IOReturn enable(IONetworkInterface *interface) = 0;
    
    virtual IOReturn disable(IONetworkInterface *interface) = 0;
    
    virtual struct ieee80211com *get80211Controller() = 0;
    
    virtual ItlDriverInfo *getDriverInfo() = 0;
    
    virtual ItlDriverController *getDriverController() = 0;

#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
    virtual ItlNativeHal *getNativeHal() { return nullptr; }
#endif
    
    virtual void free() override;

public:
    virtual bool initWithController(IOEthernetController *controller, IOWorkLoop *workloop, IOCommandGate *commandGate);
    
protected:
    
    int tsleep_nsec(void *ident, int priority, const char *wmesg, int timo);
    
    void wakeupOn(void* ident);
    
    IOEthernetController *getController();
    
    IOCommandGate *getMainCommandGate();
    
    IOWorkLoop *getMainWorkLoop();
    
private:
    IOEthernetController *controller;
    IOCommandGate *mainCommandGate;
    IOWorkLoop *mainWorkLoop;

    lck_grp_t *inner_gp;
    lck_grp_attr_t *inner_gp_attr;
    lck_attr_t *inner_attr;
    lck_mtx_t *inner_lock;
};

#endif /* ItlHalService_hpp */
