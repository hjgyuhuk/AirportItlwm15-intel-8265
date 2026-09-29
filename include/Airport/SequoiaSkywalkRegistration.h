#ifndef SEQUOIA_SKYWALK_REGISTRATION_H
#define SEQUOIA_SKYWALK_REGISTRATION_H

#include <Airport/SequoiaSkywalkQueues.h>

// The controller owns the interface, callback target, and workloop until reset
// returns. reset requires BSD/client teardown and a quiesced data path first.
// Queue creation does not enable packet traffic or publish a BSD interface.
class SequoiaSkywalkRegistration {
public:
    using RegisterBoundary = IOReturn (*)(IOSkywalkEthernetInterface *,
                                           IOSkywalkEthernetInterface::RegistrationInfo *,
                                           IOSkywalkPacketQueue **, unsigned int,
                                           IOSkywalkPacketBufferPool *, IOSkywalkPacketBufferPool *);
    SequoiaSkywalkRegistration() = default;
    SequoiaSkywalkRegistration(const SequoiaSkywalkRegistration &) = delete;
    SequoiaSkywalkRegistration &operator=(const SequoiaSkywalkRegistration &) = delete;

    IOReturn prepare(IOSkywalkEthernetInterface *interface, IOWorkLoop *loop, OSObject *owner,
                     const uint8_t *mac, IOSkywalkTxSubmissionQueue::Action txAction,
                     IOSkywalkRxCompletionQueue::Action rxAction,
                     RegisterBoundary registerBoundary = nullptr)
    {
        if (interface_ || pool_ || tx_ || rx_) return kIOReturnBusy;
        if (!interface || !loop || !owner || !mac || !txAction) return kIOReturnBadArgument;
        bool nonzero = false;
        for (unsigned i = 0; i < 6; ++i) nonzero |= mac[i] != 0;
        if (!nonzero || (mac[0] & 1)) return kIOReturnBadArgument;

        IOSkywalkEthernetInterface::RegistrationInfo info = {};
        if (!interface->initRegistrationInfo(&info, 1, sizeof(info))) return kIOReturnBadArgument;
        // 24H23 initRegistrationInfo uses qualified base getters, so an
        // unregistered interface gets subfamily 0. The copied registration
        // feeds getInterfaceSubFamily -> ifnet_init_eparams+0x140. Declare
        // Wi-Fi (3), as Apple's APSTA interface does; keep family 2/type 6.
        // These are API input bytes, not writes into an Apple object.
        const uint32_t wifiSubfamily = 3;
        memcpy(info.pad + 0x0c, &wifiSubfamily, sizeof(wifiSubfamily));
        // Preserve the default unit -1. IOSkywalkLegacyEthernetInterface
        // obtains a unit from IONetworkStack, sets IOInterfaceUnit on our
        // interface, then republishes it for IOSkywalkNetworkBSDClient.
        // copyRegistrationInfo in 24G830 publishes six MAC bytes at +0x108.
        memcpy(info.pad + 0x108, mac, 6);
        interface_ = interface;
        loop_ = loop;
        IOSkywalkPacketBufferPool::PoolOptions options = {};
        options.packetCount = options.bufferCount = 128;
        options.bufferSize = 4096;
        options.maxBuffersPerPacket = 1;
        options.memorySegmentSize = 4096;
        // 24H23 netif requires PACKET/RAW metadata (2/2). Factory type 0
        // sets KBI_QUANTUM and creates QUANTUM/PAYLOAD metadata (1/1), which
        // nx_create rejects before BSD attachment. Type 1 is the stock BCM
        // network-packet pool; keep user-pool flags and buffer sizing intact.
        constexpr unsigned networkPacketPoolType = 1;
        pool_ = IOSkywalkPacketBufferPool::withName("AirportItlwm", owner, networkPacketPoolType, &options);
        IOReturn result = kIOReturnNoMemory;
        if (!pool_) return failed(result);
        tx_ = IOSkywalkTxSubmissionQueue::withPool(pool_, 64, 0, owner, txAction, nullptr, 0);
        if (!tx_) return failed(result);
        rx_ = IOSkywalkRxCompletionQueue::withPool(pool_, 64, 0, owner, rxAction, nullptr, 0);
        if (!rx_) return failed(result);
        result = loop_->addEventSource(tx_);
        if (result != kIOReturnSuccess) return failed(result);
        txAdded_ = true;
        result = loop_->addEventSource(rx_);
        if (result != kIOReturnSuccess) return failed(result);
        rxAdded_ = true;
        IOSkywalkPacketQueue *queues[] = {tx_, rx_};
        // The 24G830 AppleBCMWLAN path enters registerInfraEthernetInterface
        // with the interface's normal BSD publication state.  Setting
        // deferBSDAttach(true) here leaves IODeferBSDAttach=Yes permanently:
        // the audited registerInfraEthernetInterface and generic
        // registerEthernetInterface paths do not clear it.  That property is
        // later consumed by Apple80211 discovery, which then rejects an
        // otherwise valid subtype-3/media interface.  The BSD child is already
        // private because attachInterface(..., false) was used by the native
        // lifecycle, so no extra deferral is needed here.
        attempted_ = true;
        registerBoundary_ = registerBoundary;
        result = registerBoundary_
            ? registerBoundary_(interface_, &info, queues, 2, pool_, pool_)
            : interface_->registerEthernetInterface(&info, queues, 2, pool_, pool_, 0);
        if (result != kIOReturnSuccess) return failed(result);
        prepared_ = true;
        return kIOReturnSuccess;
    }

