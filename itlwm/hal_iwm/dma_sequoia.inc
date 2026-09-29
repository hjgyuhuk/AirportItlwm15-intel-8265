bool allocDmaMemory2(struct iwm_dma_info *dma, size_t size, int alignment)
{
    constexpr uint64_t addressLimit = 1ULL << 36;
    if (!dma || dma->buffer || dma->cmd || !size || size >= addressLimit ||
        alignment <= 0 || (alignment & (alignment - 1)))
        return false;

    auto *buffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapInhibitCache,
        size, (addressLimit - 1) & ~static_cast<uint64_t>(alignment - 1));
    if (!buffer) return false;
    if (buffer->prepare() != kIOReturnSuccess) {
        buffer->release();
        return false;
    }
    auto *command = IODMACommand::withSpecification(kIODMACommandOutputHost64,
        36, 0, IODMACommand::kMapped, 0, alignment);
    IODMACommand::Segment64 segment = {};
    UInt64 offset = 0;
    UInt32 count = 1;
    const bool valid = command && buffer->getBytesNoCopy() &&
        command->setMemoryDescriptor(buffer) == kIOReturnSuccess &&
        command->gen64IOVMSegments(&offset, &segment, &count) == kIOReturnSuccess &&
        count == 1 && offset == size && segment.fLength == size &&
        !(segment.fIOVMAddr & (alignment - 1)) &&
        segment.fIOVMAddr < addressLimit && size <= addressLimit - segment.fIOVMAddr;
    if (!valid) {
        if (command) {
            command->clearMemoryDescriptor();
            command->release();
        }
        buffer->complete();
        buffer->release();
        return false;
    }
    dma->paddr = segment.fIOVMAddr;
    dma->vaddr = buffer->getBytesNoCopy();
    dma->size = size;
    dma->buffer = buffer;
    dma->cmd = command;
    memset(dma->vaddr, 0, size);
    return true;
}

void ItlIwm::iwm_dma_contig_free(struct iwm_dma_info *dma)
{
    if (!dma) return;
    if (dma->cmd) {
        dma->cmd->clearMemoryDescriptor();
        dma->cmd->release();
    }
    if (dma->buffer) {
        dma->buffer->complete();
        dma->buffer->release();
    }
    memset(dma, 0, sizeof(*dma));
}
