// Actual bridge implementation; Apple allocators/initializers and IOKit mocked.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <Airport/SequoiaInterfaceLifecycle.h>
static int live = 0, calls = 0, cleanupCalls = 0;
static int failure = 0;
struct OSObject {
    int refs = 1;
    std::string kind;
    explicit OSObject(const char *name = "object") : kind(name) { ++live; }
    virtual ~OSObject() { --live; }
    void retain() { ++refs; }
    void release() { assert(refs > 0); if (!--refs) delete this; }
    void *metaCast(const char *name) { return kind == name ? this : nullptr; }
};
#define OSSafeReleaseNULL(p) do { if (p) { (p)->release(); (p) = nullptr; } } while (0)
struct OSDictionary : OSObject {
    std::map<std::string, OSObject *> items;
    bool published = false;
    static OSDictionary *withCapacity(unsigned) { return new OSDictionary; }
    void setObject(const char *key, OSObject *value) {
        assert(!published); value->retain();
        auto &old = items[key]; OSSafeReleaseNULL(old); old = value;
    }
    ~OSDictionary() { for (auto &item : items) item.second->release(); }
};
struct OSArray : OSObject {
    std::vector<OSObject *> items;
    bool published = false;
    static OSArray *withCapacity(unsigned) { return new OSArray; }
    void setObject(OSObject *value) { assert(!published); value->retain(); items.push_back(value); }
    ~OSArray() { for (auto *item : items) item->release(); }
};
struct OSNumber : OSObject {
    uint64_t value;
    static OSNumber *withNumber(uint64_t n, unsigned) { auto *p = new OSNumber; p->value = n; return p; }
};
struct OSString : OSObject {
    std::string value;
    static OSString *withCString(const char *s) { assert(s); auto *p = new OSString; p->value = s; return p; }
};
struct IOService : OSObject {
    OSDictionary props;
    using OSObject::OSObject;
    void setProperty(const char *name, OSObject *value) {
        if (auto *dict = dynamic_cast<OSDictionary *>(value)) dict->published = true;
        if (auto *array = dynamic_cast<OSArray *>(value)) {
            array->published = true;
            for (auto *item : array->items)
                if (auto *dict = dynamic_cast<OSDictionary *>(item)) dict->published = true;
        }
        props.setObject(name, value);
    }
    void setProperty(const char *name, const char *value) {
        auto *s = OSString::withCString(value); setProperty(name, s); s->release();
    }
    void setProperty(const char *name, bool value) {
        auto *n = OSNumber::withNumber(value, 1); setProperty(name, n); n->release();
    }
};
struct IOWorkLoop : OSObject {};
struct Raw : OSObject {
    IOService *attached = nullptr;
    Raw() : OSObject("CCFaultReporter") {}
    ~Raw() { assert(!attached); }
};
struct Adapter : OSObject {
    OSObject *raw;
    explicit Adapter(OSObject *p) : OSObject("IO80211FaultReporter"), raw(p) { p->retain(); }
    ~Adapter() { raw->release(); }
};
static OSObject *ccReporter(OSObject *, IOWorkLoop *) { return failure == 1 ? nullptr : new Raw; }
static OSObject *faultAdapter(OSObject *p) { return failure == 2 ? nullptr : new Adapter(p); }
static bool ccAttach(OSObject *p, IOService *owner) {
    auto *raw = static_cast<Raw *>(p); assert(!raw->attached);
    if (failure == 3) return false;
    raw->attached = owner; return true;
}
static void ccDetach(OSObject *p, IOService *owner) {
    auto *raw = static_cast<Raw *>(p); assert(raw->attached == owner); raw->attached = nullptr;
}
// Map object identity to exact-layout storage; the extracted helper's read/take
// operations use these byte arrays without assuming the host mock object ABI.
static std::map<const void *, void *> storage;
namespace HostLayout {
template <class T> T read(const void *p, size_t offset) {
    auto it = storage.find(p);
    return SequoiaInterfaceLifecycle::read<T>(it == storage.end() ? p : it->second, offset);
}
}
template <class T> static void put(void *p, size_t off, T value) {
    memcpy(static_cast<char *>(p) + off, &value, sizeof(value));
}
struct Manager : OSObject {
    unsigned char object[32] = {}, ivars[0xc10] = {};
    Manager() {
        storage[this] = object;
        if (failure != 5) put(object, 0x18, static_cast<void *>(ivars));
    }
    ~Manager() { assert(!SequoiaInterfaceLifecycle::read<void *>(ivars, 0x710)); storage.erase(this); }
};
namespace OSMetaClass {
static OSObject *allocClassWithName(const char *name) {
    assert(std::string(name) == "IO80211PeerManager"); return failure == 4 ? nullptr : new Manager;
}
}
static bool peerInit(OSObject *p, void *vif, void *) {
    ++calls; assert(!vif);
    auto *manager = static_cast<Manager *>(p);
    if (failure == 5 || failure == 6) return false;
    put(manager->ivars, 0x78, static_cast<void *>(manager));
    put(manager->ivars, 0x710, static_cast<OSObject *>(new OSObject));
    return failure == 0;
}
static void peerFreeResources(OSObject *p) {
    ++cleanupCalls;
    assert(HostLayout::read<void *>(HostLayout::read<void *>(p, 0x18), 0x78));
}
static unsigned depth = 1024;
static unsigned queueDepth(void *) { return depth; }
static const char *name = "en0";
static const char *bsdName(void *) { return name; }
#include "sequoia_peer_bridge_methods.inc"

