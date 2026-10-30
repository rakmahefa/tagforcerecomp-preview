/* Mod "TF3 Cards" (plugin, API 3): cards ported from Yu-Gi-Oh! GX Tag Force 3.
 *
 * The card data (name, text, stats) comes from the mod's own PSP_GAME/USRDIR/duelsys/cardinfo_eng.ehp (made by tools/mods/tf3_porte_carta.py);
 * this plugin holds the behavior: ported cards borrow the effect record of an equivalent game card (ported.txt), plus a test aid: `force_copies` of card `force_card` replace the first cards of the
 * player's main deck, so the card shows up in the opening hand (the duel shuffles the deck, so use many copies). Settings: config.ini in the
 * mod folder (force_card=7234, force_copies=0 = off, force_deck_size=0 = keep the size; a deck of 8 cards with 3 copies always has the card in hand: more than 3
 * copies of one card hang the duel start); env TF3_FORCE_COPIES / TF3_FORCE_CARD / TF3_FORCE_DECK override them.
 *
 * Deck handling follows the Card Randomizer mod: the deck reader (0884a694) fills a 0x108 byte buffer {[0] = n_main, [1] = n_side,
 * [2] = n_extra, main at +0x0c (u16 ids)}; the player's deck is copied into the duel setup block (prepare duel, 0883f4f8) without going
 * through the reader, so it is found from the reader's output (see randomize_player_slot there). */
#include "tfmod.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <string>

static const TfModApi *A;
static uint32_t g_pos;      /* TF3_FORCE_POS: first deck position of the forced cards (the duel start runs without shuffling when the deck is kept whole) */
static bool g_in_start;
static const uint32_t F_DUEL_START = 0x09eb14f4, F_RANDOM = 0x09eb30b8;
static const uint32_t F_READ_DECK = 0x0884a694, F_PREPARE_DUEL = 0x0883f4f8;
/* libduel_eng 09eb4574: finds the effect record (6 words {id, handlers...}, one per card with special rules) of the card whose key is at a0
 * (u16 id at +0, the record kind in the low 6 bits of the byte at +3); binary search in tables sorted by id, returns the record or 0. */
static const uint32_t F_EFFECT_RECORD = 0x09eb4574;
/* The ported cards (ported.txt in the mod folder, written by tools/mods/tf3_porte_carta.py): each plays under the id of a TF1 card (slot); `like` is the
 * TF1 card whose effect record it borrows (0 = no effect yet). */
struct Ported { uint16_t slot, tf3, like; std::string name; };
static std::vector<Ported> PORTED;
static std::map<uint16_t, uint16_t> g_trig;         /* card id -> donor id of its trigger (trigger.bin) */
static std::map<uint16_t, uint16_t> g_trig_real;   /* donor id of an aliased trigger -> the new card that triggered (see hk_trigger) */
struct TriggerSource { uint16_t donor, card; uint8_t side; uint16_t token = 0xffff; bool allowed = true; };
static std::vector<TriggerSource> g_trigger_scope;
static std::map<uint16_t, uint16_t> g_trigger_conditions;
static std::map<uint16_t, uint16_t> g_trigger_record_donors;
static std::map<uint64_t, uint16_t> g_event_sources;
struct DeferredTrigger { uint32_t bucket, word, param; uint16_t token, card; };
static std::vector<DeferredTrigger> g_deferred_sources;
static uint64_t event_key(const uint8_t *o) {
    return *(const uint16_t *)o | (uint64_t)(o[2] & 1) << 16 |
        (uint64_t)((*(const uint16_t *)(o + 4) >> 6) & 0x1ff) << 17;
}
static bool real_event_ids(void) {
    static const bool enabled = !getenv("TF3_EVENT_IDS") || strcmp(getenv("TF3_EVENT_IDS"), "0") != 0;
    return enabled;
}
static uint16_t trigger_card(uint16_t id, uint32_t side, uint32_t object = 0) {
    if (real_event_ids()) {
        for (auto it = g_trigger_scope.rbegin(); it != g_trigger_scope.rend(); ++it)
            if (it->donor == id && it->side == side) {
                if (object >= 0x08000000 && object < 0x0a000000 - 24 && it->token != 0xffff &&
                    ((*(const uint16_t *)A->mem(object + 4) >> 6) & 0x1ff) != it->token) continue;
                return it->card;
            }
        const uint32_t p = object ? object : *(const uint32_t *)A->mem(0x09f7866c);
        if (p >= 0x08000000u && p < 0x0a000000u - 24) {
            const uint8_t *o = A->mem(p);
            if (*(const uint16_t *)o == id && (o[2] & 1) == side) {
                const auto found = g_event_sources.find(event_key(o));
                if (found != g_event_sources.end()) return found->second;
            }
        }
        return id;
    }
    const auto it = g_trig_real.find(id);
    return it != g_trig_real.end() ? it->second : id;
}

/* Numbers of the draw / life point effects (amount.bin, "TF3V", u32 n, {u16 card id, u8 kind (1 draw, 2 life points), u8 0, i32 self, i32 opp}), written by
 * tools/mods/tf3_efeitos_param.py: the game fixes them per card id inside its handlers, so for the new cards the handlers read them from here instead. */
struct Amount { int32_t self, opp; };
static std::map<std::pair<uint16_t, uint8_t>, Amount> g_amt;
struct AmountPer { uint8_t flags; std::vector<uint16_t> ids; };   /* flags: bit 0 count different ids only, bits 1-2 side (0 own, 1 opponent, 2 both) */
static std::map<std::pair<uint16_t, uint8_t>, AmountPer> g_amt_per;
struct AmountCost { uint8_t count; bool to_deck; std::vector<uint16_t> ids; };   /* cost: move `count` hand cards of these ids to the Graveyard (or the Deck); nothing happens if the hand has too few */
static std::map<std::pair<uint16_t, uint8_t>, AmountCost> g_amt_cost;
static std::map<std::pair<uint16_t, uint8_t>, AmountCost> g_amt_mill;   /* after the draw: send `count` cards of these ids from the Deck to the Graveyard */
static uint32_t zone_card(uint32_t side, uint32_t slot);
static Amount amount_scaled(const Amount *am, uint16_t id, uint8_t kind, uint32_t side, uint32_t object) {   /* the amount times its "per" count of monsters, when it has one */
    auto it = g_amt_per.find({id, kind});
    if (it == g_amt_per.end()) it = g_amt_per.find({trigger_card(id, side, object), kind});
    if (it == g_amt_per.end()) return *am;
    const AmountPer &r = it->second;
    const uint32_t mode = (r.flags >> 1) & 3;
    std::vector<uint16_t> seen;
    int32_t n = 0;
    for (uint32_t s = 0; s < 2; s++) {
        if (mode != 2 && s != (mode == 0 ? side : 1 - side)) continue;
        for (uint32_t k = 0; k < 5; k++) {
            const uint16_t m = (uint16_t)zone_card(s, k);
            if (!m || !std::binary_search(r.ids.begin(), r.ids.end(), m)) continue;
            if ((r.flags & 1) && std::find(seen.begin(), seen.end(), m) != seen.end()) continue;
            seen.push_back(m); n++;
        }
    }
    return Amount{am->self * n, am->opp * n};
}
static const Amount *amount_of(uint16_t id, uint8_t kind, uint32_t side, uint32_t object = 0) {   /* id = the id of the effect object */
    auto it = g_amt.find({id, kind});
    if (it != g_amt.end()) return &it->second;
    if ((it = g_amt.find({trigger_card(id, side, object), kind})) != g_amt.end()) return &it->second;
    return nullptr;
}
static void amt_load(void) {
    const std::wstring p = std::wstring(A->mod_dir) + L"\\amount.bin";
    FILE *f = _wfopen(p.c_str(), L"rb");
    if (!f) return;
    uint32_t h[2];
    if (fread(h, 4, 2, f) == 2 && h[0] == 0x56334654u /* "TF3V" */)
        for (uint32_t i = 0; i < h[1]; i++) {
            uint16_t id; uint8_t kind, pad; int32_t v[2];
            if (fread(&id, 2, 1, f) != 1 || fread(&kind, 1, 1, f) != 1 || fread(&pad, 1, 1, f) != 1 || fread(v, 4, 2, f) != 2) break;
            g_amt[{id, kind}] = Amount{v[0], v[1]};
        }
    char tag[4]; uint32_t n = 0;
    if (fread(tag, 1, 4, f) == 4 && !memcmp(tag, "TF3M", 4) && fread(&n, 4, 1, f) == 1)   /* "per" rules: the amount is multiplied by a count of monsters */
        for (uint32_t i = 0; i < n; i++) {
            uint16_t id, cnt; uint8_t kind, flags;
            if (fread(&id, 2, 1, f) != 1 || fread(&kind, 1, 1, f) != 1 || fread(&flags, 1, 1, f) != 1 || fread(&cnt, 2, 1, f) != 1) break;
            AmountPer r{flags, std::vector<uint16_t>(cnt)};
            if (cnt && fread(r.ids.data(), 2, cnt, f) != cnt) break;
            g_amt_per[{id, kind}] = std::move(r);
        }
    if (fread(tag, 1, 4, f) == 4 && !memcmp(tag, "TF3C", 4) && fread(&n, 4, 1, f) == 1)   /* discard costs */
        for (uint32_t i = 0; i < n; i++) {
            uint16_t id, cnt; uint8_t kind, need;
            if (fread(&id, 2, 1, f) != 1 || fread(&kind, 1, 1, f) != 1 || fread(&need, 1, 1, f) != 1 || fread(&cnt, 2, 1, f) != 1) break;
            AmountCost r{(uint8_t)(need & 0x7f), (need & 0x80) != 0, std::vector<uint16_t>(cnt)};
            if (cnt && fread(r.ids.data(), 2, cnt, f) != cnt) break;
            g_amt_cost[{id, kind}] = std::move(r);
        }
    if (fread(tag, 1, 4, f) == 4 && !memcmp(tag, "TF3X", 4) && fread(&n, 4, 1, f) == 1)   /* Deck -> Graveyard after the draw */
        for (uint32_t i = 0; i < n; i++) {
            uint16_t id, cnt; uint8_t kind, need;
            if (fread(&id, 2, 1, f) != 1 || fread(&kind, 1, 1, f) != 1 || fread(&need, 1, 1, f) != 1 || fread(&cnt, 2, 1, f) != 1) break;
            AmountCost r{need, false, std::vector<uint16_t>(cnt)};
            if (cnt && fread(r.ids.data(), 2, cnt, f) != cnt) break;
            g_amt_mill[{id, kind}] = std::move(r);
        }
    fclose(f);
    A->log("TF3 Cards: %u draw / life point amounts loaded", (unsigned)g_amt.size());
}
/* collection records, copy limits and the save format are per card index: see tf3_porte_carta.py */
static const uint32_t F_LP_AMOUNTS = 0x09ee3efc, F_FRAME = 0x08833fc4, F_COLLECTION_ENTRY = 0x0883de3c, SAVE = 0x08a65418, SAVE_VERSION = 0x10002;
static std::vector<int> g_cards;   /* force_cards=a,b,c: several cards, force_copies copies of each */
static int g_card = 7234, g_copies, g_deck_size, g_grant = 3, g_opponent;   /* g_card: a TF1 slot id or a TF3 id of ported.txt */
static bool g_setup_pending;

/* Cards beyond the 2480 indices of the game (tools/mods/tf3_porte_ext.py, ids from 6958 on): the save has no room for their collection records, so the
 * plugin keeps them in PSP memory (4 bytes per card like the game's: byte 0 bits 1-2 = status, byte 1 bits 0-6 = copies, byte 2 bits 0-1 = copy limit),
 * hands one out when the game asks for the record of such an id (BOOT 0883de3c) and keeps them in collection_ext.bin in the mod folder. */
static const uint32_t EXT_FIRST_ID = 6958;
static uint32_t g_ext, g_ext_n;
static std::vector<uint8_t> g_ext_saved;
static bool g_ext_writable = true;

static std::wstring ext_path(void) { return std::wstring(A->mod_dir) + L"\\collection_ext.bin"; }

static void ext_init(void) {
    for (const auto &p : PORTED)
        if (p.slot >= EXT_FIRST_ID && p.slot - EXT_FIRST_ID + 1 > g_ext_n) g_ext_n = p.slot - EXT_FIRST_ID + 1;
    if (!g_ext_n) return;
    g_ext = A->guest_alloc(4 * g_ext_n);
    uint8_t *r = A->mem(g_ext);
    for (uint32_t i = 0; i < g_ext_n; i++) { r[i * 4] = 0x02; r[i * 4 + 1] = 3; r[i * 4 + 2] = 3; r[i * 4 + 3] = 0; }   /* new, 3 copies, limit 3 */
    if (FILE *f = _wfopen(ext_path().c_str(), L"rb")) {
        std::vector<uint8_t> b(4 * g_ext_n);
        const size_t n = fread(b.data(), 1, b.size(), f);
        const bool extra = n == b.size() && fgetc(f) != EOF;
        const bool invalid = ferror(f) || n % 4 != 0 || extra;
        fclose(f);
        if (!invalid) { memcpy(r, b.data(), n); A->log("TF3 Cards: collection_ext.bin loaded (%u records)", (unsigned)(n / 4)); }
        else { g_ext_writable = false; A->log("TF3 Cards: collection file has an invalid size or could not be read; preserving it without saving"); }
    }
    g_ext_saved.assign(r, r + 4 * g_ext_n);
    A->log("TF3 Cards: %u cards beyond the game's table (ids %u..%u), collection kept in collection_ext.bin", g_ext_n, EXT_FIRST_ID, EXT_FIRST_ID + g_ext_n - 1);
}

