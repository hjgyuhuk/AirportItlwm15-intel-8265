#define IWM_SCAN_TRANSPORT_INTEGRATION
#include "iwm_command_test_support.h"
#include <Airport/SequoiaScanDelivery.h>
#define KASSERT(value, message) assert(value)
#define XYLog(...) ((void)0)
#define free(pointer) testResponseFree(pointer)
#include "iwm_scan_production_methods.inc"
#undef free

#include "../itlwm/hal_iwm/scan_request_sequoia.inc"
#define malloc(bytes, type, flags) testResponseAllocate(bytes, type, flags)
#define free(pointer) testResponseFree(pointer)
#include "../itlwm/hal_iwm/scan_trial_sequoia.inc"
#undef malloc
#undef free

static void notifyScan(ItlIwm &driver, SequoiaWCL::ScanDelivery &delivery, uint64_t token)
{
    uint8_t phy[88] = {}; phy[22] = 1; phy[28] = 55;
    driver.iwm_scan_trial_rx(IWM_REPLY_RX_PHY_CMD, phy, sizeof(phy));
    uint8_t mpdu[46] = {}; mpdu[0] = 38; mpdu[4] = 0x80; mpdu[20] = 2; mpdu[42] = 3;
    driver.iwm_scan_trial_rx(IWM_REPLY_RX_MPDU_CMD, mpdu, sizeof(mpdu));
    ItlScan::LegacyPhy metadata;
    assert(ItlScan::decodeLegacyPhy(phy, sizeof(phy), metadata) == ItlScan::PhyDecode::Accepted);
    uint8_t result[SequoiaWCL::kMaxScanBeaconMessage] = {};
    const auto encoded = SequoiaWCL::encodeIwmBeacon(metadata, mpdu + 4, 38, result, sizeof(result));
    assert(encoded >= sizeof(SequoiaWCL::BeaconMetadata) && encoded <= sizeof(result));
    assert(delivery.queueBeacon(token, metadata, mpdu + 4, 38) == SequoiaWCL::ScanDelivery::Enqueue::Queued);
    uint8_t iteration[28] = {};
    iteration[0] = 2; iteration[1] = 1;
    iteration[12] = 1; iteration[13] = 1;
    iteration[20] = 36;
    driver.iwm_scan_trial_rx(IWM_SCAN_ITERATION_COMPLETE, iteration, sizeof(iteration));
    uint8_t terminal[12] = {}; terminal[2] = 1;
    driver.iwm_scan_trial_rx(IWM_SCAN_OFFLOAD_COMPLETE, terminal, sizeof(terminal));
    ItlScan::ScanNotification notification;
    ItlScan::Outcome outcome;
    assert(ItlScan::decodeIwmNotification(ItlScan::IwmNotification::LmacComplete, terminal, sizeof(terminal), notification));
    assert(ItlScan::terminalOutcome(notification, outcome));
    assert(delivery.terminal(notification.firmware, notification.uid, outcome));
}

enum class Fault { None, DqaTimeout, QueueTimeout, StationTimeout, StationBadStatus,
                   StationShort, StationLong, ScanTimeout, TerminalTimeout, ResetAux };

