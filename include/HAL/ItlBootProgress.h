#ifndef ITL_BOOT_PROGRESS_H
#define ITL_BOOT_PROGRESS_H

// Diagnostic samples only. Static string literals are required. Recording
// never allocates, waits, accesses hardware, or enters the console. Concurrent
// snapshots are best effort, not an exact instruction/retirement trace.
struct ItlBootProgress {
    static_assert(__atomic_always_lock_free(sizeof(unsigned), nullptr), "Counter must not use a runtime lock");
    static_assert(__atomic_always_lock_free(sizeof(const char *), nullptr), "Samples must not use a runtime lock");
    static constexpr unsigned capacity = 128;
    enum IwxDetail {
        Command, CommandIndex, Submitted, DoneCommand, DoneIndex, Completed,
        RxCode, RxQidIndex, RxCount, RxRejected, InterruptError, ReplyError,
        PnvmSeen, PnvmStatus, InitComplete, WaitRxCursor, WaitQueued,
        CompletionRbid, CompletionSlot, CompletionMismatch,
        PnvmRawBytes, PnvmBytes, PnvmVersion, PnvmReadResult,
        PnvmSku0, PnvmSku1, PnvmSku2, PnvmMac, PnvmRf,
        LmacReadError, LmacValid, LmacError, LmacData1, LmacData2, LmacData3, LmacCommand, UmacReadError, UmacValid, UmacError, UmacData1, UmacData2, UmacData3, UmacCommand, ScanWireVersion, ScanIterations, ScanIterationStatus, ScanIterationChannels, ScanIterationBytes, ScanIterationRejected, ScanTerminalStatus, ScanAdmission, DetailCount
    };
    unsigned iwx[DetailCount] = {};
    static const char *iwxName(unsigned index) {
        static const char *const names[] = {
            "Command", "CommandIndex", "Submitted", "DoneCommand", "DoneIndex", "Completed",
            "RxCode", "RxQidIndex", "RxCount", "RxRejected", "InterruptError", "ReplyError",
            "PnvmSeen", "PnvmStatus", "InitComplete", "WaitRxCursor", "WaitQueued",
            "CompletionRbid", "CompletionSlot", "CompletionMismatch",
            "PnvmRawBytes", "PnvmBytes", "PnvmVersion", "PnvmReadResult",
            "PnvmSku0", "PnvmSku1", "PnvmSku2", "PnvmMac", "PnvmRf",
            "LmacReadError", "LmacValid", "LmacError", "LmacData1", "LmacData2", "LmacData3", "LmacCommand", "UmacReadError", "UmacValid", "UmacError", "UmacData1", "UmacData2", "UmacData3", "UmacCommand",
            "ScanWireVersion", "ScanIterations", "ScanIterationStatus", "ScanIterationChannels", "ScanIterationBytes", "ScanIterationRejected", "ScanTerminalStatus", "ScanAdmission"
        };
        static_assert(sizeof(names) / sizeof(names[0]) == DetailCount, "Diagnostic names drifted");
        return index < DetailCount ? names[index] : "Unknown";
    }
    void iwxRecord(IwxDetail key, unsigned value) {
        __atomic_store_n(&iwx[key], value, __ATOMIC_RELAXED);
    }
    void iwxBump(IwxDetail key) { __atomic_fetch_add(&iwx[key], 1U, __ATOMIC_RELAXED); }
    unsigned iwxValue(IwxDetail key) const { return __atomic_load_n(&iwx[key], __ATOMIC_RELAXED); }
    unsigned events = 0;
    int result = 0;
    const char *last = "none";
    const char *recent[capacity] = {};

    void record(const char *label) {
        const unsigned index = __atomic_fetch_add(&events, 1U, __ATOMIC_RELAXED);
        __atomic_store_n(&recent[index % capacity], label, __ATOMIC_RELAXED);
        __atomic_store_n(&last, label, __ATOMIC_RELAXED);
    }
    unsigned count() const { return __atomic_load_n(&events, __ATOMIC_RELAXED); }
    void recordResult(int value) { __atomic_store_n(&result, value, __ATOMIC_RELAXED); }
    int lastResult() const { return __atomic_load_n(&result, __ATOMIC_RELAXED); }
    const char *current() const { return __atomic_load_n(&last, __ATOMIC_RELAXED); }
    const char *sample(unsigned index) const {
        return __atomic_load_n(&recent[index % capacity], __ATOMIC_RELAXED);
    }
};
#endif
