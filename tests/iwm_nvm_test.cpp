#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <HAL/IwmNvmDiagnostics.h>

#define htole16(value) (value)
#define le16toh(value) (value)
constexpr unsigned IWM_NVM_DEFAULT_CHUNK_SIZE = 2048, IWM_NVM_READ_OPCODE = 0;
constexpr unsigned IWM_NVM_ACCESS_CMD = 0x88, IWM_CMD_WANT_RESP = 2;
constexpr unsigned IWM_CMD_SEND_IN_RFKILL = 4, IWM_CMD_RESP_MAX = 4096, IWM_CMD_FAILED_MSK = 0x40;
constexpr int IWM_DEVICE_FAMILY_7000 = 7000, IWM_DEVICE_FAMILY_8000 = 8000;
struct iwm_softc { int sc_device_family = IWM_DEVICE_FAMILY_7000; };
struct iwm_nvm_access_cmd { uint8_t op_code, target; uint16_t type, offset, length; };
struct iwm_nvm_access_resp { uint16_t offset, length, type, status; uint8_t data[0]; };
struct iwm_rx_packet { size_t payload; struct { uint8_t flags; } hdr; alignas(8) uint8_t data[4096]; };
static size_t iwm_rx_packet_payload_len(const iwm_rx_packet *packet) { return packet->payload; }
struct iwm_host_cmd {
    unsigned id, flags;
    size_t resp_pkt_len;
    const void *data[2];
    uint16_t len[2];
    iwm_rx_packet *resp_pkt;
};
struct ItlIwm {
    bool nativeNvmTrial = true;
    bool isHardwareTrial() const { return nativeNvmTrial; }
    IwmNvmDiagnostics nativeNvmDiagnostics;
    std::vector<uint8_t> source;
    std::vector<uint16_t> offsets;
    unsigned freed = 0, fault = 0;
    iwm_rx_packet packet = {};
    int iwm_send_cmd(iwm_softc *, iwm_host_cmd *command)
    {
        const auto *request = static_cast<const iwm_nvm_access_cmd *>(command->data[0]);
        assert(command->id == IWM_NVM_ACCESS_CMD && request->op_code == 0 && request->target == 0);
        offsets.push_back(request->offset);
        if (fault == 1) return ETIMEDOUT;
        packet = {};
        auto *response = reinterpret_cast<iwm_nvm_access_resp *>(packet.data);
        response->offset = request->offset;
        response->type = request->type;
        response->length = request->offset >= source.size() ? 0 :
            std::min<size_t>(request->length, source.size() - request->offset);
        if (response->length) memcpy(response->data, source.data() + request->offset, response->length);
        packet.payload = sizeof(*response) + response->length;
        if (fault == 2) packet.payload = 7;
        if (fault == 3) ++response->offset;
        if (fault == 4) ++response->type;
        if (fault == 5) response->length = request->length + 1;
        if (fault == 6) packet.payload = sizeof(*response);
        if (fault == 7) packet.hdr.flags = IWM_CMD_FAILED_MSK;
        if (fault == 8 || (fault == 9 && request->offset >= source.size())) response->status = 1;
        if (fault == 10 || (fault == 11 && request->offset >= source.size())) {
            // A status-only error has no usable data span or echoed read fields.
            response->status = 1;
            response->offset = 0xffff;
            response->type = 0xffff;
            response->length = 0xffff;
            packet.payload = sizeof(*response);
        }
        if (fault == 12) {
            // d3 hardware evidence: status-only missing optional section.
            response->status = 256;
            response->length = request->length;
            packet.payload = sizeof(*response);
        }
        command->resp_pkt = &packet;
        return 0;
    }
    void iwm_free_resp(iwm_softc *, iwm_host_cmd *command) { ++freed; command->resp_pkt = nullptr; }
    int iwm_nvm_read_chunk(iwm_softc *, uint16_t, uint16_t, uint16_t, uint8_t *, uint16_t *);
    int iwm_nvm_read_section(iwm_softc *, uint16_t, uint8_t *, uint16_t *, size_t);
};
#include "../itlwm/hal_iwm/nvm_sequoia.inc"

