#ifndef ITL_NATIVE_HAL_HPP
#define ITL_NATIVE_HAL_HPP

#include <stdint.h>

#include <HAL/ItlJoinEvent.h>
#include <HAL/ItlNativeScan.h>
#include <Airport/SequoiaScanPlan.h>
#include <Airport/SequoiaNativeJoin.h>
#include <Airport/SequoiaJoinCandidate.h>

struct ieee80211com;
struct ieee80211_node;
struct ieee80211_key;
class IONetworkInterface;

// Common Sequoia WCL-facing HAL contract.  The Airport layer must not depend
// on a particular Intel generation (IWM versus IWX); each HAL owns the
// transport and state representation behind this small interface.
class ItlNativeHal {
public:
    struct VhtRate {
        uint32_t index, nss, bandwidthMHz, guardIntervalNs;
    };
    virtual ~ItlNativeHal() {}
    // Workloop-owned; copied by the join worker before teardown can change it.
    SequoiaWCL::JoinCandidateDiagnostic nativeCandidateDiagnostic;
    ItlScan::CacheDiagnostic nativeCacheDiagnostic;
    // Probe build: where the native scan request construction gave up and
    // which channels it was asked for.
    uint32_t nativeScanFailStage = 0;
    uint32_t nativeScanFailChannel = 0;
    uint32_t nativeScanChannelCount = 0;
    uint16_t nativeScanChannelList[64] = {};
    uint32_t nativeScanWireCount = 0, nativeScanWireCapacity = 0;
    uint32_t nativeScanTxAnt = 0, nativeScanRxAnt = 0;
    uint32_t nativeScanSourceValid = 0;
    // The channels the request was actually built from.
    uint32_t nativeScanUsableCount = 0;
    uint16_t nativeScanUsableList[64] = {};
    // Why the command submission was refused, if it was.
    uint32_t nativeScanSubmitReject = 0;
    // Which request flavor the last scan used: 0 = the LMAC request this
    // tree builds, 1 = the UMAC request the firmware asks for.
    uint32_t nativeScanUmac = 0;

    // IWX UMAC supports scanning without resetting the connected firmware.
    virtual bool nativeAssociatedScanSupported() const { return false; }
    virtual int nativeScan(const SequoiaWCL::ScanPlan &, const ItlScan::NativeSink &) = 0;
    // The legacy child is optional: Skywalk owns its BSD ifnet via Apple's
    // client. Radio initialization uses the HAL net80211 state in either mode.
    virtual IOReturn nativeEnable(IONetworkInterface *) = 0;
    virtual IOReturn nativeDisable(IONetworkInterface *) = 0;
    virtual void cancelNativeScan() = 0;
    virtual int nativeJoinStart(const SequoiaWCL::JoinRequestSnapshot &, size_t) = 0;
    virtual int nativeLeave(bool, uint16_t, unsigned) = 0;
    virtual int nativeWatchdog(uint64_t) = 0;
    virtual void nativeJoinEvent(const ItlJoin::Event &) = 0;
    virtual bool nativePortReady() const = 0;
    // Latest firmware TX rate, not the peer's negotiated maximum. Gate-owned.
    virtual bool nativeVhtRate(VhtRate &) const { return false; }
    virtual bool nativeTxNss(uint32_t &) const { return false; }
    virtual bool nativeMcsIndex(uint32_t &) const { return false; }
    virtual bool nativeTxRateMbps(uint32_t &) const { return false; }
    virtual void nativeClearKeys() = 0;

    virtual bool nativeDetachedState() const = 0;
    virtual bool nativeCommandTransportReady() const = 0;
    virtual int nativeStateErrorCode() const = 0;
    virtual uint64_t nativeGeneration() const = 0;
    virtual const ItlJoin::Progress &nativeJoinProgressState() const = 0;
    virtual struct ieee80211com *nativeController() const = 0;
};

#endif
