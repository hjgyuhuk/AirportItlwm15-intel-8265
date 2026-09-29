#include "firmware_policy_sequoia.inc"

int ItlIwm::iwm_send_cmd(struct iwm_softc *sc, struct iwm_host_cmd *hcmd)
{
    auto *loop = getMainWorkLoop();
    if (!loop || !getMainCommandGate() || !hcmd)
        return ENXIO;
    if (!loop->inGate()) {
        return loop->runAction([](OSObject *owner, void *softc, void *command, void *, void *) -> IOReturn {
            return static_cast<ItlIwm *>(owner)->iwm_send_cmd(
                static_cast<iwm_softc *>(softc), static_cast<iwm_host_cmd *>(command));
        }, this, sc, hcmd);
    }
    const bool async = (hcmd->flags & IWM_CMD_ASYNC) != 0;
    const bool response = (hcmd->flags & IWM_CMD_WANT_RESP) != 0;
    hcmd->resp_pkt = nullptr;
    if (isHardwareTrial())
        nativeNvmDiagnostics.beginCommand(hcmd->id);
    if (nativeNvmTrial) {
        // The hardware trial cannot submit scan, calibration, join, paging,
        // key or NVM-write commands even if a future caller reaches this path.
        const bool readNvm = hcmd->id == IWM_NVM_ACCESS_CMD && hcmd->data[0] &&
            hcmd->len[0] == 8 && !hcmd->len[1] &&
            static_cast<const uint8_t *>(hcmd->data[0])[0] == 0;
        if (!readNvm && hcmd->id != IWM_BT_CONFIG)
            return EPERM;
    }
    if (nativeFirmwareTrial && !iwm_firmware_trial_command_allowed(sc, hcmd))
        return EPERM;
    // Releasing the gate cannot wake an RX action if this is its own thread.
    if (!async && loop->onThread())
        return EWOULDBLOCK;
    if ((response && (async || hcmd->resp_pkt_len < sizeof(iwm_rx_packet) ||
                     hcmd->resp_pkt_len > IWM_CMD_RESP_MAX)) ||
        sc->cmdqid < 0 || sc->cmdqid >= nitems(sc->txq))
        return EINVAL;
    if (!commandTransportReady || (sc->sc_flags & (IWM_FLAG_SHUTDOWN | IWM_FLAG_HW_ERR)))
        return ENXIO;

    auto &ring = sc->txq[sc->cmdqid];
    const auto index = ring.cur;
    if (!ring.desc || !ring.cmd || index < 0 || index >= IWM_TX_RING_COUNT ||
        ring.queued >= IWM_TX_RING_COUNT - 1)
        return ENOSPC;
    size_t payload = 0;
    for (size_t i = 0; i < nitems(hcmd->len); ++i) {
        if ((hcmd->len[i] && !hcmd->data[i]) || hcmd->len[i] > IWM_MAX_CMD_PAYLOAD_SIZE - payload)
            return EINVAL;
        payload += hcmd->len[i];
    }
    IwmCommand::Token token = {};
    const uint16_t commandID = IWM_WIDE_ID(iwm_cmd_groupid(hcmd->id), iwm_cmd_opcode(hcmd->id));
    if (!commandSlots.reserve(index, commandID, !async, token))
        return ENOSPC;

    auto *desc = &ring.desc[index];
    auto *txdata = &ring.data[index];
    iwm_device_cmd *cmd = &ring.cmd[index];
    const bool wide = iwm_cmd_groupid(hcmd->id) != 0;
    const size_t header = wide ? sizeof(cmd->hdr_wide) : sizeof(cmd->hdr);
    const size_t inlineSize = wide ? sizeof(cmd->data_wide) : sizeof(cmd->data);
    const size_t total = header + payload;
    bus_addr_t physical = txdata->cmd_paddr;
    mbuf_t allocated = nullptr;
    int error = 0;

    if (total > 0xfff) {
        commandSlots.cancel(token);
        return EINVAL;
    }

    // No field in a free slot may still own a DMA or response allocation.
    if (txdata->m || sc->sc_cmd_resp_pkt[index]) {
        commandSlots.cancel(token);
        return EBUSY;
    }
    if (response) {
        sc->sc_cmd_resp_pkt[index] = static_cast<uint8_t *>(malloc(
            hcmd->resp_pkt_len, M_DEVBUF, M_NOWAIT | M_ZERO));
        if (!sc->sc_cmd_resp_pkt[index]) {
            error = ENOMEM;
            goto not_submitted;
        }
        sc->sc_cmd_resp_len[index] = hcmd->resp_pkt_len;
    }
    if (payload > inlineSize) {
        unsigned chunks = 1;
        IOPhysicalSegment segment;
        error = mbuf_allocpacket(MBUF_DONTWAIT, total, &chunks, &allocated);
        if (error || !allocated) {
            error = ENOMEM;
            goto not_submitted;
        }
        mbuf_setlen(allocated, total);
        mbuf_pkthdr_setlen(allocated, total);
        if (!txdata->map || !txdata->map->cursor) {
            error = ENOBUFS;
            goto not_submitted;
        }
        txdata->map->dm_nsegs = txdata->map->cursor->getPhysicalSegmentsWithCoalesce(allocated, &segment, 1);
        if (txdata->map->dm_nsegs != 1 || segment.length < total) {
            error = ENOBUFS;
            goto not_submitted;
        }
        physical = segment.location;
        cmd = mtod(allocated, iwm_device_cmd *);
    }
    if (physical >> 36) {
        error = EFBIG;
        goto not_submitted;
    }
    memset(cmd, 0, header);
    if (wide) {
        cmd->hdr_wide.opcode = iwm_cmd_opcode(hcmd->id);
        cmd->hdr_wide.group_id = iwm_cmd_groupid(hcmd->id);
        cmd->hdr_wide.qid = ring.qid;
        cmd->hdr_wide.idx = index;
        cmd->hdr_wide.length = htole16(payload);
        cmd->hdr_wide.version = iwm_cmd_version(hcmd->id);
    } else {
        cmd->hdr.code = iwm_cmd_opcode(hcmd->id);
        cmd->hdr.qid = ring.qid;
        cmd->hdr.idx = index;
    }
    {
        auto *bytes = reinterpret_cast<uint8_t *>(cmd) + header;
        size_t offset = 0;
        for (size_t i = 0; i < nitems(hcmd->len); ++i) {
            if (hcmd->len[i])
                memcpy(bytes + offset, hcmd->data[i], hcmd->len[i]);
            offset += hcmd->len[i];
        }
        uint32_t low = htole32(static_cast<uint32_t>(physical));
        memset(desc, 0, sizeof(*desc));
        memcpy(&desc->tbs[0].lo, &low, sizeof(low));
        desc->tbs[0].hi_n_len = htole16(iwm_get_dma_hi_addr(physical) | (total << 4));
        desc->num_tbs = 1;
    }
    if (sc->sc_device_family == IWM_DEVICE_FAMILY_7000 && !ring.queued && !iwm_nic_lock(sc)) {
        error = EBUSY;
        goto not_submitted;
    }
    txdata->m = allocated;
    allocated = nullptr;
    if (!commandSlots.publish(token)) {
        /* Do not panic if a reset raced publication of this slot. */
        error = ENXIO;
        if (txdata->m) {
            mbuf_freem(txdata->m);
            txdata->m = nullptr;
        }
        goto not_submitted;
    }
    iwm_update_sched(sc, ring.qid, index, 0, 0);
    ++ring.queued;
    ring.cur = (index + 1) % IWM_TX_RING_COUNT;
    if (isHardwareTrial()) {
        nativeNvmDiagnostics.commandQueue = ring.qid;
        nativeNvmDiagnostics.commandIndex = index;
        nativeNvmDiagnostics.commandBytes = static_cast<uint32_t>(total);
    }
    IWM_WRITE(sc, IWM_HBUS_TARG_WRPTR, ring.qid << 8 | ring.cur);
    if (async)
        return 0;
    {
        AbsoluteTime deadline;
        clock_interval_to_deadline(2, kSecondScale, &deadline);
        // Predicate, sleep registration and ACK all share the workloop gate.
        // A wakeup is not completion; reuse one absolute deadline on spurious wakes.
        while (commandSlots.state(token) == IwmCommand::State::Pending) {
            int wait = getMainCommandGate()->commandSleep(desc, deadline, THREAD_INTERRUPTIBLE);
            if (commandSlots.state(token) != IwmCommand::State::Pending)
                break;
            if (wait != THREAD_AWAKENED) {
                if (isHardwareTrial()) {
                    nativeNvmDiagnostics.timeoutFlags = sc->sc_flags;
                    nativeNvmDiagnostics.timeoutQueued = ring.queued;
                }
                commandSlots.abandon(token);
                ::free(sc->sc_cmd_resp_pkt[index]);
                sc->sc_cmd_resp_pkt[index] = nullptr;
                sc->sc_cmd_resp_len[index] = 0;
                // DMA remains owned by the slot until ACK or hardware reset.
                return wait == THREAD_TIMED_OUT ? ETIMEDOUT : EINTR;
            }
        }
    }
    if (!commandSlots.consume(token, error))
        return ENXIO; // A hardware reset invalidated this host token.
    if (!error) {
        hcmd->resp_pkt = reinterpret_cast<iwm_rx_packet *>(sc->sc_cmd_resp_pkt[index]);
    } else {
        ::free(sc->sc_cmd_resp_pkt[index]);
    }
    sc->sc_cmd_resp_pkt[index] = nullptr;
    sc->sc_cmd_resp_len[index] = 0;
    return error;

not_submitted:
    if (allocated)
        mbuf_freem(allocated);
    ::free(sc->sc_cmd_resp_pkt[index]);
    sc->sc_cmd_resp_pkt[index] = nullptr;
    sc->sc_cmd_resp_len[index] = 0;
    commandSlots.cancel(token);
    return error;
}

