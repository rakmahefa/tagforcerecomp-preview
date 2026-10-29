// Check mill event routing without changing the original Deck entries.
#include "../../plugins/src/TF3Cards/tf3_cards.cpp"
#include <cassert>

static uint8_t bytes[0x2000000];
static uint32_t regs[32];
static unsigned queued;
static uint8_t *memory(uint32_t p) { assert(p >= 0x08000000 && p < 0x0a000000); return bytes + p - 0x08000000; }
static uint32_t reg(TfCpu *, int i) { return regs[i]; }
static void set_reg(TfCpu *, int i, uint32_t v) { regs[i] = v; }
static void call(TfCpu *, uint32_t fn) {
    assert(fn == 0x09eb5640 && !g_trigger_scope.empty());
    const auto &source = g_trigger_scope.back();
    assert(source.card == 7860 && source.donor == 5091 && source.token == regs[5]);
    assert((regs[4] & 0xffff) == 7860);
    ++queued;
}
static int original(TfCpu *cpu, uint32_t fn) {
    assert(fn == 0x09f2a7ec);
    const uint32_t deck = DUEL_SIDES + 0x3a0;
    assert((*(uint32_t *)memory(deck) & 0x1fff) == 7860);
    assert((*(uint32_t *)memory(deck + 4) & 0x1fff) == 5091);
    assert((*(uint32_t *)memory(deck + 8) & 0x1fff) == 7860);
    for (uint32_t packet : {0xe000u | 6, 0xf000u | 6, 0xe000u | 7,
                            0xe000u | 8, 0xe000u | 0x200u | 8}) {
        regs[4] = 0x1c; regs[5] = packet;
        assert(hk_sabre_mill_event(cpu) == 0);
    }
    return 0;
}
int main() {
    TfModApi api = {}; api.mem = memory; api.reg = reg; api.set_reg = set_reg;
    api.call = call; api.call_original = original; A = &api;
    PORTED.push_back({7860, 7749, 4310, "Sabre"});
    *(uint32_t *)memory(DUEL_SIDES + 16) = 3;
    const uint32_t deck = DUEL_SIDES + 0x3a0;
    *(uint32_t *)memory(deck) = 7860 | (3u << 22);
    *(uint32_t *)memory(deck + 4) = 5091 | (7u << 22);
    *(uint32_t *)memory(deck + 8) = 7860 | (4u << 22);
    regs[4] = 0; regs[5] = 3;
    assert(hk_sabre_mill(nullptr) == 1);
    assert(queued == 2 && g_milled_sabres.empty() && g_trigger_scope.empty());
    assert((*(uint32_t *)memory(deck) & 0x1fff) == 7860);
}
