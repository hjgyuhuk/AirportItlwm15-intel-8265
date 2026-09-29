#include "iwm_command_test_support.h"

static iwm_host_cmd request(uint32_t flags = 0)
{
    iwm_host_cmd command;
    command.id = 0x132;
    command.flags = flags;
    if (flags & IWM_CMD_WANT_RESP) command.resp_pkt_len = 16;
    return command;
}

static void immediateAndAsync()
{
    ItlIwm driver;
    auto command = request(IWM_CMD_WANT_RESP);
    driver.onKick = [&] { driver.acknowledge(0, command.id, 0, 16); };
    assert(driver.iwm_send_cmd(&driver.sc, &command) == 0);
    assert(command.resp_pkt && driver.loop.sleeps == 0);
    assert(driver.sc.txq[0].queued == 0 && driver.nicLocks == 0);
    testResponseFree(command.resp_pkt);
    driver.onKick = nullptr;
    command = request(IWM_CMD_ASYNC);
    assert(driver.iwm_send_cmd(&driver.sc, &command) == 0);
    driver.gated([&] {
        assert(driver.sc.txq[0].queued == 1 && driver.nicLocks == 1);
        driver.acknowledge(1, 0x133);
        assert(driver.sc.txq[0].queued == 1);
        driver.acknowledge(1, command.id);
        driver.acknowledge(1, command.id);
        assert(driver.sc.txq[0].queued == 0 && driver.nicLocks == 0);
    });
}

static void failuresBeforeSubmission()
{
    ItlIwm driver;
    std::vector<uint8_t> bytes(400, 0xab);
    auto command = request(IWM_CMD_WANT_RESP);
    command.data[0] = bytes.data(); command.len[0] = bytes.size();
    const auto rejected = [&](int expected) {
        assert(driver.iwm_send_cmd(&driver.sc, &command) == expected);
        assert(driver.sc.txq[0].cur == 0 && driver.sc.txq[0].queued == 0);
        assert(driver.submissions == 0 && liveMbufs == 0 && responseAllocations.empty());
    };
    failResponse = true; rejected(ENOMEM); failResponse = false;
    failMbuf = true; rejected(ENOMEM); failMbuf = false;
    driver.cursor.segments = 0; rejected(ENOBUFS); driver.cursor.segments = 1;
    driver.cursor.shortSegment = true; rejected(ENOBUFS); driver.cursor.shortSegment = false;
    driver.cursor.address = 1ULL << 36; rejected(EFBIG); driver.cursor.address = 0x20000;
    driver.nicAvailable = false; rejected(EBUSY); driver.nicAvailable = true;
    command.data[0] = nullptr; rejected(EINVAL); command.data[0] = bytes.data();
    command.len[0] = IWM_MAX_CMD_PAYLOAD_SIZE; rejected(EINVAL); command.len[0] = bytes.size();
    command.flags |= IWM_CMD_ASYNC; rejected(EINVAL); command.flags &= ~IWM_CMD_ASYNC;
    eventThread = true; rejected(EWOULDBLOCK); eventThread = false;
    driver.sc.sc_flags = IWM_FLAG_SHUTDOWN; rejected(ENXIO); driver.sc.sc_flags = 0;
    driver.onKick = [&] { driver.acknowledge(0, command.id, 0, 16); };
    assert(driver.iwm_send_cmd(&driver.sc, &command) == 0);
    testResponseFree(command.resp_pkt);
}

static void failedResponses()
{
    for (int failure = 0; failure < 3; ++failure) {
        ItlIwm driver;
        auto command = request(IWM_CMD_WANT_RESP);
        driver.onKick = [&] { driver.acknowledge(0, command.id,
            failure == 0 ? EIO : 0, failure == 1 ? 17 : (failure == 2 ? 7 : 16)); };
        assert(driver.iwm_send_cmd(&driver.sc, &command) == EIO);
        assert(!command.resp_pkt && responseAllocations.empty());
        assert(driver.sc.txq[0].queued == 0);
    }
}

