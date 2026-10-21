#include <mach/kmod.h>

#ifndef SEQUOIA_NVM_TRIAL_VERSION
#error Build with the version supplied by build_sequoia_nvm_trial.py
#endif

extern "C" {
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);
KMOD_EXPLICIT_DECL(com.zxystd.AirportItlwm.SequoiaNvmTrial, SEQUOIA_NVM_TRIAL_VERSION, _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = nullptr;
__private_extern__ kmod_stop_func_t *_antimain = nullptr;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
}
