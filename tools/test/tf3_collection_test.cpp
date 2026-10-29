#include "../../plugins/src/TF3Cards/tf3_cards.cpp"
#include <assert.h>

static uint8_t memory[0x2000000];
static uint32_t regs[32];
static uint8_t *test_mem(uint32_t addr) { return memory + (addr - 0x08000000); }
static uint32_t test_alloc(uint32_t) { return 0x08000010; }
static uint32_t test_reg(TfCpu *, int i) { return regs[i]; }
static void test_set_reg(TfCpu *, int i, uint32_t value) { regs[i] = value; }
static uint32_t spell_immunity;
static uint32_t argument_five, argument_six;
static void test_call(TfCpu *, uint32_t) { argument_five = regs[8]; argument_six = regs[9]; regs[2] = spell_immunity; }
static uint32_t stored_event;
static int test_original(TfCpu *, uint32_t fn) {
    if (fn == 0x09eb53b0) {
        stored_event = regs[5];
        const uint32_t counter = regs[4] ? 0x09f78660 : 0x09f78670;
        uint32_t *count = (uint32_t *)test_mem(counter);
        uint8_t *o = test_mem((regs[4] ? 0x09f784e0 : 0x09f781e0) + (*count)++ * 24);
        memset(o, 0, 24);
        *(uint16_t *)o = (uint16_t)stored_event;
        o[2] = (uint8_t)(stored_event >> 31);
        *(uint16_t *)(o + 4) = (uint16_t)((regs[6] & 0x1ff) << 6);
    }
    return 0;
}
static void test_log(const char *, ...) {}

