#ifndef SEQUOIA_SCAN_DELIVERY_H
#define SEQUOIA_SCAN_DELIVERY_H

#include <Airport/SequoiaScanFrame.h>

namespace SequoiaWCL {

// Staged native delivery, not enabled by the counter-only firmware trial.
// The owner preallocates storage and serializes every call, including posting,
// on the IO80211 workqueue. This class is neither a lock nor an RX drain barrier.
class ScanDelivery {
public:
    static constexpr size_t MaxSlots = 128;
    struct Slot { size_t size = 0; uint8_t bytes[kMaxScanBeaconMessage] = {}; };
    enum class Enqueue { Queued, Invalid, Closed, Full };
    enum class Delivery { Idle, Retry, Result, Complete, NeedsStatusMapping, NotReady };

    ScanDelivery(Slot *storage = nullptr, size_t capacity = 0) : slots_(storage), capacity_(capacity) {}
    ScanDelivery(const ScanDelivery &) = delete;
    ScanDelivery &operator=(const ScanDelivery &) = delete;

    bool setStorage(Slot *storage, size_t capacity)
    {
        if (posting_ || lifecycle_.busy() || count_ || !storage || !capacity || capacity > MaxSlots)
            return false;
        slots_ = storage;
        capacity_ = capacity;
        clear();
        return true;
    }

    uint64_t begin(ItlScan::Firmware firmware, uint32_t uid = 0)
    {
        if (posting_ || !slots_ || !capacity_ || capacity_ > MaxSlots) return 0;
        const auto token = lifecycle_.begin(firmware, uid);
        if (!token) return 0;
        clear();
        lastCompletionStatus_ = 0;
        accepted_ = closed_ = overflow_ = withheld_ = false;
        return token;
    }

    bool submitted(uint64_t token, ItlScan::Submission result)
    {
        if (posting_ || !lifecycle_.submitted(token, result)) return false;
        accepted_ = result == ItlScan::Submission::Accepted;
        if (!accepted_) { closed_ = true; clear(); }
        return true;
    }

    Enqueue queueBeacon(uint64_t token, const ItlScan::LegacyPhy &phy, const void *frame, size_t size)
    {
        if (posting_ || !token || token != lifecycle_.token() || closed_ || lifecycle_.drained())
            return Enqueue::Closed;
        if (count_ == capacity_) {
            overflow_ = true;
            closed_ = true;
            return Enqueue::Full;
        }
        auto &slot = slots_[(head_ + count_) % capacity_];
        const auto encoded = encodeIwmBeacon(phy, frame, size, slot.bytes, sizeof(slot.bytes));
        if (!encoded) return Enqueue::Invalid;
        slot.size = encoded;
        ++count_;
        return Enqueue::Queued;
    }

    bool terminal(ItlScan::Firmware firmware, uint32_t uid, ItlScan::Outcome outcome)
    {
        if (posting_ || !lifecycle_.terminal(firmware, uid, outcome)) return false;
        closed_ = true;
        return true;
    }

    bool cancel(uint64_t token)
    {
        if (posting_ || !lifecycle_.cancel(token)) return false;
        closed_ = true;
        return true;
    }

    bool timeout(uint64_t token)
    {
        if (posting_ || !lifecycle_.timeout(token)) return false;
        closed_ = true;
        return true;
    }

    bool beginAbort(uint64_t token) { return !posting_ && lifecycle_.beginAbort(token); }
    bool transportFailed(uint64_t token)
    {
        if (posting_ || !lifecycle_.transportFailed(token)) return false;
        closed_ = true;
        return true;
    }
    bool aborted(uint64_t token, bool accepted) { return !posting_ && lifecycle_.aborted(token, accepted); }

    // Only after the owning HAL has stopped hardware AND drained RX/workers.
    bool hardwareStopped()
    {
        if (posting_) return false;
        lifecycle_.hardwareStopped();
        closed_ = true;
        return true;
    }

    // Associated-scan health gate: after a successful terminal, drop the
    // still-queued beacons and force the completion through the failure-mapping
    // path. A failed completion makes the WCL scan FSM discard this scan's
    // generation instead of committing a result set that only saw the
    // associated AP - r213 committed exactly such a set and it replaced the
    // cached neighbour list, emptying the menu's "other networks". Only valid
    // while the lifecycle is busy AND drained (terminal arrived, completion not
    // yet acknowledged) and outside a post, which is precisely the window
    // between the synchronous HAL scan returning and the first delivery pass.
    bool withhold()
    {
        if (posting_ || !lifecycle_.busy() || !lifecycle_.drained()) return false;
        clear();
        closed_ = true;
        withheld_ = true;
        return true;
    }

    // The recipient has detached: discard without claiming delivery. Pending
    // commands and undrained hardware still prohibit releasing result storage.
    bool discardForDetach()
    {
        if (posting_ || !slots_ || !capacity_ || capacity_ > MaxSlots || !lifecycle_.drained()) return false;
        if (lifecycle_.busy()) {
            ItlScan::Completion pending;
            if (!lifecycle_.completion(pending) || !lifecycle_.acknowledge(pending.token)) return false;
        }
        clear();
        accepted_ = false;
        closed_ = true;
        return true;
    }

    bool releaseStorage()
    {
        if (!discardForDetach()) return false;
        slots_ = nullptr;
        capacity_ = 0;
        return true;
    }

