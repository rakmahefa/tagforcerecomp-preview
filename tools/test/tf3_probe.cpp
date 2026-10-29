/* Test-only field queries and opt-in removal actions in isolated TF_CTL sessions. */
#include "tfmod.h"
#include <cstdlib>
#include <cstring>
#include <vector>

static const TfModApi *A;
static uint32_t mailbox;
static const uint32_t SIDES = 0x09f79cec, SIDE_BYTES = 0xfac;

static int frame(TfCpu *c) {
    uint32_t *m = (uint32_t *)A->mem(mailbox);
    if (m[1] == m[2]) return 0;
    const uint32_t sequence = m[1];
    m[3] = 1;
    if (!A->module_loaded("libduel_eng")) { m[2] = sequence; return 0; }
    if (m[4] > 1 || m[5] >= (m[7] == 6 ? 11u : 5u) || m[6] > 22 || m[7] > 12 ||
        ((m[7] == 6 || m[7] == 8 || m[7] == 9 || m[7] == 10 || m[7] == 11) && !std::getenv("TF3_TEST_ACTIONS")) ||
        (m[7] == 10 && (m[6] || m[24] < 1 || m[24] > 5)) ||
        (m[7] == 8 && (m[6] || m[24] < 4000 || m[24] > 8191 || m[25] > 1)) ||
        (m[7] == 9 && (m[6] || m[24] < 4000 || m[24] > 8191 || m[25] > 1 || (m[26] & ~0x07ff07ffu))) ||
        (m[7] == 6 && (m[6] || m[25] < 11 || m[25] > 15 || (m[26] & ~0x11003u) || (m[24] && (m[24] < 4000 || m[24] > 8191)))) ||
        (m[7] == 1 && (m[24] > 1 || m[25] >= 5))) { m[3] = 2; m[2] = sequence; return 0; }
    for (uint32_t i = 0; i < m[6]; i++) {
        const uint32_t *f = (const uint32_t *)A->mem(mailbox + 128 + i * 28);
        if (f[0] > 1 || f[1] > 10) { m[3] = 2; m[2] = sequence; return 0; }
    }
    std::vector<uint8_t> saved(A->mem(SIDES), A->mem(SIDES) + SIDE_BYTES * 2);
    std::vector<uint8_t> links(A->mem(0x09f7c460), A->mem(0x09f7c460) + 22 * 8);
    if (m[7] != 4 && m[7] != 6 && m[7] != 7 && m[7] != 8 && m[7] != 9 && m[7] != 10) for (uint32_t side = 0; side < 2; side++) std::memset(A->mem(SIDES + side * SIDE_BYTES + 0x30), 0, 20 * 11);
    for (uint32_t i = 0; i < m[6]; i++) {
        const uint32_t *f = (const uint32_t *)A->mem(mailbox + 128 + i * 28);
        std::memcpy(A->mem(SIDES + f[0] * SIDE_BYTES + 0x30 + f[1] * 20), f + 2, 20);
    }
    // Fixture link: 1..5 names an own monster, 6..10 an opposing monster.
    // Materialize native type-10 equipment nodes; +4 remains an unrelated turn number.
    for (uint32_t i = 0; i < m[6]; ++i) {
        const uint32_t *f = (const uint32_t *)A->mem(mailbox + 128 + i * 28);
        if (f[1] < 5 || !f[3] || f[3] > 10) continue;
        const uint32_t target_side = f[0] ^ (f[3] > 5), target_slot = (f[3] - 1) % 5;
        const uint32_t target = SIDES + target_side * SIDE_BYTES + 0x30 + target_slot * 20;
        uint16_t *node = (uint16_t *)A->mem(0x09f7c460 + i * 8);
        node[0] = (uint16_t)(f[0] | (f[1] << 8)); node[1] = 10; node[2] = 0;
        node[3] = *(uint16_t *)A->mem(target + 10);
        *(uint16_t *)A->mem(target + 10) = (uint16_t)(i + 1);
        *(uint32_t *)A->mem(SIDES + f[0] * SIDE_BYTES + 0x30 + f[1] * 20 + 4) = 17;
    }
    uint32_t regs[32];
    for (int i = 1; i < 32; i++) regs[i] = A->reg(c, i);
    std::memset(A->mem(mailbox + 32), 0, 64);
    if (m[7] == 0 || m[7] == 7) {
        A->set_reg(c, 4, m[4]); A->set_reg(c, 5, m[5]); A->set_reg(c, 6, mailbox + 32);
        A->call(c, 0x09f56da8);
    } else if (m[7] == 1) {
        A->set_reg(c, 4, m[24]); A->set_reg(c, 5, m[25]);
        A->set_reg(c, 6, m[4]); A->set_reg(c, 7, m[5]); A->set_reg(c, 8, m[26]);
        A->call(c, 0x09f5153c);
        *(uint32_t *)A->mem(mailbox + 32) = A->reg(c, 2);
    } else if (m[7] == 2) {
        A->set_reg(c, 4, m[4]); A->set_reg(c, 5, m[5]);
        A->call(c, 0x09f53358);
        *(uint32_t *)A->mem(mailbox + 32) = A->reg(c, 2);
    } else if (m[7] == 3) {
        A->set_reg(c, 4, m[24]);
        A->call(c, 0x0884d1b8);
        *(uint32_t *)A->mem(mailbox + 32) = A->reg(c, 2);
    } else if (m[7] == 4) {
        const uint32_t error = *(uint32_t *)A->mem(0x09f7d4ec);
        A->set_reg(c, 4, m[4]); A->set_reg(c, 5, m[5]);
        A->call(c, 0x09f4fc18);
        *(uint32_t *)A->mem(mailbox + 32) = A->reg(c, 2);
        A->set_reg(c, 4, m[4]); A->set_reg(c, 5, m[5]); A->set_reg(c, 6, 0x1419);
        A->call(c, 0x09f465c0);
        *(uint32_t *)A->mem(mailbox + 36) = A->reg(c, 2);
        *(uint32_t *)A->mem(0x09f7d4ec) = error;
    } else if (m[7] == 5) {
        memset(A->mem(mailbox + 800), 0, 24);
        *(uint16_t *)A->mem(mailbox + 800) = (uint16_t)m[24];
        A->mem(mailbox + 802)[0] = (uint8_t)(m[25] & 1);
        A->set_reg(c, 4, mailbox + 800); A->set_reg(c, 5, m[4]); A->set_reg(c, 6, m[5]);
        A->call(c, 0x09eb9fc8);
        *(uint32_t *)A->mem(mailbox + 32) = A->reg(c, 2);
    } else if (m[7] == 12) {
        std::memset(A->mem(mailbox + 800), 0, 24);
        *(uint16_t *)A->mem(mailbox + 800) = (uint16_t)m[24];
        A->mem(mailbox + 802)[0] = (uint8_t)(m[4] | (5 << 1));
        A->set_reg(c, 4, mailbox + 800);
        A->call(c, 0x09ed26d0);
        *(uint32_t *)A->mem(mailbox + 32) = A->reg(c, 2);
    } else if (m[7] == 11) {
        // A hand-zone event avoids field destruction; this still invokes native event machinery.
        std::memset(A->mem(mailbox + 800), 0, 24);
        *(uint16_t *)A->mem(mailbox + 800) = (uint16_t)m[24];
        A->mem(mailbox + 802)[0] = (uint8_t)(m[4] | (11 << 1));
        A->set_reg(c, 4, 0); A->set_reg(c, 5, mailbox + 800);
        A->call(c, 0x09ee55c8);
        *(uint32_t *)A->mem(mailbox + 32) = A->mem(mailbox + 804)[0];
    } else if (m[7] == 10) {
        A->set_reg(c, 4, m[4]); A->set_reg(c, 5, m[24]); A->set_reg(c, 6, 0);
        A->call(c, 0x09f2a7ec);
    } else if (m[7] == 9) {
        std::memset(A->mem(mailbox + 800), 0, 24);
        *(uint16_t *)A->mem(mailbox + 800) = (uint16_t)m[24];
        A->mem(mailbox + 802)[0] = (uint8_t)m[25];
        A->set_reg(c, 4, mailbox + 800); A->set_reg(c, 5, m[26]);
        A->set_reg(c, 6, 14); A->set_reg(c, 7, 2);
        A->call(c, 0x09f35580);
        *(uint32_t *)A->mem(mailbox + 32) = A->reg(c, 2);
    } else if (m[7] == 8) {
        const uint32_t saved_mask = *(uint32_t *)A->mem(0x09f7bc48);
        *(uint32_t *)A->mem(0x09f7bc48) = m[26];
        A->set_reg(c, 4, m[4]); A->set_reg(c, 5, m[5]); A->set_reg(c, 6, 2);
        A->set_reg(c, 7, m[24]); A->set_reg(c, 8, m[25]);
        A->call(c, 0x09f3332c);
        *(uint32_t *)A->mem(mailbox + 32) = *(uint32_t *)A->mem(0x09f7bc48);
        *(uint32_t *)A->mem(0x09f7bc48) = saved_mask;
    } else {
        // Explicit action in this isolated duel: Allegrex argument five is t0.
        A->set_reg(c, 4, m[4]); A->set_reg(c, 5, m[5]); A->set_reg(c, 6, m[25]);
        A->set_reg(c, 7, m[26]); A->set_reg(c, 8, m[24]);
        A->call(c, 0x09f34d64);
        *(uint32_t *)A->mem(mailbox + 32) = A->reg(c, 2);
    }
    for (int i = 1; i < 32; i++) if (i != 26 && i != 27) A->set_reg(c, i, regs[i]);
    if (m[7] != 6 && m[7] != 9 && m[7] != 10) std::memcpy(A->mem(SIDES), saved.data(), saved.size());
    if (m[7] != 6 && m[7] != 9 && m[7] != 10) std::memcpy(A->mem(0x09f7c460), links.data(), links.size());
    m[3] = 0; m[2] = sequence;
    return 0;
}

extern "C" __declspec(dllexport) int tfmod_init(const TfModApi *api) {
    A = api;
    if (api->version < 2 || !std::getenv("TF_CTL") || !std::getenv("TF3_TEST_PROBE")) return 1;
    mailbox = A->guest_alloc(1024);
    if (!mailbox) return 1;
    std::memset(A->mem(mailbox), 0, 1024);
    A->log("TF3 test mailbox %08x", mailbox);
    A->hook(0x08833fc4, frame);
    return 0;
}
