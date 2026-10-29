// Verify isolated mandatory records and tribute-source instance matching.
#include "../../plugins/src/TF3Cards/tf3_cards.cpp"
#include <cassert>
static uint8_t bytes[0x2000000];
static uint32_t registers_[32];
static bool allowed, effect_flag;
static uint16_t looked_up;
static uint8_t *memory(uint32_t p) { assert(p >= 0x08000000 && p < 0x0a000000); return bytes + p - 0x08000000; }
static uint32_t reg(TfCpu *, int i) { return registers_[i]; }
static void set_reg(TfCpu *, int i, uint32_t v) { registers_[i] = v; }
static uint32_t allocate(uint32_t n) { assert(n == 24); return 0x08000600; }
static int original(TfCpu *, uint32_t function) {
    if (function == F_EFFECT_RECORD) {
        looked_up = *(uint16_t *)memory(registers_[4]); registers_[2] = 0x08000500;
    } else {
        assert(function == F_TRIGGER && !g_trigger_scope.empty());
        allowed = g_trigger_scope.back().allowed;
        effect_flag = (registers_[7] & 0x80000) != 0;
    }
    return 0;
}
int main() {
    TfModApi api = {}; api.mem = memory; api.reg = reg; api.set_reg = set_reg;
    api.guest_alloc = allocate; api.call_original = original; A = &api;
    PORTED.push_back({7328, 7152, 4308, "Sword"});
    PORTED.push_back({7473, 7309, 4310, "Gladius"});
    g_trigger_conditions[7328] = 16;
    g_trigger_conditions[7473] = 32;
    const uint32_t object = 0x08000100;
    *(uint16_t *)memory(object) = 7328; memory(object)[3] = 0x20;
    uint32_t record[6] = {5724, 1, 2, 3, 0x09ec7cd8, 5};
    memcpy(memory(0x08000500), record, sizeof record);
    registers_[4] = object;
    assert(hk_effect_record(nullptr) == 1 && looked_up == 5724);
    assert(*(uint16_t *)memory(object) == 7328 && registers_[2] == 0x08000600);
    assert(*(uint32_t *)memory(0x08000610) == 0);
    assert(memcmp(memory(0x08000500), record, sizeof record) == 0);
    assert(memcmp(memory(0x08000600), record, 16) == 0);
    *(uint16_t *)memory(object) = 7473;
    assert(hk_effect_record(nullptr) == 1 && registers_[2] == 0x08000600);
    assert(*(uint16_t *)memory(object) == 7473 && *(uint32_t *)memory(0x08000610) == 0);
    *(uint16_t *)memory(object) = 7328;
    memory(object)[3] = 0;
    assert(hk_effect_record(nullptr) == 1 && looked_up == 4308 && registers_[2] == 0x08000500);
    *(uint16_t *)memory(object) = 5724; memory(object)[3] = 0x20;
    assert(hk_effect_record(nullptr) == 0);
    g_trig[7328] = 5724; g_trig[7473] = 5724;
    for (uint16_t card : {7328, 7473}) for (unsigned owner = 0; owner < 2; ++owner) for (unsigned controller = 0; controller < 2; ++controller) {
        *(uint16_t *)memory(object) = (uint16_t)(card | (owner << 13));
        *(uint16_t *)memory(object + 2) = 4 << 6;
        for (unsigned match = 0; match < 2; ++match) {
            g_tribute_equips.clear(); g_deck_equips.clear();
            (card == 7328 ? g_tribute_equips : g_deck_equips).push_back({card, (uint16_t)(8 + owner + (match ? 0 : 2))});
            registers_[4] = controller << 9; registers_[5] = object; registers_[7] = 2;
            assert(hk_trigger(nullptr) == 1);
            assert(allowed == (match != 0) && effect_flag == (match != 0));
            assert(*(uint16_t *)memory(object) == (card | (owner << 13)));
            assert(g_trigger_scope.empty());
        }
    }
}