int main() {
    unsigned traces = 0;
    for (failure = 0; failure < 4; ++failure) {
        IOService stream("CCDataStream"), owner; IOWorkLoop loop;
        OSObject *raw = nullptr;
        auto *adapter = itlwmCreateFaultAdapter(&stream, &loop, &raw);
        assert((adapter != nullptr) == (failure == 0 || failure == 3));
        assert(!raw || !static_cast<Raw *>(raw)->attached);
        const bool attached = itlwmAttachFaultReporter(raw, &owner);
        assert(attached == (failure == 0));
        itlwmReleaseFaultAdapter(&adapter, &raw, attached ? &owner : nullptr);
        itlwmReleaseFaultAdapter(&adapter, &raw, nullptr);
        ++traces;
    }
    assert(live == 0);
    for (int mode = 0; mode < 17; ++mode) {
        IOService interface, controller, evidence; IOWorkLoop loop;
        Raw raw; Adapter adapter(&raw); OSObject wrong("CCStream");
        unsigned char ifObject[0x128] = {}, ifIvars[0x100] = {};
        unsigned char ctrlObject[0x128] = {}, ctrlIvars[0x100] = {};
        storage[&interface] = ifObject; storage[&controller] = ctrlObject;
        put(ifObject, 0x110, static_cast<void *>(ifIvars));
        put(ctrlObject, 0x120, static_cast<void *>(ctrlIvars));
        put(ctrlIvars, 0x58, static_cast<OSObject *>(&adapter));
        put(ifIvars, 0x30, &controller); put(ifIvars, 0x38, &loop);
        put(ifIvars, 0x58, uint32_t(1)); put(ifIvars, 0x5c, uint32_t(1));
        put(ifIvars, 0x78, &raw);
        failure = 0; depth = 1024; name = "en0"; calls = cleanupCalls = 0;
        if (mode == 1) put(ifIvars, 0x30, static_cast<void *>(nullptr));
        if (mode == 2) put(ifIvars, 0x38, static_cast<void *>(nullptr));
        if (mode == 3) put(ctrlIvars, 0x58, &wrong);
        if (mode == 4) put(ifIvars, 0x58, uint32_t(2));
        if (mode == 5) put(ifIvars, 0x5c, uint32_t(0));
        if (mode == 6) name = nullptr;
        if (mode == 7) name = "";
        if (mode == 8) put(ifIvars, 0x78, static_cast<void *>(nullptr));
        if (mode == 9) put(ifIvars, 0x28, &raw);
        if (mode == 10) put(ifIvars, 0x68, &interface);
        if (mode == 11) depth = 0;
        if (mode == 12) depth = 0x10000;
        if (mode >= 13) failure = mode - 9; // alloc, ivars, lock, later-init failure
        auto *result = static_cast<OSObject *>(itlwmCreatePeerWithDiagnostics(&interface, &controller, &evidence, &loop));
        assert((result != nullptr) == (mode == 0 || mode == 7));
        assert(calls == (mode == 0 || mode == 7 || mode >= 14 ? 1 : 0));
        assert(cleanupCalls == (mode == 16 ? 1 : 0));
        if (result) {
            // On success the family owns the returned allocation reference.
            auto *manager = static_cast<Manager *>(result);
            auto *timer = SequoiaInterfaceLifecycle::take<OSObject>(manager->ivars, 0x710);
            timer->release(); result->release();
        }
        storage.erase(&interface); storage.erase(&controller); ++traces;
    }
    assert(live == 0 && storage.empty());
    printf("Actual peer bridge: %u preflight/partial-init/ownership traces passed; Apple internals mocked\n", traces);
}
