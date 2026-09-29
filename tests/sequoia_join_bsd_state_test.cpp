#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <net/if.h>
#include <initializer_list>
#include <Airport/SequoiaJoinCandidate.h>
#include "sequoia_tree.h"
#include "sequoia_join_bsd_types.inc"
#define IEEE80211_STA_ONLY 1
#define IO80211FAMILY_V2 1
#define __MAC_15_0 150000
#define __IO80211_TARGET __MAC_15_0
#define XYLog(...) ((void)0)
#define DPRINTF(args) ((void)0)
#define IPL_NET 0
static void splassert(int) {}
static int splnet() { return 0; }
static void splx(int) {}

struct ieee80211_channel { unsigned ic_flags = IEEE80211_CHAN_2GHZ; };
struct ieee80211_node {
    unsigned ni_sa_query_to = 0;
    uint8_t ni_macaddr[6] = {}, ni_bssid[6] = {}, ni_essid[32] = {};
    unsigned ni_associd = 0, ni_rstamp = 0, ni_rsn_supp_state = 0, ni_assoc_fail = 0;
    unsigned ni_fails = 0, ni_txrate = 0, ni_flags = 0, ni_txmcs = 0, ni_esslen = 0, ni_rsnakms = 0;
    RB_ENTRY(ieee80211_node) ni_node;
    unsigned ni_state = IEEE80211_STA_CACHE, ni_refcnt = 0, ni_port_valid = 0;
    unsigned ni_capinfo = 1, ni_intval = 100;
    uint64_t ni_age_ts = 1000;
    struct { unsigned k_cipher = IEEE80211_CIPHER_NONE; } ni_pairwise_key;
    uint8_t *ni_rsnie = nullptr, *ni_rsnie_tlv = nullptr;
    uint32_t ni_rsnie_tlv_len = 0;
    void (*ni_unref_cb)() = nullptr;
    void *ni_unref_arg = nullptr;
    size_t ni_unref_arg_size = 0;
    ieee80211_rateset ni_rates = {};
    ieee80211_channel *ni_chan = nullptr;
};
RB_HEAD(ieee80211_tree, ieee80211_node);
static int node_compare(ieee80211_node *a, ieee80211_node *b) {
    return memcmp(a->ni_macaddr, b->ni_macaddr, 6);
}
RB_GENERATE(ieee80211_tree, ieee80211_node, ni_node, node_compare)
struct _ifnet {
    unsigned if_flags = 0;
    void (*if_start)(_ifnet *) = nullptr;
};
struct ieee80211com {
    _ifnet ic_if;
    ieee80211_tree ic_tree = {};
    unsigned ic_nnodes = 0;
    void (*ic_node_free)(ieee80211com *, ieee80211_node *) = nullptr;
    ieee80211_state ic_state = IEEE80211_S_INIT;
    ieee80211_opmode ic_opmode = IEEE80211_M_STA;
    ieee80211_node *ic_bss = nullptr;
    unsigned ic_flags = 0, ic_xflags = 0, ic_bgscan_timeout = 0, ic_bgscan_fail = 0;
    unsigned ic_mgt_timer = 0, ic_mgtq = 0, ic_pwrsaveq = 0;
    ieee80211_rateset ic_sup_rates[8] = {};
    int (*ic_send_mgmt)(ieee80211com *, ieee80211_node *, int, int, int) = nullptr;
};
static const uint8_t etherbroadcastaddr[6] = {255,255,255,255,255,255};
static unsigned scans = 0, frames = 0, frees = 0, stops = 0, starts = 0, groupClears = 0;
static int lastType = 0, lastArg = 0, link = LINK_STATE_DOWN;
static void ieee80211_set_link_state(ieee80211com *, int state) { link = state; }
static void ieee80211_stop_ampdu_tx(ieee80211com *, ieee80211_node *, int) { ++stops; }
static void ieee80211_ba_del(ieee80211_node *) {}
static void timeout_del(unsigned *timer) { *timer = 0; }
static void timeout_free(unsigned *timer) { *timer = 0; }
static void mq_purge(unsigned *queue) { *queue = 0; }
static void ieee80211_ba_free(ieee80211_node *) {}
static void ieee80211_node_cleanup(ieee80211com *, ieee80211_node *);
static void releaseNode(ieee80211com *ic, ieee80211_node *node) {
    ieee80211_node_cleanup(ic, node); ++frees; delete node;
}
static void panic(const char *, ...) { abort(); }
#include "sequoia_join_node_cleanup.inc"
static void ieee80211_crypto_clear_groupkeys(ieee80211com *) { ++groupClears; }
static unsigned ieee80211_chan2mode(ieee80211com *, ieee80211_channel *) { return 0; }
static void ieee80211_begin_scan(_ifnet *) { ++scans; }
static void ieee80211_clean_sta_bss_node(ieee80211com *) {}
static void ieee80211_check_wpa_supplicant_failure(ieee80211com *, ieee80211_node *) {}
static bool ieee80211_is_8021x_akm(ieee80211_akm akm) { return akm == IEEE80211_AKM_8021X; }
static void ieee80211_set_beacon_miss_threshold(ieee80211com *) {}
static void ieee80211_print_essid(const uint8_t *, int) {}
static int send(ieee80211com *, ieee80211_node *, int type, int arg, int extra) {
    assert(!extra); ++frames; lastType = type; lastArg = arg; return 0;
}
static void start(_ifnet *) { ++starts; }
// Logging-only rate is retained by the production function in non-debug builds.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#include "sequoia_join_bsd_state_function.inc"
#pragma clang diagnostic pop

