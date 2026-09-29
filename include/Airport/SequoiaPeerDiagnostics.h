#ifndef SEQUOIA_PEER_DIAGNOSTICS_H
#define SEQUOIA_PEER_DIAGNOSTICS_H
class OSObject;
class IOService;
class IOWorkLoop;
// Owns one reference to each result. The stream must outlive both.
OSObject *itlwmCreateFaultAdapter(IOService *, IOWorkLoop *, OSObject **);
bool itlwmAttachFaultReporter(OSObject *, IOService *);
void itlwmReleaseFaultAdapter(OSObject **, OSObject **, IOService *);
bool itlwmFaultAdapterInstalled(IOService *, OSObject *);
void *itlwmCreatePeerWithDiagnostics(IOService *, IOService *, IOService *, IOWorkLoop *);
#endif