void ItlIwm::iwm_command_ack(struct iwm_softc *sc, const iwm_rx_packet *packet, size_t length)
{
    _KASSERT(getMainWorkLoop()->inGate());
    const auto index = packet->hdr.idx;
    const uint16_t command = IWM_WIDE_ID(packet->hdr.flags & ~IWM_CMD_FAILED_MSK, packet->hdr.code);
    const bool accepted = packet->hdr.qid == sc->cmdqid && commandSlots.accepts(index, command);
    if (isHardwareTrial())
        nativeNvmDiagnostics.recordAck(IWM_WIDE_ID(packet->hdr.flags, packet->hdr.code),
            (uint16_t(packet->hdr.qid) << 8) | index, accepted);
    if (!accepted)
        return;
    auto &ring = sc->txq[sc->cmdqid];
    int error = (packet->hdr.flags & IWM_CMD_FAILED_MSK) ? EIO : 0;
    if (sc->sc_cmd_resp_pkt[index]) {
        if (length < sizeof(*packet) || length > sc->sc_cmd_resp_len[index])
            error = EIO;
        else if (!error)
            memcpy(sc->sc_cmd_resp_pkt[index], packet, length);
    }
    if (isHardwareTrial()) {
        nativeNvmDiagnostics.ackResult = error;
        nativeNvmDiagnostics.ackFlags = packet->hdr.flags;
        nativeNvmDiagnostics.ackBytes = static_cast<uint32_t>(length);
    }
    if (ring.data[index].m) {
        mbuf_freem(ring.data[index].m);
        ring.data[index].m = nullptr;
    }
    const auto result = commandSlots.acknowledge(index, command, error);
    if (result == IwmCommand::Ack::Ignored || ring.queued <= 0) {
        /* Duplicate or stale firmware ACK; ownership was already reclaimed. */
        return;
    }
    if (--ring.queued == 0 && sc->sc_device_family == IWM_DEVICE_FAMILY_7000)
        iwm_nic_unlock(sc);
    if (result == IwmCommand::Ack::Wake)
        getMainCommandGate()->commandWakeup(&ring.desc[index], false);
}

