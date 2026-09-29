#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <net/if.h>
#include <vector>
#include "sequoia_join_bsd_types.inc"

#define DIV_ROUND_UP(a, b) (((a) + (b) - 1) / (b))
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "little-endian fixture");
static uint32_t le32toh(uint32_t value) { return value; }
static uint16_t le16toh(uint16_t value) { return value; }
static uint8_t hweight8(uint8_t value) { return __builtin_popcount(value); }

struct ieee80211_node {
    ieee80211_he_cap_elem ni_he_cap_elem;
    ieee80211_he_mcs_nss_supp ni_he_mcs_nss_supp;
    uint8_t ni_ppe_thres[IEEE80211_HE_PPE_THRES_MAX_LEN];
    uint32_t ni_he_oper_params;
    uint16_t ni_he_oper_nss_set;
    uint8_t ni_he_optional[8];
};
#include "sequoia_join_he_functions.inc"

int main()
{
    unsigned truncated = 0, accepted = 0;
    ieee80211_node node;
    memset(&node, 0xa5, sizeof(node));
    const auto initial = node;
    assert(!ieee80211_setup_hecaps(&node, nullptr, 255));
    assert(!memcmp(&node, &initial, sizeof(node)));
    assert(!ieee80211_setup_heop(&node, nullptr, 255));
    for (unsigned widths = 0; widths < 4; ++widths) {
        for (unsigned ppe = 0; ppe < 257; ++ppe) {
            std::vector<uint8_t> frame(sizeof(ieee80211_he_cap_elem) + 4, 0);
            if (widths & 1) {
                frame[6] |= IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_160MHZ_IN_5G;
                frame.resize(frame.size() + 4);
            }
            if (widths & 2) {
                frame[6] |= IEEE80211_HE_PHY_CAP0_CHANNEL_WIDTH_SET_80PLUS80_MHZ_IN_5G;
                frame.resize(frame.size() + 4);
            }
            const auto mcsSize = frame.size() - sizeof(ieee80211_he_cap_elem);
            memset(frame.data() + sizeof(ieee80211_he_cap_elem), 0x42, mcsSize);
            if (ppe < 256) {
                frame[12] |= IEEE80211_HE_PHY_CAP6_PPE_THRESHOLD_PRESENT;
                const unsigned ru = __builtin_popcount(ppe & IEEE80211_PPE_THRES_RU_INDEX_BITMASK_MASK);
                const unsigned nss = (ppe & 7) + 1;
                const unsigned bytes = (ru * nss * 6 + 7 + 7) / 8;
                frame.push_back(ppe);
                frame.resize(frame.size() + bytes - 1, 0x5a);
            }
            for (unsigned length = 0; length < frame.size(); ++length) {
                // An exact-sized allocation exposes reads that stay within a
                // larger fixture buffer but go past the advertised IE length.
                std::vector<uint8_t> shortFrame(frame.begin(), frame.begin() + length);
                node = initial;
                assert(!ieee80211_setup_hecaps(&node, shortFrame.data(), length));
                assert(!memcmp(&node, &initial, sizeof(node)));
                ++truncated;
            }
            node = initial;
            assert(ieee80211_setup_hecaps(&node, frame.data(), frame.size()));
            assert(!memcmp(&node.ni_he_cap_elem, frame.data(), sizeof(node.ni_he_cap_elem)));
            const auto *mcs = reinterpret_cast<const uint8_t *>(&node.ni_he_mcs_nss_supp);
            for (unsigned i = 0; i < sizeof(node.ni_he_mcs_nss_supp); ++i)
                assert(mcs[i] == (i < mcsSize ? 0x42 : 0));
            if (ppe == 256) for (auto byte : node.ni_ppe_thres) assert(!byte);
            ++accepted;
        }
    }
    for (unsigned size = 0; size < 6; ++size) {
        std::vector<uint8_t> bytes(size, 0x11);
        node = initial;
        assert(!ieee80211_setup_heop(&node, bytes.data(), bytes.size()));
        assert(!memcmp(&node, &initial, sizeof(node)));
        ++truncated;
    }
    for (unsigned flags = 0; flags < 8; ++flags) {
        const uint32_t params = (flags & 1 ? IEEE80211_HE_OPERATION_VHT_OPER_INFO : 0) |
            (flags & 2 ? IEEE80211_HE_OPERATION_CO_HOSTED_BSS : 0) |
            (flags & 4 ? IEEE80211_HE_OPERATION_6GHZ_OP_INFO : 0);
        const unsigned optional = (flags & 1 ? 3 : 0) + (flags & 2 ? 1 : 0) + (flags & 4 ? 5 : 0);
        for (unsigned size = 6; size < 256; ++size) {
            std::vector<uint8_t> bytes(size, 0x11);
            memcpy(bytes.data(), &params, 4);
            node = initial;
            const int result = ieee80211_setup_heop(&node, bytes.data(), bytes.size());
            if (size < 6 + optional || optional > sizeof(node.ni_he_optional)) {
                assert(!result && !memcmp(&node, &initial, sizeof(node)));
                ++truncated;
            } else {
                assert(result && node.ni_he_oper_params == params && node.ni_he_oper_nss_set == 0x1111);
                for (unsigned i = 0; i < sizeof(node.ni_he_optional); ++i)
                    assert(node.ni_he_optional[i] == (i < optional ? 0x11 : 0));
                ++accepted;
            }
        }
    }
    printf("Actual BSD HE parsing: %u exact-allocation truncations and %u accepted width/PPE/operation cases passed\n", truncated, accepted);
}