    // At most one event per invocation. post must synchronously COPY the bytes
    // and return true only on enqueue success, as audited controller postMessage
    // does. It must not retain this pointer or throw. A scheduler owns retries.
    // Failure mapping is deliberately supplied by the native adapter: Intel
    // errno/firmware status must not be passed off as a Broadcom WCL status.
    template <typename Post, typename FailureStatus>
    Delivery deliverOne(Post post, FailureStatus mapFailure)
    {
        if (posting_ || !lifecycle_.busy()) return Delivery::Idle;
        if (accepted_ && count_) {
            auto &slot = slots_[head_];
            posting_ = true;
            const bool sent = post(kScanResultMessage, slot.bytes, slot.size);
            posting_ = false;
            if (!sent) return Delivery::Retry;
            memset(&slot, 0, sizeof(slot));
            head_ = (head_ + 1) % capacity_;
            --count_;
            return Delivery::Result;
        }
        ItlScan::Completion completion;
        if (!lifecycle_.completion(completion)) return Delivery::Idle;
        uint32_t status = 0;
        if (!completion.accepted || completion.outcome != ItlScan::Outcome::Success ||
            overflow_ || withheld_) {
            posting_ = true;
            const bool mapped = mapFailure(completion, overflow_, status);
            posting_ = false;
            if (!mapped || !status)
                return Delivery::NeedsStatusMapping;
        }
        posting_ = true;
        const bool sent = post(kScanCompleteMessage, &status, sizeof(status));
        posting_ = false;
        if (!sent) return Delivery::Retry;
        lastCompletionStatus_ = status;
        lifecycle_.acknowledge(completion.token);
        return Delivery::Complete;
    }

    bool busy() const { return lifecycle_.busy(); }
    bool drained() const { return lifecycle_.drained(); }
    bool withheld() const { return withheld_; }
    // Read-only completion probe: true while an outcome is known but the
    // completion message has not been posted yet.
    bool completion(ItlScan::Completion &output) const { return lifecycle_.completion(output); }
    size_t pending() const { return count_; }
    bool overflowed() const { return overflow_; }
    uint32_t lastCompletionStatus() const { return lastCompletionStatus_; }

private:
    ItlScan::Lifecycle lifecycle_;
    Slot *slots_;
    size_t capacity_, head_ = 0, count_ = 0;
    bool accepted_ = false, closed_ = true, overflow_ = false, withheld_ = false, posting_ = false;
    uint32_t lastCompletionStatus_ = 0;

    void clear()
    {
        // Storage belongs to this session until every queued payload is posted.
        memset(slots_, 0, capacity_ * sizeof(*slots_));
        head_ = count_ = 0;
    }
};

// The interface's own postMessage, not the controller's. 24G830's
// IO80211SkywalkInterface::postMessageInternal is
//
//     xorb $0x1, %r8b             ; r8b = !parsed
//     movq 0x110(%rdi), %rax      ; this->ivars
//     movq 0xd8(%rax), %rax       ; ivars+0xd8 -> the glue
//     testq %rax,%rax ; sete %r9b ; orb %r8b,%r9b
//     jne  <postMessageSync>      ; !parsed or no glue -> the synchronous path
//     movq %rax, %rdi
//     jmp  IO80211Glue::routeEventToWcl(kind, buf, size)
//
// so the route to the WCL is a direct field load on the interface object, and
// parsed=true is what selects it. IO80211Controller::postMessage, which this
// delivery used while the port recorded the interface's as "void-returning" and
// therefore unusable, instead loads *(controller->ivars+0x120)+0xb08 and jumps
// through that object's vtable slot 0x120 - an indirection that returned zero on
// every boot from r158 to r160 while the family's WCL never logged a single
// scan-result handler and the scan FSM never left SCAN_MANAGER_STATE_IN_PROGRESS,
// because it never received SCAN_MANAGER_EVENT_SCAN_RESULT. The family's own glue
// query path reads the same field (0x110(iface) -> 0xd8), which is what fixes the
// offset.
// Controller must be the retained native IO80211Controller for the workqueue
// check. Every producer must use this same workqueue.
// The kernel compile fixture instantiates this against the actual declarations.
// (r159 bound this call to the mangled name of IO80211Controller::postMessage on
// the theory that the port's class layout put it in the wrong slot. The r159 boot
// was indistinguishable from r158's - same EBUSY, same empty list - so the member
// call was already resolving to the family's implementation and the binding was
// reverted.)
extern "C" long itlwm_controller_post_message(void *controller, void *interface, uint32_t kind,
                                              void *bytes, unsigned long size, bool parsed)
    __asm("__ZN17IO80211Controller11postMessageEP23IO80211SkywalkInterfacejPvmb");
template <typename Controller, typename Interface, typename FailureStatus>
ScanDelivery::Delivery postScanDeliveryOnWorkQueue(Controller *controller, Interface *interface,
                                                  ScanDelivery &delivery, FailureStatus mapFailure)
{
    if (!controller || !interface) return ScanDelivery::Delivery::NotReady;
    auto *workqueue = controller->getWorkQueue();
    if (!workqueue || !workqueue->inGate()) return ScanDelivery::Delivery::NotReady;
    return delivery.deliverOne([&](uint32_t kind, const void *bytes, size_t size) {
        // AppleBCMWLANScanAdapter passes true for parsed 0xce results and
        // 0xed scan completion. Keep the same dispatch mode for WCL delivery.
        return controller->postMessage(interface, kind, const_cast<void *>(bytes), size, true) == 0;
    }, mapFailure);
}

} // namespace SequoiaWCL
#endif