void ItlIwm::iwm_command_reset(struct iwm_softc *sc)
{
    _KASSERT(getMainWorkLoop()->inGate());
    commandTransportReady = false;
    nativeCalibrationActive = false;
    if (commandEpoch == UINT64_MAX) {
        /* Keep reset recoverable; all outstanding tokens were just drained. */
        XYLog("%s: command epoch wrapped after reset\n", __FUNCTION__);
        commandEpoch = 1;
    } else {
        ++commandEpoch;
    }
    commandSlots.resetAfterDrain();
    for (size_t i = 0; i < IWM_TX_RING_COUNT; ++i) {
        ::free(sc->sc_cmd_resp_pkt[i]);
        sc->sc_cmd_resp_pkt[i] = nullptr;
        sc->sc_cmd_resp_len[i] = 0;
        if (sc->cmdqid >= 0 && sc->cmdqid < nitems(sc->txq) && sc->txq[sc->cmdqid].desc)
            getMainCommandGate()->commandWakeup(&sc->txq[sc->cmdqid].desc[i], false);
    }
    getMainCommandGate()->commandWakeup(&sc->sc_fw, false);
    getMainCommandGate()->commandWakeup(&sc->sc_uc, false);
    getMainCommandGate()->commandWakeup(&sc->sc_init_complete, false);
    getMainCommandGate()->commandWakeup(&nativePassiveDone, false);
    getMainCommandGate()->commandWakeup(&nativeScanDone, false);
}

int ItlIwm::iwm_wait_status(const int *status, int mask, void *event, unsigned seconds, const int *failure)
{
    if (!status || !mask || !getMainWorkLoop()->inGate() || getMainWorkLoop()->onThread())
        return EWOULDBLOCK;
    const uint64_t epoch = commandEpoch;
    AbsoluteTime deadline;
    clock_interval_to_deadline(seconds, kSecondScale, &deadline);
    while (true) {
        if (failure && *failure)
            return *failure;
        if ((*status & mask) == mask)
            return 0;
        int wait = getMainCommandGate()->commandSleep(event, deadline, THREAD_INTERRUPTIBLE);
        if (commandEpoch != epoch)
            return ENXIO;
        if (failure && *failure)
            return *failure;
        if ((*status & mask) == mask)
            return 0;
        if (wait != THREAD_AWAKENED)
            return wait == THREAD_TIMED_OUT ? ETIMEDOUT : EINTR;
    }
}
