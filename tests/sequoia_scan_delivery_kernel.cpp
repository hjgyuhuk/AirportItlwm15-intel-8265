#include "AirportItlwmV2.hpp"
#include <Airport/SequoiaScanDelivery.h>
#include <Airport/SequoiaSkywalkRegistration.h>
#include <Airport/SequoiaSkywalkPacket.h>
#include <Airport/SequoiaPacketIO.h>

static_assert(sizeof(IOSkywalkNetworkInterface::RegistrationInfo) == 0x108, "network registration ABI");
static_assert(sizeof(IOSkywalkEthernetInterface::RegistrationInfo) == 0x130, "ethernet registration ABI");
static_assert(__is_same(decltype(((IO80211SkywalkInterface *)nullptr)->prepareBSDInterface(nullptr, 0)),
                        IOReturn), "prepareBSDInterface must not truncate IOReturn to bool");
static_assert(__is_same(decltype(((IO80211SkywalkInterface *)nullptr)->reportLinkStatus(0, 0)),
                        IOReturn), "reportLinkStatus returns a fallible IOReturn");

IOReturn checkNativeLinkReport(IO80211SkywalkInterface *interface)
{
    return interface->reportLinkStatus(0x80000003u, 0x80);
}

IOReturn checkNativeRegistration(SequoiaSkywalkRegistration &registration,
    IO80211SkywalkInterface *interface, IOWorkLoop *loop, OSObject *owner, const uint8_t *mac,
    IOSkywalkTxSubmissionQueue::Action tx, IOSkywalkRxCompletionQueue::Action rx)
{
    return registration.prepare(interface, loop, owner, mac, tx, rx);
}

// Compile-only ABI fixture. This is not a kext entry point or a live caller.
SequoiaWCL::ScanDelivery::Delivery checkNativeScanDelivery(IO80211Controller *controller,
    IO80211SkywalkInterface *interface, SequoiaWCL::ScanDelivery &delivery)
{
    return SequoiaWCL::postScanDeliveryOnWorkQueue(controller, interface, delivery,
        [](const ItlScan::Completion &, bool, uint32_t &) { return false; });
}

IOReturn checkNativeReceive(IOSkywalkPacketBufferPool *pool, IOSkywalkRxCompletionQueue *queue,
                           const uint8_t *bytes, size_t length)
{
    return SequoiaPacketIO::receive(pool, queue, length, [&](uint8_t *output, size_t size) {
        memcpy(output, bytes, size); return true;
    });
}

UInt checkNativeTransmit(IOSkywalkPacketBufferPool *pool, IOSkywalkPacket *const *packets, UInt count)
{
    return SequoiaPacketIO::transmit(pool, packets, count, [](const uint8_t *, size_t) {
        return SequoiaPacketIO::Submission::Accepted;
    });
}