static void ext_save(void) {
    if (!g_ext_n || !g_ext_writable) return;
    const uint8_t *r = A->mem(g_ext);
    if (memcmp(r, g_ext_saved.data(), g_ext_saved.size()) == 0) return;
    const std::wstring path = ext_path(), temp = path + L".tmp";
    FILE *f = _wfopen(temp.c_str(), L"wb");
    if (!f) { A->log("TF3 Cards: cannot open collection temp file; will retry"); return; }
    const bool wrote = fwrite(r, 1, 4 * g_ext_n, f) == 4 * g_ext_n;
    const bool closed = fclose(f) == 0;
    if (wrote && closed && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        g_ext_saved.assign(r, r + 4 * g_ext_n);
    else A->log("TF3 Cards: collection save failed; will retry");
}

static int hk_collection_entry(TfCpu *c) {
    const uint32_t id = A->reg(c, 4) & 0xffff;
    if (id < EXT_FIRST_ID || id - EXT_FIRST_ID >= g_ext_n) return 0;
    A->set_reg(c, 2, g_ext + (id - EXT_FIRST_ID) * 4);
    return 1;
}

/* Deck editor trunk list (libdeck 08efcb68(list)): the game walks the ids 4007..6957 and writes one 6-byte record per owned card
 * {id, sort key, copies} plus an index array at +0x10; it returns the count. After it, add the same records for the ids beyond the table. */
static const uint32_t F_TRUNK_LIST = 0x08efcb68, F_DK_COPIES = 0x08f18d84, F_DK_VISIBLE = 0x08f1926c, F_DK_NEW = 0x08f18c4c,
                      F_DK_SORT_TABLE = 0x08f06498, F_DK_SORT_KEY = 0x08f0dd90, NEW_COUNTER = 0x09ccb168;

static uint32_t g_trunk_obj, g_trunk_n0, g_trunk_n;
/* libdeck 08efca00(list, kind) gives a card list its two buffers from the game's own heap: 6 bytes and 2 bytes per card, the count read from a u16 table
 * indexed by kind (at 08f1b588). The trunk's count is the 2451 cards of the table; while the buffers are taken it is raised by the new cards, so the game
 * allocates (and later frees, when the deck editor closes) buffers big enough for all of them. Buffers of the plugin's own would crash the game on exit. */
static const uint32_t F_LIST_ALLOC = 0x08efca00, LIST_COUNTS = 0x08f20000u - 19064u, TRUNK_TABLE_CARDS = 2451;
static int hk_list_alloc(TfCpu *c) {
    uint16_t *count = (uint16_t *)A->mem(LIST_COUNTS + 2 * (A->reg(c, 5) & 0xffff));
    if (*count != TRUNK_TABLE_CARDS || !g_ext_n) return 0;
    const uint16_t saved = *count;
    *count = (uint16_t)(saved + g_ext_n + 8);
    A->call_original(c, F_LIST_ALLOC);
    *count = saved;
    return 1;
}

static int hk_trunk_list(TfCpu *c) {
    const uint32_t list = A->reg(c, 4), ra = A->reg(c, 31);
    A->call_original(c, F_TRUNK_LIST);
    uint32_t n = A->reg(c, 2) & 0xffff;
    const uint32_t n0 = n;
    if (getenv("TF3_DEBUG_TRUNK")) { const uint32_t *w = (const uint32_t *)A->mem(list); A->log("TF3 Cards: trunk object %08x: %08x %08x %08x %08x %08x %08x", list, w[0], w[1], w[2], w[3], w[4], w[5]); }
    const uint32_t lim = getenv("TF3_TRUNK_MAX") ? (uint32_t)atoi(getenv("TF3_TRUNK_MAX")) : g_ext_n;
    for (uint32_t id = EXT_FIRST_ID; id < EXT_FIRST_ID + g_ext_n && id < EXT_FIRST_ID + lim; id++) {
        A->set_reg(c, 4, id); A->call(c, F_DK_COPIES);
        const int32_t copies = (int32_t)A->reg(c, 2);
        A->set_reg(c, 4, id); A->call(c, F_DK_VISIBLE);
        if (A->reg(c, 2) && copies > 0) {
            A->set_reg(c, 4, 0x8031); A->call(c, F_DK_SORT_TABLE);
            A->set_reg(c, 5, A->reg(c, 2)); A->set_reg(c, 4, id); A->call(c, F_DK_SORT_KEY);
            const uint32_t key = A->reg(c, 2) & 0xffff;
            A->log("TF3 Cards: trunk id %u key %u table %08x", id, key, A->reg(c, 5));
            uint8_t *rec = A->mem(*(uint32_t *)A->mem(list + 4) + n * 6);
            *(uint16_t *)rec = (uint16_t)id; *(uint16_t *)(rec + 2) = (uint16_t)key;
            rec[4] = (uint8_t)(copies & 0x7f); rec[5] &= 0xfe;
            *(uint16_t *)A->mem(*(uint32_t *)A->mem(list + 16) + n * 2) = (uint16_t)n;
            n++;
        }
        A->set_reg(c, 4, id); A->call(c, F_DK_NEW);
        if ((int32_t)A->reg(c, 2) > 0) (*(uint16_t *)A->mem(NEW_COUNTER))++;
    }
    g_trunk_obj = list; g_trunk_n0 = n0; g_trunk_n = n;
    A->log("TF3 Cards: trunk list %u cards + %u beyond the table", n0, n - n0);
    A->set_reg(c, 31, ra); A->set_reg(c, 2, n);
    return 1;
}


/* Art on demand for the cards beyond the table. The .cip files of the mod (tf3_porte_arte.extend, lazy mode) hold no art for them: their index
 * entries are empty and the file ends with a pool of placeholder blocks (marker b"TF3X" + kind + lang + u16 slot at +0x80 of each). When the game asks for
 * the art of one of the new ids (BOOT stream API: request 08851b7c, ready 08851f00, get 08851d24) the plugin points the id's index entry at a pool block
 * (least recently used), and when the game builds the texture from a block (08834d34(texture, data)) that is a pool block, the block is filled with the
 * art of its current owner, read from ext_art_<kind>_<lang>.bin in the mod folder. */
static const uint32_t F_STREAM_REQUEST = 0x08851b7c, F_STREAM_READY = 0x08851f00, F_STREAM_GET = 0x08851d24, F_TEXTURE_FROM_DATA = 0x08834d34;
struct PoolSlot { uint32_t id; uint64_t used; };
struct LazyFile { std::vector<uint8_t> bytes; bool tried; };
static std::map<uint32_t, std::vector<PoolSlot>> g_pools;          /* stream object -> its pool */
static std::map<uint32_t, uint32_t> g_pool_block;                   /* data pointer the game used -> block start, learnt from the marker */
static std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_block_slot;   /* block start -> (obj, slot) */
static std::map<std::string, LazyFile> g_lazy_files;
static bool g_debug_lazy;

static uint32_t rd32(uint32_t a) { return *(const uint32_t *)A->mem(a); }

static void lazy_assign(uint32_t obj, uint32_t id) {
    if (id < EXT_FIRST_ID || id - EXT_FIRST_ID >= g_ext_n || obj < 0x08000000u || obj > 0x0a000000u - 0x400) return;
    if (rd32(obj + 172 + 8) != 0x58334654u) return;            /* "TF3X": a file of the mod with a pool */
    const uint32_t lo = rd32(obj + 192), hi = rd32(obj + 196), idx = rd32(obj + 200), blocks = rd32(obj + 204);
    if (id < lo || id > hi || !blocks) return;
    const uint32_t pool_start = rd32(obj + 172 + 0xc), pool = rd32(obj + 172 + 0x24);
    uint32_t *entry = (uint32_t *)A->mem(obj + 172 + idx + (id - lo) * 8);
    auto &slots = g_pools[obj];
    if (slots.empty()) slots.assign(pool, PoolSlot{0, 0});
    const uint64_t now = A->frame();
    uint32_t pick = 0;
    for (uint32_t k = 0; k < slots.size(); k++) {
        if (slots[k].id == id && entry[0] == pool_start + k * blocks) { slots[k].used = now; return; }
    }
    for (uint32_t k = 1; k < slots.size(); k++) if (slots[k].used < slots[pick].used) pick = k;
    if (slots[pick].id && slots[pick].used + 3 > now && g_debug_lazy) A->log("TF3 Cards: art pool of %08x is full (slot %u of id %u was used %llu frames ago)", obj, pick, slots[pick].id, (unsigned long long)(now - slots[pick].used));
    if (slots[pick].id) {                                       /* the previous owner loses the block */
        uint32_t *old = (uint32_t *)A->mem(obj + 172 + idx + (slots[pick].id - lo) * 8);
        if (old[0] == pool_start + pick * blocks) { old[0] = 0; old[1] = 0; }
    }
    slots[pick] = PoolSlot{id, now};
    entry[0] = pool_start + pick * blocks; entry[1] = 0;
    if (g_debug_lazy) A->log("TF3 Cards: id %u gets art pool slot %u (unit %u) in stream %08x", id, pick, entry[0], obj);
}

static const std::vector<uint8_t> *lazy_file(char kind, char lang);
static std::set<uint32_t> g_pool_addr;                              /* addresses (entry states) known to hold a pool block */

/* after the game's own code ran: when the entry of an id holds the address of its (loaded) block, put the art in it */
static void lazy_fill(uint32_t obj, uint32_t id) {
    if (!g_pools.count(obj)) return;
    const uint32_t lo = rd32(obj + 192), idx = rd32(obj + 200), blocks = rd32(obj + 204);
    const uint32_t *entry = (const uint32_t *)A->mem(obj + 172 + idx + (id - lo) * 8);
    const uint32_t addr = entry[1];
    if (addr < 0x08800000u || addr > 0x0a000000u - 0x2000) return;
    if (!g_pool_addr.count(addr)) {
        const uint8_t *m = A->mem(addr + 0x80);
        if (memcmp(m, "TF3X", 4) != 0) return;
        g_pool_addr.insert(addr);
        if (g_debug_lazy) A->log("TF3 Cards: pool block of id %u is loaded at %08x", id, addr);
    }
    const uint8_t *h = A->mem(obj + 172 + 0x28);
    const std::vector<uint8_t> *f = lazy_file((char)h[0], (char)h[1]);
    const uint32_t bytes = blocks * 0x800, k = id - EXT_FIRST_ID;
    if (f->size() < (size_t)(k + 1) * bytes) return;
    uint8_t *dst = A->mem(addr);
    if (memcmp(dst, f->data() + (size_t)k * bytes, bytes) != 0) {
        memcpy(dst, f->data() + (size_t)k * bytes, bytes);
        if (g_debug_lazy) A->log("TF3 Cards: block %08x filled with the %c art of id %u", addr, h[0], id);
    }
}

static int stream_hook(TfCpu *c, uint32_t fn, uint32_t obj, uint32_t id) {
    lazy_assign(obj, id);
    A->call_original(c, fn);
    lazy_fill(obj, id);
    return 1;
}
static int hk_stream_request(TfCpu *c) { return stream_hook(c, F_STREAM_REQUEST, A->reg(c, 4), A->reg(c, 5) & 0xffff); }
static int hk_stream_ready(TfCpu *c) { return stream_hook(c, F_STREAM_READY, A->reg(c, 4), A->reg(c, 5) & 0xffff); }
static int hk_stream_get(TfCpu *c) { return stream_hook(c, F_STREAM_GET, A->reg(c, 5), A->reg(c, 6) & 0xffff); }

static const std::vector<uint8_t> *lazy_file(char kind, char lang) {
    const std::string name = std::string("ext_art_") + kind + "_" + lang + ".bin";
    LazyFile &f = g_lazy_files[name];
    if (!f.tried) {
        f.tried = true;
        const std::wstring path = std::wstring(A->mod_dir) + L"\\" + std::wstring(name.begin(), name.end());
        if (FILE *fp = _wfopen(path.c_str(), L"rb")) {
            fseek(fp, 0, SEEK_END); const long n = ftell(fp); fseek(fp, 0, SEEK_SET);
            f.bytes.resize((size_t)n); if (fread(f.bytes.data(), 1, (size_t)n, fp) != (size_t)n) f.bytes.clear();
            fclose(fp);
        }
        A->log("TF3 Cards: %s: %zu bytes", name.c_str(), f.bytes.size());
    }
    return &f.bytes;
}

static int hk_texture_from_data(TfCpu *c) {
    const uint32_t p = A->reg(c, 5);
    if (p < 0x08800000u || p > 0x0a000000u - 0x2000) return 0;
    uint32_t block = 0;
    auto it = g_pool_block.find(p);
    if (it != g_pool_block.end()) block = it->second;
    else {
        for (uint32_t o = 0; o <= 0x200; o += 4) {              /* the block start is p, p + 0x100...: look for the marker near the pointer */
            const uint8_t *m = A->mem(p + o - 0x100 + 0x80);
            if (memcmp(m, "TF3X", 4) == 0 && (m[4] == 'h' || m[4] == 'm')) { block = p + o - 0x100; break; }
        }
        if (!block) {
            if (g_debug_lazy) { static int n; for (uint32_t o = 0; o < 0x1000 && n < 20; o += 4) if (memcmp(A->mem(p + o), "TF3Xm", 5) == 0) { A->log("TF3 Cards: cardm marker at +%x of texture data %08x (tex %08x)", o, p, A->reg(c, 4)); n++; break; } }
            return 0;
        }
        g_pool_block[p] = block;
        if (g_debug_lazy) A->log("TF3 Cards: texture data %08x is the pool block at %08x (offset %d)", p, block, (int)(block - p));
    }
    const uint8_t *m = A->mem(block + 0x80);
    char kind, lang; uint32_t slot;
    auto bs = g_block_slot.find(block);
    static std::map<uint32_t, std::pair<char, char>> kinds;
    if (memcmp(m, "TF3X", 4) == 0) { kind = (char)m[4]; lang = (char)m[5]; slot = *(const uint16_t *)(m + 6); kinds[block] = {kind, lang}; g_block_slot[block] = {0, slot}; }
    else if (bs != g_block_slot.end()) { slot = bs->second.second; kind = kinds[block].first; lang = kinds[block].second; }
    else return 0;
    uint32_t id = 0;
    for (auto &po : g_pools) { auto &sl = po.second; const uint8_t *h = A->mem(po.first + 172 + 0x28); if (slot < sl.size() && sl[slot].id && h[0] == (uint8_t)kind && h[1] == (uint8_t)lang) { id = sl[slot].id; break; } }
    if (!id) return 0;
    const std::vector<uint8_t> *f = lazy_file(kind, lang);
    const uint32_t bytes = (kind == 'h' ? 2 : 1) * 0x800, k = id - EXT_FIRST_ID;
    if (f->size() < (size_t)(k + 1) * bytes) return 0;
    uint8_t *dst = A->mem(block);
    if (memcmp(dst, f->data() + (size_t)k * bytes, bytes) != 0) {
        memcpy(dst, f->data() + (size_t)k * bytes, bytes);
        if (g_debug_lazy) A->log("TF3 Cards: block %08x filled with the %c art of id %u (%c)", block, kind, id, lang);
    }
    return 0;
}

/* The tables of the cards beyond the game's table. The game reads the whole cardinfo_<l>.ehp into a fixed buffer (BOOT 08ac7714, little room), so the mod
 * does not carry that file: ext_info_<l>.bin (tools/mods/tf3_porte_ext.py) holds the rows of the new cards, and when the game looks a CARD_ member up in the
 * file (BOOT 08833540(ehp, name, &size), which it does once while loading) the plugin answers with a copy of the table plus the new rows (guest memory,
 * never freed, one per member). Every consumer then sees the longer tables: CARD_Prop/Pass/Genre rows, CARD_IntID, CARD_Indx/Name/Desc and CARD_Sort. */
static const uint32_t F_EHP_MEMBER = 0x08833540, CARDINFO_BUF = 0x08ac7714, TF1_INDEX_COUNT = 2480;
struct ExtInfo { std::vector<uint8_t> raw; bool tried; };
static std::map<char, ExtInfo> g_info;
struct Built { uint32_t orig, orig_size, ptr, size; };
static std::map<std::string, Built> g_built;

static ExtInfo &ext_info(char lang) {
    ExtInfo &e = g_info[lang];
    if (e.tried) return e;
    e.tried = true;
    const std::wstring name = std::wstring(L"ext_info_") + (wchar_t)lang + L".bin";
    if (FILE *f = _wfopen((std::wstring(A->mod_dir) + L"\\" + name).c_str(), L"rb")) {
        fseek(f, 0, SEEK_END); const long len = ftell(f); fseek(f, 0, SEEK_SET);
        e.raw.resize((size_t)len);
        if (fread(e.raw.data(), 1, e.raw.size(), f) != e.raw.size() || len < 20 || memcmp(e.raw.data(), "TF3I", 4) != 0 || *(const uint32_t *)(e.raw.data() + 4) != g_ext_n) e.raw.clear();
        fclose(f);
    }
    A->log("TF3 Cards: ext_info_%c.bin: %zu bytes", lang, e.raw.size());
    return e;
}

static uint32_t ehp_member_size(uint32_t base, const char *want) {
    const uint8_t *b = A->mem(base);
    if (memcmp(b, "EHP\x03", 4) != 0) return 0;
    const uint32_t n = *(const uint32_t *)(b + 12);
    for (uint32_t i = 0; i < n && i < 64; i++) {
        const uint32_t no = *(const uint32_t *)(b + 16 + 8 * i);
        if (no > 0x2000) return 0;
        const char *name = (const char *)(b + no);
        if (strcmp(name, want) == 0) return *(const uint32_t *)(b + no + strlen(name) + 1);
    }
    return 0;
}

static int hk_ehp_member(TfCpu *c) {
    const uint32_t base = A->reg(c, 4), name_ptr = A->reg(c, 5), size_ptr = A->reg(c, 6);
    if (!g_ext_n || base != CARDINFO_BUF || name_ptr < 0x08000000u || name_ptr > 0x0a000000u - 64) return 0;
    char name[64]; memcpy(name, A->mem(name_ptr), 63); name[63] = 0;
    if (strncmp(name, "CARD_", 5) != 0) return 0;
    A->call_original(c, F_EHP_MEMBER);
    const uint32_t orig = A->reg(c, 2);
    if (!orig) return 1;
    const char *what = name + 5;
    char lang = 'e';
    if (strncmp(what, "Indx_", 5) == 0 || strncmp(what, "Name_", 5) == 0 || strncmp(what, "Desc_", 5) == 0 || strncmp(what, "Sort_", 5) == 0) lang = (char)tolower((unsigned char)what[5]);
    else if (strncmp(what, "Prop.", 5) != 0 && strncmp(what, "Pass.", 5) != 0 && strncmp(what, "Genre", 5) != 0 && strncmp(what, "IntID", 5) != 0) return 1;
    const uint32_t orig_size = ehp_member_size(base, name);
    Built &b = g_built[name];
    if (b.orig != orig || b.orig_size != orig_size) {
        b = Built{orig, orig_size, 0, 0};
        const ExtInfo &info = ext_info(lang);
        const uint32_t n = g_ext_n;
        if (info.raw.empty()) return 1;
        const uint8_t *r = info.raw.data();
        const uint32_t sort_n = *(const uint32_t *)(r + 8), nbytes = *(const uint32_t *)(r + 12), dbytes = *(const uint32_t *)(r + 16);
        const uint8_t *prop = r + 20, *pass = prop + 8 * n, *genre = pass + 4 * n, *intid = genre + 6 * n, *sort = intid + 2 * n,
                      *noffs = sort + 2 * sort_n, *ntext = noffs + 4 * (n + 1), *doffs = ntext + nbytes, *dtext = doffs + 4 * (n + 1);
        (void)dbytes;
        std::vector<uint8_t> out;
        const uint8_t *src = A->mem(orig);
        auto append = [&](const uint8_t *p, size_t len) { out.insert(out.end(), p, p + len); };
        bool ok = true;
        if (strncmp(what, "Prop.", 5) == 0)       { ok = orig_size == 8 * TF1_INDEX_COUNT; append(src, orig_size); append(prop, 8 * n); }
        else if (strncmp(what, "Pass.", 5) == 0)  { ok = orig_size == 4 * TF1_INDEX_COUNT; append(src, orig_size); append(pass, 4 * n); }
        else if (strncmp(what, "Genre", 5) == 0)  { ok = orig_size == 6 * TF1_INDEX_COUNT; append(src, orig_size); append(genre, 6 * n); }
        else if (strncmp(what, "IntID", 5) == 0)  { ok = orig_size == 2 * (EXT_FIRST_ID - 4000); append(src, orig_size); append(intid, 2 * n); }
        else if (strncmp(what, "Sort_", 5) == 0)  { ok = orig_size == 2 * TF1_INDEX_COUNT && sort_n == TF1_INDEX_COUNT + n; append(sort, 2 * sort_n); }
        else if (strncmp(what, "Name_", 5) == 0)  { append(src, orig_size); append(ntext, nbytes); }
        else if (strncmp(what, "Desc_", 5) == 0)  { append(src, orig_size); append(dtext, dbytes); }
        else if (strncmp(what, "Indx_", 5) == 0) {
            ok = orig_size == 8 * (TF1_INDEX_COUNT + 1);
            if (ok) {
                const uint32_t *sent = (const uint32_t *)(src + 8 * TF1_INDEX_COUNT);       /* the sentinel: where the Name and Desc members end */
                append(src, 8 * TF1_INDEX_COUNT);
                for (uint32_t k = 0; k <= n; k++) {
                    const uint32_t e[2] = {sent[0] + ((const uint32_t *)noffs)[k], sent[1] + ((const uint32_t *)doffs)[k]};
                    append((const uint8_t *)e, 8);
                }
            }
        }
        if (!ok) { A->log("TF3 Cards: %s has an unexpected size (%u): left alone", name, orig_size); out.clear(); }
        if (!out.empty()) {
            b.ptr = A->guest_alloc((uint32_t)out.size()); b.size = (uint32_t)out.size();
            memcpy(A->mem(b.ptr), out.data(), out.size());
            A->log("TF3 Cards: %s extended: %u -> %u bytes", name, orig_size, b.size);
        }
    }
    if (b.ptr) {
        A->set_reg(c, 2, b.ptr);
        if (size_ptr >= 0x08000000u && size_ptr < 0x0a000000u) *(uint32_t *)A->mem(size_ptr) = b.size;
    }
    return 1;
}

static void ported_load(void) {
    const std::wstring p = std::wstring(A->mod_dir) + L"\\ported.txt";
    if (FILE *f = _wfopen(p.c_str(), L"r")) {
        char l[256]; unsigned slot, tf3, like; int off;
        while (fgets(l, sizeof l, f)) {
            if (sscanf(l, "%u %u %u %n", &slot, &tf3, &like, &off) < 3) continue;
            std::string name = l + off;
            while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
            PORTED.push_back({(uint16_t)slot, (uint16_t)tf3, (uint16_t)like, name});
        }
        fclose(f);
    }
}

static void cfg_load(void) {
    const std::wstring p = std::wstring(A->mod_dir) + L"\\config.ini";
    if (FILE *f = _wfopen(p.c_str(), L"r")) {
        char l[128]; int v;
        while (fgets(l, sizeof l, f)) {
            if (sscanf(l, "force_card=%d", &v) == 1) g_card = v;
            else if (sscanf(l, "force_copies=%d", &v) == 1) g_copies = v;
            else if (sscanf(l, "force_deck_size=%d", &v) == 1) g_deck_size = v;
            else if (sscanf(l, "grant_copies=%d", &v) == 1) g_grant = v;
            else if (strncmp(l, "force_cards=", 12) == 0) {
                g_cards.clear();
                for (const char *q = l + 12; *q;) { char *e; const long id = strtol(q, &e, 10); if (e == q) break; g_cards.push_back((int)id); q = *e ? e + 1 : e; }
            }
        }
        fclose(f);
    }
    if (const char *e = getenv("TF3_FORCE_COPIES")) g_copies = atoi(e);
    if (const char *e = getenv("TF3_FORCE_CARD")) g_card = atoi(e);
    if (const char *e = getenv("TF3_FORCE_OPP")) g_opponent = atoi(e);   /* 1: force the copies into the opponent's deck instead of yours */
    for (const auto &a : PORTED) if (a.tf3 == g_card) g_card = a.slot;   /* the TF3 id of a ported card is its slot's id in the deck */
    if (const char *e = getenv("TF3_FORCE_CARDS")) {
        g_cards.clear();
        for (const char *q = e; *q;) { char *x; const long id = strtol(q, &x, 10); if (x == q) break; g_cards.push_back((int)id); q = *x ? x + 1 : x; }
    }
    if (g_cards.empty()) g_cards.push_back(g_card);
    for (auto &id : g_cards) for (const auto &a : PORTED) if (a.tf3 == id) { id = a.slot; break; }
    if (const char *e = getenv("TF3_FORCE_POS")) g_pos = (uint32_t)atoi(e);
    if (const char *e = getenv("TF3_FORCE_DECK")) g_deck_size = atoi(e);
}

static void force_cards(uint32_t out) {
    uint8_t *o = A->mem(out);
    uint32_t nm = ((uint32_t *)o)[0];
    if (nm < 1 || nm > 80) return;
    uint16_t *main = (uint16_t *)(o + 0x0c);
    if (g_deck_size > 0 && (uint32_t)g_deck_size < nm) ((uint32_t *)o)[0] = nm = (uint32_t)g_deck_size;   /* a tiny deck puts the copies in the opening hand */
    uint32_t n = 0;
    for (const int id : g_cards)
        for (int k = 0; k < g_copies && n < nm; k++) main[n++] = (uint16_t)id;
    A->log("TF3 Cards: %u forced cards (%u kinds, %d copies each) put in the deck at %08x (%u cards)", n, (unsigned)g_cards.size(), g_copies, out, nm);
}

/* A whole deck with the forced cards in it: the duel start shuffles with the game random, so make that random return 0 while it runs (the order is
 * then fixed and the forced cards are placed where the opening hand takes them). The first-turn draw is the same call, so it also becomes fixed. */
static int hk_duel_start(TfCpu *c) {
    g_event_sources.clear();
    g_deferred_sources.clear();
    g_trig_real.clear();
    if (g_copies <= 0 || g_deck_size > 0) return 0;
    g_in_start = true; A->call_original(c, F_DUEL_START); g_in_start = false;
    return 1;
}
static int hk_random(TfCpu *c) {
    if (!g_in_start) return 0;
    A->set_reg(c, 2, 0);
    return 1;
}

static int hk_prepare_duel(TfCpu *c) {
    (void)c;
    g_setup_pending = g_copies > 0;
    return 0;
}

/* the player's deck sits in one of the setup block's slots (deck index at slot + 0x124 >= 0x15f): try every block alignment */
static void force_player_slot(uint32_t out) {
    if (!g_setup_pending) return;
    for (int k = 0; k < 4; k++) {
        const uint32_t base = out - 0x1a4 - (uint32_t)k * 0x18c;
        if (base < 0x08000000u || base > 0x0a000000u - 0x4000) continue;
        for (int m = 0; m < 4; m++) {
            const uint32_t slot = base + (uint32_t)m * 0x18c;
            if (*(uint16_t *)A->mem(slot + 0x124) < 0x15f) continue;
            const uint32_t *h0 = (const uint32_t *)A->mem(base), *h1 = (const uint32_t *)A->mem(slot + 0x1a4);
            if (h1[0] < 1 || h1[0] > 80 || memcmp(h0, h1, 12)) continue;
            g_setup_pending = false;
            force_cards(slot + 0x1a4);
            memcpy(A->mem(base), A->mem(slot + 0x1a4), 0x108);   /* the copy at the start of the block stays equal */
            return;
        }
    }
    for (int j = 1; j <= 4; j++) {   /* `out` is slot j's deck buffer; the player's is slot 0 */
        const uint32_t base = out - 0x1a4 - (uint32_t)(j % 4) * 0x18c;
        if (base < 0x08000000u || base > 0x0a000000u - 0x300) continue;
        const uint32_t *h0 = (const uint32_t *)A->mem(base), *h1 = (const uint32_t *)A->mem(base + 0x1a4);
        if (h0[0] < 1 || h0[0] > 80 || memcmp(h0, h1, 12)) continue;
        g_setup_pending = false;
        force_cards(base + 0x1a4);
        memcpy(A->mem(base), A->mem(base + 0x1a4), 0x108);
        return;
    }
}

static int hk_read_deck(TfCpu *c) {   /* libehsys_85A292D7(rcp_index, out): observe only, the original still runs */
    if (g_copies <= 0) return 0;
    const uint32_t out = A->reg(c, 5);
    A->call_original(c, F_READ_DECK);
    if (g_opponent) force_cards(out);   /* test aid: the decks the reader fills belong to the opponent */
    else force_player_slot(out);
    return 1;
}

/* collection record of a card id (BOOT 0883de3c: SAVE + 0x155c + index * 4; byte 0 bits 1-2 = status 0 unseen / 1 new / 2 seen, byte 1 bits 0-6 = copies) */
static uint32_t collection_entry(TfCpu *c, uint32_t id) {
    uint32_t s[32]; for (int i = 1; i < 32; i++) s[i] = A->reg(c, i);
    A->set_reg(c, 4, id); A->call(c, F_COLLECTION_ENTRY); const uint32_t r = A->reg(c, 2);
    for (int i = 1; i < 32; i++) if (i != 26 && i != 27) A->set_reg(c, i, s[i]);
    return r;
}

/* grant_copies: a ported card the player does not own yet is added to the collection (marked "new") so it shows up in the trunk. The game reloads
 * the save when Continue is pressed, so this runs every half second once a save is in memory; it only touches cards with 0 copies. */
static int hk_frame(TfCpu *c) {
    static uint64_t dbg_next;
    if (g_trunk_n && getenv("TF3_DEBUG_TRUNK") && A->frame() >= dbg_next) {
        dbg_next = A->frame() + 300;
        const uint16_t *ord = (const uint16_t *)A->mem(*(uint32_t *)A->mem(g_trunk_obj + 16)), *base = nullptr;
        (void)base;
        for (uint32_t i = 0; i < g_trunk_n; i++) if (ord[i] >= g_trunk_n0) A->log("TF3 Cards: trunk position %u shows entry %u", i, ord[i]);
    }
    if (const char *e = getenv("TF3_DEBUG_BUMP")) {   /* "<game id>:<copies>": set the copies of an ext card once (test of collection_ext.bin) */
        static bool done;
        unsigned id, n;
        if (!done && A->frame() > 600 && g_ext_n && sscanf(e, "%u:%u", &id, &n) == 2 && id >= EXT_FIRST_ID && id - EXT_FIRST_ID < g_ext_n) {
            done = true;
            uint8_t *r = A->mem(g_ext + (id - EXT_FIRST_ID) * 4);
            r[0] = (uint8_t)((r[0] & ~6) | 4); r[1] = (uint8_t)((r[1] & 0x80) | (n & 0x7f));
            A->log("TF3 Cards: debug bump id %u -> %u copies", id, n);
        }
    }
    static uint64_t next, ext_next;
    if (g_ext_n && A->frame() >= ext_next) { ext_next = A->frame() + 60; ext_save(); }
    if (g_grant <= 0 || A->frame() < next) return 0;
    next = A->frame() + 30;
    if (*(uint32_t *)A->mem(SAVE) != SAVE_VERSION) return 0;
    for (const auto &pc : PORTED) {
        const uint16_t id = pc.slot;
        if (id >= EXT_FIRST_ID) continue;                  /* the plugin owns those records */
        const uint32_t e = collection_entry(c, id);
        if (!e) continue;
        uint8_t &flags = *A->mem(e), &copies = *A->mem(e + 1);
        if (copies & 0x7f) continue;
        copies = (uint8_t)((copies & 0x80) | (g_grant > 3 ? 3 : g_grant));
        if (((flags >> 1) & 3) == 0) flags = (uint8_t)((flags & 0xf9) | (1 << 1));
        A->log("TF3 Cards: granted %d copies of card %u", g_grant, id);
    }
    return 0;
}

/* libduel_eng 09ee3efc: the life point change of the life point cards, by card id. a0 = the effect object (u16 id at +0, byte +2 bit 0 = side, byte +3 bit 1 = swapped
 * side), a1 = out int[2] (LP delta per player index; the caller damages the negative ones and heals the positive ones). Ookazi writes -800 to the opponent's index. */
static int hk_lp_amounts(TfCpu *c) {
    const uint32_t obj = A->reg(c, 4), out = A->reg(c, 5);
    if (obj < 0x08000000u || obj > 0x0a000000u - 0x20 || out < 0x08000000u || out > 0x0a000000u - 8) return 0;
    const uint8_t *o = A->mem(obj);
    static const bool debug = getenv("TF3_DEBUG_LP") != nullptr;   /* research: shows how the game fills the amounts for its own cards */
    if (debug) {
        const uint16_t id = *(const uint16_t *)o;
        A->call_original(c, F_LP_AMOUNTS);
        const int32_t *d = (const int32_t *)A->mem(out);
        A->log("LP amounts: card %u byte2 %02x byte3 %02x -> player0 %d, player1 %d", id, o[2], o[3], d[0], d[1]);
        return 1;
    }
    if (const Amount *am = amount_of(*(const uint16_t *)o, 2, o[2] & 1, obj)) {
        const uint32_t side = (uint32_t)((o[2] & 1) ^ ((o[3] >> 6) & 1));
        if (getenv("TF3_DEBUG_FX")) A->log("life point effect of id %u: controller %d, other player %d", *(const uint16_t *)o, am->self, am->opp);
        const Amount n = amount_scaled(am, *(const uint16_t *)o, 2, o[2] & 1, obj);
        int32_t *d = (int32_t *)A->mem(out);
        d[side] = n.self;
        d[1 - side] = n.opp;
        return 1;
    }
    return 0;
}

/* Suit of Sword X (TF3 3734): "toss a coin; heads: destroy all monsters your opponent controls; tails: destroy all monsters you control".
 * Built from what the game already has: it borrows the record of Dark Hole (driver 09ee3b7c walks every monster zone and asks the target filter 09ebbbe8 of the
 * record whether the monster is hit, then destroys the chosen ones). The driver hook tosses the coin with the game's own random (09eb30b8) and shows the coin
 * (animation command 09e998a0, as Fiend Comedian does); the filter hook then narrows Dark Hole's "every monster" to one side while that effect resolves. */
static const uint16_t TF3_SUIT_OF_SWORD_X = 3734;
static const uint32_t F_EFFECT_DRIVER = 0x09ee3b7c, F_TARGET_ALL = 0x09ebbbe8, F_TARGET_OPP = 0x09ebbca0, F_TARGET_ST = 0x09ebe088, F_TARGET_ATK = 0x09ebe884, F_ATTACK_TRIGGER = 0x09ed5870, F_COIN_ANIM = 0x09e998a0;
static int g_coin = -1;                                       /* -1 = not resolving; 1 = heads (opponent's side), 0 = tails (own side) */

static uint16_t slot_of(uint16_t tf3) { for (const auto &p : PORTED) if (p.tf3 == tf3) return p.slot; return 0; }
static bool sword_return_enabled() {
    const auto rule = g_trigger_conditions.find(slot_of(7152));
    return rule != g_trigger_conditions.end() && (rule->second & 16);
}
static bool gladius_return_enabled() {
    const auto rule = g_trigger_conditions.find(slot_of(7309));
    return rule != g_trigger_conditions.end() && (rule->second & 32);
}
static bool sabre_mill_enabled() {
    const auto rule = g_trigger_conditions.find(slot_of(7749));
    return rule != g_trigger_conditions.end() && (rule->second & 64);
}

static uint32_t call_game(TfCpu *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {   /* keeps every caller register */
    uint32_t s[32]; for (int i = 1; i < 32; i++) s[i] = A->reg(c, i);
    A->set_reg(c, 4, a0); A->set_reg(c, 5, a1); A->set_reg(c, 6, a2); A->set_reg(c, 7, a3); A->call(c, fn);
    const uint32_t r = A->reg(c, 2);
    for (int i = 1; i < 32; i++) if (i != 26 && i != 27) A->set_reg(c, i, s[i]);
    return r;
}

static uint32_t call_game6(TfCpu *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
    // Allegrex EABI passes arguments five and six in t0/t1, not on the stack.
    const uint32_t saved[2] = {A->reg(c, 8), A->reg(c, 9)};
    A->set_reg(c, 8, a4); A->set_reg(c, 9, a5);
    const uint32_t result = call_game(c, fn, a0, a1, a2, a3);
    A->set_reg(c, 8, saved[0]); A->set_reg(c, 9, saved[1]);
    return result;
}

/* Discard cost: moves `count` matching cards of the player's hand to the Graveyard with the engine's own steps (09f4be50 removes the card from the hand zone
 * into the record at 09f788d8, 09f4bff8 adds the record to the Graveyard). Returns false (and moves nothing) when the hand has too few matching cards. */
static bool discard_cost(TfCpu *c, uint32_t side, const AmountCost &cost) {
    const uint32_t base = 0x09f80000u - 25364u + side * 4012u;   /* the player block: see DUEL_SIDES */
    std::vector<uint32_t> pick;
    const uint32_t hand = *(const uint32_t *)A->mem(base + 12);
    for (uint32_t i = 0; i < hand && i < 20 && pick.size() < cost.count; i++)
        if (std::binary_search(cost.ids.begin(), cost.ids.end(), (uint16_t)(*(const uint32_t *)A->mem(base + 0x120 + i * 4) & 0x1fff))) pick.push_back(i);
    if (pick.size() < cost.count) return false;
    for (size_t k = pick.size(); k-- > 0;) {   /* highest index first: the lower ones keep their place */
        call_game(c, 0x09f4be50u, side, 11, pick[k], 0);
        call_game(c, 0x09f4bff8u, side, cost.to_deck ? 13 : 14, *(const uint32_t *)A->mem(base + (cost.to_deck ? 16 : 20)), 0x09f80000u - 28456u);
    }
    return true;
}

/* Deck -> Graveyard: the first `count` Deck cards that match (all of them when the id list holds every card). */
static void mill_cards(TfCpu *c, uint32_t side, const AmountCost &m) {
    const uint32_t base = 0x09f80000u - 25364u + side * 4012u;
    for (uint32_t k = 0; k < m.count; k++) {
        const uint32_t n = *(const uint32_t *)A->mem(base + 16);
        uint32_t i = 0;
        while (i < n && i < 160 && !std::binary_search(m.ids.begin(), m.ids.end(), (uint16_t)(*(const uint16_t *)A->mem(base + 928 + i * 4) & 0x1fff))) i++;
        if (i >= n || i >= 160) break;
        call_game(c, 0x09f4be50u, side, 13, i, 0);
        call_game(c, 0x09f4bff8u, side, 14, *(const uint32_t *)A->mem(base + 20), 0x09f80000u - 28456u);
    }
}

/* libduel_eng 09ee56a0: the resolution of the draw effects (Pot of Greed and others). It decides by card id how many cards are drawn and by whom, then calls
 * 09f2a3fc(player, count). For the cards of amount.bin it draws the listed numbers: self = the card's controller, opp = the other player. */
static const uint32_t F_DRAW = 0x09ee56a0, F_DRAW_CARDS = 0x09f2a3fc;
static int hk_draw(TfCpu *c) {
    const uint32_t obj = A->reg(c, 4);
    if (obj < 0x08000000u || obj > 0x0a000000u - 0x20) return 0;
    const uint8_t *o = A->mem(obj);
    const Amount *am = amount_of(*(const uint16_t *)o, 1, o[2] & 1, obj);
    if (!am) return 0;
    const uint32_t side = (uint32_t)((o[2] & 1) ^ ((o[3] & 0x7f) >> 6));
    Amount n = amount_scaled(am, *(const uint16_t *)o, 1, o[2] & 1, obj);
    bool paid = true;
    {
        auto ct = g_amt_cost.find({*(const uint16_t *)o, 1});
        if (ct == g_amt_cost.end()) ct = g_amt_cost.find({trigger_card(*(const uint16_t *)o, o[2] & 1, obj), 1});
        if (ct != g_amt_cost.end() && !discard_cost(c, side, ct->second)) n = Amount{0, 0}, paid = false;   /* the cost could not be paid: nothing is drawn */
    }
    static const bool dbg = getenv("TF3_DEBUG_FX") != nullptr;
    if (dbg) A->log("draw effect of id %u: controller %u draws %d, other player draws %d", *(const uint16_t *)o, side, n.self, n.opp);
    if (n.self > 0) call_game(c, F_DRAW_CARDS, side, (uint32_t)n.self, 0, 0);
    if (n.opp > 0) call_game(c, F_DRAW_CARDS, 1 - side, (uint32_t)n.opp, 0, 0);
    if (paid) {
        auto mt = g_amt_mill.find({*(const uint16_t *)o, 1});
        if (mt == g_amt_mill.end()) mt = g_amt_mill.find({trigger_card(*(const uint16_t *)o, o[2] & 1, obj), 1});
        if (mt != g_amt_mill.end()) mill_cards(c, side, mt->second);
    }
    A->set_reg(c, 2, 0);
    return 1;
}

struct DestroyRule { uint8_t side, zones, cond, param; std::vector<uint16_t> ids; };   /* side: 1 own, 2 opponent, 3 both; zones: 1 monsters, 2 Spell / Trap, 3 both */
static std::map<uint16_t, DestroyRule> g_destroy;
static int g_destroy_taken = 0;                                  /* cards a "destroy 1" rule has already picked while resolving */
static std::map<uint16_t, DestroyRule>::iterator destroy_find(uint32_t obj) {   /* the rule of the effect object's own id, or of the card a trigger alias stands for (FLIP / battle monsters answer with the donor's id) */
    const uint8_t *o = A->mem(obj);
    auto it = g_destroy.find(*(const uint16_t *)o);
    if (it == g_destroy.end()) it = g_destroy.find(trigger_card(*(const uint16_t *)o, o[2] & 1, obj));
    return it;
}
static bool g_destroying = false;                                /* true while the driver resolves a rule's card: the condition was checked when the card was activated */

static bool is_card(uint32_t obj, uint16_t tf3) {
    if (obj < 0x08000000u || obj > 0x0a000000u - 0x20) return false;
    const uint16_t slot = slot_of(tf3);
    return slot && *(const uint16_t *)A->mem(obj) == slot;
}

static int hk_effect_driver(TfCpu *c) {
    const uint32_t obj = A->reg(c, 4);
    if (obj >= 0x08000000u && obj <= 0x0a000000u - 0x20 && destroy_find(obj) != g_destroy.end()) {
        g_destroying = true; g_destroy_taken = 0;
        A->call_original(c, F_EFFECT_DRIVER);
        g_destroying = false;
        return 1;
    }
    if (!is_card(obj, TF3_SUIT_OF_SWORD_X)) return 0;
    const uint8_t b2 = A->mem(obj)[2];
    static const char *force = getenv("TF3_FORCE_COIN");   /* test aid: 0 = tails, 1 = heads */
    g_coin = force ? atoi(force) & 1 : (int)(call_game(c, F_RANDOM, 2, 0, 0, 0) & 1);
    call_game(c, F_COIN_ANIM, b2 & 1, (b2 >> 1) & 31, 1, (uint32_t)g_coin);
    A->log("Suit of Sword X: coin %s", g_coin ? "heads" : "tails");
    A->call_original(c, F_EFFECT_DRIVER);
    g_coin = -1;
    return 1;
}

static const uint32_t DUEL_SIDES = 0x09f80000u - 25364u, DUEL_SIDE_SIZE = 4012;
static uint32_t zone_card(uint32_t side, uint32_t slot);

/* "Destroy all" effects as data (destroy.bin, written by tools/mods/tf3_efeitos_param.py): the card borrows the record of Dark Hole / Raigeki / Heavy Storm; the
 * target filters of those records (09ebbbe8, 09ebbca0, 09ebe088) are asked once per zone with (effect object, side, zone; zones 0..4 = monsters, 5.. = Spell / Trap)
 * both to check that the card can be activated and to pick the cards to destroy. For the cards of the file the filter narrows the donor's choice to the sides and zones
 * of the rule, and while the card is only being checked for activation it also evaluates the rule's condition ("Activate only when ..."). */
static void destroy_load(void) {
    const std::wstring p = std::wstring(A->mod_dir) + L"\\destroy.bin";
    FILE *f = _wfopen(p.c_str(), L"rb");
    if (!f) return;
    uint32_t h[2];
    if (fread(h, 4, 2, f) == 2 && h[0] == 0x44334654u /* "TF3D" */) {
        for (uint32_t i = 0; i < h[1]; i++) {
            uint16_t id, n; DestroyRule r;
            uint8_t b[4];
            if (fread(&id, 2, 1, f) != 1 || fread(b, 1, 4, f) != 4 || fread(&n, 2, 1, f) != 1) break;
            r.side = b[0]; r.zones = b[1]; r.cond = b[2]; r.param = b[3]; r.ids.resize(n);
            if (n && fread(r.ids.data(), 2, n, f) != n) break;
            g_destroy[id] = std::move(r);
        }
    }
    fclose(f);
    A->log("TF3 Cards: %u destroy rules loaded", (unsigned)g_destroy.size());
}

static bool g_in_cond = false;
static bool destroy_cond(TfCpu *c, uint32_t obj, const DestroyRule &r, uint32_t own) {
    if (r.cond == 1 || r.cond == 2 || r.cond == 3) {   /* 1: all 5 monster zones of the opponent are occupied; 2: all 5 Spell / Trap zones of the opponent; 3: all 5 of both kinds of this side */
        int n = 0;
        for (uint32_t k = 0; k < 5; k++) n += r.cond == 2 ? (*(const uint16_t *)A->mem(DUEL_SIDES + (1 - own) * DUEL_SIDE_SIZE + 0x30 + 100 + 20 * k) & 0x1fff) != 0
                                                          : (*(const uint16_t *)A->mem(DUEL_SIDES + (1 - own) * DUEL_SIDE_SIZE + 0x30 + 20 * k) & 0x1fff) != 0;
        return n == 5;
    }
    if (r.cond == 5) {   /* at least `param` Attack Position monsters of the opponent: counted with the filter of Mirror Force (the game's own "face-up Attack Position monster of the other side") */
        int n = 0;
        g_in_cond = true;
        for (uint32_t k = 0; k < 5; k++) n += call_game(c, F_TARGET_ATK, obj, 1 - own, k, 0) & 1;
        g_in_cond = false;
        if (getenv("TF3_DEBUG_FX")) A->log("attack position monsters of the opponent: %d (needs %u)", n, (unsigned)r.param);
        return n >= r.param;
    }
    if (r.cond == 4) {   /* you control at least `param` face-up monsters from the id set */
        int n = 0;
        for (uint32_t k = 0; k < 5; k++) { const uint32_t id = zone_card(own, k); if (id && std::binary_search(r.ids.begin(), r.ids.end(), (uint16_t)id)) n++; }
        return n >= r.param;
    }
    return true;
}

static int hk_target_all(TfCpu *c) {   /* a0 = effect object, a1 = side, a2 = zone; false for the monsters the coin spares */
    const uint32_t obj = A->reg(c, 4);
    if (g_in_cond) return 0;
    static const bool dbg0 = getenv("TF3_DEBUG_FX") != nullptr;
    if (dbg0 && obj >= 0x08000000u && obj <= 0x0a000000u - 0x20 && A->reg(c, 6) == 0) A->log("target filter %08x: object id %u side %u", A->reg(c, 31), (unsigned)*(const uint16_t *)A->mem(obj), A->reg(c, 5));
    if (obj >= 0x08000000u && obj <= 0x0a000000u - 0x20) {
        const auto d = destroy_find(obj);
        if (d != g_destroy.end()) {
            const DestroyRule &r = d->second;
            const uint32_t own = A->mem(obj)[2] & 1, side = A->reg(c, 5) & 1, zone = A->reg(c, 6);
            const bool ok = ((r.side & (side == own ? 1 : 2)) != 0) && ((r.zones & (zone < 5 ? 1 : 2)) != 0) && (g_destroying || destroy_cond(c, obj, r, own));
            static const bool dbg = getenv("TF3_DEBUG_FX") != nullptr;
            if (dbg) A->log("destroy filter of id %u: side %u zone %u resolving %d -> %s", (unsigned)*(const uint16_t *)A->mem(obj), side, zone, (int)g_destroying, ok ? "ask the game" : "no");
            if (!ok) { A->set_reg(c, 2, 0); return 1; }
            if ((r.cond == 6 || r.cond == 7) && zone < 5) {   /* "Destroy 1 face-up <filter> monster": only face-up monsters of the id set are targets, and only the first one is destroyed */
                const uint32_t id = zone_card(side, zone);
                bool hit = id && std::binary_search(r.ids.begin(), r.ids.end(), (uint16_t)id);
                if (hit && r.cond == 6 && g_destroying && g_destroy_taken++ >= 1) hit = false;
                A->set_reg(c, 2, hit);
                return 1;
            }
            if ((r.cond == 6 || r.cond == 7) && zone >= 5 && zone < 11) {   /* "Destroy 1 Spell / Trap Card": any card there is a target (cond 6: the first one only while resolving) */
                bool hit = (*(const uint16_t *)A->mem(DUEL_SIDES + side * DUEL_SIDE_SIZE + 0x30 + 20 * zone) & 0x1fff) != 0;
                if (hit && r.cond == 6 && g_destroying && g_destroy_taken++ >= 1) hit = false;
                A->set_reg(c, 2, hit);
                return 1;
            }
            if (zone >= 5 && zone < 11) {   /* Spell / Trap and field zones: the donor's own filter only knows monsters, so any card there is a target */
                A->set_reg(c, 2, (*(const uint16_t *)A->mem(DUEL_SIDES + side * DUEL_SIDE_SIZE + 0x30 + 20 * zone) & 0x1fff) != 0);
                return 1;
            }
            return 0;
        }
    }
    if (!is_card(A->reg(c, 4), TF3_SUIT_OF_SWORD_X)) return 0;
    if (g_coin < 0) { A->set_reg(c, 2, 1); return 1; }         /* the activation check: the card can always be played, the coin decides afterwards */
    const uint32_t own = A->mem(A->reg(c, 4))[2] & 1, side = A->reg(c, 5) & 1;
    if ((g_coin == 0) != (side == own)) { A->set_reg(c, 2, 0); return 1; }
    return 0;
}

static int hk_attack_trigger(TfCpu *c) {   /* 09ed5870(effect object): the "opponent declared an attack" test of Mirror Force; a destroy rule adds its own condition to it */
    const uint32_t obj = A->reg(c, 4);
    if (obj < 0x08000000u || obj > 0x0a000000u - 0x20) return 0;
    const auto d = g_destroy.find(*(const uint16_t *)A->mem(obj));
    if (d == g_destroy.end() || !d->second.cond) return 0;
    A->call_original(c, F_ATTACK_TRIGGER);
    if (A->reg(c, 2) & 1) {
        const bool ok = destroy_cond(c, obj, d->second, A->mem(obj)[2] & 1);
        static const bool dbg = getenv("TF3_DEBUG_FX") != nullptr;
        if (dbg) A->log("attack trigger of id %u: condition %s", (unsigned)d->first, ok ? "met" : "not met");
        A->set_reg(c, 2, ok ? 1 : 0);
    }
    return 1;
}

static int hk_effect_record(TfCpu *c) {   /* looks the record up as the card it plays like, then puts the key back */
    const uint32_t p = A->reg(c, 4);
    if (p < 0x08000000u || p > 0x0a000000u - 8) return 0;
    uint16_t *key = (uint16_t *)A->mem(p);
    const uint16_t real = trigger_card(*key, A->mem(p)[2] & 1, p);
    if (sabre_mill_enabled() && real == slot_of(7749) &&
        ((A->mem(p)[3] >> 4) & 3) == 2) {
        const uint16_t saved = *key;
        *key = 5091;
        A->call_original(c, F_EFFECT_RECORD);
        *key = saved;
        return 1;
    }
    if (((sword_return_enabled() && real == slot_of(7152)) ||
         (gladius_return_enabled() && real == slot_of(7309))) && ((A->mem(p)[3] >> 4) & 3) == 2) {
        const uint16_t saved = *key;
        *key = 5724;
        A->call_original(c, F_EFFECT_RECORD);
        *key = saved;
        const uint32_t native = A->reg(c, 2);
        if (native >= 0x08000000 && native <= 0x0a000000 - 24) {
            static uint32_t mandatory_record;
            if (!mandatory_record) mandatory_record = A->guest_alloc(24);
            if (mandatory_record) {
                memcpy(A->mem(mandatory_record), A->mem(native), 24);
                // Elma's Graveyard record has an optional-activation callback at +16.
                // Sword's return is mandatory and has no activation cost.
                *(uint32_t *)A->mem(mandatory_record + 16) = 0;
                A->set_reg(c, 2, mandatory_record);
            }
        }
        return 1;
    }
    if (real != *key)
        for (const auto &a : PORTED) {
            const auto t = g_trig.find(a.slot);
            const auto override_record = g_trigger_record_donors.find(real);
            const uint16_t record = override_record != g_trigger_record_donors.end() ? override_record->second : a.like;
            if (a.slot != real || !record || t == g_trig.end() || t->second == record) continue;
            const uint16_t donor = *key;
            *key = record;
            A->call_original(c, F_EFFECT_RECORD);
            *key = donor;
            return 1;
        }
    for (const auto &a : PORTED) {
        if (*key != a.slot || !a.like) continue;
        *key = a.like;
        A->call_original(c, F_EFFECT_RECORD);
        *key = a.slot;
        return 1;
    }
    return 0;
}

/* Data-driven effects (effects.bin in the mod folder, written by tools/mods/tf3_efeitos_param.py): a new card borrows the record of a game card that plays the
 * same way (`like`) and only says WHICH cards its effect may pick: the game keeps one small function per card that scans a zone and queues the cards it accepts
 * (table at 09f76818, dispatchers 09f243c8 / 09f24428 keyed by the effect's card id; 09f240b4(side, kind, index) queues one card of zone `kind`). For the cards
 * of this file the dispatchers run the scan below instead: every card of the zone whose id is in the card's id set is queued.
 */
static const uint32_t F_CAND_RUN = 0x09f243c8, F_CAND_COUNT = 0x09f24428, F_CAND_ADD = 0x09f240b4;
static const uint32_t CAND_COUNTER = 0x09f80000u - 28440u;
struct DataEffect { uint8_t zone, extra, to_deck, draw; std::vector<uint16_t> ids; };   /* extra: further cards of the same zone moved to the hand after the picked one */
static std::map<uint16_t, DataEffect> g_fx;
static const DataEffect *g_pick_extra;   /* the data effect whose card list was last offered to the player */

static void fx_load(void) {
    const std::wstring p = std::wstring(A->mod_dir) + L"\\effects.bin";
    FILE *f = _wfopen(p.c_str(), L"rb");
    if (!f) return;
    uint32_t h[2];
    if (fread(h, 4, 2, f) == 2 && h[0] == 0x45334654u /* "TF3E" */) {
        for (uint32_t i = 0; i < h[1]; i++) {
            uint16_t id; uint8_t zone, pad; uint32_t n;
            if (fread(&id, 2, 1, f) != 1 || fread(&zone, 1, 1, f) != 1 || fread(&pad, 1, 1, f) != 1 || fread(&n, 4, 1, f) != 1 || n > 0x10000) break;
            DataEffect e{(uint8_t)(zone & 15), pad, (uint8_t)((zone >> 4) & 1), (uint8_t)(zone >> 5), std::vector<uint16_t>(n)};
            if (n && fread(e.ids.data(), 2, n, f) != n) break;
            g_fx[id] = std::move(e);
        }
    }
    fclose(f);
    A->log("TF3 Cards: %u data effects loaded", (unsigned)g_fx.size());
}

/* zones: 1 = deck (ids at side+928, count at side+16, kind 13), 2 = graveyard (ids at side+1808, count at side+20, kind 14), 3 = graveyard cards that can be
 * Special Summoned (as 2, plus the game's summon check 09f55288 of Call of the Haunted), 4 = hand: the game calls the function once per hand slot (a2 = slot,
 * ids at side+288, kind 11, plus the summon check 09f43db8 of Magnet Circle LV2) */
static const uint32_t F_SUMMON_CHECK_GY = 0x09f55288, F_SUMMON_CHECK_HAND = 0x09f43db8;
static void fx_scan(TfCpu *c, uint32_t side_arg, uint32_t slot, const DataEffect &e) {
    const uint32_t base = DUEL_SIDES + (side_arg & 1) * DUEL_SIDE_SIZE;
    const auto wanted = [&](uint32_t off, uint32_t i) { return std::binary_search(e.ids.begin(), e.ids.end(), (uint16_t)(*(const uint16_t *)A->mem(base + off + i * 4) & 0x1fff)); };
    if (e.zone == 4) {
        if (slot < 40 && wanted(288, slot) && call_game(c, F_SUMMON_CHECK_HAND, side_arg, *(const uint16_t *)A->mem(base + 288 + slot * 4) & 0x1fff, 0, 0))
            call_game(c, F_CAND_ADD, side_arg, 11, slot, 0);
        return;
    }
    const uint32_t off = e.zone == 1 ? 928 : 1808, n = *(const uint32_t *)A->mem(base + (e.zone == 1 ? 16 : 20)), kind = e.zone == 1 ? 13 : 14;
    for (uint32_t i = 0; i < n && i < 160; i++) {
        if (!wanted(off, i)) continue;
        if (e.zone == 3 && !call_game(c, F_SUMMON_CHECK_GY, side_arg, side_arg, i, 0)) continue;
        call_game(c, F_CAND_ADD, side_arg, kind, i, 0);
    }
}

static uint16_t fx_card(uint16_t id, uint32_t side) {   /* the card whose data effect applies to an effect object with this id */
    if (g_fx.count(id)) return id;
    const uint16_t real = trigger_card(id, side);
    return g_fx.count(real) ? real : id;
}

static int hk_cand_run(TfCpu *c) {   /* (side, effect card id, step): the queue starts empty; returns how many cards were queued */
    const auto it = g_fx.find(fx_card((uint16_t)A->reg(c, 5), A->reg(c, 4) & 1));
    if (it == g_fx.end()) { g_pick_extra = nullptr; return 0; }
    uint32_t *ctr = (uint32_t *)A->mem(CAND_COUNTER);
    *ctr = 0;
    fx_scan(c, A->reg(c, 4), A->reg(c, 6), it->second);
    g_pick_extra = ((it->second.extra || it->second.to_deck || it->second.draw) && (it->second.zone == 1 || it->second.zone == 2)) ? &it->second : nullptr;
    A->set_reg(c, 2, *ctr);
    return 1;
}

static int hk_cand_count(TfCpu *c) {   /* same scan, but only counts: the queue is put back as it was */
    const auto it = g_fx.find(fx_card((uint16_t)A->reg(c, 5), A->reg(c, 4) & 1));
    if (it == g_fx.end()) return 0;
    uint32_t *ctr = (uint32_t *)A->mem(CAND_COUNTER);
    const uint32_t before = *ctr;
    fx_scan(c, A->reg(c, 4), A->reg(c, 6), it->second);
    A->set_reg(c, 2, *ctr - before);
    *ctr = before;
    return 1;
}

/* 09f3aa3c(effect, a1, a2, buffer): the step that moves the card the player picked (global reference u16 at 09f788c2: bit 0 side, bits 1-5 zone kind, bits 6+ index)
 * from the Graveyard / Deck to the hand. After it, an effect with `count` > 1 repeats the step for the next matching cards of the same zone (the first ones in
 * zone order) by writing their reference into the global. */
static const uint32_t F_MOVE_PICKED = 0x09f3aa3c, PICK_REF = 0x09f80000u - 30526u, MOVE_PHASE = 0x09f80000u - 28464u;   /* the step runs in phases 0, 1, 2; phase 2 does the move */
static int hk_move_to_hand(TfCpu *c) {
    const DataEffect *e = g_pick_extra;
    if (getenv("TF3_DEBUG_FX")) A->log("move step: a1 %x a2 %x phase %u refs %04x %04x %04x pending %d", A->reg(c, 5), A->reg(c, 6), *(const uint32_t *)A->mem(MOVE_PHASE), *(const uint16_t *)A->mem(PICK_REF), *(const uint16_t *)A->mem(PICK_REF + 2), *(const uint16_t *)A->mem(PICK_REF + 4), e != nullptr);
    if (!e || *(const uint32_t *)A->mem(MOVE_PHASE) != 1) return 0;
    const uint16_t ref = *(const uint16_t *)A->mem(PICK_REF);
    const uint32_t kind = (ref >> 1) & 31, side = ref & 1;
    if (kind != (e->zone == 1 ? 13u : 14u)) return 0;
    g_pick_extra = nullptr;
    const uint32_t a[4] = {A->reg(c, 4), A->reg(c, 5), A->reg(c, 6), A->reg(c, 7)};
    const uint32_t deck_ref_base = DUEL_SIDES + side * DUEL_SIDE_SIZE;
    if (e->to_deck) *(uint16_t *)A->mem(PICK_REF + 2) = (uint16_t)((*(const uint32_t *)A->mem(deck_ref_base + 16) << 6) | (13 << 1) | side);
    A->call_original(c, F_MOVE_PICKED);
    const uint32_t result = A->reg(c, 2);
    const uint32_t phase = *(const uint32_t *)A->mem(MOVE_PHASE);
    const uint16_t dest = *(const uint16_t *)A->mem(PICK_REF + 2);
    const uint32_t base = DUEL_SIDES + side * DUEL_SIDE_SIZE, off = e->zone == 1 ? 928 : 1808;
    for (uint32_t k = 0; k < e->extra; k++) {
        const uint32_t n = *(const uint32_t *)A->mem(base + (e->zone == 1 ? 16 : 20));
        uint32_t i = 0;
        while (i < n && i < 160 && !std::binary_search(e->ids.begin(), e->ids.end(), (uint16_t)(*(const uint16_t *)A->mem(base + off + i * 4) & 0x1fff))) i++;
        if (i >= n || i >= 160) break;
        *(uint16_t *)A->mem(PICK_REF) = (uint16_t)((i << 6) | (kind << 1) | side);   /* source; the next halfword is the destination: the next free hand slot */
        *(uint16_t *)A->mem(PICK_REF + 2) = e->to_deck ? (uint16_t)((*(const uint32_t *)A->mem(base + 16) << 6) | (13 << 1) | side)
                                                       : (uint16_t)((*(const uint32_t *)A->mem(base + 12) << 6) | (11 << 1) | side);
        *(uint32_t *)A->mem(MOVE_PHASE) = 1;
        const uint32_t r = call_game(c, F_MOVE_PICKED, a[0], a[1], a[2], a[3]);
        if (getenv("TF3_DEBUG_FX")) A->log("extra pick %u: zone index %u -> result %u, hand %u", k, i, r, *(const uint32_t *)A->mem(base + 12));
    }
    *(uint16_t *)A->mem(PICK_REF) = ref; *(uint16_t *)A->mem(PICK_REF + 2) = dest;
    if (e->draw) call_game(c, F_DRAW_CARDS, side, e->draw, 0, 0);
    *(uint32_t *)A->mem(MOVE_PHASE) = phase;
    A->set_reg(c, 2, result);
    return 1;
}


/* Passive ATK/DEF rules (stat.bin, written by tools/mods/tf3_efeitos_param.py from tools/mods/tf3_stats.json) for face-up monsters of the new cards.
 * The game computes a monster's stats in 09f56da8(side, slot, out, duel): out+20 = ATK, out+24 = DEF (a chain of `id == N` rules per card). After the
 * original runs, every rule whose source monster is face-up on the field adds its bonus to the monster being asked about.
 * Monster zones: id (u16 & 0x1fff) at side+0x30+20*slot, flags (u32) at +0x40+20*slot (bit 1 / bit 5 = not face-up). */
static const uint32_t F_STATS = 0x09f56da8;
static const uint32_t F_LEVEL = 0x09f5a284;
struct StatRule { uint16_t src; uint8_t scope, cond_side, per, excl; int32_t atk, def; std::vector<uint16_t> target, cond; };
static std::vector<StatRule> g_stat;
static std::map<uint16_t, int16_t> g_equip_level;
struct AttackProtection { uint8_t exclude, type, attribute; };
static std::map<uint16_t, AttackProtection> g_attack_protect;
static std::set<uint16_t> g_control_lock;

static void stat_load(void) {
    const std::wstring p = std::wstring(A->mod_dir) + L"\\stat.bin";
    FILE *f = _wfopen(p.c_str(), L"rb");
    if (!f) return;
    uint32_t h[2];
    if (fread(h, 4, 2, f) == 2 && h[0] == 0x53334654u /* "TF3S" */) {
        for (uint32_t i = 0; i < h[1]; i++) {
            StatRule r; uint8_t b[4]; uint32_t nt, nc; int32_t ad[2];
            if (fread(&r.src, 2, 1, f) != 1 || fread(b, 1, 4, f) != 4 || fread(ad, 4, 2, f) != 2 || fread(&nt, 4, 1, f) != 1 || fread(&nc, 4, 1, f) != 1 || nt > 0x10000 || nc > 0x10000) break;
            r.scope = b[0]; r.cond_side = b[1]; r.per = b[2]; r.excl = b[3]; r.atk = ad[0]; r.def = ad[1];
            r.target.resize(nt); r.cond.resize(nc);
            if ((nt && fread(r.target.data(), 2, nt, f) != nt) || (nc && fread(r.cond.data(), 2, nc, f) != nc)) break;
            g_stat.push_back(std::move(r));
        }
        uint32_t tail[2];
        while (fread(tail, 4, 2, f) == 2 && tail[1] <= 0x10000) {
            if (tail[0] == 0x314c564c) {
                for (uint32_t i = 0; i < tail[1]; i++) {
                    uint16_t id; int16_t delta;
                    if (fread(&id, 2, 1, f) != 1 || fread(&delta, 2, 1, f) != 1) break;
                    if (delta > 0) g_equip_level[id] = delta;
                }
            } else if (tail[0] == 0x31505441) {
                for (uint32_t i = 0; i < tail[1]; i++) {
                    uint16_t id; uint8_t b[4];
                    if (fread(&id, 2, 1, f) != 1 || fread(b, 1, 4, f) != 4) break;
                    if (b[0] == 1 || b[0] == 2) g_attack_protect[id] = AttackProtection{b[0], b[1], b[2]};
                }
            } else if (tail[0] == 0x314c5443) {
                for (uint32_t i = 0; i < tail[1]; i++) {
                    uint16_t id;
                    if (fread(&id, 2, 1, f) != 1) break;
                    g_control_lock.insert(id);
                }
            } else break;
        }
    }
    fclose(f);
    A->log("TF3 Cards: %u passive stat rules loaded", (unsigned)g_stat.size());
}

static uint32_t zone_card(uint32_t side, uint32_t slot) {   /* face-up monster id of a zone, 0 when empty or not face-up */
    const uint32_t base = DUEL_SIDES + side * DUEL_SIDE_SIZE + 20 * slot;
    const uint32_t id = *(const uint16_t *)A->mem(base + 0x30) & 0x1fff, flags = *(const uint32_t *)A->mem(base + 0x40);
    return (flags & 0x22) ? 0 : id;
}

static bool zone_effect_active(uint32_t side, uint32_t slot) {
    const uint32_t entry = DUEL_SIDES + side * DUEL_SIDE_SIZE + 0x30 + 20 * slot;
    const uint32_t flags = *(const uint32_t *)A->mem(entry + 16);
    const uint32_t enabled = *(const uint16_t *)A->mem(entry + 8);
    // Match the engine's continuous-effect gate, including effect negation.
    return (~((flags & 3) >> 1) & enabled & ~((flags & 0x3f) >> 5)) != 0;
}

// Experimental: identify a protected native Spell/Trap event.
static bool venus_protects_event(TfCpu *c, uint32_t target) {
    if (target < 0x08000000 || target > 0x0a000000 - 24) return false;
    const uint8_t *event = A->mem(target);
    const uint16_t card = *(const uint16_t *)event & 0x1fff;
    const uint32_t type = call_game(c, 0x09f678f0, card, 0, 0, 0);
    if (type != 22 && type != 23) return false;
    const uint32_t side = event[2] & 1;
    for (uint32_t slot = 0; slot < 5; ++slot) {
        if (zone_card(side, slot) == slot_of(7347) && slot_of(7347) && zone_effect_active(side, slot)) {
            return true;
        }
    }
    return false;
}

static int hk_venus_negation(TfCpu *c) {
    if (!venus_protects_event(c, A->reg(c, 5))) return 0;
    A->set_reg(c, 2, 0);
    return 1;
}

static unsigned g_venus_effect_scope;
static uint32_t g_venus_effect_side, g_venus_effect_card;
static int hk_venus_continuous_negation(TfCpu *c) {
    if (!g_venus_effect_scope || A->reg(c, 4) != g_venus_effect_side ||
        (A->reg(c, 5) & 0x1fff) != g_venus_effect_card) return 0;
    A->set_reg(c, 2, 0);
    return 1;
}

static int hk_venus_effect_negation(TfCpu *c) {
    if (!venus_protects_event(c, A->reg(c, 4))) return 0;
    const uint32_t event = A->reg(c, 4), saved_side = g_venus_effect_side, saved_card = g_venus_effect_card;
    g_venus_effect_side = A->mem(event)[2] & 1;
    g_venus_effect_card = *(uint16_t *)A->mem(event) & 0x1fff;
    ++g_venus_effect_scope;
    A->call_original(c, 0x09ed26d0);
    --g_venus_effect_scope;
    g_venus_effect_side = saved_side; g_venus_effect_card = saved_card;
    return 1;
}

static unsigned g_venus_chain_scope;
static int hk_venus_chain(TfCpu *c) {
    const uint32_t count = *(uint32_t *)A->mem(0x09f78660);
    if (!count || count > 16 || *(uint32_t *)A->mem(0x09f78784) != 4) return 0;
    const uint32_t event = 0x09f784e0 + (count - 1) * 24;
    if (!venus_protects_event(c, event)) return 0;
    ++g_venus_chain_scope;
    A->call_original(c, 0x09eb7248);
    --g_venus_chain_scope;
    return 1;
}

static int hk_venus_frontier(TfCpu *c) {
    if (!g_venus_chain_scope || A->reg(c, 5) != 5322) return 0;
    A->set_reg(c, 2, 0);
    return 1;
}

static int hk_venus_phoenix(TfCpu *c) {
    if (!g_venus_chain_scope || A->reg(c, 5) != 6663) return 0;
    A->set_reg(c, 2, 0);
    return 1;
}

static std::vector<TriggerSource> g_milled_sabres;
static int hk_sabre_mill(TfCpu *c) {
    const uint32_t side = A->reg(c, 4), requested = A->reg(c, 5);
    const uint16_t sabre = slot_of(7749);
    if (side > 1 || !sabre) return 0;
    const uint32_t base = DUEL_SIDES + side * DUEL_SIDE_SIZE;
    const uint32_t count = std::min(requested, *(uint32_t *)A->mem(base + 16));
    const size_t scope = g_milled_sabres.size();
    for (uint32_t i = 0; i < count && i < 80; ++i) {
        const uint32_t entry = base + 0x3a0 + i * 4;
        const uint16_t packed = *(uint16_t *)A->mem(entry);
        if ((packed & 0x1fff) != sabre) continue;
        const uint16_t token = (uint16_t)(((*(uint16_t *)A->mem(entry + 2) & 0x3fff) >> 6) * 2 + ((packed >> 13) & 1));
        g_milled_sabres.push_back({5091, sabre, (uint8_t)side, token, true});
    }
    A->call_original(c, 0x09f2a7ec);
    g_milled_sabres.resize(scope);
    return 1;
}

static int hk_sabre_mill_event(TfCpu *c) {
    if (A->reg(c, 4) != 0x1c || g_milled_sabres.empty()) return 0;
    const uint32_t packet = A->reg(c, 5);
    if (((packet >> 12) & 15) != 14) return 0;
    for (const auto &source : g_milled_sabres) {
        if (source.token != (packet & 0x1ff) || source.side != ((packet >> 9) & 1)) continue;
        const size_t scope = g_trigger_scope.size();
        g_trigger_scope.push_back(source);
        // Reuse the native milled-card event packet without changing the card's type or identity.
        call_game(c, 0x09eb5640, ((uint32_t)source.side << 31) | 0x384e0000 | source.card, source.token, packet, 0);
        g_trigger_scope.resize(scope);
        break;
    }
    return 0;
}

static int hk_equip_target(TfCpu *c) {
    const uint32_t object = A->reg(c, 4), side = A->reg(c, 5), slot = A->reg(c, 6);
    if (object < 0x08000000 || object > 0x0a000000 - 24 || side > 1 || slot > 4) return 0;
    const uint16_t raw = *(const uint16_t *)A->mem(object);
    const uint16_t source = trigger_card(raw, A->mem(object)[2] & 1, object);
    if (source == slot_of(7749) && ((A->mem(object)[3] >> 4) & 3) == 2 &&
        side != (A->mem(object)[2] & 1)) {
        A->set_reg(c, 2, 0);
        return 1;
    }
    for (const auto &r : g_stat) {
        if (r.src != source || (r.scope & 7) != 4 || (!(r.scope & 32) && r.target.empty())) continue;
        const uint16_t target = (uint16_t)zone_card(side, slot);
        if (!std::binary_search(r.target.begin(), r.target.end(), target)) {
            A->set_reg(c, 2, 0);
            return 1;
        }
    }
    return 0;
}

static bool equipped_to(uint32_t source_side, uint32_t source_slot, uint32_t side, uint32_t slot) {
    uint32_t node = *(const uint16_t *)A->mem(DUEL_SIDES + side * DUEL_SIDE_SIZE + 0x30 + 20 * slot + 10);
    // Native equipment relationships are effect-list nodes, not the summon-turn field at +4.
    for (unsigned count = 0; node && count < 65536; ++count) {
        const uint32_t entry = 0x09f7c458 + node * 8;
        const uint16_t ref = *(const uint16_t *)A->mem(entry);
        const uint8_t type = *A->mem(entry + 2) & 15;
        if ((type == 10 || type == 11) && (ref & 255) == source_side && (ref >> 8) == source_slot) return true;
        node = *(const uint16_t *)A->mem(entry + 6);
    }
    return false;
}

// Reuse native destruction replacement, including its simultaneous-removal prepass.
static unsigned g_replacement_depth;
struct TributeEquip { uint16_t card, token; };
static std::vector<TributeEquip> g_tribute_equips;
static std::vector<TributeEquip> g_deck_equips;
static int hk_equipment_cleanup(TfCpu *c) {
    const uint32_t side = A->reg(c, 5), slot = A->reg(c, 6), flags = A->reg(c, 7);
    if (getenv("TF3_DEBUG_SWORD")) A->log("Sword cleanup: side %u slot %u flags %08x key %08x id %u head %u", side, slot, flags, A->reg(c, 4), *(uint16_t *)A->mem(A->reg(c, 4)) & 0x1fff, *(uint16_t *)A->mem(A->reg(c, 4) + 10));
    const size_t size = g_tribute_equips.size();
    if (side < 2 && slot < 5 && (flags & 0x1000)) {
        for (uint32_t ss = 0; ss < 2; ++ss) for (uint32_t sk = 5; sk < 10; ++sk) {
            const uint32_t entry = DUEL_SIDES + ss * DUEL_SIDE_SIZE + 0x30 + sk * 20;
            const uint16_t packed = *(uint16_t *)A->mem(entry);
            if ((packed & 0x1fff) != slot_of(7152) || !equipped_to(ss, sk, side, slot)) continue;
            const uint16_t token = (uint16_t)(((*(uint16_t *)A->mem(entry + 2) & 0x3fff) >> 6) * 2 + ((packed >> 13) & 1));
            g_tribute_equips.push_back({(uint16_t)(packed & 0x1fff), token});
        }
    }
    A->call_original(c, 0x09f34044);
    g_tribute_equips.resize(size);
    return 1;
}
static uint32_t ebon_replacement(TfCpu *c, uint32_t side, uint32_t slot) {
    if (side > 1 || slot >= 5) return 0xffff;
    const uint16_t target = (uint16_t)zone_card(side, slot);
    const uint16_t steed = slot_of(7000);
    if (!target || !steed) return 0xffff;
    const uint32_t immunity = call_game(c, 0x09f53430, side, slot, 0, 0);
    for (const auto &rule : g_stat) {
        if (rule.src != steed || (rule.scope & 15) != 4 ||
            !std::binary_search(rule.target.begin(), rule.target.end(), target)) continue;
        for (uint32_t ss = 0; ss < 2; ++ss) for (uint32_t sk = 5; sk < 10; ++sk) {
            if (*(const uint32_t *)A->mem(0x09f7bc48) & (1u << (sk + ss * 16))) continue;
            if (zone_card(ss, sk) == steed && zone_effect_active(ss, sk) &&
                !(immunity & (ss == side ? 1 : 2)) && equipped_to(ss, sk, side, slot)) return ss | (sk << 8);
        }
    }
    return 0xffff;
}

static int hk_replacement_present(TfCpu *c) {
    const uint32_t side = A->reg(c, 4), slot = A->reg(c, 5), id = A->reg(c, 6);
    if (g_venus_chain_scope && id == 5728) { A->set_reg(c, 2, 0); return 1; }
    A->call_original(c, 0x09f454b8);
    // Include the mass-destruction prepass in the native replacement mechanism.
    if (id == 6181 && !A->reg(c, 2) && ebon_replacement(c, side, slot) != 0xffff) A->set_reg(c, 2, 1);
    return 1;
}

static int hk_replacement_equip(TfCpu *c) {
    if (!g_replacement_depth || A->reg(c, 6) != 1) return 0;
    const uint32_t source = ebon_replacement(c, A->reg(c, 4), A->reg(c, 5));
    if (source == 0xffff) return 0;
    // Select the protecting equip explicitly, never an unrelated first equip.
    A->set_reg(c, 2, source);
    return 1;
}

static int replacement_wrap(TfCpu *c, uint32_t address) {
    const size_t size = g_deck_equips.size();
    const uint32_t side = A->reg(c, 4), slot = A->reg(c, 5);
    if (address == 0x09f34d64 && gladius_return_enabled() && side < 2 && slot < 5 && A->reg(c, 6) == 13) {
        for (uint32_t sk = 5; sk < 10; ++sk) {
            const uint32_t entry = DUEL_SIDES + side * DUEL_SIDE_SIZE + 0x30 + sk * 20;
            const uint16_t packed = *(uint16_t *)A->mem(entry);
            if ((packed & 0x1fff) != slot_of(7309) || !equipped_to(side, sk, side, slot)) continue;
            const uint16_t token = (uint16_t)(((*(uint16_t *)A->mem(entry + 2) & 0x3fff) >> 6) * 2 + ((packed >> 13) & 1));
            g_deck_equips.push_back({(uint16_t)(packed & 0x1fff), token});
        }
    }
    ++g_replacement_depth;
    A->call_original(c, address);
    --g_replacement_depth;
    g_deck_equips.resize(size);
    return 1;
}
static int hk_destroy_mark(TfCpu *c) { return replacement_wrap(c, 0x09f3332c); }
static int hk_destroy_remove(TfCpu *c) { return replacement_wrap(c, 0x09f34d64); }
static int hk_battle_destroy_replace(TfCpu *c) {
    // The native battle path stages doomed monsters outside their field zones.
    // Replace before that move, so the existing monster instance stays in place.
    const uint32_t stage = *(uint32_t *)A->mem(0x09f790d0);
    const uint32_t side = (*(uint16_t *)A->mem(0x09f788c0) >> 15) & 1;
    const uint32_t slot = *(uint16_t *)A->mem(0x09f788c2);
    const uint32_t participant = *(uint16_t *)A->mem(0x09f788c4);
    if (participant > 1) return 0;
    const uint32_t source = ebon_replacement(c, side, slot);
    if (getenv("TF3_DEBUG_REPLACE")) A->log("replacement staging: stage %u side %u slot %u participant %u source %04x", stage, side, slot, participant, source);
    if (stage != 1) return 0;
    if (source == 0xffff) return 0;
    if (!call_game6(c, 0x09f34d64, source & 255, source >> 8, 14, 2, 0, 0)) return 0;
    const uint32_t battle = 0x09f77dcc + 0x38 * participant;
    *(uint32_t *)A->mem(battle + 44) = 0;
    *(uint32_t *)A->mem(battle + 48) = 1;
    *(uint32_t *)A->mem(0x09f790cc) = 0;
    return 1;
}

/* How many cards match the condition of a stat rule (`cond`: face-up monsters, or Graveyard cards when cond_side bit 2 is set, of the side relative to the source
 * card's controller `ss`); `excl_slot` is the source's own monster zone that does not count when the rule says so. */
static int32_t stat_cond_count(const StatRule &r, uint32_t ss, int excl_slot) {
    int32_t n = 0;
    for (uint32_t cs = 0; cs < 2; cs++) {
        const bool own = cs == ss;
        const uint8_t cside = r.cond_side & 3;
        if (cside == 1 ? !own : cside == 2 ? own : false) continue;
        if (r.cond_side & 4) {   /* count the Graveyard cards of the id set instead of the monsters of the field */
            const uint32_t base = DUEL_SIDES + cs * DUEL_SIDE_SIZE, gn = *(const uint32_t *)A->mem(base + 20);
            for (uint32_t gk = 0; gk < gn && gk < 160; gk++)
                if (std::binary_search(r.cond.begin(), r.cond.end(), (uint16_t)(*(const uint16_t *)A->mem(base + 1808 + gk * 4) & 0x1fff))) n++;
            continue;
        }
        for (uint32_t ck = 0; ck < 5; ck++) {
            if (r.excl && cs == ss && (int)ck == excl_slot) continue;
            const uint32_t id = zone_card(cs, ck);
            if (id && std::binary_search(r.cond.begin(), r.cond.end(), (uint16_t)id)) n++;
        }
    }
    return n;
}

static int hk_stats(TfCpu *c) {
    const uint32_t side = A->reg(c, 4) & 1, slot = A->reg(c, 5), out = A->reg(c, 6);
    static const bool dbg = getenv("TF3_DEBUG_STATS") != nullptr;
    if (dbg) { static int n; if (n++ < 10) A->log("stat entry: side %u slot %u output %08x", side, slot, out); }
    A->call_original(c, F_STATS);
    if (slot >= 5 || out < 0x08000000u || out > 0x0a000000u - 64) return 1;
    const uint32_t me = zone_card(side, slot);
    if (!me) return 1;
    int32_t *atk = (int32_t *)A->mem(out + 20), *def = (int32_t *)A->mem(out + 24);
    int spell_immunity = -1;
    const auto spell_affects = [&](uint32_t source_side) {
        if (spell_immunity < 0) spell_immunity = (int)call_game(c, 0x09f53430, side, slot, 0, 0);
        return (spell_immunity & (source_side == side ? 1 : 2)) == 0;
    };
    if (dbg) {
        static std::set<uint32_t> seen_cards;
        if (seen_cards.insert(me).second) A->log("stat query: target %u side %u slot %u base ATK %d DEF %d", me, side, slot, *atk, *def);
    }
    for (const auto &r : g_stat) {
        if ((r.scope & 15) != 4) continue;
        if (((r.scope & 32) || !r.target.empty()) && !std::binary_search(r.target.begin(), r.target.end(), (uint16_t)me)) continue;
        for (uint32_t ss = 0; ss < 2; ++ss) for (uint32_t k = 0; k < 5; k++) {
            const uint32_t e = DUEL_SIDES + ss * DUEL_SIDE_SIZE + 0x30 + 100 + 20 * k;
            const uint32_t id = *(const uint16_t *)A->mem(e) & 0x1fff, flags = *(const uint32_t *)A->mem(e + 0x10);
            if (id == r.src && !(flags & 0x22) && zone_effect_active(ss, k + 5) && equipped_to(ss, k + 5, side, slot) && spell_affects(ss)) {
                int32_t mult = 1;
                if (r.cond_side) {   /* "gains N ATK for each ...": the count is taken relative to the equip card's controller */
                    const int32_t n = stat_cond_count(r, ss, -1);
                    if (!n) continue;
                    mult = (r.per & 1) ? n : 1;
                }
                *atk += r.atk * mult; *def += r.def * mult;
            }
        }
    }
    for (const auto &r : g_stat)
        for (uint32_t ss = 0; ss < 2; ss++)
            for (uint32_t sk = 0; sk < ((r.scope & 16) ? 6u : 5u); sk++) {
                const uint8_t sc = r.scope & 15;   /* bit 4: the source is a face-up Spell / Trap card of the field (continuous Spell) instead of a monster */
                if (sc == 4) continue;
                if (r.scope & 16) {
                    const uint32_t e = DUEL_SIDES + ss * DUEL_SIDE_SIZE + 0x30 + 100 + 20 * sk;
                    if ((*(const uint16_t *)A->mem(e) & 0x1fff) != r.src || (*(const uint32_t *)A->mem(e + 0x10) & 0x22)) continue;
                } else if (zone_card(ss, sk) != r.src) continue;
                if (!zone_effect_active(ss, sk + ((r.scope & 16) ? 5 : 0))) continue;
                if ((r.scope & 16) && !spell_affects(ss)) continue;
                const bool mine = ss == side;
                if (sc == 0 ? (ss != side || sk != slot) : sc == 1 ? !mine : sc == 2 ? mine : false) continue;
                if (sc != 0 && ((r.scope & 32) || !r.target.empty()) && !std::binary_search(r.target.begin(), r.target.end(), (uint16_t)me)) continue;
                int32_t mult = 1;
                if (r.cond_side) {
                    const int32_t n = stat_cond_count(r, ss, (int)sk);
                    if (!n) continue;
                    mult = (r.per & 1) ? n : 1;
                }
                if (r.per & 2) mult *= *(const int32_t *)A->mem(out + 16);
                *atk += r.atk * mult; *def += r.def * mult;
                if (dbg) {
                    static std::set<uint64_t> seen;
                    const uint64_t key = ((uint64_t)r.src << 32) | ((uint64_t)me << 16) | (side << 8) | slot;
                    if (seen.insert(key).second) A->log("stat rule: source %u target %u side %u slot %u -> ATK %d DEF %d", r.src, me, side, slot, *atk, *def);
                }
            }
    if (*atk < 0) *atk = 0;
    if (*def < 0) *def = 0;
    return 1;
}

static int hk_level(TfCpu *c) {
    const uint32_t side = A->reg(c, 4) & 1, slot = A->reg(c, 5);
    A->call_original(c, F_LEVEL);
    const uint32_t me = slot < 5 ? zone_card(side, slot) : 0;
    if (!me) return 1;
    int32_t delta = 0;
    const uint32_t immunity = call_game(c, 0x09f53430, side, slot, 0, 0);
    for (uint32_t ss = 0; ss < 2; ++ss) for (uint32_t k = 0; k < 5; k++) {
        const uint32_t e = DUEL_SIDES + ss * DUEL_SIDE_SIZE + 0x30 + 20 * (k + 5);
        const uint16_t id = *(const uint16_t *)A->mem(e) & 0x1fff;
        const auto level = g_equip_level.find(id);
        if (level == g_equip_level.end() || !zone_effect_active(ss, k + 5) || !equipped_to(ss, k + 5, side, slot) || (immunity & (ss == side ? 1 : 2))) continue;
        for (const auto &r : g_stat) {
            if (r.src != id || (r.scope & 15) != 4) continue;
            if (((r.scope & 32) || !r.target.empty()) && !std::binary_search(r.target.begin(), r.target.end(), (uint16_t)me)) continue;
            delta += level->second;
            break;
        }
    }
    if (delta) A->set_reg(c, 2, A->reg(c, 2) + delta);
    return 1;
}

static int hk_attack_target(TfCpu *c) {
    const uint32_t side = A->reg(c, 6), slot = A->reg(c, 7);
    // Like Command Knight, enforce protection when choosing a target, not when resolving an established attack.
    if (!A->reg(c, 8) || side > 1 || slot >= 5) return 0;
    const uint16_t me = (uint16_t)zone_card(side, slot);
    const auto rule = g_attack_protect.find(me);
    if (rule == g_attack_protect.end() || !zone_effect_active(side, slot)) return 0;
    A->call_original(c, 0x09f5153c);
    if (!A->reg(c, 2)) return 1;
    for (uint32_t k = 0; k < 5; k++) {
        const uint16_t id = (uint16_t)zone_card(side, k);
        if (!id || k == slot || (rule->second.exclude == 2 && id == me)) continue;
        if (rule->second.type && call_game(c, 0x09f5a690, side, k, 0, 0) != rule->second.type) continue;
        if (rule->second.attribute && call_game(c, 0x09f5ab44, side, k, 0, 0) != rule->second.attribute) continue;
        A->set_reg(c, 2, 0);
        break;
    }
    return 1;
}

static int hk_control(TfCpu *c) {
    const uint32_t side = A->reg(c, 4), slot = A->reg(c, 5);
    if (side > 1 || slot >= 5 || !g_control_lock.count((uint16_t)zone_card(side, slot)) || !zone_effect_active(side, slot)) return 0;
    A->call_original(c, 0x09f53358);
    A->set_reg(c, 2, 0);
    return 1;
}


/* research aid (TF3_DEBUG_CAND=1): logs every candidate scan the game asks for (side, id, step) and how many cards it queued */
static int cand_debug(TfCpu *c, uint32_t fn, const char *what) {
    const uint32_t ra = A->reg(c, 31), side = A->reg(c, 4), id = A->reg(c, 5), step = A->reg(c, 6);
    A->call_original(c, fn);
    static int n;
    if (n++ < 600) A->log("cand %s: side %u id %x step %u -> %d (ra %08x)", what, side, id, step, (int)A->reg(c, 2), ra);
    return 1;
}
static int hk_cand_run_debug(TfCpu *c) { return cand_debug(c, F_CAND_RUN, "run"); }
static int hk_cand_count_debug(TfCpu *c) { return cand_debug(c, F_CAND_COUNT, "count"); }

/* research aid (TF3_DEBUG_FN=addr,addr,...): logs the arguments and the result of the listed libduel functions (up to 8) while the duel runs; a call is shown
 * only when one of its arguments (or the u16 at an argument that points into RAM) is the id of a new card */
template <int N> static int hk_fn_log(TfCpu *c);
static uint32_t g_fn_addr[8];
static uint32_t g_fn_id;   /* TF3_DEBUG_FN_ID: show the calls that carry this id instead of the ids of the new cards */
static bool new_id(uint32_t v) {
    const auto hit = [](uint32_t u) { return g_fn_id ? u == g_fn_id : u >= 6958 && u < 8192; };
    if (hit(v & 0xffff)) return true;
    if (v >= 0x08000000u && v < 0x0a000000u - 8) return hit(*(const uint16_t *)A->mem(v) & 0x1fff);
    return false;
}
template <int N> static int hk_fn_log(TfCpu *c) {
    const uint32_t a[4] = {A->reg(c, 4), A->reg(c, 5), A->reg(c, 6), A->reg(c, 7)}, ra = A->reg(c, 31);
    static const bool all = getenv("TF3_DEBUG_FN_ALL") != nullptr;
    const bool show = all || new_id(a[0]) || new_id(a[1]) || new_id(a[2]) || new_id(a[3]);
    const uint32_t k0 = a[0] >= 0x08000000u && a[0] < 0x0a000000u - 8 ? *(const uint16_t *)A->mem(a[0]) & 0x1fff : 0;   /* id the first argument points to */
    A->call_original(c, g_fn_addr[N]);
    static int n;
    if (show && n++ < 400) A->log("fn %08x(%x[id %u], %x, %x, %x) -> %x (ra %08x)", g_fn_addr[N], a[0], k0, a[1], a[2], a[3], A->reg(c, 2), ra);
    return 1;
}
static void fn_log_init(const char *list) {
    if (const char *id = getenv("TF3_DEBUG_FN_ID")) g_fn_id = (uint32_t)atoi(id);
    static int (*const fns[8])(TfCpu *) = {hk_fn_log<0>, hk_fn_log<1>, hk_fn_log<2>, hk_fn_log<3>, hk_fn_log<4>, hk_fn_log<5>, hk_fn_log<6>, hk_fn_log<7>};
    int k = 0;
    for (const char *p = list; *p && k < 8;) {
        char *e; const uint32_t v = (uint32_t)strtoul(p, &e, 16);
        if (e == p) break;
        g_fn_addr[k] = v; A->hook(v, fns[k]); A->log("debug hook %08x", v); k++;
        p = *e == ',' ? e + 1 : e;
    }
}

/* research aid (TF3_DEBUG_TRIG=1): logs every trigger check (libduel 09f30ddc(event, key, a2, flags): does the effect of the card of `key` trigger on this event?) */
static const uint32_t F_TRIGGER = 0x09f30ddc;
static int hk_trigger_debug(TfCpu *c) {
    const uint32_t ev = A->reg(c, 4), key = A->reg(c, 5), a2 = A->reg(c, 6), fl = A->reg(c, 7);
    if (key < 0x08000000u || key > 0x0a000000u - 8) return 0;
    const uint16_t id = *(const uint16_t *)A->mem(key) & 0x1fff;
    A->call_original(c, F_TRIGGER);
    static int n;
    if (n < 4000) { n++; A->log("trigger: card %u event %08x a2 %u flags %08x key+2 %02x%02x -> %u", id, ev, a2, fl, A->mem(key)[2], A->mem(key)[3], A->reg(c, 2)); }
    return 1;
}


/* Triggers (trigger.bin, "TF3T", u32 n, {u16 card id, u16 donor id}): the game decides WHEN a card's effect starts in one predicate, 09f30ddc(event, key, a2, flags),
 * a chain of `id == N` branches. A new card whose trigger is the same as a game card's ("when this card is sent to the Graveyard", "FLIP", ...) is asked as that
 * donor: the key's id is swapped while the predicate runs. The effect itself comes from the borrowed record and the data effect of the card. */
static std::map<uint16_t, bool> g_trig_battle;   /* cards whose alias holds while the battle resolution runs (09ea28d8: "destroys a monster by battle", ...) */
static std::map<uint16_t, bool> g_trig_summon;   /* cards whose alias also holds while the game scans the events of a finished action (Summon triggers) */
static std::set<uint16_t> g_after_attack_defense;

static void trig_load(void) {
    const std::wstring p = std::wstring(A->mod_dir) + L"\\trigger.bin";
    FILE *f = _wfopen(p.c_str(), L"rb");
    if (!f) return;
    uint32_t h[2];
    if (fread(h, 4, 2, f) == 2 && h[0] == 0x54334654u /* "TF3T" */)
        for (uint32_t i = 0; i < h[1]; i++) {
            uint16_t e[2];
            if (fread(e, 2, 2, f) != 2) break;
            g_trig[e[0]] = e[1] & 0x1fff;
            if (e[1] & 0x8000) g_trig_summon[e[0]] = true;
            if (e[1] & 0x4000) g_trig_battle[e[0]] = true;
            if (e[1] & 0x2000) g_after_attack_defense.insert(e[0]);
        }
    uint32_t trailer[2];
    while (fread(trailer, 4, 2, f) == 2 && trailer[1] <= 0x10000) {
        if (trailer[0] != 0x31434754 && trailer[0] != 0x31524754) break;
        for (uint32_t i = 0; i < trailer[1]; ++i) {
            uint16_t entry[2];
            if (fread(entry, sizeof entry, 1, f) != 1) break;
            if (trailer[0] == 0x31434754) g_trigger_conditions[entry[0]] = entry[1];
            else g_trigger_record_donors[entry[0]] = entry[1];
        }
    }
    fclose(f);
    A->log("TF3 Cards: %u trigger aliases loaded", (unsigned)g_trig.size());
}

static int hk_trigger(TfCpu *c) {
    const uint32_t key = A->reg(c, 5);
    if (key < 0x08000000u || key > 0x0a000000u - 8) return 0;
    uint16_t *idp = (uint16_t *)A->mem(key);
    const auto it = g_trig.find(*idp & 0x1fff);
    if (it == g_trig.end()) return 0;
    const uint16_t saved = *idp;
    const size_t scope = g_trigger_scope.size();
    bool adapt_tribute = false;
    const uint32_t original_flags = A->reg(c, 7);
    if (real_event_ids()) {
        const uint16_t token = (uint16_t)(((*(const uint16_t *)A->mem(key + 2) & 0x3fff) >> 6) * 2 + ((saved >> 13) & 1));
        const uint8_t owner = (uint8_t)((saved >> 13) & 1), controller = (uint8_t)((A->reg(c, 4) >> 9) & 1);
        const uint16_t condition = g_trigger_conditions[(uint16_t)(saved & 0x1fff)];
        const uint32_t event = A->reg(c, 4), flags = A->reg(c, 7);
        const bool effect = (flags & 0x80000) && !(event & 0x10000);
        const bool allowed = (!(condition & 1) || (flags & 2)) && (!(condition & 2) || effect) &&
            (!(condition & 4) || (effect && (flags & 1))) &&
            (!(condition & 8) || (A->mem(key + 8)[0] & 1)) &&
            (!(condition & 16) || std::any_of(g_tribute_equips.begin(), g_tribute_equips.end(),
                [&](const TributeEquip &e) { return e.card == (saved & 0x1fff) && e.token == token; })) &&
            (!(condition & 32) || std::any_of(g_deck_equips.begin(), g_deck_equips.end(),
                [&](const TributeEquip &e) { return e.card == (saved & 0x1fff) && e.token == token; })) &&
            (!(condition & 64) || (((event >> 10) & 15) == 12));
        g_trigger_scope.push_back({it->second, (uint16_t)(saved & 0x1fff), owner, token, allowed});
        adapt_tribute = allowed && (condition & 48) && it->second == 5724;
        if (controller != owner) g_trigger_scope.push_back({it->second, (uint16_t)(saved & 0x1fff), controller, token, allowed});
    }
    *idp = (uint16_t)((saved & ~0x1fff) | it->second);
    if (adapt_tribute) A->set_reg(c, 7, original_flags | 0x80000);
    A->call_original(c, F_TRIGGER);
    if (adapt_tribute) A->set_reg(c, 7, original_flags);
    *idp = saved;
    g_trigger_scope.resize(scope);
    static const bool dbg = getenv("TF3_DEBUG_TRIG2") != nullptr;
    if (dbg) { static int n; if (n++ < 300) A->log("trigger alias: card %u as %u event %08x a2 %u flags %08x key %02x%02x%02x%02x -> %u", saved & 0x1fff, it->second, A->reg(c, 4), A->reg(c, 6), A->reg(c, 7), A->mem(key)[0], A->mem(key)[1], A->mem(key)[2], A->mem(key)[3], A->reg(c, 2)); }
    if (!real_event_ids() && A->reg(c, 2)) g_trig_real[it->second] = (uint16_t)(saved & 0x1fff);
    return 1;
}

/* BOOT keeps a fixed list of FLIP monsters, independent of their effect records.
 * Borrow that classification too, so a face-down imported monster attacked in
 * battle reaches the normal FLIP event validation and resolution path. */
static int hk_flip_kind(TfCpu *c) {
    const uint32_t id = A->reg(c, 4);
    const auto it = g_trig.find((uint16_t)id);
    if (id > 0x1fff || it == g_trig.end()) return 0;
    A->set_reg(c, 4, it->second);
    A->call_original(c, 0x0884d1b8);
    A->set_reg(c, 4, id);
    return 1;
}

/* Summon triggers: after every action the game scans the events it caused in 09e89110, a long chain of `id == N` branches over the id of the card that acted,
 * which the action record at 09f788a4 keeps in bits 8..20 of its first word. While the scan runs, a new card flagged as a Summon trigger shows its donor's id
 * there, so the scan queues the donor's event; the effect object then carries the donor id and fx_card() maps it back to the new card. */
static const uint32_t F_POST_ACTION = 0x09e89110, ACTION_STATE = 0x09f80000u - 30556u;
static int hk_post_action(TfCpu *c) {
    uint32_t *w = (uint32_t *)A->mem(ACTION_STATE);
    const uint16_t id = (uint16_t)((*w >> 8) & 0x1fff);
    static const bool dbg = getenv("TF3_DEBUG_TRIG2") != nullptr;
    static const uint32_t dbg_id = getenv("TF3_DEBUG_POST") ? (uint32_t)atoi(getenv("TF3_DEBUG_POST")) : 0;   /* also log the actions of this card (a game card to compare with) */
    if ((dbg && id >= 6958) || (dbg_id && id == dbg_id)) { static int n; if (n++ < 300) A->log("post action: record %08x id %u (action type %u)", *w, id, A->mem(0x09f80000u - 30532u)[0]); }
    const auto it = g_trig.find(id);
    if (it == g_trig.end() || !g_trig_summon.count(id)) { g_trig_real.erase(id); return 0; }
    const uint32_t saved = *w;
    const size_t scope = g_trigger_scope.size();
    // The action record stores its controller in bit 0.
    if (real_event_ids()) {
        const uint32_t side = saved & 1, slot = (saved & 0x1f) >> 1;
        const uint8_t *entry = A->mem(DUEL_SIDES + side * DUEL_SIDE_SIZE + 0x30 + slot * 20);
        const uint16_t token = (uint16_t)(((*(const uint16_t *)(entry + 2) & 0x3fff) >> 6) * 2 + ((entry[1] >> 5) & 1));
        g_trigger_scope.push_back({it->second, id, (uint8_t)side, zone_card(side, slot) == id ? token : (uint16_t)0xffff});
    }
    *w = (saved & ~(0x1fffu << 8)) | ((uint32_t)it->second << 8);
    if (!real_event_ids()) g_trig_real[it->second] = id;
    A->call_original(c, F_POST_ACTION);
    g_trigger_scope.resize(scope);
    if (((*w >> 8) & 0x1fff) == it->second) *w = (*w & ~(0x1fffu << 8)) | (saved & (0x1fffu << 8));
    return 1;
}

/* Battle triggers: the battle resolution 09ea28d8(side) reads the card ids of the two fighters from the battle records (0x38 bytes each, id at 09f77ddc + 0x38 * side)
 * and queues the events ("destroys a monster by battle", ...) through a chain of `id == N` branches. A new card flagged for it shows its donor's id there. */
static const uint32_t F_BATTLE = 0x09ea28d8, BATTLE_ID = 0x09f77ddc;
static const uint32_t F_BATTLE_DMG = 0x09f28de8;   /* "inflicts Battle Damage": queues 0x1e22xxxx events through 09eb5640 */
/* research aid (TF3_DEBUG_BATTLE=1): dumps the two battle records (14 words each) before and after the battle resolution functions run */
static void battle_dump(const char *when, const char *what, uint32_t side) {
    const uint32_t *r = (const uint32_t *)A->mem(BATTLE_ID);
    char b[512]; int n = 0;
    for (int i = 0; i < 28; i++) n += snprintf(b + n, sizeof b - n, "%s%x", i == 14 ? " | " : i ? " " : "", r[i]);
    A->log("battle %s %s(side %u): %s", when, what, side, b);
}
static int battle_gate(TfCpu *c, uint32_t fn) {
    uint32_t *ids[2] = {(uint32_t *)A->mem(BATTLE_ID), (uint32_t *)A->mem(BATTLE_ID + 0x38)};
    uint32_t saved[2] = {*ids[0], *ids[1]};
    bool any = false;
    const size_t scope = g_trigger_scope.size();
    for (int k = 0; k < 2; k++) {
        const auto it = g_trig.find((uint16_t)(saved[k] & 0xffff));
        if (saved[k] > 0x1fff || it == g_trig.end() || !g_trig_battle.count((uint16_t)saved[k])) continue;
        *ids[k] = it->second;
        if (real_event_ids()) g_trigger_scope.push_back(TriggerSource{it->second, (uint16_t)saved[k], (uint8_t)k,
            (uint16_t)(*(const uint32_t *)A->mem(BATTLE_ID + 0x38 * k - 4) & 0x1ff)});
        else g_trig_real[it->second] = (uint16_t)saved[k];
        any = true;
    }
    if (!any) {
        static const bool dbg = getenv("TF3_DEBUG_BATTLE") != nullptr;
        if (!dbg) return 0;
        const char *nm = fn == F_BATTLE ? "09ea28d8" : "09f28de8";
        battle_dump("before", nm, A->reg(c, 4)); A->call_original(c, fn); battle_dump("after", nm, A->reg(c, 4));
        return 1;
    }
    if (getenv("TF3_DEBUG_FX")) A->log("battle gate: ids %u %u -> %u %u", saved[0], saved[1], *ids[0], *ids[1]);
    A->call_original(c, fn);
    g_trigger_scope.resize(scope);
    for (int k = 0; k < 2; k++) if (*ids[k] != saved[k] && saved[k] > 0 && g_trig.count((uint16_t)saved[k])) *ids[k] = saved[k];
    return 1;
}
static int hk_battle(TfCpu *c) { return battle_gate(c, F_BATTLE); }
static int hk_battle_dmg(TfCpu *c) { return battle_gate(c, F_BATTLE_DMG); }

/* Reuse the native Goblin Attack Force attack marker and timed position lock.
 * The draw trigger keeps its own donor; position behavior is independent. */
static int hk_after_attack_mark(TfCpu *c) {
    if (*(const uint32_t *)A->mem(0x09f7d4a0) != 1) return 0;
    const uint32_t side = *(const uint32_t *)A->mem(0x09f77da0);
    const uint32_t slot = *(const uint32_t *)A->mem(0x09f77dbc);
    if (side > 1 || slot > 4 || !g_after_attack_defense.count(zone_card(side, slot)) || !zone_effect_active(side, slot)) return 0;
    call_game6(c, 0x09f2ccb4, side, slot, 0x14d6, 2, 0, 0);
    return 0;
}

static int hk_after_attack_defense(TfCpu *c) {
    if (*(const uint32_t *)A->mem(0x09f7d4a0) != 1) return 0;
    for (uint32_t side = 0; side < 2; ++side) for (uint32_t slot = 0; slot < 5; ++slot) {
        const uint16_t id = (uint16_t)zone_card(side, slot);
        const uint8_t *entry = A->mem(DUEL_SIDES + side * DUEL_SIDE_SIZE + 0x30 + slot * 20);
        if (!g_after_attack_defense.count(id) || !zone_effect_active(side, slot) || *(const uint16_t *)(entry + 6)) continue;
        if (!call_game(c, 0x09f465c0, side, slot, 0x14d6, 0)) continue;
        call_game(c, 0x09f2cec8, side, slot, 0x14d6, 1);
        const uint32_t token = ((*(const uint16_t *)(entry + 2) & 0x3fff) >> 6) * 2 + ((entry[1] >> 5) & 1);
        call_game(c, 0x09f289cc, side, slot, token, 0);
        call_game6(c, 0x09f2dd6c, side, slot, 0, 0, id, 0);
        call_game6(c, 0x09f2ccb4, side, slot, 0x1419, 0x22, 0, 0);
    }
    return 0;
}

static int hk_event_store(TfCpu *c) {
    const uint32_t word = A->reg(c, 5);
    const uint16_t id = (uint16_t)word;
    uint16_t real = id;
    for (auto it = g_trigger_scope.rbegin(); it != g_trigger_scope.rend(); ++it)
        if (it->donor == id && it->side == (word >> 31) &&
            (it->token == 0xffff || it->token == (A->reg(c, 6) & 0x1ff))) {
            if (!it->allowed) return 1;
            real = it->card;
            break;
        }
    const uint32_t bucket = A->reg(c, 4);
    if (bucket > 1) return 0;
    const uint32_t turn = *(const uint32_t *)A->mem(0x09f7d460);
    const bool deferred = *(const uint32_t *)A->mem(0x09f7d47c) != 0 && turn < 2 &&
        *(const uint32_t *)A->mem(0x09f77888 + turn * 4) == 2;
    uint16_t token = (uint16_t)A->reg(c, 6);
    const uint32_t param = A->reg(c, 7);
    if (deferred && real != id && id) {
        // Normalize exactly as the native writer does before serializing its packet.
        if (token < 2) {
            token = (uint16_t)call_game(c, 0x09f4a5a4, word >> 31, id, 0, 0);
            A->set_reg(c, 6, token);
        }
        g_deferred_sources.push_back({bucket, word, param, token, real});
    } else if (!deferred && real == id) {
        for (auto it = g_deferred_sources.begin(); it != g_deferred_sources.end(); ++it) {
            if (it->bucket != bucket || it->word != word || it->token != token || it->param != param) continue;
            real = it->card;
            g_deferred_sources.erase(it);
            break;
        }
    }
    const uint32_t counter = bucket ? 0x09f78660 : 0x09f78670;
    const uint32_t before = *(const uint32_t *)A->mem(counter);
    A->call_original(c, 0x09eb53b0);
    const uint32_t after = *(const uint32_t *)A->mem(counter);
    if (after == before + 1 && before < (bucket ? 16u : 32u)) {
        const uint8_t *o = A->mem((bucket ? 0x09f784e0 : 0x09f781e0) + before * 24);
        const uint64_t key = event_key(o);
        if (real != id) g_event_sources[key] = real;
        else g_event_sources.erase(key);
        if (getenv("TF3_DEBUG_FX")) A->log("event identity: side %u donor %u source %u token %u", word >> 31, id, real, (unsigned)(key >> 17));
    }
    return 1;
}

/* research aid (TF3_DEBUG_STREAM=1): dumps the card-art stream object when the game queues a read far beyond the file (BOOT 0882ead0:
 * a1 = stream object, a3 = file position; the object keeps the .cip header at +172, the index entries {unit, state} follow at +0x30) */
static int hk_debug_obj(TfCpu *c) {   /* TF3_DEBUG_OBJ: first request of each .cip stream object (slot ring of the preloader) */
    static uint32_t seen[8]; static int ns;
    const uint32_t obj = A->reg(c, 5);
    if (obj < 0x08000000u || obj > 0x0a000000u - 0x8000) return 0;
    const uint32_t magic = *(const uint32_t *)A->mem(obj + 172);
    if (magic != 0x1a4d5043u && magic != 0x1a504943u) return 0;
    for (int i = 0; i < ns; i++) if (seen[i] == obj) return 0;
    if (ns < 8) seen[ns++] = obj;
    const uint32_t *w = (const uint32_t *)A->mem(obj);
    A->log("cip stream obj %08x: size field %08x lo %u hi %u", obj, w[(172 + 4) / 4], w[(172 + 20) / 4], w[(172 + 24) / 4]);
    A->log("  +128 %08x +132 %08x +136 %08x +140 %08x +144 %08x +148 base %08x +152 slots %u +156 next %u +160 %08x +164 %08x +188 %u +192 %08x +196 %08x +200 %u +204 chunk-units %u",
           w[32], w[33], w[34], w[35], w[36], w[37], w[38], w[39], w[40], w[41], w[47], w[48], w[49], w[50], w[51]);
    return 0;
}

static int hk_debug_stream(TfCpu *c) {
    static int n;
    const uint32_t obj = A->reg(c, 5), pos = A->reg(c, 7);
    if (n >= 1 || obj < 0x08000000u || obj > 0x0a000000u - 0x8000) return 0;
    const uint32_t magic = *(const uint32_t *)A->mem(obj + 172);
    if ((magic != 0x1a4d5043u && magic != 0x1a504943u) || pos < 0x10000000u) return 0;   /* "CPM" / "CIP", garbage position */
    n++;
    A->log("stream dump: obj %08x pos %08x", obj, pos);
    const uint32_t *w = (const uint32_t *)A->mem(obj);
    for (int i = 0; i < 56; i += 4) A->log("  obj+%03x: %08x %08x %08x %08x", i * 4, w[i], w[i + 1], w[i + 2], w[i + 3]);
    const uint32_t *h = (const uint32_t *)A->mem(obj + 172);
    A->log("  header: size=%08x end_index=%08x lo=%u hi=%u idx_off=%08x chunk=%u", h[1], h[4], h[5], h[6], h[7], h[8]);
    const uint32_t *e = (const uint32_t *)A->mem(obj + 172 + h[7]);
    const uint32_t end = (h[4] - h[7]) / 8 + 40;
    for (uint32_t i = 0; i < end; i++) {
        if (e[i * 2 + 1] != 0 || e[i * 2] == 0xffffffffu || i > (h[4] - h[7]) / 8 - 6)
            A->log("  entry %u (+%x): unit %08x state %08x", i, h[7] + i * 8, e[i * 2], e[i * 2 + 1]);
    }
    return 0;
}

/* research aid (TF3_DEBUG_REQ=1): logs every card-art read request (stream object, magic of its .cip header, file position, size) */
static int hk_debug_req(TfCpu *c) {
    static int n;
    const uint32_t obj = A->reg(c, 5), pos = A->reg(c, 7);
    if (n >= 600 || obj < 0x08000000u || obj > 0x0a000000u - 0x8000) return 0;
    const uint32_t magic = *(const uint32_t *)A->mem(obj + 172);
    if (magic != 0x1a4d5043u && magic != 0x1a504943u) return 0;
    n++;
    A->log("art request: %s pos %08x (unit %u) a4=%08x a6=%08x", magic == 0x1a4d5043u ? "CPM" : "CIP", pos, pos / 0x800, A->reg(c, 8), A->reg(c, 6));
    return 0;
}

extern "C" __declspec(dllexport) int tfmod_init(const TfModApi *api) {
    A = api;
    ported_load();
    fx_load();
    trig_load();
    amt_load();
    stat_load();
    destroy_load();
    ext_init();
    cfg_load();
    const char *off = getenv("TF3_OFF");   /* research aid: letters of the hook groups to leave out (d deck/duel, r record, f frame, c collection, t trunk, s art stream, e tables) */
    const auto on = [&](char k) { return !(off && strchr(off, k)); };
    if (on('d')) { A->hook(F_READ_DECK, hk_read_deck); A->hook(F_PREPARE_DUEL, hk_prepare_duel); }
    if (on('r')) A->hook(F_EFFECT_RECORD, hk_effect_record);
    if (on('f')) A->hook(F_FRAME, hk_frame);
    if (on('c')) A->hook(F_COLLECTION_ENTRY, hk_collection_entry);
    A->hook(F_DUEL_START, hk_duel_start);
    A->hook(F_RANDOM, hk_random);
    if (g_ext_n && on('t')) { A->hook(F_LIST_ALLOC, hk_list_alloc); A->hook(F_TRUNK_LIST, hk_trunk_list); }
    if (g_ext_n) {
        g_debug_lazy = getenv("TF3_DEBUG_LAZY") != nullptr;
        if (on('s')) { A->hook(F_STREAM_REQUEST, hk_stream_request); A->hook(F_STREAM_READY, hk_stream_ready); A->hook(F_STREAM_GET, hk_stream_get); A->hook(F_TEXTURE_FROM_DATA, hk_texture_from_data); }
        if (on('e')) A->hook(F_EHP_MEMBER, hk_ehp_member);
    }
    if (const char *fl = getenv("TF3_DEBUG_FN")) fn_log_init(fl);
    if (getenv("TF3_DEBUG_TRIG")) A->hook(F_TRIGGER, hk_trigger_debug);
    else if (!g_trig.empty()) A->hook(F_TRIGGER, hk_trigger);
    if (!g_trig.empty()) A->hook(0x0884d1b8, hk_flip_kind);
    if (!g_after_attack_defense.empty()) {
        A->hook(0x09ea00fc, hk_after_attack_mark);
        A->hook(0x09ea53f4, hk_after_attack_defense);
    }
    if (!g_trig_summon.empty()) A->hook(F_POST_ACTION, hk_post_action);
    if (real_event_ids() && !g_trig.empty()) A->hook(0x09eb53b0, hk_event_store);
    if (!g_trig_battle.empty() || getenv("TF3_DEBUG_BATTLE")) { A->hook(F_BATTLE, hk_battle); A->hook(F_BATTLE_DMG, hk_battle_dmg); }
    if (!g_stat.empty()) {
        A->hook(F_STATS, hk_stats);
        A->hook(0x09eb9fc8, hk_equip_target);
    }
    if (!g_equip_level.empty()) A->hook(F_LEVEL, hk_level);
    if (!g_attack_protect.empty()) A->hook(0x09f5153c, hk_attack_target);
    if (!g_control_lock.empty()) A->hook(0x09f53358, hk_control);
    if (slot_of(7000) || gladius_return_enabled() || getenv("TF3_TEST_VENUS_CHAIN")) {
        A->hook(0x09f454b8, hk_replacement_present);
        A->hook(0x09f45fec, hk_replacement_equip);
        A->hook(0x09f3332c, hk_destroy_mark);
        A->hook(0x09f34d64, hk_destroy_remove);
        A->hook(0x09f3a730, hk_battle_destroy_replace);
    }
    if (sword_return_enabled()) {
        A->hook(0x09f34044, hk_equipment_cleanup);
    }
    if (sabre_mill_enabled()) {
        A->hook(0x09f2a7ec, hk_sabre_mill);
        A->hook(0x09f15694, hk_sabre_mill_event);
    }
    if (getenv("TF3_TEST_VENUS")) {
        A->hook(0x09ee55c8, hk_venus_negation);
        A->hook(0x09ed26d0, hk_venus_effect_negation);
        A->hook(0x09ed2568, hk_venus_continuous_negation);
    }
    if (getenv("TF3_TEST_VENUS_CHAIN")) {
        A->hook(0x09eb7248, hk_venus_chain);
        A->hook(0x09f4c490, hk_venus_frontier);
        A->hook(0x09f4cc24, hk_venus_phoenix);
    }
    if (getenv("TF3_DEBUG_CAND")) { A->hook(F_CAND_RUN, hk_cand_run_debug); A->hook(F_CAND_COUNT, hk_cand_count_debug); }
    else if (!g_fx.empty()) { A->hook(F_CAND_RUN, hk_cand_run); A->hook(F_CAND_COUNT, hk_cand_count); A->hook(F_MOVE_PICKED, hk_move_to_hand); }
    if (!g_amt.empty()) A->hook(F_DRAW, hk_draw);
    A->hook(F_LP_AMOUNTS, hk_lp_amounts);
    A->hook(F_EFFECT_DRIVER, hk_effect_driver);
    A->hook(F_TARGET_ALL, hk_target_all);
    if (!g_destroy.empty()) { A->hook(F_TARGET_OPP, hk_target_all); A->hook(F_TARGET_ST, hk_target_all); A->hook(F_TARGET_ATK, hk_target_all); A->hook(0x09ebc0a8, hk_target_all); A->hook(0x09ebfe68, hk_target_all); A->hook(F_ATTACK_TRIGGER, hk_attack_trigger); }
    if (getenv("TF3_DEBUG_STREAM")) A->hook(0x0882ead0, hk_debug_stream);
    if (getenv("TF3_DEBUG_OBJ")) A->hook(0x0882ead0, hk_debug_obj);
    if (getenv("TF3_DEBUG_REQ")) A->hook(0x0882ead0, hk_debug_req);
    for (const auto &a : PORTED) A->log("TF3 Cards: card %u (%s) plays as id %u%s", a.tf3, a.name.c_str(), a.slot, a.like ? ", effect of another card" : "");
    A->log("TF3 Cards: force_card=%d force_copies=%d", g_card, g_copies);
    return 0;
}
