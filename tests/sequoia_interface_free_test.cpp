#include <cassert>
#include <cstdio>

struct IOSkywalkEthernetInterface {
    unsigned ethernetFree = 0;
    void free() { ++ethernetFree; }
};
struct IO80211InfraProtocol : IOSkywalkEthernetInterface {
    void *skywalkIvars = nullptr, *infraIvars = nullptr;
    unsigned infraFree = 0;
    void free() {
        // Model the observed unconditional Skywalk dereference, not its allocator.
        assert(skywalkIvars);
        ++infraFree;
        IOSkywalkEthernetInterface::free();
    }
};
struct AirportItlwmSkywalkInterface : IO80211InfraProtocol { void free(); };
#define super IO80211InfraProtocol
#include "sequoia_interface_free.inc"

int main()
{
    int allocation;
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        for (unsigned stage = 0; stage < 3; ++stage) {
            AirportItlwmSkywalkInterface interface;
            if (stage >= 1) interface.skywalkIvars = &allocation;
            if (stage >= 2) interface.infraIvars = &allocation;
            interface.free();
            assert(interface.ethernetFree == 1);
            assert(interface.infraFree == (stage ? 1u : 0u));
        }
    }
    puts("Native interface free: base/Skywalk/Infra allocation failure dispatch passed");
}