static void scenario(Fault fault, bool early)
{
    ItlIwm driver;
    SequoiaWCL::ScanDelivery::Slot slots[2];
    SequoiaWCL::ScanDelivery delivery(slots, 2);
    const auto deliveryToken = delivery.begin(ItlScan::Firmware::LMAC);
    assert(deliveryToken);
    auto &sc = driver.sc;
    sc.sc_uc_current = IWM_UCODE_TYPE_REGULAR;
    sc.txq[0].cur = 18;
    sc.txq[1].qid = 1;
    sc.sc_ic.ic_channels[1] = {2412, 1}; sc.sc_ic.ic_channels[36] = {5180, 1};
    driver.nativeFirmwareTrial = true;
    auto &result = driver.nativeFirmwareDiagnostics;
    result.stage = IwmFirmwareStage::Scan;
    result.configurationResult = 0;
    result.phy.cfgBytes = result.phy.nchBytes = 8;
    result.phy.channels = result.phy.coveredChannels = 2;
    std::vector<unsigned> sent;
    std::vector<std::thread> notifications;
    driver.onKick = [&] {
        const unsigned index = (sc.txq[0].cur + 255) % 256;
        auto &slot = sc.txq[0].data[index];
        const uint8_t *wire = slot.m ? slot.m->bytes.data() : reinterpret_cast<uint8_t *>(&driver.commands[index]);
        const unsigned id = IWM_WIDE_ID(wire[1], wire[0]);
        const unsigned expected[] = {0x500, IWM_SCD_QUEUE_CFG, IWM_ADD_STA, IWM_SCAN_OFFLOAD_REQUEST_CMD};
        assert(sent.size() < 4 && id == expected[sent.size()]);
        sent.push_back(id);
        assert(wire[2] == index && wire[3] == 0);
        if (id == 0x500) {
            assert(index == 18 && wire[4] == 4 && wire[5] == 0 && wire[6] == 0 && wire[7] == 0);
            if (fault == Fault::DqaTimeout) return;
        }
        if (id == IWM_SCD_QUEUE_CFG) {
            assert(driver.nicLocks == 2);
            if (fault == Fault::QueueTimeout) return;
            if (fault == Fault::ResetAux) { driver.reset(); return; }
        }
        if (id == IWM_ADD_STA) {
            assert(driver.nicLocks == 2 && sc.sc_cmd_resp_len[index] == 12);
            if (fault == Fault::StationTimeout) return;
        }
        if (id == IWM_SCAN_OFFLOAD_REQUEST_CMD) {
            assert(slot.m && liveMbufs == 1 && driver.nicLocks == 1);
            const auto *scan = reinterpret_cast<const iwm_scan_req_lmac *>(wire + 4);
            assert(scan->n_channels == 2 && scan->iter_num == 1);
            if (early && fault != Fault::TerminalTimeout) notifyScan(driver, delivery, deliveryToken);
            if (fault == Fault::ScanTimeout) return;
        }
        size_t bytes = id == IWM_ADD_STA ? 12 : 8;
        if (id == IWM_ADD_STA && fault == Fault::StationShort) bytes = 11;
        if (id == IWM_ADD_STA && fault == Fault::StationLong) bytes = 13;
        std::vector<uint8_t> reply(bytes, 0);
        auto *packet = reinterpret_cast<iwm_rx_packet *>(reply.data());
        packet->len_n_flags = bytes - 4;
        packet->hdr = {static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(index), 0};
        if (id == IWM_ADD_STA && bytes >= 12) {
            uint32_t status = fault == Fault::StationBadStatus ? 0 : IWM_ADD_STA_SUCCESS;
            memcpy(packet->data, &status, sizeof(status));
        }
        driver.iwm_command_ack(&sc, packet, bytes);
        if (id == IWM_SCAN_OFFLOAD_REQUEST_CMD && !early && fault != Fault::TerminalTimeout) {
            notifications.emplace_back([&] { driver.gated([&] { notifyScan(driver, delivery, deliveryToken); }); });
        }
    };
    int error = -1;
    driver.gated([&] { error = driver.iwm_passive_scan_trial(&sc); });
    for (auto &thread : notifications) thread.join();
    const int expected = fault == Fault::None ? 0 : fault == Fault::ResetAux ? ENXIO :
        (fault == Fault::StationBadStatus || fault == Fault::StationShort || fault == Fault::StationLong) ? EIO : ETIMEDOUT;
    assert(error == expected);
    assert(delivery.submitted(deliveryToken, error ? ItlScan::Submission::Indeterminate : ItlScan::Submission::Accepted));
    assert(!driver.nativePassiveRequest && !driver.nativePassiveRequestBytes && responseAllocations.empty());
    if (fault == Fault::ScanTimeout) {
        assert(liveMbufs == 1 && sc.txq[0].queued == 1 && driver.nicLocks == 1);
    } else assert(!liveMbufs);
    if (fault == Fault::None) {
        assert(sent.size() == 4 && result.scanDqaResult == 0 && result.scanAuxResult == 0);
        assert(result.scanSubmitResult == 0 && result.scanWaitResult == 0 && driver.nativePassiveScan.completeCoverage());
        assert(driver.nativePassiveScan.evidence.phy == 1 && driver.nativePassiveScan.evidence.beacons == 1);
        assert(driver.nativePassiveScan.evidence.phyLastBytes == 88 && !driver.nativePassiveScan.evidence.malformed);
        std::vector<unsigned> delivered;
        bool reject = true;
        auto post = [&](uint32_t kind, const void *data, size_t size) {
            if (reject) { reject = false; return false; }
            delivered.push_back(kind);
            if (kind == SequoiaWCL::kScanResultMessage) {
                assert(size >= sizeof(SequoiaWCL::BeaconMetadata));
                const auto *record = static_cast<const uint8_t *>(data);
                SequoiaWCL::BeaconMetadata metadata = {};
                memcpy(&metadata, record, sizeof(metadata));
                assert(metadata.ieLength == size - sizeof(metadata));
                assert(metadata.ssidLength <= sizeof(metadata.ssid));
            }
            else { uint32_t status; assert(size == 4); memcpy(&status, data, 4); assert(!status); }
            return true;
        };
        auto unmapped = [](const ItlScan::Completion &, bool, uint32_t &) { return false; };
        assert(delivery.deliverOne(post, unmapped) == SequoiaWCL::ScanDelivery::Delivery::Retry);
        assert(delivery.deliverOne(post, unmapped) == SequoiaWCL::ScanDelivery::Delivery::Result);
        assert(delivery.deliverOne(post, unmapped) == SequoiaWCL::ScanDelivery::Delivery::Complete);
        assert((delivered == std::vector<unsigned>{SequoiaWCL::kScanResultMessage,
                                                    SequoiaWCL::kScanCompleteMessage}) &&
               !delivery.busy());
    }
    if (fault == Fault::DqaTimeout) assert(result.scanAuxResult == UINT32_MAX && sent.size() == 1);
    if (fault == Fault::QueueTimeout || fault == Fault::ResetAux) assert(result.scanSubmitResult == UINT32_MAX && sent.size() == 2);
    if (fault == Fault::StationTimeout || fault == Fault::StationBadStatus || fault == Fault::StationShort || fault == Fault::StationLong)
        assert(result.scanSubmitResult == UINT32_MAX && sent.size() == 3);
    driver.gated([&] {
        driver.reset();
        driver.nativePassiveScan.hardwareStopped();
        assert(delivery.hardwareStopped() && delivery.discardForDetach());
        assert(!sc.txq[0].queued && !driver.nicLocks && !liveMbufs);
        assert(driver.iwm_passive_scan_trial(&sc) == EPERM);
    });
}

int main()
{
    for (bool early : {false, true})
        for (auto fault : {Fault::None, Fault::DqaTimeout, Fault::QueueTimeout, Fault::StationTimeout,
                           Fault::StationBadStatus, Fault::StationShort, Fault::StationLong,
                           Fault::ScanTimeout, Fault::TerminalTimeout, Fault::ResetAux})
            scenario(fault, early);
    puts("Production DQA -> queue -> station/status -> DMA scan -> terminal: 20 integrated scenarios passed");
}
