#ifndef SEQUOIA_SKYWALK_PACKET_H
#define SEQUOIA_SKYWALK_PACKET_H

#include <IOKit/IOTypes.h>

class IOSkywalkPacketQueue;

// Opaque direct-call view of 24G830 IOSkywalkPacket. The system owns the
// IOCommand-derived object and its vtable. Never allocate, subclass, access
// ivars, or cast this view to the legacy IOSkywalkNetworkPacket declaration.
class IOSkywalkPacket {
    IOSkywalkPacket() = delete;
public:
    UInt getPacketBufferCount() const;
    UInt getBufferSize() const;
    UInt getDataLength() const;
    long long getDataOff() const;
    void *getDataVirtualAddress() const;
    IOReturn setDataOffsetAndLength(unsigned short, UInt);
    IOReturn prepare(IOSkywalkPacketQueue *, unsigned long long, UInt);
};

#endif
