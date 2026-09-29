#ifndef SEQUOIA_PACKET_IO_H
#define SEQUOIA_PACKET_IO_H

#include <stddef.h>
#include <stdint.h>
#include <Airport/SequoiaPacketDiagnostics.h>

// Include after the real (kernel) or test Skywalk declarations. The caller
// owns registration/queue lifetime, serializes the workloop, and never sleeps
// in the copy/enqueue callback. No Skywalk memory may escape that callback.
namespace SequoiaPacketIO {

constexpr size_t kEthernetHeader = 14;
constexpr size_t kMaxEthernetFrame = 1518;
constexpr size_t kPoolBufferSize = 4096;
constexpr UInt kAllocateNoSleep = 1; // 24G830 selects kern_pbufpool_alloc_nosleep.
enum class Submission { Accepted, Dropped, Retry };

inline bool transmitView(IOSkywalkPacket *packet, const uint8_t *&bytes, size_t &length)
{
    bytes = nullptr;
    length = 0;
    if (!packet || packet->getPacketBufferCount() != 1) return false;
    const auto capacity = packet->getBufferSize();
    const auto offset = packet->getDataOff();
    const auto size = packet->getDataLength();
    if (!capacity || capacity > kPoolBufferSize || offset < 0 ||
        static_cast<uint64_t>(offset) > capacity || size < kEthernetHeader ||
        size > kMaxEthernetFrame || size > capacity - static_cast<size_t>(offset)) return false;
    const auto *base = static_cast<const uint8_t *>(packet->getDataVirtualAddress());
    if (!base) return false;
    // getDataVirtualAddress returns the buffer base, without its data offset.
    bytes = base + offset;
    length = size;
    return true;
}

template <typename CopyAndEnqueue>
UInt transmit(IOSkywalkPacketBufferPool *pool, IOSkywalkPacket *const *packets,
              UInt count, CopyAndEnqueue copyAndEnqueue)
{
    if (!pool || !packets) return 0;
    UInt consumed = 0;
    for (; consumed < count; ++consumed) {
        auto *packet = packets[consumed];
        const uint8_t *bytes;
        size_t length;
        const auto result = transmitView(packet, bytes, length)
            ? copyAndEnqueue(bytes, length) : Submission::Dropped;
        if (result == Submission::Retry) break;
        // deallocatePacket completes a prepared TX packet AND returns its
        // source-queue credit. Calling complete() first loses that credit.
        // legacyDequeue advances only by our return count; it does not free.
        pool->IOSkywalkPacketBufferPool::deallocatePacket(packet);
    }
    return consumed;
}

template <typename CopyBytes>
IOReturn receive(IOSkywalkPacketBufferPool *pool, IOSkywalkRxCompletionQueue *queue,
                 size_t length, CopyBytes copyBytes, ReceiveDiagnostics *diagnostics = nullptr)
{
    ReceiveDiagnostics local;
    auto &d = diagnostics ? *diagnostics : local;
    if (d.attempts != UINT32_MAX) ++d.attempts;
    d.stage = 1; d.buffers = d.capacity = 0;
    auto finish = [&](IOReturn result) {
        d.result = static_cast<uint32_t>(result);
        if (result == kIOReturnSuccess) {
            if (d.delivered != UINT32_MAX) ++d.delivered;
        } else {
            if (d.failed != UINT32_MAX) ++d.failed;
            d.failureStage = d.stage; d.failureResult = d.result;
        }
        return result;
    };
    if (!pool || !queue || length < kEthernetHeader || length > kMaxEthernetFrame)
        return finish(kIOReturnBadArgument);
    IOSkywalkPacket *packet = nullptr;
    d.stage = 2;
    IOReturn error = pool->IOSkywalkPacketBufferPool::allocatePacket(1, &packet, kAllocateNoSleep);
    if (error != kIOReturnSuccess) return finish(error);
    if (!packet) return finish(kIOReturnNoMemory);
    d.stage = 3;
    // This is a locally allocated copy, not a packet borrowed from a submission
    // queue. The completion queue is its DESTINATION, never its source.
    // 24G830 prepare stores this argument as getSourceQueue(); completion and
    // pool deallocation notify that source. RxCompletionQueue inherits the
    // panic-only default packetCompletion, on both success and error paths.
    error = packet->prepare(nullptr, 0, 0);
    if (error == kIOReturnSuccess) {
        d.stage = 4;
        d.buffers = packet->getPacketBufferCount(); d.capacity = packet->getBufferSize();
        if (d.buffers != 1 || d.capacity < length || d.capacity > kPoolBufferSize)
            error = kIOReturnBadArgument;
        else {
            d.stage = 5;
            error = packet->setDataOffsetAndLength(0, static_cast<UInt>(length));
        }
    }
    if (error == kIOReturnSuccess) {
        d.stage = 6;
        auto *bytes = static_cast<uint8_t *>(packet->getDataVirtualAddress());
        if (!bytes || !copyBytes(bytes, length)) error = kIOReturnError;
    }
    if (error == kIOReturnSuccess) {
        d.stage = 7;
        error = queue->enqueuePackets(&packet, 1, 0);
    }
    // enqueuePackets's error paths consume nothing. On success the networking
    // path finalizes/disposes the wrapper and hands off the kernel packet.
    if (error != kIOReturnSuccess)
        pool->IOSkywalkPacketBufferPool::deallocatePacket(packet);
    return finish(error);
}

} // namespace SequoiaPacketIO
#endif