int main()
{
    for (bool secured : {false, true}) {
        ieee80211com ic;
        ieee80211_node node;
        ic.ic_bss = &node;
        ic.ic_node_free = releaseNode;
        ic.ic_send_mgmt = send;
        ic.ic_if.if_start = start;
        if (secured) { ic.ic_flags = IEEE80211_F_RSNON; node.ni_rsnakms = IEEE80211_AKM_PSK; }
        node.ni_rates.rs_nrates = 1;
        node.ni_rates.rs_rates[0] = 2;
        scans = frames = frees = stops = starts = groupClears = 0;
        link = LINK_STATE_DOWN;
        for (int invalid : {-1, 0, IEEE80211_FC0_SUBTYPE_ASSOC_REQ, IEEE80211_FC0_SUBTYPE_DEAUTH}) {
            assert(ieee80211_newstate(&ic, IEEE80211_S_AUTH, invalid) == EINVAL);
            assert(ic.ic_state == IEEE80211_S_INIT && !frames && !groupClears);
        }
        ic.ic_opmode = IEEE80211_M_MONITOR;
        assert(ieee80211_newstate(&ic, IEEE80211_S_AUTH, IEEE80211_FC0_SUBTYPE_AUTH) == EINVAL);
        ic.ic_opmode = IEEE80211_M_STA;
        assert(!ieee80211_newstate(&ic, IEEE80211_S_AUTH, IEEE80211_FC0_SUBTYPE_AUTH));
        assert(ic.ic_state == IEEE80211_S_AUTH && frames == 1 && !scans && !frees);
        assert(lastType == IEEE80211_FC0_SUBTYPE_AUTH && lastArg == IEEE80211_AUTH_OPEN_REQUEST);
        assert(link == LINK_STATE_DOWN && groupClears == secured);
        assert(!ieee80211_newstate(&ic, IEEE80211_S_ASSOC, 0));
        assert(frames == 2 && lastType == IEEE80211_FC0_SUBTYPE_ASSOC_REQ && !lastArg);
        assert(!ieee80211_newstate(&ic, IEEE80211_S_RUN, 0));
        assert(frames == 2 && starts == 1 && !scans);
        assert(link == (secured ? LINK_STATE_DOWN : LINK_STATE_UP));
        assert(!ieee80211_newstate(&ic, IEEE80211_S_INIT, -1));
        assert(frames == 2 && frees == 0 && link == LINK_STATE_DOWN);
    }
    // Synthetic AP records only. Actual BSD state transition, RB tree deletion,
    // cleanup, lookup and candidate admission; no network identity or key input.
    for (bool selected : {false, true})
    for (auto state : {IEEE80211_S_AUTH, IEEE80211_S_ASSOC, IEEE80211_S_RUN}) {
        ieee80211com ic;
        ieee80211_node bss;
        ieee80211_channel channel;
        ic.ic_bss = &bss; ic.ic_node_free = releaseNode; ic.ic_state = state;
        ic.ic_flags = IEEE80211_F_RSNON;
        bss.ni_rsnie = static_cast<uint8_t *>(malloc(2));
        bss.ni_rsnie_tlv = static_cast<uint8_t *>(malloc(4));
        bss.ni_rsnie_tlv_len = 4;
        ic.ic_mgtq = ic.ic_pwrsaveq = ic.ic_bgscan_timeout = ic.ic_mgt_timer = 1;
        auto *candidate = new ieee80211_node;
        candidate->ni_macaddr[0] = candidate->ni_bssid[0] = 2;
        candidate->ni_macaddr[5] = candidate->ni_bssid[5] = 1;
        candidate->ni_chan = &channel; candidate->ni_esslen = 1; candidate->ni_essid[0] = 'x';
        candidate->ni_rates.rs_nrates = 1;
        candidate->ni_rsnie = static_cast<uint8_t *>(malloc(2));
        candidate->ni_rsnie[0] = 48; candidate->ni_rsnie[1] = 0;
        RB_INSERT(ieee80211_tree, &ic.ic_tree, candidate); ++ic.ic_nnodes;
        auto *oldPeer = new ieee80211_node;
        oldPeer->ni_macaddr[0] = 2; oldPeer->ni_macaddr[5] = 2;
        oldPeer->ni_state = IEEE80211_STA_BSS;
        RB_INSERT(ieee80211_tree, &ic.ic_tree, oldPeer); ++ic.ic_nnodes;
        SequoiaWCL::JoinRequest request = {};
        request.ssidLength = 1; request.ssid[0] = 'x'; request.candidates[0].channelSpec = 0x1006;
        memcpy(request.candidates[0].bssid, candidate->ni_bssid, 6);
        auto inspect = [&](uint64_t now) {
            auto *found = ieee80211_find_node(&ic, request.candidates[0].bssid);
            return SequoiaWCL::inspectJoinCandidate(&ic, found, &channel, request, 0, now, IEEE80211_STA_CACHE);
        };
        assert(inspect(2000).mask == 0);
        if (selected) {
            // Actual node_join_bss sets this marker on the tree observation;
            // authentication/keys belong to the separate ic_bss copy.
            ieee80211_node_newstate(candidate, IEEE80211_STA_BSS);
            memcpy(bss.ni_macaddr, candidate->ni_macaddr, 6);
            memcpy(bss.ni_bssid, candidate->ni_bssid, 6);
            assert(inspect(2000).mask == SequoiaWCL::CandidateNotCached);
        }
        const auto beforeGroups = groupClears;
        assert(!ieee80211_newstate(&ic, IEEE80211_S_INIT, -1));
        assert(!bss.ni_rsnie && !bss.ni_rsnie_tlv && !bss.ni_rsnie_tlv_len);
        assert(!ic.ic_mgtq && !ic.ic_pwrsaveq && !ic.ic_bgscan_timeout && !ic.ic_mgt_timer);
        assert(groupClears == beforeGroups + 1 && link == LINK_STATE_DOWN);
        // r248 deleted all candidates; r259 still deleted selected records.
        assert(inspect(2000).mask == 0);
        assert(ic.ic_nnodes == 1 && candidate->ni_rsnie[0] == 48);
        assert(inspect(31001).mask == SequoiaWCL::CandidateAge);
        // A second INIT during MAC stop does not refresh age or reset records.
        assert(!ieee80211_newstate(&ic, IEEE80211_S_INIT, -1));
        assert(candidate->ni_age_ts == 1000 && inspect(2000).mask == 0);
        ieee80211_free_allnodes(&ic, 1); // detach still releases everything
        assert(ic.ic_nnodes == 0 && RB_EMPTY(&ic.ic_tree));
    }
    // Only the selected, unowned scan observation may lose its BSS marker.
    // All mismatched/dirty records still take the original release path.
    for (unsigned fault = 0; fault < 19; ++fault) {
        ieee80211com ic; ieee80211_node bss;
        ic.ic_bss = &bss; ic.ic_node_free = releaseNode; ic.ic_state = IEEE80211_S_RUN;
        auto *n = new ieee80211_node;
        n->ni_macaddr[0] = n->ni_bssid[0] = 2;
        n->ni_macaddr[5] = n->ni_bssid[5] = 1;
        memcpy(bss.ni_macaddr, n->ni_macaddr, 6);
        memcpy(bss.ni_bssid, n->ni_bssid, 6);
        n->ni_state = IEEE80211_STA_BSS;
        switch (fault) {
            case 1: ++bss.ni_macaddr[5]; break;
            case 2: ++bss.ni_bssid[5]; break;
            case 3: n->ni_refcnt = 1; break;
            case 4: n->ni_pairwise_key.k_cipher = IEEE80211_CIPHER_CCMP; break;
            case 5: n->ni_port_valid = 1; break;
            case 6: n->ni_flags = IEEE80211_NODE_TXRXPROT; break;
            case 7: n->ni_flags = IEEE80211_NODE_PMK; break;
            case 8: n->ni_unref_cb = [] {}; break;
            case 9: n->ni_unref_arg = malloc(8); break;
            case 10: n->ni_associd = 1; break;
            case 11: n->ni_unref_arg_size = 8; break;
            case 12: n->ni_flags = IEEE80211_NODE_RXMGMTPROT; break;
            case 13: n->ni_flags = IEEE80211_NODE_TXMGMTPROT; break;
            case 14: n->ni_flags = IEEE80211_NODE_PMKID; break;
            case 15: n->ni_flags = IEEE80211_NODE_SA_QUERY; break;
            case 16: n->ni_flags = IEEE80211_NODE_SA_QUERY_FAILED; break;
            case 17: ic.ic_opmode = IEEE80211_M_MONITOR; break;
            case 18: n->ni_state = IEEE80211_STA_COLLECT; break;
        }
        RB_INSERT(ieee80211_tree, &ic.ic_tree, n); ++ic.ic_nnodes;
        assert(!ieee80211_newstate(&ic, IEEE80211_S_INIT, -1));
        assert(ic.ic_nnodes == (fault == 0 ? 1u : 0u));
        if (!fault) assert(n->ni_state == IEEE80211_STA_CACHE && n->ni_age_ts == 1000);
        ieee80211_free_allnodes(&ic, 1);
        assert(!ic.ic_nnodes && RB_EMPTY(&ic.ic_tree));
    }
    // Never retain session-owned/dirty cache records. Exercise the real tree
    // traversal with interleaved keep/free nodes, including full capacity.
    for (bool station : {false, true}) {
        ieee80211com ic; ieee80211_node bss;
        ic.ic_bss = &bss; ic.ic_node_free = releaseNode;
        ic.ic_opmode = station ? IEEE80211_M_STA : IEEE80211_M_MONITOR;
        ic.ic_state = IEEE80211_S_RUN;
        for (unsigned i = 0; i < 200; ++i) {
            auto *n = new ieee80211_node;
            n->ni_macaddr[0] = 2; n->ni_macaddr[5] = i;
            switch (i % 10) {
                case 1: n->ni_state = IEEE80211_STA_BSS; break;
                case 2: n->ni_state = IEEE80211_STA_COLLECT; break;
                case 3: n->ni_refcnt = 1; break;
                case 4: n->ni_pairwise_key.k_cipher = IEEE80211_CIPHER_CCMP; break;
                case 5: n->ni_port_valid = 1; break;
                case 6: n->ni_flags = IEEE80211_NODE_TXRXPROT; break;
                case 7: n->ni_flags = IEEE80211_NODE_PMK; break;
                case 8: n->ni_unref_cb = [] {}; break;
                case 9: n->ni_unref_arg = malloc(8); break;
            }
            RB_INSERT(ieee80211_tree, &ic.ic_tree, n); ++ic.ic_nnodes;
        }
        assert(!ieee80211_newstate(&ic, IEEE80211_S_INIT, -1));
        assert(ic.ic_nnodes == (station ? 20u : 0u));
        for (unsigned i = 0; i < 200; ++i) {
            uint8_t address[6] = {2,0,0,0,0,static_cast<uint8_t>(i)};
            assert(bool(ieee80211_find_node(&ic, address)) == (station && i % 10 == 0));
        }
        ieee80211_free_allnodes(&ic, 1);
        assert(!ic.ic_nnodes && RB_EMPTY(&ic.ic_tree));
    }
    puts("Actual BSD INIT/cache lifecycle: AUTH/ASSOC/RUN teardown, selected and unselected observations retain original age/IE, stale candidate rejected, 19 selected-record guards, 400 mixed clean/session-owned nodes, non-STA cleanup and detach passed; timers/keys/hardware boundaries mocked");
    puts("Actual BSD state machine: explicit INIT->AUTH->ASSOC->RUN, WPA2 link held down pending keys, invalid idle admissions and cleanup passed; management TX/key/hardware boundaries mocked");
}
