#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "HAL/ItlBootProgress.h"
static ItlBootProgress progress;
#define ITLWM_IWX_DETAIL(key, value) progress.iwxRecord(ItlBootProgress::key, (value))
#define ITLWM_IWX_BUMP(key) progress.iwxBump(ItlBootProgress::key)
#define IO80211FAMILY_V2 1
#define __MAC_15_0 150000
#define __IO80211_TARGET __MAC_15_0
#define PAGE_SIZE 4096
#define htole16(x) (x)
#define htole64(x) (x)
#define MIN(x, y) std::min<size_t>(x, y)
#define nitems(a) (sizeof(a) / sizeof((a)[0]))
#define XYLog(...) ((void)0)
#define DPRINTF(...) ((void)0)
using bus_addr_t = uint64_t;
enum { IWX_CMD_ASYNC = 1, IWX_CMD_WANT_RESP = 2, MBUF_WAITOK = 0,
    M_NOWAIT = 0, M_ZERO = 0, PCATCH = 0 };
#include "iwx_command_types.inc"

struct Buffer { std::vector<uint8_t> bytes; };
using mbuf_t = Buffer *;
static unsigned liveMbufs, doorbells;
static bool failMbuf, failResponse;
static std::set<void *> responses;
static void *malloc(size_t size, int, int) {
    if (failResponse) return nullptr;
    void *value = std::calloc(1, size);
    assert(value && responses.insert(value).second);
    return value;
}
static void releaseResponse(void *value) {
    if (!value) return;
    assert(responses.erase(value) == 1);
    std::free(value);
}
#define free releaseResponse
static int mbuf_allocpacket(int, size_t size, unsigned *, mbuf_t *packet) {
    if (failMbuf) { *packet = nullptr; return ENOMEM; }
    *packet = new Buffer; (*packet)->bytes.resize(size); ++liveMbufs; return 0;
}
static void mbuf_freem(mbuf_t packet) { assert(packet && liveMbufs); --liveMbufs; delete packet; }
static int mbuf_setlen(mbuf_t packet, size_t size) { assert(packet->bytes.size() == size); return 0; }
static int mbuf_pkthdr_setlen(mbuf_t packet, size_t size) { assert(packet->bytes.size() == size); return 0; }
#define mtod(m, type) reinterpret_cast<type>((m)->bytes.data())
struct IOPhysicalSegment { uint64_t location, length; };
struct Cursor {
    unsigned count = 1;
    int lengthDelta = 0;
    uint64_t address = 0x12345000;
    bool relocate = false;
    unsigned getPhysicalSegmentsWithCoalesce(mbuf_t packet, IOPhysicalSegment *segment, int capacity) {
        assert(capacity == 1);
        if (relocate) { auto copy = packet->bytes; packet->bytes.swap(copy); }
        if (count) *segment = {address, packet->bytes.size() + lengthDelta};
        return count;
    }
};
struct Map { Cursor *cursor; unsigned dm_nsegs = 0; };
struct iwx_tx_data { Map *map = nullptr; bus_addr_t cmd_paddr = 0x23456000; mbuf_t m = nullptr; int flags = 0; };
struct iwx_tx_ring {
    iwx_tfh_tfd *desc;
    iwx_device_cmd *cmd;
    iwx_tx_data data[4];
    unsigned ring_count = 4;
    int qid = IWX_DQA_CMD_QUEUE, queued = 0, cur = 0;
};
struct iwx_softc {
    iwx_tx_ring txq[1];
    struct { unsigned cur = 7; } rxq;
    unsigned sc_flags = 0;
    int sc_generation = 1;
    uint8_t *sc_cmd_resp_pkt[4] = {};
    size_t sc_cmd_resp_len[4] = {};
};
struct Loop {
    bool gated = true, on = false;
    bool inGate() { return gated; }
    bool onThread() { return on; }
};
static int splnet() { return 0; }
static void splx(int) {}
#undef IWX_WRITE
#define IWX_WRITE(sc, reg, value) (++doorbells)
static uint64_t SEC_TO_NSEC(unsigned seconds) { return uint64_t(seconds) * 1000000000; }
class ItlIwx {
public:
    Loop loop;
    unsigned wakes = 0;
    void wakeupOn(void *) { ++wakes; }
    void iwx_cmd_done(iwx_softc *, int, int, int);
    bool nativeDetached = false;
    int nativeError = 0, sleepError = 0;
    std::function<void()> onSleep;
    Loop *getMainWorkLoop() { return &loop; }
    int getTxQueueSize() { return 4; }
    int tsleep_nsec(void *, int, const char *, int) { if (onSleep) onSleep(); return sleepError; }
    int iwx_send_cmd(iwx_softc *, iwx_host_cmd *);
};
#include "iwx_command_method.inc"

