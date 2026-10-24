#ifndef AIRPORT_ITLWM_SEQUOIA_PEER_FACTORY_H
#define AIRPORT_ITLWM_SEQUOIA_PEER_FACTORY_H

// Public surface of SequoiaPeerFactory.cpp: the ABI bridges into the private
// IO80211Family classes. The definitions carry the provenance of every fixed
// offset; this header only repeats the signatures so the .inc fragments stop
// re-declaring them at each call site (two of those ad-hoc declarations once
// drifted out of sync with the definitions).

#include <stddef.h>
#include <stdint.h>

#if __IO80211_TARGET >= __MAC_15_0

class OSObject;
class IOService;
class IOWorkLoop;

// Verdict of the one-shot ABI gate the chain resolvers run: the fixed offsets
// were audited against one kernel build, so the first resolved chain is asked
// to prove its objects really are the family classes before anything is
// posted through them. Ok and WrongClass are cached for the boot; the other
// values mean the walk has not (yet) produced a chain worth checking.
enum class WclAbiVerdict { NotChecked, Ok, WrongClass, Unresolved };

// Chain readers.
void *itlwmInterfaceGlue(void *interface);
bool itlwmEnsureInterfaceGlue(void *interface, void **installed);
bool itlwmRebuildInterfaceGlue(void *interface, void **installed);
bool itlwmControllerDeliveryChain(void *controller, void **first, void **second);
bool itlwmGlueDeliveryChain(void *interface, void **objectA, void **objectB,
                            void **delivery);
bool itlwmGlueMsgBitmap(void *interface, void **board, uint8_t *bytes, size_t len);
bool itlwmGlueBulletinBoardCorrect(void *interface, void **board, uint8_t *bytes,
                                   size_t len);
bool itlwmGlueScheduleEvents(void *interface);
bool itlwmGlueWakeState(void *interface, void **gate, unsigned *wake);
bool itlwmReadWclControllerChain(void *interface, void **controller,
                                 void **scanManager);
bool itlwmReadWclCacheQueueCount(void *interface, uint64_t *count,
                                 void **cacheStore, void **queue);
void itlwmReadWclBulletinBoardBaseInit(uint64_t *table, uint64_t *slot98);
void itlwmReadScanBitmapCorrect(void *interface, uint16_t *out, unsigned *blockFlag);
bool itlwmDumpScanManagerHandlers(void *interface, uint64_t *count,
                                  uint32_t *indexes, void **handlers,
                                  size_t maxEntries);

// Delivery.
bool itlwmPostWclScanResult(void *interface, uint32_t msgId, void *bytes,
                            size_t size, uint32_t *lastResult);
bool itlwmEnableScanMessages(void *interface);

// ABI gate.
WclAbiVerdict itlwmWclAbiVerdict(void *interface);
const char *itlwmWclAbiVerdictName(WclAbiVerdict verdict);

// Fault reporting and the peer manager build.
OSObject *itlwmCreateFaultAdapter(IOService *stream, IOWorkLoop *loop,
                                  OSObject **reporter);
bool itlwmAttachFaultReporter(OSObject *reporter, IOService *controller);
void itlwmReleaseFaultAdapter(OSObject **adapter, OSObject **reporter,
                              IOService *attachedController);
bool itlwmFaultAdapterInstalled(IOService *controller, OSObject *adapter);
void *itlwmCreatePeerWithDiagnostics(IOService *interface, IOService *controller,
                                     IOService *evidence, IOWorkLoop *loop);

// Queue and BSD-client observers.
bool itlwmPublishQueueState(IOService *target, const char *prefix, void *queue);
bool itlwmPublishInterfaceChildren(IOService *target, void *interface,
                                   unsigned *count);
void itlwmPublishBsdClientProbeSample(IOService *target, void *interface,
                                      unsigned seq);
void itlwmBsdBurstReset();
bool itlwmBsdBurstSample(IOService *target, void *interface, unsigned iteration);
void itlwmBsdBurstFinalize(IOService *target);

#endif // __IO80211_TARGET >= __MAC_15_0
#endif // AIRPORT_ITLWM_SEQUOIA_PEER_FACTORY_H