int main()
{
    IwmNvmDiagnostics diagnostics;
    const uint8_t alive[] = {0x34, 0x12};
    diagnostics.recordAlive(alive, 1);
    assert(diagnostics.aliveBytes == 1 && diagnostics.aliveStatus == UINT32_MAX);
    diagnostics.recordAlive(alive, sizeof(alive));
    assert(diagnostics.aliveStatus == 0x1234);
    diagnostics.recordAlive(nullptr, 0);
    assert(diagnostics.aliveStatus == UINT32_MAX);
    ItlIwm driver;
    for (unsigned i = 0; i < 5000; ++i) driver.source.push_back(static_cast<uint8_t>(i / 37));
    std::vector<uint8_t> output(6000, 0xa5);
    uint16_t length = 0;
    assert(!driver.iwm_nvm_read_section(nullptr, 1, output.data(), &length, output.size()));
    assert(length == 5000 && driver.offsets == std::vector<uint16_t>({0, 2048, 4096}));
    assert(std::equal(driver.source.begin(), driver.source.end(), output.begin()));
    assert(output[5000] == 0xa5);
    assert(driver.nativeNvmDiagnostics.bytesRead == 5000);
    assert(driver.nativeNvmDiagnostics.section == 1 && driver.nativeNvmDiagnostics.offset == 4096);
    assert(driver.nativeNvmDiagnostics.responseLength == 904 && driver.nativeNvmDiagnostics.nvmError == 0);
    for (driver.fault = 1; driver.fault <= 8; ++driver.fault) {
        const unsigned freed = driver.freed;
        std::fill(output.begin(), output.end(), 0xa5);
        const int expected = driver.fault == 1 ? ETIMEDOUT : (driver.fault == 8 ? ENOENT : EIO);
        assert(driver.iwm_nvm_read_section(nullptr, 1, output.data(), &length, output.size()) == expected);
        assert(length == 0 && output[0] == 0xa5);
        assert(driver.freed == freed + (driver.fault != 1));
        assert(driver.nativeNvmDiagnostics.nvmError == expected);
        if (driver.fault == 1) {
            assert(!strcmp(driver.nativeNvmDiagnostics.nvmCheck, "transport"));
            assert(driver.nativeNvmDiagnostics.responseStatus == UINT32_MAX);
        }
    }
    driver.fault = 0;
    assert(!driver.iwm_nvm_read_section(nullptr, 1, output.data(), &length, 4097));
    assert(length == 4097 && output[4097] == 0xa5);
    assert(driver.iwm_nvm_read_section(nullptr, 1, output.data(), &length, 65536) == EINVAL);
    driver.source.resize(2048);
    assert(!driver.iwm_nvm_read_section(nullptr, 1, output.data(), &length, output.size()));
    assert(length == 2048);
    driver.fault = 9;
    assert(!driver.iwm_nvm_read_section(nullptr, 1, output.data(), &length, output.size()));
    assert(length == 2048);
    for (driver.fault = 10; driver.fault <= 11; ++driver.fault) {
        const unsigned freed = driver.freed;
        std::fill(output.begin(), output.end(), 0xa5);
        const bool missing = driver.fault == 10;
        assert(driver.iwm_nvm_read_section(nullptr, 1, output.data(), &length, output.size()) ==
               (missing ? ENOENT : 0));
        assert(length == (missing ? 0 : 2048));
        assert(driver.freed == freed + (missing ? 1 : 2));
        assert(!strcmp(driver.nativeNvmDiagnostics.nvmCheck, "firmware-status"));
        assert(driver.nativeNvmDiagnostics.responseStatus == 1);
        assert(driver.nativeNvmDiagnostics.responseLength == 0xffff);
        assert(std::all_of(output.begin() + length, output.end(), [](uint8_t byte) { return byte == 0xa5; }));
        if (!missing) assert(std::equal(driver.source.begin(), driver.source.end(), output.begin()));
    }
    const auto retained = driver.nativeNvmDiagnostics.bytesRead;
    driver.fault = 12;
    std::fill(output.begin(), output.end(), 0xa5);
    assert(driver.iwm_nvm_read_section(nullptr, 12, output.data(), &length, output.size()) == ENOENT);
    assert(length == 0 && driver.nativeNvmDiagnostics.responseStatus == 256);
    assert(driver.nativeNvmDiagnostics.responseLength == 2048 && driver.nativeNvmDiagnostics.payloadBytes == 8);
    assert(driver.nativeNvmDiagnostics.bytesRead == retained);
    assert(std::all_of(output.begin(), output.end(), [](uint8_t byte) { return byte == 0xa5; }));
    driver.nativeNvmTrial = false;
    driver.fault = 0;
    assert(!driver.iwm_nvm_read_section(nullptr, 1, output.data(), &length, output.size()));
    assert(driver.nativeNvmDiagnostics.bytesRead == retained);
    puts("Production IWM NVM: multi-chunk offsets, bounds, truncation, identity and timeout tests passed");
}
