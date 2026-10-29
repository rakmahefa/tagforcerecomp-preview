/* Isolated engine integration for pending TF3 effect identity.
 * Compile the production hooks into this test DLL without initializing the mod.
 * The native queue writer and queue transfer run against the real duel engine.
 */
#define tfmod_init tf3_subject_init
#include "../../plugins/src/TF3Cards/tf3_cards.cpp"
#undef tfmod_init

static uint32_t mailbox;
static bool capture_packets;
static bool probing_store;
static int probe_store(TfCpu *c) { return probing_store ? hk_event_store(c) : 0; }
static int probe_record(TfCpu *c) { return probing_store ? hk_effect_record(c) : 0; }
static std::vector<std::vector<uint8_t>> packets;
static int capture_packet(TfCpu *c) {
    if (!capture_packets || A->reg(c, 4) != 0x14 || A->reg(c, 7) != 12) return 0;
    const uint8_t *p = A->mem(A->reg(c, 6));
    packets.emplace_back(p, p + 12);
    return 1;
}
static int probe_frame(TfCpu *c) {
    uint32_t *m = (uint32_t *)A->mem(mailbox);
    if (m[1] == m[2] || !A->module_loaded("libduel_eng")) return 0;
    const uint32_t sequence = m[1];
    probing_store = true;
    uint32_t regs[32];
    for (int i = 1; i < 32; ++i) regs[i] = A->reg(c, i);
    std::vector<uint8_t> queues(A->mem(0x09f781e0), A->mem(0x09f78780));
    const uint32_t deferred = *(uint32_t *)A->mem(0x09f7d47c);
    *(uint32_t *)A->mem(0x09f7d47c) = 0;
    memset(A->mem(0x09f781e0), 0, queues.size());
    PORTED = {{7173, 6960, 4226, "test A"}, {7174, 6961, 4226, "test B"}};
    g_trig = {{7173, 4226}, {7174, 4226}};
    g_amt = {{{7173, 1}, {2, 0}}, {{7174, 1}, {3, 0}}};
    g_event_sources.clear(); g_trigger_scope.clear(); g_deferred_sources.clear(); g_trigger_conditions.clear(); g_trigger_record_donors.clear();
    m[3] = 0; m[4] = 0;
    auto check = [&](bool pass, const char *name) {
        ++m[4]; if (!pass) ++m[3];
        A->log("%s %s", pass ? "PASS" : "FAIL", name);
    };
    // Six pending events: two imported sources and the original on both sides.
    for (unsigned side = 0; side < 2; ++side) {
        for (unsigned i = 0; i < 3; ++i) {
            A->set_reg(c, 4, 0);
            A->set_reg(c, 5, (side << 31) | 0x200000 | 4226);
            A->set_reg(c, 6, 40 + side * 3 + i); A->set_reg(c, 7, 0);
            if (i < 2) g_trigger_scope.push_back({4226, (uint16_t)(7173 + i), (uint8_t)side, (uint16_t)(40 + side * 3 + i)});
            hk_event_store(c);
            g_trigger_scope.clear();
        }
    }
    check(*(uint32_t *)A->mem(0x09f78670) == 6, "native pending queue contains six events");
    call_game(c, 0x09eb9654, 0, 0, 0, 0);
    check(*(uint32_t *)A->mem(0x09f78660) == 6, "native queue transfer retains six events");
    for (int index = 5; index >= 0; --index) {
        const uint32_t p = 0x09f784e0 + index * 24;
        const uint8_t *o = A->mem(p);
        const unsigned side = index / 3, i = index % 3;
        *(uint32_t *)A->mem(0x09f7866c) = p;
        check(*(const uint16_t *)o == 4226, "queued event retains native donor");
        const Amount *am = amount_of(4226, 1, side, p);
        check(i == 2 ? !am : am && am->self == (int)i + 2 && am->opp == 0,
              "source amount survives transfer and reverse resolution");
        check(trigger_card(4226, side, p) == (i == 2 ? 4226 : 7173 + i), "source identity matches descriptor");
        A->set_reg(c, 4, p);
        if (!hk_effect_record(c)) A->call_original(c, F_EFFECT_RECORD);
        const uint32_t record = A->reg(c, 2);
        check(record >= 0x08000000 && record < 0x0a000000 && *(uint32_t *)A->mem(record) == 4226,
              "native effect record remains available");
    }
    g_trigger_scope.push_back({4226, 7173, 0, 40});
    A->set_reg(c, 4, 1); A->set_reg(c, 5, 0x200000 | 4226);
    A->set_reg(c, 6, 88); A->set_reg(c, 7, 0);
    hk_event_store(c);
    const uint32_t native_object = 0x09f784e0 + 6 * 24;
    check(trigger_card(4226, 0, native_object) == 4226, "native donor queued inside alias scope retains identity");
    check(!amount_of(4226, 1, 0, native_object), "native donor queued inside alias scope retains native amount");
    g_trigger_scope.clear();
    // Capture native deferred packets before transport, then replay their exact payloads.
    // Transport itself is outside this fixture; packet serialization and replay storage are real.
    const uint32_t turn = *(uint32_t *)A->mem(0x09f7d460);
    const uint32_t mode = *(uint32_t *)A->mem(0x09f77888);
    *(uint32_t *)A->mem(0x09f7d460) = 0;
    *(uint32_t *)A->mem(0x09f77888) = 2;
    *(uint32_t *)A->mem(0x09f7d47c) = 1;
    *(uint32_t *)A->mem(0x09f78670) = 0;
    packets.clear(); capture_packets = true;
    for (unsigned i = 0; i < 3; ++i) {
        A->set_reg(c, 4, 0); A->set_reg(c, 5, 0x200000 | 4226);
        A->set_reg(c, 6, 50 + i); A->set_reg(c, 7, 100 + i);
        if (i < 2) g_trigger_scope.push_back({4226, (uint16_t)(7173 + i), 0, (uint16_t)(50 + i)});
        hk_event_store(c); g_trigger_scope.clear();
    }
    capture_packets = false;
    check(packets.size() == 3, "native deferred writer serialized three packets");
    check(*(uint32_t *)A->mem(0x09f78670) == 0, "deferred packets are not prematurely queued");
    *(uint32_t *)A->mem(0x09f7d47c) = 0;
    for (unsigned i = 0; i < packets.size(); ++i) {
        const uint16_t *p = (const uint16_t *)packets[i].data();
        A->set_reg(c, 4, p[0]); A->set_reg(c, 5, p[1] | (uint32_t)p[2] << 16);
        A->set_reg(c, 6, p[5]); A->set_reg(c, 7, p[3] | (uint32_t)p[4] << 16);
        hk_event_store(c);
        const uint32_t object = 0x09f781e0 + i * 24;
        check(trigger_card(4226, 0, object) == (i == 2 ? 4226 : 7173 + i), "deferred replay retains source identity");
        const Amount *am = amount_of(4226, 1, 0, object);
        check(i == 2 ? !am : am && am->self == (int)i + 2, "deferred replay retains source amount");
    }
    check(g_deferred_sources.empty(), "deferred source metadata is consumed");
    // Exercise the real field-to-Graveyard trigger predicate, not a simulated writer.
    const uint32_t sides = 0x09f79cec;
    std::vector<uint8_t> field_saved(A->mem(sides), A->mem(sides) + 0xfac * 2);
    for (unsigned side = 0; side < 2; ++side) memset(A->mem(sides + side * 0xfac + 0x30), 0, 20 * 11);
    PORTED.push_back({7175, 6842, 4317, "test Tank"});
    g_trig[7175] = 4317; g_amt[{7175, 2}] = {0, -600};
    const uint32_t equip = sides + 0x30 + 5 * 20;
    for (unsigned test = 0; test < 5; ++test) {
        const bool destroyed = test != 1 && test != 4, face_up = test != 2;
        const unsigned destination = test == 3 ? 15 : 14;
        memset(A->mem(equip), 0, 20);
        *(uint32_t *)A->mem(equip) = 7175 | 0x00c00000;
        *(uint32_t *)A->mem(equip + 4) = 1;
        *(uint16_t *)A->mem(equip + 8) = face_up;
        *(uint32_t *)A->mem(equip + 16) = face_up ? 0x80 : 0;
        g_trigger_conditions[7175] = test == 4 ? 0 : 1;
        *(uint32_t *)A->mem(0x09f78670) = 0;
        const uint32_t event = 6 | (5 << 10) | (destroyed ? 1 << 15 : 0) | (face_up ? 1 << 17 : 0) | (1 << 18) | (destination << 19);
        A->set_reg(c, 4, event); A->set_reg(c, 5, equip); A->set_reg(c, 6, 4909);
        A->set_reg(c, 7, 0x80001 | (destroyed ? 2 : 0));
        hk_trigger(c);
        const uint32_t count = *(uint32_t *)A->mem(0x09f78670);
        const bool expected = test == 0 || test == 2 || test == 4;
        check(count == (expected ? 1u : 0u), "Tank destruction/Set/non-destruction/banish trigger condition");
        if (expected && count) {
            const Amount *amount = amount_of(4317, 2, 0, 0x09f781e0);
            check(amount && amount->self == 0 && amount->opp == -600, "Tank pending LP amount uses 600 damage");
        }
    }
    PORTED.push_back({7176, 7125, 4310, "test Lucky Axe"});
    g_trig[7176] = 4317; g_amt[{7176, 1}] = {1, 0};
    g_trigger_conditions[7176] = 15; g_trigger_record_donors[7176] = 6502;
    for (unsigned test = 0; test < 9; ++test) {
        const bool destroyed = test != 4, face_up = test != 3, opponent = test != 1 && test != 7;
        const unsigned side = test >= 6 ? 1 : 0, owner = test == 8 ? 0 : side, token = 6 + owner;
        const uint32_t source = equip + side * 0xfac;
        for (unsigned p = 0; p < 2; ++p) memset(A->mem(sides + p * 0xfac + 0x30), 0, 20 * 11);
        *(uint32_t *)A->mem(source) = 7176 | (owner << 13) | 0x00c00000;
        *(uint32_t *)A->mem(source + 4) = 1;
        *(uint16_t *)A->mem(source + 8) = face_up;
        *(uint32_t *)A->mem(source + 16) = face_up ? 0x80 : 0;
        *(uint32_t *)A->mem(0x09f78670) = 0;
        const uint32_t event = token | (side << 9) | (5 << 10) | (destroyed ? 1 << 15 : 0) | (face_up ? 1 << 17 : 0) |
            ((side ^ (unsigned)opponent) << 18) | (14 << 19) | (test == 2 ? 1 << 16 : 0);
        A->set_reg(c, 4, event); A->set_reg(c, 5, source); A->set_reg(c, 6, test == 5 ? 0 : 4909);
        A->set_reg(c, 7, (test == 5 ? 0 : 0x80000) | (opponent ? 1 : 0) | (destroyed ? 2 : 0));
        hk_trigger(c);
        const uint32_t count = *(uint32_t *)A->mem(0x09f78670);
        const bool expected = test == 0 || test == 6 || test == 8;
        check(count == (expected ? 1u : 0u), "Lucky Axe requires face-up destruction by an opponent effect on either side");
        if (expected && count) {
            const Amount *amount = amount_of(4317, 1, owner, 0x09f781e0);
            check(amount && amount->self == 1 && amount->opp == 0, "Lucky Axe uses draw-one amount");
            A->set_reg(c, 4, 0x09f781e0);
            hk_effect_record(c);
            const uint32_t record = A->reg(c, 2);
            check(record >= 0x08000000 && record < 0x0a000000 && *(uint32_t *)A->mem(record) == 6502,
                  "Lucky Axe borrows a draw record only for its pending trigger");
        }
    }
    memcpy(A->mem(sides), field_saved.data(), field_saved.size());
    *(uint32_t *)A->mem(0x09f7d460) = turn;
    *(uint32_t *)A->mem(0x09f77888) = mode;
    memcpy(A->mem(0x09f781e0), queues.data(), queues.size());
    *(uint32_t *)A->mem(0x09f7d47c) = deferred;
    for (int i = 1; i < 32; ++i) if (i != 26 && i != 27) A->set_reg(c, i, regs[i]);
    g_event_sources.clear(); g_trigger_scope.clear(); g_deferred_sources.clear(); g_trigger_conditions.clear(); g_trigger_record_donors.clear();
    probing_store = false;
    m[2] = sequence;
    return 0;
}

extern "C" __declspec(dllexport) int tfmod_init(const TfModApi *api) {
    A = api;
    if (api->version < 2 || !getenv("TF_CTL") || !getenv("TF3_TEST_PROBE") || !getenv("TF3_EVENT_IDS")) return 1;
    mailbox = A->guest_alloc(128);
    if (!mailbox) return 1;
    memset(A->mem(mailbox), 0, 128);
    A->log("TF3 trigger mailbox %08x", mailbox);
    A->hook(F_FRAME, probe_frame);
    A->hook(0x09eb53b0, probe_store);
    A->hook(F_EFFECT_RECORD, probe_record);
    A->hook(0x09f5b280, capture_packet);
    return 0;
}
