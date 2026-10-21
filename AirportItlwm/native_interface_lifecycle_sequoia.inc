bool AirportItlwmSkywalkInterface::start(IOService *provider)
{
    using namespace SequoiaInterfaceLifecycle;
    // Native primary interface owns its manager, created during family start.
    const uint32_t nativeRole = skywalkIvars ? read<uint32_t>(skywalkIvars, Role) : 0;
    if (nativeStartAttempted || nativeStopDone || !skywalkIvars || !infraIvars ||
        !instance || provider != instance || nativeRole != 1 || read<void *>(skywalkIvars, PeerManager))
        return false;
    nativeStartAttempted = true;
    if (super::start(provider)) return true;
    stop(provider);
    return false;
}

void AirportItlwmSkywalkInterface::quiesceNativeTimers()
{
    using namespace SequoiaInterfaceLifecycle;
    if (!nativeStartAttempted || !instance || !instance->getWorkQueue()) return;
    // These timers borrow the interface. Drain their gate before system stop;
    // 24G830 Infra::stop omits the data-rate timer at +0x150 entirely.
    instance->getWorkQueue()->runAction([](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
        auto *self = static_cast<AirportItlwmSkywalkInterface *>(owner);
        if (self->nativeTimersQuiesced) return kIOReturnSuccess;
        self->nativeTimersQuiesced = true;
        const size_t offsets[] = {InfraLinkTimer, InfraDataTimer};
        for (size_t offset : offsets) {
            auto *timer = read<IOTimerEventSource *>(self->infraIvars, offset);
            if (timer) { timer->cancelTimeout(); timer->disable(); }
        }
        return kIOReturnSuccess;
    }, this);
}

void AirportItlwmSkywalkInterface::dumpNativeIvars(uint64_t *peer, uint64_t *ctrl, uint32_t *role,
                                                   uint64_t *logStream, uint64_t *logPipe)
{
    using namespace SequoiaInterfaceLifecycle;
    const uint64_t vPeer = reinterpret_cast<uint64_t>(read<void *>(skywalkIvars, PeerManager));
    const uint64_t vCtrl = reinterpret_cast<uint64_t>(read<void *>(skywalkIvars, Controller));
    const uint32_t vRole = read<uint32_t>(skywalkIvars, Role);
    const uint64_t vStream = reinterpret_cast<uint64_t>(read<void *>(skywalkIvars, LogStream));
    const uint64_t vPipe = reinterpret_cast<uint64_t>(read<void *>(skywalkIvars, LogPipe));
    if (peer) *peer = vPeer;
    if (ctrl) *ctrl = vCtrl;
    if (role) *role = vRole;
    if (logStream) *logStream = vStream;
    if (logPipe) *logPipe = vPipe;
    // Printed as well: the console is a second channel in case the caller cannot
    // publish, and on a verbose boot the line is visible while the start runs.
    // Format a bounded snapshot first. panic9 belongs to an internal boot;
    // it is not evidence that this driver's varargs ABI is broken.
#if SEQUOIA_DIAGNOSTICS
    char line[224];
    snprintf(line, sizeof(line),
             "itlwm-native: ivars skywalk=%p infra=%p peer=%p ctrl=%p role=%u logStream=%p logPipe=%p\n",
             skywalkIvars, infraIvars, reinterpret_cast<void *>(vPeer), reinterpret_cast<void *>(vCtrl),
             vRole, reinterpret_cast<void *>(vStream), reinterpret_cast<void *>(vPipe));
    ITLWM_LOG("%s", line);
#endif
}

void AirportItlwmSkywalkInterface::stop(IOService *provider)
{
    using namespace SequoiaInterfaceLifecycle;
    if (!nativeStartAttempted || nativeStopDone || provider != instance) return;
    nativeStopDone = true;
    quiesceNativeTimers();
    // commonInit can fail before allocating ScanManager's ivars. Its free is
    // null-safe; its stop is not. Remove only that empty object before stop.
    auto *scan = read<OSObject *>(infraIvars, ScanManager);
    if (scan && !read<void *>(scan, ScanManagerIvars)) {
        take<OSObject>(infraIvars, ScanManager)->release();
    }
    if (read<void *>(skywalkIvars, PeerManager)) {
        // A published peer comes only from a successful system factory.
        // All subsequent partial-start stages satisfy Skywalk::stop's needs.
        super::stop(provider);
    } else if (read<void *>(skywalkIvars, Controller)) {
        // The controller is stored only after Ethernet::start succeeds.
        // Failure before the peer factory precedes PMinit and Infra resources.
        // Skywalk::stop would dereference the missing peer in this branch.
        auto *pipe = read<CCPipe *>(skywalkIvars, LogPipe);
        if (pipe) {
            pipe->CCPipe::stopPipe();
            take<OSObject>(skywalkIvars, LogPipe)->release();
        }
        auto *stream = take<OSObject>(skywalkIvars, LogStream);
        if (stream) stream->release();
        IOSkywalkEthernetInterface::stop(provider);
    }
    // Infra::start retains this factory result, but 24G830 stop/free omit it.
    // It contains borrowed interface/log references, no timers or callbacks.
    auto *bonjour = take<OSObject>(infraIvars, BonjourAgent);
    if (bonjour) bonjour->release();
}
