#ifndef ITL_JOIN_EVENT_H
#define ITL_JOIN_EVENT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace ItlJoin {

enum class Stage : uint8_t { Auth, Assoc, Deauth, Disassoc };

// Internal BSD observation, NOT an Apple/WCL wire payload. Status is the
// IEEE status code for Auth/Assoc, or IEEE reason code for Deauth/Disassoc.
struct Event {
    Stage stage;
    uint8_t bssid[6];
    uint16_t status;
};

// Serialized by the shared workloop. A new attempt requires a full HAL stop
// and RX/task drain; a host epoch cannot disambiguate old over-air responses.
class Progress {
public:
    bool begin(const uint8_t *bssid, int generation)
    {
        if (active_ || !bssid || (bssid[0] & 1)) return false;
        uint8_t any = 0;
        for (size_t i = 0; i < sizeof(bssid_); ++i) any |= bssid[i];
        if (!any) return false;
        memcpy(bssid_, bssid, sizeof(bssid_));
        generation_ = generation;
        active_ = true;
        return true;
    }

    bool record(const Event &event, int generation)
    {
        if (!ownsPeer(event.bssid, generation)) return false;
        switch (event.stage) {
            case Stage::Auth:
                if (authSeen_ || assocSeen_) return false;
                auth_ = event; authSeen_ = true; failed_ = event.status != 0;
                return true;
            case Stage::Assoc:
                if (!authSeen_ || auth_.status || assocSeen_) return false;
                assoc_ = event; assocSeen_ = true; failed_ = event.status != 0;
                return true;
            case Stage::Deauth:
            case Stage::Disassoc:
                disconnect_ = event; disconnected_ = failed_ = true;
                return true;
        }
        return false;
    }

    // Only the owner calls this after reset/drain, or before AUTH was queued.
    void clear() { *this = Progress(); }
    bool active() const { return active_; }
    bool failed() const { return failed_; }
    bool ownsPeer(const uint8_t *bssid, int generation) const
    {
        return active_ && !failed_ && bssid && generation == generation_ &&
            !memcmp(bssid_, bssid, sizeof(bssid_));
    }
    const Event *auth() const { return authSeen_ ? &auth_ : nullptr; }
    const Event *assoc() const { return assocSeen_ ? &assoc_ : nullptr; }
    const Event *disconnect() const { return disconnected_ ? &disconnect_ : nullptr; }

private:
    uint8_t bssid_[6] = {};
    int generation_ = 0;
    Event auth_ = {}, assoc_ = {}, disconnect_ = {};
    bool active_ = false, failed_ = false, authSeen_ = false, assocSeen_ = false, disconnected_ = false;
};

} // namespace ItlJoin
#endif
