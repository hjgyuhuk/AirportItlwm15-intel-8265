#ifndef IWM_COMMAND_SLOTS_H
#define IWM_COMMAND_SLOTS_H

#include <stddef.h>
#include <stdint.h>

namespace IwmCommand {

enum class State : uint8_t { Free, Reserved, Pending, Complete, Abandoned };
enum class Ack : uint8_t { Ignored, Release, Wake };

struct Token {
    uint64_t serial;
    size_t index;
};

// The caller holds the HAL workloop gate. A host token is not a firmware UID:
// reuse after an ACK relies on ordered RX consumption, and reset requires DMA
// stopped plus the old RX ring drained. An undrained timeout never frees a slot.
template <size_t Count>
class Slots {
    struct Slot {
        uint64_t serial;
        uint16_t command;
        State state;
        bool synchronous;
        int error;
    } slots_[Count] = {};
    uint64_t issued_ = 0;

    Slot *find(Token token) {
        return token.serial && token.index < Count &&
            slots_[token.index].serial == token.serial ? &slots_[token.index] : nullptr;
    }

public:
    bool reserve(size_t index, uint16_t command, bool synchronous, Token &token) {
        if (index >= Count || slots_[index].state != State::Free || issued_ == UINT64_MAX)
            return false;
        auto &slot = slots_[index];
        slot = {++issued_, command, State::Reserved, synchronous, 0};
        token = {slot.serial, index};
        return true;
    }

    bool publish(Token token) {
        auto *slot = find(token);
        if (!slot || slot->state != State::Reserved)
            return false;
        slot->state = State::Pending;
        return true;
    }

    bool cancel(Token token) {
        auto *slot = find(token);
        if (!slot || slot->state != State::Reserved)
            return false;
        slot->state = State::Free;
        return true;
    }

    bool accepts(size_t index, uint16_t command) const {
        if (index >= Count)
            return false;
        const auto &slot = slots_[index];
        return slot.command == command &&
            (slot.state == State::Pending || slot.state == State::Abandoned);
    }

    Ack acknowledge(size_t index, uint16_t command, int error) {
        if (!accepts(index, command))
            return Ack::Ignored;
        auto &slot = slots_[index];
        if (slot.state == State::Abandoned || !slot.synchronous) {
            slot.state = State::Free;
            return Ack::Release;
        }
        slot.error = error;
        slot.state = State::Complete;
        return Ack::Wake;
    }

    State state(Token token) const {
        if (!token.serial || token.index >= Count || slots_[token.index].serial != token.serial)
            return State::Free;
        return slots_[token.index].state;
    }

    bool consume(Token token, int &error) {
        auto *slot = find(token);
        if (!slot || slot->state != State::Complete)
            return false;
        error = slot->error;
        slot->state = State::Free;
        return true;
    }

    bool abandon(Token token) {
        auto *slot = find(token);
        if (!slot || slot->state != State::Pending || !slot->synchronous)
            return false;
        slot->state = State::Abandoned;
        return true;
    }

    void resetAfterDrain() {
        for (auto &slot : slots_)
            slot = {};
        // Keep issued_ monotonic so a sleeping pre-reset caller cannot consume
        // a new command that happens to use the same ring index.
    }
};

} // namespace IwmCommand
#endif
