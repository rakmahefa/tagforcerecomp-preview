// Selection regressions for the opt-in Ebon Steed replacement implementation.
#include "../../plugins/src/TF3Cards/tf3_cards.cpp"
#include <cassert>
static uint8_t memory[0x2000000];
static uint32_t regs[32], immunity;
static uint8_t *mem(uint32_t addr) { assert(addr >= 0x08000000 && addr < 0x0a000000); return memory + addr - 0x08000000; }
static uint32_t reg(TfCpu *, int n) { return regs[n]; }
static void set_reg(TfCpu *, int n, uint32_t v) { regs[n] = v; }
static void call(TfCpu *, uint32_t fn) { assert(fn == 0x09f53430); regs[2] = immunity; }
static uint32_t entry(unsigned side, unsigned slot) { return DUEL_SIDES + side * DUEL_SIDE_SIZE + 0x30 + slot * 20; }
int main() {
    TfModApi api = {}; api.mem = mem; api.reg = reg; api.set_reg = set_reg; api.call = call; A = &api;
    PORTED.push_back({7213, 7000, 4310, "Legendary Ebon Steed"});
    StatRule rule = {}; rule.src = 7213; rule.scope = 4 | 32; rule.target = {7003}; g_stat.push_back(rule);
    for (unsigned side = 0; side < 2; ++side) for (unsigned source = 0; source < 2; ++source) {
        memset(memory, 0, sizeof memory); immunity = 0;
        *(uint16_t *)mem(entry(side, 2)) = 7003;
        *(uint16_t *)mem(entry(source, 7)) = 7213;
        *(uint16_t *)mem(entry(source, 7) + 8) = 1;
        *(uint16_t *)mem(entry(side, 2) + 10) = 1;
        *(uint16_t *)mem(0x09f7c460) = source | (7 << 8);
        mem(0x09f7c462)[0] = 10;
        assert(ebon_replacement(nullptr, side, 2) == (source | (7 << 8)));
        *(uint32_t *)mem(0x09f7bc48) = 1u << (7 + source * 16);
        assert(ebon_replacement(nullptr, side, 2) == 0xffff);
        *(uint32_t *)mem(0x09f7bc48) = 0;
        *(uint16_t *)mem(entry(source, 7) + 8) = 0;
        assert(ebon_replacement(nullptr, side, 2) == 0xffff);
        *(uint16_t *)mem(entry(source, 7) + 8) = 1;
        *(uint32_t *)mem(entry(source, 7) + 16) = 2;
        assert(ebon_replacement(nullptr, side, 2) == 0xffff);
        *(uint32_t *)mem(entry(source, 7) + 16) = 0;
        immunity = source == side ? 1 : 2;
        assert(ebon_replacement(nullptr, side, 2) == 0xffff);
        immunity = source == side ? 2 : 1;
        assert(ebon_replacement(nullptr, side, 2) != 0xffff);
        immunity = 0;
        *(uint16_t *)mem(entry(side, 2)) = 4009;
        assert(ebon_replacement(nullptr, side, 2) == 0xffff);
        *(uint16_t *)mem(entry(side, 2)) = 7003;
        mem(0x09f7c462)[0] = 9;
        assert(ebon_replacement(nullptr, side, 2) == 0xffff);
        mem(0x09f7c462)[0] = 11;
        assert(ebon_replacement(nullptr, side, 2) != 0xffff);
        *(uint16_t *)mem(entry(side, 2) + 10) = 0;
        assert(ebon_replacement(nullptr, side, 2) == 0xffff);
    }
    assert(ebon_replacement(nullptr, 2, 0) == 0xffff);
    assert(ebon_replacement(nullptr, 0, 5) == 0xffff);
}
