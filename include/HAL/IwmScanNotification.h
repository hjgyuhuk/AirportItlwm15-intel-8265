#ifndef IWM_SCAN_NOTIFICATION_H
#define IWM_SCAN_NOTIFICATION_H

#include <HAL/ItlScan.h>

namespace ItlScan {

enum class IwmNotification : uint16_t {
    LmacComplete = 0x6d,
    LmacIteration = 0xe7,
    UmacComplete = 0x0f,
    UmacIteration = 0xb5,
};

struct ScanNotification {
    Firmware firmware;
    bool terminal;
    uint32_t uid;
    uint8_t status;
    uint8_t channels;
};

// Kind must include the RX group (IWM_WIDE_ID), not just the opcode. These
// notifications use group zero, unlike the corresponding UMAC commands.
// Input is a bounded firmware payload, excluding the RX packet header. The
// legacy Intel notification definitions use 12/16-byte fixed headers and
// 8-byte channel results. Unknown statuses remain explicit, never success.
inline bool decodeIwmNotification(IwmNotification kind, const void *payload,
                                  size_t size, ScanNotification &output)
{
    if (!payload)
        return false;
    const auto *bytes = static_cast<const uint8_t *>(payload);
    ScanNotification decoded = {};
    size_t headerSize;
    size_t statusOffset;
    size_t channelOffset = 0;
    switch (kind) {
        case IwmNotification::LmacComplete:
            decoded.firmware = Firmware::LMAC;
            decoded.terminal = true;
            headerSize = 12;
            statusOffset = 2;
            break;
        case IwmNotification::LmacIteration:
            decoded.firmware = Firmware::LMAC;
            headerSize = 12;
            statusOffset = 1;
            break;
        case IwmNotification::UmacComplete:
            decoded.firmware = Firmware::UMAC;
            decoded.terminal = true;
            headerSize = 16;
            statusOffset = 6;
            break;
        case IwmNotification::UmacIteration:
            decoded.firmware = Firmware::UMAC;
            headerSize = 16;
            statusOffset = 5;
            channelOffset = 4;
            break;
        default:
            return false;
    }
    if (size < headerSize)
        return false;
    if (decoded.firmware == Firmware::UMAC)
        decoded.uid = uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
                      (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
    decoded.status = bytes[statusOffset];
    if (!decoded.terminal) {
        decoded.channels = bytes[channelOffset];
        if (decoded.channels > (size - headerSize) / 8)
            return false;
    }
    output = decoded;
    return true;
}

inline bool terminalOutcome(const ScanNotification &notification, Outcome &output)
{
    if (!notification.terminal)
        return false;
    output = notification.status == 1 ? Outcome::Success :
             notification.status == 2 ? Outcome::Aborted : Outcome::TransportError;
    return true;
}

} // namespace ItlScan
#endif
