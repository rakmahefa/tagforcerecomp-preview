// Verify the experimental native activation-negation interception.
#include "../../plugins/src/TF3Cards/tf3_cards.cpp"
#include <cassert>
static uint8_t bytes[0x2000000];
static uint32_t regs[32];
static bool other_rejection;
static uint8_t *memory(uint32_t p) { assert(p >= 0x08000000 && p < 0x0a000000); return bytes + p - 0x08000000; }
static uint32_t reg(TfCpu *, int i) { return regs[i]; }
static void set_reg(TfCpu *, int i, uint32_t v) { regs[i] = v; }
static void call(TfCpu *, uint32_t fn) {
    assert(fn == 0x09f678f0);
    regs[2] = regs[4] == 100 ? 22 : regs[4] == 101 ? 23 : 1;
}
static int original(TfCpu *cpu, uint32_t fn) {
    if (fn == 0x09eb7248) {
        assert(g_venus_chain_scope == 1);
        regs[5] = 5322; assert(hk_venus_frontier(cpu) == 1 && regs[2] == 0);
        regs[5] = 6663; assert(hk_venus_phoenix(cpu) == 1 && regs[2] == 0);
        regs[6] = 5728; assert(hk_replacement_present(cpu) == 1 && regs[2] == 0);
        regs[5] = 4009; assert(hk_venus_frontier(cpu) == 0 && hk_venus_phoenix(cpu) == 0);
        return 0;
    }
    assert(fn == 0x09ed26d0 && g_venus_effect_scope == 1);
    regs[4] = g_venus_effect_side; regs[5] = g_venus_effect_card;
    assert(hk_venus_continuous_negation(cpu) == 1 && regs[2] == 0);
    regs[5]++;
    assert(hk_venus_continuous_negation(cpu) == 0);
    if (other_rejection) regs[2] = 1;
    return 0;
}
int main() {
    TfModApi api = {}; api.mem = memory; api.reg = reg; api.set_reg = set_reg; api.call = call; api.call_original = original; A = &api;
    PORTED.push_back({7400, 7347, 0, "Venus"});
    const uint32_t object = 0x08001000;
    regs[5] = object;
    for (unsigned controller = 0; controller < 2; ++controller) {
        const uint32_t entry = DUEL_SIDES + controller * DUEL_SIDE_SIZE + 0x30;
        *(uint16_t *)memory(entry) = 7400;
        *(uint16_t *)memory(entry + 8) = 1;
        for (unsigned target_side = 0; target_side < 2; ++target_side) {
            memory(object)[2] = (uint8_t)target_side;
            for (uint16_t id : {100, 101, 102}) {
                *(uint16_t *)memory(object) = id;
                assert(hk_venus_negation(nullptr) == (controller == target_side && id != 102));
                regs[4] = object;
                assert(hk_venus_effect_negation(nullptr) == (controller == target_side && id != 102));
                regs[5] = object;
            }
        }
        memory(object)[2] = (uint8_t)controller; *(uint16_t *)memory(object) = 100;
        regs[4] = object; other_rejection = true;
        assert(hk_venus_effect_negation(nullptr) == 1 && regs[2] == 1);
        other_rejection = false; regs[5] = object;
        *(uint32_t *)memory(0x09f78660) = 1;
        *(uint32_t *)memory(0x09f78784) = 4;
        *(uint16_t *)memory(0x09f784e0) = 100;
        memory(0x09f784e2)[0] = (uint8_t)controller;
        assert(hk_venus_chain(nullptr) == 1 && g_venus_chain_scope == 0);
        *(uint32_t *)memory(0x09f78784) = 3;
        assert(hk_venus_chain(nullptr) == 0);
        regs[5] = object;
        memory(object)[2] = (uint8_t)controller; *(uint16_t *)memory(object) = 100;
        *(uint16_t *)memory(entry + 8) = 0;
        assert(hk_venus_negation(nullptr) == 0);
        *(uint16_t *)memory(entry + 8) = 1;
        *(uint32_t *)memory(entry + 16) = 0x20;
        assert(hk_venus_negation(nullptr) == 0);
        *(uint16_t *)memory(entry) = 0; *(uint32_t *)memory(entry + 16) = 0;
    }
}