struct Fixture {
    ItlIwx driver;
    iwx_softc sc = {};
    iwx_tfh_tfd descriptors[4] = {};
    iwx_device_cmd commands[4] = {};
    Cursor cursor;
    Map map = {&cursor};
    iwx_host_cmd command = {};
    std::vector<uint8_t> payload;
    explicit Fixture(bool large = true, bool response = true) : payload(large ? 600 : 16, 0x5a) {
        assert(!liveMbufs && responses.empty());
        failMbuf = failResponse = false; doorbells = 0; progress = {};
        sc.txq[0].desc = descriptors; sc.txq[0].cmd = commands;
        for (auto &data : sc.txq[0].data) data.map = &map;
        command.id = 0x123456;
        command.len[0] = payload.size(); command.data[0] = payload.data();
        command.flags = response ? IWX_CMD_WANT_RESP : 0;
        command.resp_pkt_len = sizeof(iwx_rx_packet) + 4;
    }
    ~Fixture() {
        // Model completion/reset only after assertions inspect ownership.
        for (auto &data : sc.txq[0].data) if (data.m) mbuf_freem(data.m);
        for (auto *response : sc.sc_cmd_resp_pkt) releaseResponse(response);
        releaseResponse(command.resp_pkt);
        assert(!liveMbufs && responses.empty());
    }
    int send() { return driver.iwx_send_cmd(&sc, &command); }
    void rejected(int expected) {
        assert(send() == expected);
        assert(!doorbells && !sc.txq[0].queued && !sc.txq[0].cur);
        assert(!liveMbufs && responses.empty() && !command.resp_pkt);
        assert(!sc.sc_cmd_resp_pkt[0] && !sc.sc_cmd_resp_len[0]);
    }
    void payloadMatches() {
        const auto *cmd = sc.txq[0].data[0].m ? mtod(sc.txq[0].data[0].m, iwx_device_cmd *) : &commands[0];
        assert(cmd->hdr_wide.cmd == iwx_cmd_opcode(command.id));
        assert(cmd->hdr_wide.length == payload.size());
        assert(!memcmp(cmd->data_wide, payload.data(), payload.size()));
    }
};

int main(int argc, char **argv) {
    const std::string only = argc > 1 ? argv[1] : "";
    if (only.empty() || only == "mapping") {
        Fixture f; f.cursor.count = 0; f.rejected(EIO);
    }
    if (only.empty() || only == "allocation") {
        Fixture f; failMbuf = true; f.rejected(ENOMEM);
    }
    if (only.empty() || only == "relocation") {
        Fixture f; f.cursor.relocate = true;
        assert(!f.send() && doorbells == 1 && liveMbufs == 1); f.payloadMatches();
    }
    if (!only.empty()) return 0;
    { Fixture f; failResponse = true; f.rejected(ENOMEM); }
    { Fixture f; f.cursor.lengthDelta = -1; f.rejected(EIO); }
    { Fixture f; f.cursor.count = 2; f.rejected(EIO); }
    { Fixture f; f.cursor.address = UINT64_MAX - 8; f.rejected(EIO); }
    for (bool large : {false, true}) {
        for (bool response : {false, true}) {
            Fixture f(large, response);
            assert(!f.send() && doorbells == 1 && f.sc.txq[0].queued == 1);
            assert(liveMbufs == unsigned(large)); f.payloadMatches();
            assert(bool(f.command.resp_pkt) == response);
        }
    }
    for (int result : {ETIMEDOUT, EINTR}) {
        Fixture f; f.driver.sleepError = result;
        assert(f.send() == result && doorbells == 1 && liveMbufs == 1);
        assert(responses.empty()); // DMA mbuf remains device-owned until reset/completion.
        assert(progress.iwxValue(ItlBootProgress::Command) == f.command.id);
        assert(progress.iwxValue(ItlBootProgress::Submitted) == 1);
        assert(progress.iwxValue(ItlBootProgress::WaitQueued) == 1);
        assert(progress.iwxValue(ItlBootProgress::WaitRxCursor) == 7);
    }
    { Fixture f; f.driver.onSleep = [&] { ++f.sc.sc_generation; };
      assert(f.send() == ENXIO && doorbells == 1 && liveMbufs == 1); }
    { Fixture f(true, false); f.command.flags = IWX_CMD_ASYNC; f.driver.loop.on = true;
      assert(!f.send() && doorbells == 1 && liveMbufs == 1); }
    { Fixture f; f.driver.loop.gated = false; f.rejected(EWOULDBLOCK); }
    { Fixture f; f.driver.loop.on = true; f.rejected(EWOULDBLOCK); }
    { Fixture f; f.driver.nativeDetached = true; f.rejected(ENXIO); }
    { Fixture f; f.sc.sc_flags |= IWX_FLAG_SHUTDOWN; f.rejected(ENXIO); }
    { Fixture f; f.driver.nativeError = EIO; f.rejected(EIO); }
    { Fixture f; f.command.data[0] = nullptr; f.rejected(EINVAL); }
    { Fixture f; f.command.flags |= IWX_CMD_ASYNC; f.rejected(EINVAL); }
    { Fixture f; f.sc.txq[0].desc = nullptr; f.rejected(ENXIO); }
    { Fixture f; f.sc.txq[0].ring_count = 3; f.rejected(ENXIO); }
    { Fixture f; f.sc.txq[0].data[0].map = nullptr; f.rejected(ENXIO); }
    { Fixture f; f.map.cursor = nullptr; f.rejected(ENXIO); }
    { Fixture f; assert(!f.send());
      f.driver.iwx_cmd_done(&f.sc, IWX_DQA_CMD_QUEUE, 0, f.command.id);
      assert(!liveMbufs && !f.sc.txq[0].queued && f.driver.wakes == 1);
      assert(progress.iwxValue(ItlBootProgress::Completed) == 1);
      assert(progress.iwxValue(ItlBootProgress::DoneCommand) == f.command.id);
      assert(progress.iwxValue(ItlBootProgress::DoneIndex) == 0);
      f.driver.iwx_cmd_done(&f.sc, IWX_DQA_CMD_QUEUE, 4, f.command.id);
      assert(progress.iwxValue(ItlBootProgress::Completed) == 1 && f.driver.wakes == 1); }
    puts("IWX command: preparation, relocation, failure, completion diagnostics and post-submit ownership cases passed; DMA/kernel boundaries mocked");
}
