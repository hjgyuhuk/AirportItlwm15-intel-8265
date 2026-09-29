//
//  IOPCIEDeviceWrapper.hpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef IOPCIEDeviceWrapper_hpp
#define IOPCIEDeviceWrapper_hpp

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOTypes.h>

#include <HAL/ItlHalService.hpp>

// The peer manager is built inside the native lifecycle, where the SDK's IEEE
// 80211 declarations are the ones in scope; see native_lifecycle_sequoia.inc.
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
#include <HAL/SequoiaPCIOwner.h>
#include <Airport/SequoiaThreadCall.h>
#endif

class IOPCIEDeviceWrapper : public IOService {
    OSDeclareDefaultStructors(IOPCIEDeviceWrapper)
    
public:
    virtual IOService* probe(IOService* provider, SInt32* score) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
    virtual void free() override;
#endif
    virtual IOWorkLoop* getWorkLoop() const override;
    virtual IOReturn setPowerState(
        unsigned long powerStateOrdinal,
                                   IOService *   whatDevice ) override;
    
public:
    ItlHalService *fHalService;
    IOPCIDevice *pciNub;
#if defined(IO80211FAMILY_V2) && __IO80211_TARGET >= __MAC_15_0
    bool ownsNativeWorkLoop = false;
    SequoiaPCIOwner<IOPCIDevice, IOPCIEDeviceWrapper> nativePCI;
    // Publishing this node is what lets the NetworkController personality match,
    // and matching runs AirportItlwm::start -> startNative on the IOKit
    // configuration thread. IOResourceMatch=IOBSD on that personality is
    // satisfied at BSD init, i.e. before the storage stack finishes starting, so
    // publishing here puts the whole native lifecycle into the same early window
    // that still has to bring up NVMe/AHCI - and two boots died there while the
    // split candidate that never matched a controller booted normally. The
    // publish is therefore deferred until the boot has settled; see the
    // deferred-publish block in IOPCIEDeviceWrapper.cpp for the policies.
    bool startDeferredPublish();
    void stopDeferredPublish();
    static void publishAction(thread_call_param_t param0, thread_call_param_t param1);
    SequoiaThreadCall *publishCall = nullptr;
    volatile bool publishCancelled = false;
#endif
};

#endif /* IOPCIEDeviceWrapper_hpp */
