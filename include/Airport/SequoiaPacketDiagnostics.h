#ifndef SEQUOIA_PACKET_DIAGNOSTICS_H
#define SEQUOIA_PACKET_DIAGNOSTICS_H
#include <stdint.h>
namespace SequoiaPacketIO {
struct ReceiveDiagnostics {
    uint32_t attempts = 0, delivered = 0, failed = 0;
    uint32_t stage = 0, result = 0, failureStage = 0, failureResult = 0;
    uint32_t buffers = 0, capacity = 0;
};
}
#endif
