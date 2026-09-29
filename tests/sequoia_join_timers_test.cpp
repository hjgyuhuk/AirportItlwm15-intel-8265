#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/types.h>
#include <net/if.h>
#include "sequoia_join_bsd_types.inc"
#define IEEE80211_STA_ONLY 1
#define nitems(a) int(sizeof(a) / sizeof((a)[0]))
enum { EDCA_AC_BE, EDCA_AC_BK, EDCA_AC_VI, EDCA_AC_VO };
struct Timer {};
struct ieee80211_node {
    uint64_t before[8];
    unsigned ni_flags;
    Timer *ni_sa_query_to;
    Timer *ni_addba_req_to[IEEE80211_NUM_TID];
    int ni_addba_req_intval[IEEE80211_NUM_TID];
    // Stand-ins for BA records and the other fields following the timers.
    uint8_t preserved[4096];
};
static Timer timers[5];
static unsigned calls;
static void timeout_set(Timer **slot, void (*)(void *), void *) {
    assert(*slot == nullptr && calls < 5); *slot = &timers[calls++];
}
static void ieee80211_sa_query_timeout(void *) {}
static void ieee80211_node_addba_request_ac_be_to(void *) {}
static void ieee80211_node_addba_request_ac_bk_to(void *) {}
static void ieee80211_node_addba_request_ac_vi_to(void *) {}
static void ieee80211_node_addba_request_ac_vo_to(void *) {}
#include "sequoia_join_timers.inc"
int main() {
    for (unsigned fill = 0; fill < 256; ++fill) {
        ieee80211_node node, expected;
        memset(&node, fill, sizeof(node)); memcpy(&expected, &node, sizeof(node));
        expected.ni_flags &= ~(IEEE80211_NODE_SA_QUERY | IEEE80211_NODE_SA_QUERY_FAILED);
        expected.ni_sa_query_to = &timers[0];
        for (unsigned i = 0; i < IEEE80211_NUM_TID; ++i) {
            expected.ni_addba_req_to[i] = i < 4 ? &timers[i + 1] : nullptr;
            expected.ni_addba_req_intval[i] = 1;
        }
        calls = 0;
        ieee80211_node_set_timeouts(&node);
        assert(calls == 5 && !memcmp(&node, &expected, sizeof(node)));
    }
    puts("Actual node timer initialization: 256 patterns preserve adjacent node state; timer allocation mocked");
}