static void timeoutAndLateResponse()
{
    ItlIwm driver;
    std::vector<uint8_t> bytes(400);
    auto command = request(IWM_CMD_WANT_RESP);
    command.data[0] = bytes.data(); command.len[0] = bytes.size();
    assert(driver.iwm_send_cmd(&driver.sc, &command) == ETIMEDOUT);
    assert(!command.resp_pkt && responseAllocations.empty() && liveMbufs == 1);
    driver.gated([&] {
        assert(driver.sc.txq[0].queued == 1 && driver.nicLocks == 1);
        driver.sc.txq[0].cur = 0;
        assert(driver.iwm_send_cmd(&driver.sc, &command) == ENOSPC);
        driver.acknowledge(0, command.id, 0, 16);
        assert(liveMbufs == 0 && driver.sc.txq[0].queued == 0);
    });
}

static void waitingAcrossEvents()
{
    for (int event = 0; event < 4; ++event) {
        ItlIwm driver;
        auto command = request(IWM_CMD_WANT_RESP);
        std::promise<void> kicked;
        driver.onKick = [&] { kicked.set_value(); };
        auto result = std::async(std::launch::async, [&] { return driver.iwm_send_cmd(&driver.sc, &command); });
        kicked.get_future().wait();
        driver.gated([&] {
            driver.loop.commandWakeup(nullptr, false);
            if (event == 0) driver.acknowledge(0, command.id, 0, 16);
            if (event == 1) driver.reset();
            if (event == 2) driver.loop.interrupt = THREAD_INTERRUPTED;
            if (event == 3) {
                driver.reset();
                driver.commandTransportReady = true; // Simulated next firmware ALIVE + queue setup.
                driver.onKick = nullptr;
                auto newCommand = request(IWM_CMD_ASYNC);
                assert(driver.iwm_send_cmd(&driver.sc, &newCommand) == 0);
                driver.acknowledge(0, newCommand.id);
            }
        });
        assert(result.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        assert(result.get() == (event == 0 ? 0 : (event == 2 ? EINTR : ENXIO)));
        if (command.resp_pkt) testResponseFree(command.resp_pkt);
    }
}

static void concurrentSubmitters()
{
    ItlIwm driver;
    driver.onKick = [&] {
        const unsigned index = (driver.sc.txq[0].cur + IWM_TX_RING_COUNT - 1) % IWM_TX_RING_COUNT;
        driver.acknowledge(index, 0x132);
    };
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 8; ++i) threads.emplace_back([&] {
        for (unsigned j = 0; j < 1000; ++j) {
            auto command = request(IWM_CMD_ASYNC);
            assert(driver.iwm_send_cmd(&driver.sc, &command) == 0);
        }
    });
    for (auto &thread : threads) thread.join();
    assert(driver.submissions == 8000 && driver.sc.txq[0].queued == 0 && driver.nicLocks == 0);
}

