void AirportItlwm::quiesceNativeSystemEvents()
{
    if (!nativeLoopRetained) return;
    _fWorkloop->runAction([](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
        using namespace SequoiaInterfaceLifecycle;
        auto *ivars = read<void *>(owner, ControllerIvars);
        auto *office = read<void *>(ivars, ControllerPostOffice);
        auto *rng = read<void *>(ivars, ControllerRNGAgent);
        auto stopTimer = [](IOTimerEventSource *timer) {
            if (timer) { timer->cancelTimeout(); timer->disable(); }
        };
        stopTimer(read<IOTimerEventSource *>(read<void *>(office, PostOfficeIvars), PostOfficeTimer));
        stopTimer(read<IOTimerEventSource *>(read<void *>(rng, RNGAgentIvars), RNGAgentTimer));
        return kIOReturnSuccess;
    }, this);
}

void AirportItlwm::releaseNativeSystemEvents()
{
    if (!nativeLoopRetained) return;
    _fWorkloop->runAction([](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
        using namespace SequoiaInterfaceLifecycle;
        auto *ivars = read<void *>(owner, ControllerIvars);
        // The interface, native workers and system controller have stopped.
        // PostOffice::free discards queued payloads without touching their
        // borrowed interface pointers. Its timer must have been disabled first.
        auto *rng = take<OSObject>(ivars, ControllerRNGAgent);
        if (rng) rng->release();
        auto *office = take<OSObject>(ivars, ControllerPostOffice);
        if (office) office->release();
        auto *factory = take<OSObject>(ivars, ControllerTimerFactory);
        if (factory) factory->release();
        return kIOReturnSuccess;
    }, this);
}
