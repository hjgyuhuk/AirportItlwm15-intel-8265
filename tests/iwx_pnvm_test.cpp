#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <vector>
#include <zlib.h>
#include "HAL/ItlBootProgress.h"

using __le16 = uint16_t;
using __le32 = uint32_t;
using u32 = uint32_t;
#define __packed __attribute__((packed))
#define le32toh(x) (x)
#define htole32(x) (x)
#define htole64(x) (x)
static uint32_t le32_to_cpup(const uint32_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t le16_to_cpup(const uint16_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
#define roundup(x, n) (((x) + (n) - 1) & ~((n) - 1))
#define XYLog(...) ((void)0)
static ItlBootProgress progress;
#define ITLWM_IWX_DETAIL(key, value) progress.iwxRecord(ItlBootProgress::key, (value))
static uint64_t SEC_TO_NSEC(unsigned n) { return uint64_t(n) * 1000000000; }
#include "iwx_pnvm_types.inc"
#include "iwx_pnvm_compat.inc"

static std::vector<uint8_t> firmware;
static bool missingResource, failUnpack, failAllocation, failDma;
static unsigned allocations, releases;
static void *malloc(size_t size, int, int) { return failAllocation ? nullptr : std::calloc(1, size); }
struct OSData {
    size_t getLength() { return firmware.size(); }
    const void *getBytesNoCopy() { return firmware.data(); }
    void release() { ++releases; delete this; }
};
#define OSSafeReleaseNULL(p) do { if (p) { (p)->release(); (p) = nullptr; } } while (0)
static OSData *getFWDescByName(const char *name) {
    assert(!strcmp(name, "iwlwifi-ty-a0-gf-a0.pnvm"));
    if (missingResource) return nullptr;
    ++allocations;
    return new OSData;
}
static bool uncompressFirmware(unsigned char *out, unsigned *size, unsigned char *data, unsigned length) {
    if (failUnpack) return false;
    uLongf n = *size;
    const int result = uncompress(out, &n, data, length);
    *size = unsigned(n);
    return result == Z_OK;
}
struct iwx_dma_info { void *vaddr = nullptr; uint64_t paddr = 0; size_t size = 0; };
struct iwx_prph_scratch_ctrl_cfg {
    struct { uint64_t pnvm_base_addr = 0; uint32_t pnvm_size = 0; } pnvm_cfg;
};
struct iwx_prph_scratch { iwx_prph_scratch_ctrl_cfg ctrl_cfg; };
struct iwx_fw_info { void *pnvm_rawdata = nullptr; size_t pnvm_rawsize = 0; };
struct iwx_softc {
    iwx_fw_info sc_fw;
    const char *sc_fwname = "iwlwifi-ty-a0-gf-a0-68.ucode";
    unsigned sc_hw_rev = 0x42 << 4, sc_hw_rf_id = 0x10d << 12;
    unsigned sku_id[3] = {0x610d0, 0, 0};
    int sc_dmat = 0;
    iwx_dma_info pnvm_dram, prph_scratch_dma;
    bool sc_pnvm_done = false;
    uint32_t sc_pnvm_status = 0;
};
class ItlIwx {
public:
    bool lockAvailable = true, earlyNotification = false, notify = true;
    unsigned status = 0, doorbells = 0, sleeps = 0;
    int sleepResult = 0;
    std::function<void()> beforeSleep;
    iwx_softc *active = nullptr;
    void iwx_pnvm_free(iwx_fw_info *);
    int iwx_pnvm_handle_section(iwx_softc *, const uint8_t *, size_t);
    int iwx_read_pnvm(iwx_softc *);
    int iwx_load_pnvm(iwx_softc *);
    void iwx_dma_contig_free(iwx_dma_info *d) { std::free(d->vaddr); *d = {}; }
    int iwx_dma_contig_alloc(int, iwx_dma_info *d, size_t size, int) {
        if (failDma) return ENOMEM;
        d->vaddr = std::calloc(1, size); assert(d->vaddr);
        d->paddr = 0x100000; d->size = size; return 0;
    }
    bool iwx_nic_lock(iwx_softc *sc) { active = sc; return lockAvailable; }
    void iwx_nic_unlock(iwx_softc *) {}
    void complete() { active->sc_pnvm_status = status; active->sc_pnvm_done = true; }
    void iwx_write_umac_prph(iwx_softc *, unsigned, unsigned) {
        ++doorbells; if (earlyNotification) complete();
    }
    int tsleep_nsec(void *event, int, const char *, uint64_t) {
        assert(event == &active->sc_pnvm_done); ++sleeps;
        if (beforeSleep) beforeSleep();
        if (notify && !sleepResult) complete();
        return sleepResult;
    }
};
#include "iwx_pnvm_methods.inc"

struct Fixture {
    ItlIwx driver;
    iwx_softc sc;
    iwx_prph_scratch scratch;
    Fixture() {
        missingResource = failUnpack = failAllocation = failDma = false;
        progress = {};
        sc.prph_scratch_dma.vaddr = &scratch;
    }
    ~Fixture() {
        driver.iwx_pnvm_free(&sc.sc_fw);
        driver.iwx_dma_contig_free(&sc.pnvm_dram);
        assert(allocations == releases);
    }
    int load() { int r = driver.iwx_load_pnvm(&sc); assert(!sc.sc_fw.pnvm_rawdata); return r; }
};
int main(int argc, char **argv) {
    assert(argc == 2);
    // Exercise the actual compatibility macro, including no-match and NUL.
    for (const char *s : {"", "abc", "-", "a-b-", "iwlwifi-ty-a0-gf-a0-68.ucode"}) {
        for (int c : {0, int('-'), int('a'), int('x'), 0x100 + '-'})
            assert(strrchr(s, c) == (std::strrchr)(s, c));
    }
    std::ifstream in(argv[1], std::ios::binary);
    firmware.assign(std::istreambuf_iterator<char>(in), {});
    assert(!firmware.empty());
    const auto original = firmware;
    // All SKU sections of the actual bundled AX210 PNVM must select and copy.
    const unsigned skus[] = {0x610d0, 0x510d0, 0x4510d0};
    const size_t sizes[] = {13748, 13804, 13808};
    for (unsigned i = 0; i < 3; ++i) {
        Fixture f; f.sc.sku_id[0] = skus[i]; assert(!f.load());
        assert(f.driver.doorbells == 1 && f.driver.sleeps == 1);
        assert(f.sc.pnvm_dram.size == sizes[i]);
        assert(f.scratch.ctrl_cfg.pnvm_cfg.pnvm_size == sizes[i]);
        assert(progress.iwxValue(ItlBootProgress::PnvmRawBytes) == 41588);
        assert(progress.iwxValue(ItlBootProgress::PnvmVersion) == 0xdda57f4f);
    }
    { Fixture f; f.sc.sku_id[0] = 0xdead; assert(f.load() == ENOENT && !f.driver.doorbells); }
    { Fixture f; f.sc.sc_hw_rev = 0; assert(f.load() == -ENOENT && !f.driver.doorbells); }
    { Fixture f; missingResource = true; assert(f.load() == EINVAL && !f.driver.doorbells); }
    { Fixture f; failUnpack = true; assert(f.load() == EINVAL && !f.driver.doorbells); }
    { Fixture f; failAllocation = true; assert(f.load() == ENOMEM && !f.driver.doorbells); }
    { Fixture f; failDma = true; assert(f.load() == ENOMEM && !f.driver.doorbells); }
    for (const char *name : {static_cast<const char *>(nullptr), "", "nohyphen", "-68.ucode"}) {
        Fixture f; f.sc.sc_fwname = name; assert(f.load() == EINVAL && !f.driver.doorbells);
    }
    { Fixture f; f.driver.lockAvailable = false; assert(f.load() == EBUSY && !f.driver.doorbells); }
    // Status is retained as opaque firmware data; no undocumented errno mapping.
    for (bool early : {false, true}) {
        Fixture f; f.driver.status = 0x20005; f.driver.earlyNotification = early;
        assert(!f.load() && f.driver.doorbells == 1);
        assert(f.sc.sc_pnvm_status == 0x20005);
        assert(f.driver.sleeps == unsigned(!early));
    }
    { Fixture f; f.driver.earlyNotification = true; f.driver.sleepResult = ETIMEDOUT;
      assert(!f.load() && !f.driver.sleeps); }
    { Fixture f; f.driver.notify = false; assert(f.load() == EIO); }
    for (int error : {ETIMEDOUT, EINTR}) {
        Fixture f; f.driver.sleepResult = error; assert(f.load() == error);
    }
    { Fixture f; f.sc.sku_id[0] = 0; assert(!f.load() && !f.driver.doorbells); }
    // Decompressed malformed TLV must not be submitted to the device.
    uint8_t invalid[8] = {64, 0, 0, 0, 255, 255, 255, 127};
    uLongf capacity = compressBound(sizeof(invalid)); firmware.resize(capacity);
    assert(compress(firmware.data(), &capacity, invalid, sizeof(invalid)) == Z_OK);
    firmware.resize(capacity);
    { Fixture f; assert(f.load() == -EINVAL && !f.driver.doorbells); }
    firmware = original;
    puts("PNVM: all 3 bundled AX210 SKU sections, error propagation, notification status, early completion, false wake, timeout and resource cleanup passed; hardware mocked");
}