int main(int argc, char **argv) {
    assert(argc == 2);
    wchar_t dir[1024];
    assert(MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, dir, 1024));
    TfModApi api = {};
    api.mod_dir = dir; api.mem = test_mem; api.guest_alloc = test_alloc; api.log = test_log;
    api.reg = test_reg; api.set_reg = test_set_reg; api.call = test_call; api.call_original = test_original; A = &api;
    regs[8] = 77; regs[9] = 88;
    call_game6(nullptr, 0, 1, 2, 3, 4, 5, 6);
    assert(argument_five == 5 && argument_six == 6 && regs[8] == 77 && regs[9] == 88);
    PORTED.push_back({6958, 1, 0, "a"});
    PORTED.push_back({6959, 2, 0, "b"});
    const uint8_t old[] = {2, 1, 3, 0};
    FILE *f = _wfopen(ext_path().c_str(), L"wb"); assert(f);
    assert(fwrite(old, 1, sizeof old, f) == sizeof old); assert(fclose(f) == 0);
    ext_init();
    assert(memcmp(test_mem(g_ext), old, sizeof old) == 0);
    assert(test_mem(g_ext)[5] == 3);
    test_mem(g_ext)[1] = 2;
    ext_save();
    f = _wfopen(ext_path().c_str(), L"rb"); assert(f);
    uint8_t saved[8]; assert(fread(saved, 1, sizeof saved, f) == sizeof saved); fclose(f);
    assert(saved[1] == 2 && saved[5] == 3);
    // A failed write must remain dirty, so the next attempt can retry.
    const std::wstring valid_dir = dir;
    api.mod_dir = L"Z:\\nonexistent-tf3-test";
    test_mem(g_ext)[1] = 1; ext_save();
    assert(g_ext_saved[1] == 2);
    api.mod_dir = valid_dir.c_str(); ext_save();
    assert(g_ext_saved[1] == 1);

    // A face-up Field Spell is the sixth Spell/Trap slot.
    regs[4] = 0; regs[5] = 0; regs[6] = 0x08000100;
    *(uint16_t *)test_mem(DUEL_SIDES + 0x30) = 4007;
    *(uint16_t *)test_mem(DUEL_SIDES + 0x30 + 20 * 10) = 7000;
    *(uint16_t *)test_mem(DUEL_SIDES + 0x30 + 20 * 10 + 8) = 1;
    StatRule field = {}; field.src = 7000; field.scope = 16 | 3; field.atk = 500;
    g_stat.push_back(field);
    hk_stats(nullptr);
    assert(*(int32_t *)test_mem(regs[6] + 20) == 500);
    // An explicitly filtered rule with no matching cards must match nothing.
    *(int32_t *)test_mem(regs[6] + 20) = 0;
    g_stat[0].scope |= 32;
    hk_stats(nullptr);
    assert(*(int32_t *)test_mem(regs[6] + 20) == 0);
    g_stat[0].scope &= ~32;
    *(uint16_t *)test_mem(DUEL_SIDES + 0x30 + 20 * 10 + 8) = 0;
    hk_stats(nullptr);
    assert(*(int32_t *)test_mem(regs[6] + 20) == 0);
    *(uint16_t *)test_mem(DUEL_SIDES + 0x30 + 20 * 10 + 8) = 1;
    *(uint32_t *)test_mem(DUEL_SIDES + 0x30 + 20 * 10 + 0x10) = 2;
    hk_stats(nullptr);
    assert(*(int32_t *)test_mem(regs[6] + 20) == 0);
    *(uint16_t *)test_mem(DUEL_SIDES + 0x30 + 20 * 10) = 0;
    *(uint32_t *)test_mem(DUEL_SIDES + 0x30 + 20 * 10 + 0x10) = 0;
    hk_stats(nullptr);
    assert(*(int32_t *)test_mem(regs[6] + 20) == 0);
    g_stat.clear();
    StatRule equip = {}; equip.src = 7001; equip.scope = 4;
    g_stat.push_back(equip); g_equip_level[7001] = 1;
    const uint32_t e = DUEL_SIDES + 0x30 + 20 * 5;
    *(uint16_t *)test_mem(e) = 7001;
    *(uint32_t *)test_mem(e + 4) = 1;
    *(uint16_t *)test_mem(e + 8) = 1;
    regs[2] = 4; hk_level(nullptr); assert(regs[2] == 5);
    spell_immunity = 3;
    regs[2] = 4; hk_level(nullptr); assert(regs[2] == 4);
    spell_immunity = 0;
    *(uint16_t *)test_mem(e + 8) = 0;
    regs[2] = 4; hk_level(nullptr); assert(regs[2] == 4);
    _putenv_s("TF3_EVENT_IDS", "1");
    regs[4] = 0; regs[6] = 8;
    g_trigger_scope.push_back({6506, 7002, 0});
    g_trigger_scope.push_back({6506, 7003, 1});
    regs[5] = 0x34200000 | 6506;
    assert(hk_event_store(nullptr) == 1 && stored_event == (0x34200000u | 6506));
    regs[5] = 0xb4200000 | 6506;
    assert(hk_event_store(nullptr) == 1 && stored_event == (0xb4200000u | 6506));
    g_trigger_scope.push_back({6506, 7004, 0});
    assert(trigger_card(6506, 0) == 7004);
    g_trigger_scope.clear();
    g_trig_real[6506] = 7002;
    assert(trigger_card(6506, 0) == 6506);
    // Queue copies retain donor/controller/instance identity despite step changes.
    uint8_t *event = test_mem(0x08000200);
    *(uint16_t *)event = 6506;
    event[2] = 0;
    *(uint16_t *)(event + 4) = 17 << 6;
    g_event_sources[event_key(event)] = 7002;
    *(uint32_t *)test_mem(0x09f7866c) = 0x08000200;
    assert(trigger_card(6506, 0) == 7002);
    *(uint16_t *)(event + 4) |= 23;
    assert(trigger_card(6506, 0) == 7002);
    *(uint16_t *)(event + 4) = 18 << 6;
    g_event_sources[event_key(event)] = 7003;
    assert(trigger_card(6506, 0) == 7003);
    assert(trigger_card(6506, 1) == 6506);
    *(uint32_t *)test_mem(0x09f7866c) = 0;
    regs[5] = 0x34200000 | 6506;
    assert(hk_event_store(nullptr) == 1);
    // Two imported cards and the native donor coexist and resolve in reverse order.
    g_event_sources.clear();
    *(uint32_t *)test_mem(0x09f78670) = 0;
    g_amt[{7002, 1}] = {2, 0}; g_amt[{7003, 1}] = {3, 0};
    for (unsigned i = 0; i < 3; ++i) {
        regs[4] = 0; regs[5] = 6506; regs[6] = 30 + i;
        if (i < 2) g_trigger_scope.push_back({6506, (uint16_t)(7002 + i), 0});
        assert(hk_event_store(nullptr) == 1);
        g_trigger_scope.clear();
    }
    for (int i = 2; i >= 0; --i) {
        memcpy(test_mem(0x08000300), test_mem(0x09f781e0 + i * 24), 24);
        *(uint32_t *)test_mem(0x09f7866c) = 0x08000300;
        const Amount *am = amount_of(6506, 1, 0);
        if (i == 2) assert(am == nullptr);
        else assert(am && am->self == i + 2 && am->opp == 0);
    }
    // An explicit callback object takes precedence over an unrelated current event.
    *(uint32_t *)test_mem(0x09f7866c) = 0x09f781e0 + 48;
    const Amount *explicit_amount = amount_of(6506, 1, 0, 0x09f781e0 + 24);
    assert(explicit_amount && explicit_amount->self == 3);
    assert(amount_of(6506, 1, 0) == nullptr);
    // An original donor can be queued while an imported donor scope is active.
    g_event_sources.clear();
    *(uint32_t *)test_mem(0x09f78670) = 0;
    g_trigger_scope.push_back({6506, 7002, 0, 30});
    for (unsigned token : {30u, 31u}) {
        regs[4] = 0; regs[5] = 6506; regs[6] = token; regs[7] = 0;
        assert(hk_event_store(nullptr) == 1);
    }
    assert(amount_of(6506, 1, 0, 0x09f781e0)->self == 2);
    assert(amount_of(6506, 1, 0, 0x09f781e0 + 24) == nullptr);
    g_trigger_scope.clear();
    // Optional condition/record trailers must load after the legacy trigger pairs.
    const std::wstring trigger_path = valid_dir + L"\\trigger.bin";
    f = _wfopen(trigger_path.c_str(), L"wb"); assert(f);
    const uint32_t trigger_header[2] = {0x54334654, 1};
    const uint16_t trigger_pair[2] = {7002, (uint16_t)(4317 | 0x6000)};
    const uint32_t condition_header[2] = {0x31434754, 1}, record_header[2] = {0x31524754, 1};
    const uint16_t condition_pair[2] = {7002, 15}, record_pair[2] = {7002, 6502};
    assert(fwrite(trigger_header, sizeof trigger_header, 1, f) == 1);
    assert(fwrite(trigger_pair, sizeof trigger_pair, 1, f) == 1);
    assert(fwrite(condition_header, sizeof condition_header, 1, f) == 1);
    assert(fwrite(condition_pair, sizeof condition_pair, 1, f) == 1);
    assert(fwrite(record_header, sizeof record_header, 1, f) == 1);
    assert(fwrite(record_pair, sizeof record_pair, 1, f) == 1);
    assert(fclose(f) == 0);
    trig_load();
    assert(g_trig[7002] == 4317 && g_trig_battle[7002] && g_after_attack_defense.count(7002));
    assert(g_trigger_conditions[7002] == 15 && g_trigger_record_donors[7002] == 6502);
    return 0;
}
