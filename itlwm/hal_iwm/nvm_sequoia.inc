// The 8000 family (8260/8265, firmware 36) numbers its NVM sections in a
// 0x5A00-based id space, while the 7000 family (7260/7265, firmware 17)
// uses the section id verbatim. Measured on a 8265 with the NVM probe:
// 1->0x5A01, 3->0x5A03, 4->0x5A04, 5->0x5A05, 10->0x5A0A, 12->0x5A0C, so
// the mapping is a fixed base rather than a per-section table. The 9000
// family keeps its own space and is not remapped.
static inline uint16_t iwm_nvm_wire_section(struct iwm_softc *sc, uint16_t section)
{
    if (sc && sc->sc_device_family == IWM_DEVICE_FAMILY_8000)
        return uint16_t(section + 0x5A00);
    return section;
}

int ItlIwm::iwm_nvm_read_chunk(struct iwm_softc *sc, uint16_t section,
    uint16_t offset, uint16_t length, uint8_t *data, uint16_t *len)
{
    if (!data || !len || !length || length > IWM_NVM_DEFAULT_CHUNK_SIZE ||
        static_cast<size_t>(offset) + length > UINT16_MAX)
        return EINVAL;
    *len = 0;
    auto *diagnostics = isHardwareTrial() ? &nativeNvmDiagnostics : nullptr;
    if (diagnostics) diagnostics->beginRead(section, offset, length);
    const uint16_t wireSection = iwm_nvm_wire_section(sc, section);
    struct iwm_nvm_access_cmd request = {};
    request.offset = htole16(offset);
    request.length = htole16(length);
    request.type = htole16(wireSection);
    request.op_code = IWM_NVM_READ_OPCODE;
    struct iwm_host_cmd command = {};
    command.id = IWM_NVM_ACCESS_CMD;
    command.flags = IWM_CMD_WANT_RESP | IWM_CMD_SEND_IN_RFKILL;
    command.resp_pkt_len = IWM_CMD_RESP_MAX;
    command.data[0] = &request;
    command.len[0] = sizeof(request);
    int error = iwm_send_cmd(sc, &command);
    if (error) {
        if (diagnostics) diagnostics->nvmError = error;
        return error;
    }
    const auto *packet = command.resp_pkt;
    if (diagnostics) {
        diagnostics->nvmCheck = "response-header";
        if (packet) diagnostics->payloadBytes = iwm_rx_packet_payload_len(packet);
    }
    if (!packet || (packet->hdr.flags & IWM_CMD_FAILED_MSK) ||
        iwm_rx_packet_payload_len(packet) < sizeof(iwm_nvm_access_resp)) {
        error = EIO;
    } else {
        const auto *response = reinterpret_cast<const iwm_nvm_access_resp *>(packet->data);
        const size_t bytes = le16toh(response->length);
        if (diagnostics) {
            diagnostics->responseStatus = le16toh(response->status);
            diagnostics->responseOffset = le16toh(response->offset);
            diagnostics->responseType = le16toh(response->type);
            diagnostics->responseLength = bytes;
            diagnostics->nvmCheck = "response-fields";
        }
        if (le16toh(response->status)) {
            if (diagnostics) diagnostics->nvmCheck = "firmware-status";
            // A missing optional section is not a transport error. Do not
            // discard already-read sections on a timeout or malformed packet.
            // Error responses have no valid data span; handle their status
            // before interpreting echoed offset/type/length or copying data.
            // Intel's status 1 on a later 2 KiB boundary is end-of-section.
            // Reference: Linux v6.12 iwl_nvm_read_chunk (NOT_VALID_ADDRESS).
            error = offset && le16toh(response->status) == 1 ? 0 : ENOENT;
        } else if (le16toh(response->offset) != offset ||
            // The firmware answers with the id of the section it actually
            // served, which is the mapped id on the 8000 family.
            (le16toh(response->type) != wireSection &&
             !(diagnostics && diagnostics->allowSectionTypeMismatch)) ||
            bytes > length || bytes > iwm_rx_packet_payload_len(packet) - sizeof(*response)) {
            error = EIO;
        } else {
            if (diagnostics && diagnostics->allowSectionTypeMismatch)
                diagnostics->recordSectionType(section, le16toh(response->type), bytes);
            memcpy(data + offset, response->data, bytes);
            *len = static_cast<uint16_t>(bytes);
            if (diagnostics) {
                diagnostics->nvmCheck = "success";
                diagnostics->bytesRead += bytes;
            }
        }
    }
    if (diagnostics) diagnostics->nvmError = error;
    iwm_free_resp(sc, &command);
    return error;
}

int ItlIwm::iwm_nvm_read_section(struct iwm_softc *sc, uint16_t section,
    uint8_t *data, uint16_t *len, size_t max_len)
{
    if (!data || !len || !max_len || max_len > UINT16_MAX)
        return EINVAL;
    *len = 0;
    while (*len < max_len) {
        const size_t remaining = max_len - *len;
        const uint16_t requested = static_cast<uint16_t>(
            remaining < IWM_NVM_DEFAULT_CHUNK_SIZE ? remaining : IWM_NVM_DEFAULT_CHUNK_SIZE);
        uint16_t received = 0;
        const int error = iwm_nvm_read_chunk(sc, section, *len, requested, data, &received);
        if (error) return error;
        *len += received;
        if (received < requested) break;
    }
    return 0;
}
