#ifndef SEQUOIA_SKYWALK_QUEUES_H
#define SEQUOIA_SKYWALK_QUEUES_H

#include <IOKit/IOEventSource.h>
#include <Airport/IOSkywalkPacketBufferPool.h>

class IOSkywalkPacket;

// Factory-only views of 24G830 objects. Only the audited IOEventSource base
// operations and static factories may be called through these declarations.
// No instance layout, derived virtual slots, allocation or subclassing API is
// exposed. The system factory, not this kext, constructs each complete object.
class IOSkywalkPacketQueue : public IOEventSource {
    IOSkywalkPacketQueue() = delete;
};

class IOSkywalkTxSubmissionQueue : public IOSkywalkPacketQueue {
    IOSkywalkTxSubmissionQueue() = delete;
public:
    using Action = UInt (*)(OSObject *, IOSkywalkTxSubmissionQueue *,
                            IOSkywalkPacket *const *, UInt, void *);
    static IOSkywalkTxSubmissionQueue *withPool(IOSkywalkPacketBufferPool *, UInt, UInt,
                                               OSObject *, Action, void *, UInt);
    IOReturn packetSubmission(bool);
};

class IOSkywalkRxCompletionQueue : public IOSkywalkPacketQueue {
    IOSkywalkRxCompletionQueue() = delete;
public:
    using Action = UInt (*)(OSObject *, IOSkywalkRxCompletionQueue *,
                            IOSkywalkPacket **, UInt, void *);
    static IOSkywalkRxCompletionQueue *withPool(IOSkywalkPacketBufferPool *, UInt, UInt,
                                               OSObject *, Action, void *, UInt);
    // Direct symbol call only. This view does not declare a derived vtable.
    IOReturn enqueuePackets(IOSkywalkPacket *const *, UInt, UInt);
};

#endif
