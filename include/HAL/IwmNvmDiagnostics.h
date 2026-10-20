#ifndef IWM_NVM_DIAGNOSTICS_H
#define IWM_NVM_DIAGNOSTICS_H

#include <stdint.h>
#include <stddef.h>

// Bounded host-side observations only. Mutate and snapshot under the HAL gate;
// retain through detach, with no firmware buffers, addresses or NVM contents.
struct IwmNvmDiagnostics {
    const char *phase = "attach-resources";
    const char *nvmCheck = "not-attempted";
    uint32_t aliveBytes = 0, aliveStatus = UINT32_MAX;
    uint32_t lastCommand = UINT32_MAX, ackResult = UINT32_MAX;
    uint32_t ackFlags = UINT32_MAX, ackBytes = 0;
    uint32_t section = UINT32_MAX, offset = 0, requested = 0;
    uint32_t responseStatus = UINT32_MAX, responseOffset = UINT32_MAX;
    uint32_t responseType = UINT32_MAX, responseLength = UINT32_MAX, payloadBytes = 0;
    uint32_t nvmError = 0, bytesRead = 0, sectionsRead = 0;
    // Probe build: the channel count as it stood right after the NVM parse,
    // so a zero count can be attributed to the parse rather than to whatever
    // cleared the map afterwards.
    uint32_t channelsAfterParse = UINT32_MAX;
    uint32_t commandQueue = UINT32_MAX, commandIndex = UINT32_MAX, commandBytes = 0;
    uint32_t rxPackets = 0, rxCommand = UINT32_MAX, rxSequence = UINT32_MAX;
    uint32_t ackSeen = 0, ackRejected = 0, rawAckCommand = UINT32_MAX, rawAckSequence = UINT32_MAX;
    uint32_t firmwareErrors = 0, firmwareErrorType = UINT32_MAX;
    uint32_t firmwareErrorCommand = UINT32_MAX, firmwareErrorSequence = UINT32_MAX;
    uint32_t timeoutFlags = UINT32_MAX, timeoutQueued = UINT32_MAX;

    // Probe build: the section id space is not the same across firmware
    // generations. Record what the firmware answered for every requested
    // section so the mapping can be recovered without guessing. Only the
    // identifiers and the returned lengths are kept, never section contents.
    uint16_t sectionTypeMap[16] = {};
    uint16_t sectionLengthMap[16] = {};
    uint32_t sectionTypeMapValid = 0;

    // Probe build: set only by the NVM trial, so the diagnostic run can
    // accept a firmware that answers with its own section id while the
    // production path keeps the strict "same id" rule.
    bool allowSectionTypeMismatch = false;

    void recordSectionType(uint16_t requested, uint16_t returned, uint16_t length)
    {
        if (requested >= 16) return;
        sectionTypeMap[requested] = returned;
        sectionLengthMap[requested] = length;
        sectionTypeMapValid |= uint32_t(1) << requested;
    }

    static void increment(uint32_t &value) { if (value != UINT32_MAX) ++value; }

    void beginCommand(uint32_t command)
    {
        lastCommand = command;
        ackResult = ackFlags = UINT32_MAX;
        ackBytes = 0;
        commandQueue = commandIndex = UINT32_MAX;
        commandBytes = rxPackets = ackSeen = ackRejected = firmwareErrors = 0;
        rxCommand = rxSequence = rawAckCommand = rawAckSequence = UINT32_MAX;
        firmwareErrorType = firmwareErrorCommand = firmwareErrorSequence = UINT32_MAX;
        timeoutFlags = timeoutQueued = UINT32_MAX;
    }

    void recordRx(uint16_t command, uint16_t sequence, const uint8_t *data, size_t length)
    {
        increment(rxPackets);
        rxCommand = command;
        rxSequence = sequence;
        // REPLY_ERROR's fixed prefix; never retain payloads, TSF or addresses.
        if (command == 2) {
            increment(firmwareErrors);
            if (data && length >= 12) {
                firmwareErrorType = uint32_t(data[0]) | (uint32_t(data[1]) << 8) |
                    (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
                firmwareErrorCommand = data[4];
                firmwareErrorSequence = uint32_t(data[6]) | (uint32_t(data[7]) << 8);
            }
        }
    }

    void recordAck(uint16_t command, uint16_t sequence, bool accepted)
    {
        increment(ackSeen);
        if (!accepted) increment(ackRejected);
        rawAckCommand = command;
        rawAckSequence = sequence;
    }

    void recordAlive(const uint8_t *data, uint32_t length)
    {
        aliveBytes = length;
        aliveStatus = data && length >= 2 ? uint32_t(data[0]) | (uint32_t(data[1]) << 8) : UINT32_MAX;
    }

    void beginRead(uint16_t type, uint16_t start, uint16_t length)
    {
        section = type;
        offset = start;
        requested = length;
        responseStatus = responseOffset = responseType = responseLength = UINT32_MAX;
        payloadBytes = nvmError = 0;
        nvmCheck = "transport";
    }
};

#endif