    // Factory queues start disabled. Link reporting/setRunningState do not
    // enable them. Use the audited IOEventSource virtuals, on the owner gate;
    // queue initialization/finalization remains owned by Apple's BSD glue.
    IOReturn setPacketQueuesEnabled(bool enabled)
    {
        if (enabled && (!prepared_ || !tx_ || !rx_)) return kIOReturnNotReady;
        if (enabled) {
            rx_->enable();
            tx_->enable();
            if (!rx_->isEnabled() || !tx_->isEnabled()) {
                setPacketQueuesEnabled(false);
                return kIOReturnNotReady;
            }
        } else {
            if (tx_) tx_->disable();
            if (rx_) rx_->disable();
        }
        return kIOReturnSuccess;
    }

    bool packetQueuesEnabled() const
    {
        return prepared_ && tx_ && rx_ && tx_->isEnabled() && rx_->isEnabled();
    }

    void reset()
    {
        prepared_ = false;
        setPacketQueuesEnabled(false);
        // The API can allocate registration storage before returning an error.
        if (attempted_) interface_->deregisterEthernetInterface(0);
        attempted_ = false;
        if (rxAdded_) loop_->removeEventSource(rx_);
        if (txAdded_) loop_->removeEventSource(tx_);
        rxAdded_ = txAdded_ = false;
        if (rx_) { rx_->release(); rx_ = nullptr; }
        if (tx_) { tx_->release(); tx_ = nullptr; }
        if (pool_) { pool_->release(); pool_ = nullptr; }
        interface_ = nullptr;
        loop_ = nullptr;
        registerBoundary_ = nullptr;
    }

    bool prepared() const { return prepared_; }
    IOSkywalkPacketBufferPool *pool() const { return pool_; }
    IOSkywalkTxSubmissionQueue *txQueue() const { return tx_; }
    IOSkywalkRxCompletionQueue *rxQueue() const { return rx_; }
    bool usedInfraBoundary() const { return registerBoundary_ != nullptr; }

private:
    IOSkywalkEthernetInterface *interface_ = nullptr;
    IOWorkLoop *loop_ = nullptr;
    IOSkywalkPacketBufferPool *pool_ = nullptr;
    IOSkywalkTxSubmissionQueue *tx_ = nullptr;
    IOSkywalkRxCompletionQueue *rx_ = nullptr;
    bool txAdded_ = false, rxAdded_ = false, attempted_ = false;
    bool prepared_ = false;
    RegisterBoundary registerBoundary_ = nullptr;

    IOReturn failed(IOReturn error) { reset(); return error; }
};

#endif