static void firmwareWaitPredicates()
{
    ItlIwm driver;
    int status = 3;
    driver.gated([&] {
        assert(driver.iwm_wait_status(&status, 3, &status, 1) == 0);
        assert(driver.loop.sleeps == 0);
    });
    assert(driver.iwm_wait_status(&status, 3, &status, 1) == EWOULDBLOCK);
    for (int reset = 0; reset < 2; ++reset) {
        status = 1;
        std::promise<void> entered;
        auto future = std::async(std::launch::async, [&] {
            int result = -1;
            driver.gated([&] {
                entered.set_value();
                result = driver.iwm_wait_status(&status, 3, &status, 2);
            });
            return result;
        });
        entered.get_future().wait();
        driver.gated([&] {
            if (reset) driver.reset();
            else status = 3;
            driver.loop.commandWakeup(&status, false);
        });
        assert(future.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        assert(future.get() == (reset ? ENXIO : 0));
    }
    auto command = request(IWM_CMD_ASYNC);
    assert(driver.iwm_send_cmd(&driver.sc, &command) == ENXIO);
    assert(driver.submissions == 0);
}

static void nvmTrialCommandPolicy()
{
    ItlIwm driver;
    driver.nativeNvmTrial = true;
    auto command = request(IWM_CMD_ASYNC);
    assert(driver.iwm_send_cmd(&driver.sc, &command) == EPERM);
    uint8_t nvmRequest[8] = {};
    command.id = IWM_NVM_ACCESS_CMD;
    command.data[0] = nvmRequest;
    command.len[0] = sizeof(nvmRequest);
    driver.onKick = [&] { driver.acknowledge(0, command.id); };
    assert(!driver.iwm_send_cmd(&driver.sc, &command));
    assert(driver.nativeNvmDiagnostics.lastCommand == IWM_NVM_ACCESS_CMD);
    assert(driver.nativeNvmDiagnostics.ackResult == 0 && driver.nativeNvmDiagnostics.ackFlags == 0);
    assert(driver.nativeNvmDiagnostics.ackBytes == sizeof(iwm_rx_packet));
    nvmRequest[0] = 1;
    assert(driver.iwm_send_cmd(&driver.sc, &command) == EPERM);
    assert(driver.submissions == 1);
    assert(driver.nativeNvmDiagnostics.ackResult == UINT32_MAX && driver.nativeNvmDiagnostics.ackBytes == 0);
    nvmRequest[0] = 0;
    command.flags = IWM_CMD_WANT_RESP;
    command.resp_pkt_len = 16;
    driver.onKick = [&] { driver.acknowledge(1, command.id, EIO, 16); };
    assert(driver.iwm_send_cmd(&driver.sc, &command) == EIO);
    assert(driver.nativeNvmDiagnostics.ackResult == EIO);
    assert(driver.nativeNvmDiagnostics.ackFlags == IWM_CMD_FAILED_MSK && driver.nativeNvmDiagnostics.ackBytes == 16);
    driver.gated([&] { driver.reset(); });
    assert(driver.nativeNvmDiagnostics.ackResult == EIO);
}

static void firmwareWaitFailure()
{
    ItlIwm driver;
    int status = 3, failure = EINVAL;
    driver.gated([&] {
        assert(driver.iwm_wait_status(&status, 3, &status, 1, &failure) == EINVAL);
        assert(driver.loop.sleeps == 0);
    });
    for (int event = 0; event < 4; ++event) {
        status = 0;
        failure = 0;
        driver.nativeCalibrationActive = true;
        std::promise<void> entered;
        auto future = std::async(std::launch::async, [&] {
            int result = -1;
            driver.gated([&] {
                entered.set_value();
                result = driver.iwm_wait_status(&status, 3, &status, 2, &failure);
            });
            return result;
        });
        entered.get_future().wait();
        driver.gated([&] {
            failure = ENOMEM;
            if (event == 1) status = 3;
            if (event == 2) {
                driver.reset();
                assert(!driver.nativeCalibrationActive);
            }
            if (event == 3) driver.loop.interrupt = THREAD_INTERRUPTED;
            driver.loop.commandWakeup(&status, false);
        });
        assert(future.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        assert(future.get() == (event == 2 ? ENXIO : ENOMEM));
    }
}

static void wideWireAndDiagnostics()
{
    ItlIwm driver;
    uint32_t payload = 0;
    iwm_host_cmd command;
    command.id = 0x500;
    command.data[0] = &payload;
    command.len[0] = sizeof(payload);
    // Exercise the actual transport without the firmware-trial command policy.
    driver.onKick = [&] {
        const uint8_t expected[] = {0, 5, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0};
        assert(!memcmp(&driver.commands[0], expected, sizeof(expected)));
        assert((driver.descriptors[0].tbs[0].hi_n_len >> 4) == sizeof(expected));
        driver.acknowledge(0, 0x000); // A missing group cannot complete a wide command.
        assert(driver.sc.txq[0].queued == 1);
        driver.acknowledge(1, 0x500); // Nor can a different slot.
        assert(driver.sc.txq[0].queued == 1);
        driver.acknowledge(0, 0x500);
    };
    assert(driver.iwm_send_cmd(&driver.sc, &command) == 0);

    driver.nativeNvmTrial = true;
    command = {};
    command.id = IWM_BT_CONFIG;
    driver.onKick = [&] { driver.acknowledge(1, 0x500); };
    assert(driver.iwm_send_cmd(&driver.sc, &command) == ETIMEDOUT);
    const auto &diag = driver.nativeNvmDiagnostics;
    assert(diag.commandIndex == 1 && diag.commandQueue == 0 && diag.commandBytes == 4);
    assert(diag.ackSeen == 1 && diag.ackRejected == 1 && diag.rawAckCommand == 0x500);
    assert(diag.rawAckSequence == 1 && diag.ackResult == UINT32_MAX && diag.ackBytes == 0);
    assert(diag.timeoutQueued == 1 && diag.timeoutFlags == 0);
    driver.gated([&] { driver.reset(); });
    assert(diag.ackRejected == 1 && diag.timeoutQueued == 1);

    IwmNvmDiagnostics rx;
    uint8_t error[12] = {0x12, 0x34, 0x56, 0x78, 0x00, 0, 0x21, 0x05};
    for (size_t size = 0; size < sizeof(error); ++size) {
        rx.beginCommand(0x500);
        rx.recordRx(2, 0x8001, error, size);
        assert(rx.firmwareErrors == 1 && rx.firmwareErrorType == UINT32_MAX);
    }
    rx.recordRx(2, 0x8001, error, sizeof(error));
    assert(rx.firmwareErrorType == 0x78563412 && rx.firmwareErrorCommand == 0);
    assert(rx.firmwareErrorSequence == 0x521 && rx.rxCommand == 2 && rx.rxSequence == 0x8001);
    rx.rxPackets = UINT32_MAX;
    rx.recordRx(0x500, 0x21, nullptr, 0);
    assert(rx.rxPackets == UINT32_MAX);
    rx.beginCommand(0x51);
    assert(!rx.rxPackets && !rx.firmwareErrors && rx.firmwareErrorType == UINT32_MAX);
}

static void observedDqaResponse()
{
    for (bool failed : {false, true}) {
        ItlIwm driver;
        driver.sc.txq[0].cur = 18;
        iwm_host_cmd command;
        uint32_t payload = 0;
        command.id = 0x500;
        command.data[0] = &payload;
        command.len[0] = sizeof(payload);
        driver.onKick = [&] {
            // Replay d8's observed group=5, opcode=0, queue=0, index=18.
            iwm_rx_packet packet = {};
            packet.hdr = {0, static_cast<uint8_t>(failed ? 0x45 : 5), 18, 0};
            driver.iwm_command_ack(&driver.sc, &packet, sizeof(packet));
        };
        assert(driver.iwm_send_cmd(&driver.sc, &command) == (failed ? EIO : 0));
        assert(!driver.loop.sleeps && !driver.sc.txq[0].queued && !driver.nicLocks);
    }
    for (unsigned group = 0; group < 64; ++group)
        for (unsigned opcode = 0; opcode < 256; ++opcode) {
            assert(IWM_WIDE_ID(group & ~IWM_CMD_FAILED_MSK, opcode) == (group * 256 + opcode));
            assert(IWM_WIDE_ID((group | IWM_CMD_FAILED_MSK) & ~IWM_CMD_FAILED_MSK, opcode) == (group * 256 + opcode));
            assert(IWM_WIDE_ID(group, opcode & 0xf) == (group * 256 + (opcode & 0xf)));
        }
}

int main()
{
    immediateAndAsync();
    failuresBeforeSubmission();
    failedResponses();
    timeoutAndLateResponse();
    waitingAcrossEvents();
    concurrentSubmitters();
    firmwareWaitPredicates();
    firmwareWaitFailure();
    nvmTrialCommandPolicy();
    wideWireAndDiagnostics();
    observedDqaResponse();
    puts("Production IWM transport: allocation/ACK/timeout/reset tests and 8,000 concurrent commands passed");
}
