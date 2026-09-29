#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using UInt64 = uint64_t;
using UInt32 = uint32_t;
constexpr int kernel_task = 0, kIODirectionInOut = 1, kIOMemoryPhysicallyContiguous = 2;
constexpr int kIOMapInhibitCache = 4, kIOReturnSuccess = 0, kIODMACommandOutputHost64 = 0;
static int buffers, commands, prepared;
static int fault;

struct IOBufferMemoryDescriptor {
    std::vector<uint8_t> bytes;
    explicit IOBufferMemoryDescriptor(size_t size) : bytes(size, 0xff) { ++buffers; }
    static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(int, int, size_t size, uint64_t)
    { return fault == 1 ? nullptr : new IOBufferMemoryDescriptor(size); }
    int prepare() { if (fault == 2) return 1; ++prepared; return 0; }
    void complete() { assert(prepared > 0); --prepared; }
    void release() { --buffers; delete this; }
    void *getBytesNoCopy() { return fault == 3 ? nullptr : bytes.data(); }
};

struct IODMACommand {
    enum { kMapped = 0 };
    struct Segment64 { uint64_t fIOVMAddr, fLength; };
    IOBufferMemoryDescriptor *buffer = nullptr;
    static IODMACommand *withSpecification(int, int bits, int, int, int, int)
    {
        assert(bits == 36);
        if (fault == 4) return nullptr;
        ++commands;
        return new IODMACommand;
    }
    int setMemoryDescriptor(IOBufferMemoryDescriptor *value)
    { buffer = value; return fault == 5 ? 1 : 0; }
    int gen64IOVMSegments(UInt64 *offset, Segment64 *segment, UInt32 *count)
    {
        segment->fIOVMAddr = fault == 10 ? (1ULL << 36) :
            (fault == 11 ? (1ULL << 36) - 2048 : (fault == 12 ? 0x20001 : 0x20000));
        segment->fLength = buffer->bytes.size() - (fault == 8 ? 1 : 0);
        *offset = buffer->bytes.size() - (fault == 9 ? 1 : 0);
        *count = fault == 7 ? 0 : 1;
        return fault == 6 ? 1 : 0;
    }
    void clearMemoryDescriptor() { buffer = nullptr; }
    void release() { assert(!buffer); --commands; delete this; }
};

struct iwm_dma_info {
    uint64_t paddr;
    void *vaddr;
    size_t size;
    IOBufferMemoryDescriptor *buffer;
    IODMACommand *cmd;
};
struct ItlIwm { void iwm_dma_contig_free(iwm_dma_info *); };
#include "../itlwm/hal_iwm/dma_sequoia.inc"

int main()
{
    ItlIwm driver;
    iwm_dma_info dma = {};
    assert(!allocDmaMemory2(nullptr, 4096, 16));
    assert(!allocDmaMemory2(&dma, 0, 16));
    assert(!allocDmaMemory2(&dma, 4096, 0));
    assert(!allocDmaMemory2(&dma, 4096, 3));
    for (fault = 1; fault <= 12; ++fault) {
        assert(!allocDmaMemory2(&dma, 4096, 16));
        assert(!dma.buffer && !dma.cmd && !dma.vaddr && !dma.size && !dma.paddr);
        assert(!buffers && !commands && !prepared);
    }
    fault = 0;
    assert(allocDmaMemory2(&dma, 4096, 4096));
    for (auto byte : dma.buffer->bytes) assert(byte == 0);
    assert(!allocDmaMemory2(&dma, 4096, 16));
    driver.iwm_dma_contig_free(&dma);
    driver.iwm_dma_contig_free(&dma);
    driver.iwm_dma_contig_free(nullptr);
    assert(!buffers && !commands && !prepared && !dma.paddr && !dma.size);
    puts("Production IWM DMA: 12 allocation/mapping failures and idempotent cleanup passed");
}
