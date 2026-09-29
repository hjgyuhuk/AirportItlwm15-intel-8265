#ifndef SEQUOIA_INTERFACE_LIFECYCLE_H
#define SEQUOIA_INTERFACE_LIFECYCLE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Exact 24G830, role-1 interface ownership fields. These are not registration
// substitutions: only read state or clear an owned reference before release.
namespace SequoiaInterfaceLifecycle {
constexpr size_t PeerManager = 0x28;
constexpr size_t Controller = 0x30;
constexpr size_t Role = 0x58;
// IO80211SkywalkInterface::start reads both: +0x68 is the parent interface it
// takes the peer manager from for role 2. Role 1 instead calls the zero-argument
// createPeerManager slot. +0x5c is the interface id passed to the peer manager.
constexpr size_t ParentInterface = 0x68;
constexpr size_t InterfaceId = 0x5c;
constexpr size_t LogStream = 0x78;
constexpr size_t LogPipe = 0x80;
constexpr size_t ScanManager = 0;
constexpr size_t ScanManagerIvars = 0x10;
constexpr size_t BonjourAgent = 0x1a0;
constexpr size_t ControllerIvars = 0x120;
constexpr size_t RetainedProvider = 0x360;
constexpr size_t ControllerRecursiveLock = 0x8;
constexpr size_t ControllerReportList = 0x4d8;
constexpr size_t ControllerPostOffice = 0xb08;
constexpr size_t ControllerTimerFactory = 0xb10;
constexpr size_t ControllerRNGAgent = 0xb18;
constexpr size_t PostOfficeIvars = 0x10;
constexpr size_t PostOfficeTimer = 0x18;
constexpr size_t RNGAgentIvars = 0x10;
constexpr size_t RNGAgentTimer = 0x8;
constexpr size_t InfraLinkTimer = 0x148;
constexpr size_t InfraDataTimer = 0x150;

template <typename T> inline T read(const void *storage, size_t offset)
{
    T value = {};
    if (storage) memcpy(&value, static_cast<const uint8_t *>(storage) + offset, sizeof(value));
    return value;
}

template <typename T> inline T *take(void *storage, size_t offset)
{
    T *value = read<T *>(storage, offset);
    T *empty = nullptr;
    if (storage) memcpy(static_cast<uint8_t *>(storage) + offset, &empty, sizeof(empty));
    return value;
}

inline bool controllerBaseStarted(const void *controller, const void *provider)
{
    // Controller::init clears this field; start stores/retains the provider
    // only after IOEthernetController::start returns true. stop clears it.
    const void *ivars = read<const void *>(controller, ControllerIvars);
    return provider && read<const void *>(ivars, RetainedProvider) == provider;
}

inline bool prepareFailedControllerInit(void *controller, void *reservedLock)
{
    auto *ivars = read<uint8_t *>(controller, ControllerIvars);
    if (!ivars) return false;
    bool consumed = false;
    if (!read<void *>(ivars, ControllerRecursiveLock) && reservedLock) {
        memcpy(ivars + ControllerRecursiveLock, &reservedLock, sizeof(reservedLock));
        consumed = true;
    }
    // These two fields are both zero only before the system initializes the
    // circular empty list. Never overwrite an existing list or its nodes.
    if (!read<void *>(ivars, ControllerReportList) &&
        !read<void *>(ivars, ControllerReportList + sizeof(void *))) {
        void *head = ivars + ControllerReportList;
        memcpy(ivars + ControllerReportList, &head, sizeof(head));
        memcpy(ivars + ControllerReportList + sizeof(head), &head, sizeof(head));
    }
    return consumed;
}
}

#endif
