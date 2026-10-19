/* More players and longer player names (docs/network_players.md).
 *
 * 1. MaxPlayers (9..12): the 8-player limit is a compile-time 8 spread over the player record
 *    table (gGameSetup+0x5a0), the DragNet lockstep channels (DN_g_Net+0x318), the per-frame input
 *    array (0x521fd0), the human cars' state (0x511c78) and the lobby's row widgets. Each of those arrays is followed by live data,
 *    so they move to bigger arrays here and every instruction that indexes them gets its disp32
 *    rewritten (players_sites.h, generated and checked against the exe by tools/gen_players.py);
 *    the loop bounds and the DirectPlay session size become the new count. On top of that:
 *    - the roster message (0x27, one record per slot) is sent and received by replacements sized
 *      for 12 slots, and the DragNet receive pool's element (341 bytes, the 8x8 input bundle) grows;
 *    - controller types 7..14 are network slots 0..7 and 15 is taken, so slots 8+ use 16+;
 *    - 4 more player colours;
 *    - the lobby lists 12 rows in the space of 8 (rows 17 px apart, chat lines 12 px apart).
 *    Every machine needs the same setting: the network version string carries the count, so a
 *    mismatched or unpatched game is refused with "Wrong game version".
 * 2. NameLength (12..31): the name fields allowed 12 characters (the record holds 95). The copies
 *    into fixed fields are bounded: the ghost (.rail) header keeps 12, the race records 15. */
#include <string.h>
#include "dkpatch.h"
#include "players_sites.h"

#define THISCALL __attribute__((thiscall))

#define MAXP          12
#define NET           0x522248u   /* DN_g_Net */
#define GAME_SETUP    0x7045c8u   /* gGameSetup */
#define REC_SIZE      0xac        /* player record */
#define CHAN_SIZE     0x2a8       /* lockstep channel */
#define ROSTER_SIZE   (2 + MAXP * REC_SIZE)
#define GS            ((void *)(uintptr_t)GAME_SETUP)

int dk_max_players = 8;
uintptr_t dk_net_channels = NET + 0x318;

static unsigned char g_slots[MAXP * REC_SIZE];
static unsigned char g_chans[MAXP * CHAN_SIZE];
static unsigned char g_framein[MAXP * 8];
static unsigned char g_cars[MAXP * 0x340];     /* human car state, 0x511c78 */
static void *g_lobby[7][MAXP];

static int (*const DN_GetLocalSlot)(void) = (void *)0x43d020;
static void (*const DN_SetLocalSlot)(int) = (void *)0x43d030;
static void (*const DN_SetHostSlot)(int) = (void *)0x43d060;
static int (THISCALL *const DN_FindPlayer)(void *, DWORD) = (void *)0x443b80;       /* this = NET+0x68 */
static int (THISCALL *const DN_SendCompressed)(void *, int, DWORD, void *, int) = (void *)0x445900;
static int (THISCALL *const DN_Decompress)(void *, void *, void *, DWORD *) = (void *)0x445a30;
static DWORD (THISCALL *const MP_GetSlotDPID)(void *, unsigned) = (void *)0x4899e0;
static void (THISCALL *const MP_ClearSlot)(void *, unsigned) = (void *)0x489df0;
static void (THISCALL *const MP_CopyLocalRecordToSlot)(void *, unsigned) = (void *)0x488fa0;
static void (THISCALL *const MP_CopySlotRecordOut)(void *, unsigned, void *) = (void *)0x489d50;
static void (THISCALL *const MP_CopySlotRecordIn)(void *, unsigned, const void *) = (void *)0x489dc0;
static void (THISCALL *const MP_MergeSlotRecord)(void *, unsigned) = (void *)0x489040;
static char (THISCALL *const MP_IsSlotActive)(void *, unsigned) = (void *)0x489880;
static int (THISCALL *const MP_GetSlotState)(void *, unsigned) = (void *)0x489cc0;
static unsigned char (THISCALL *const MP_GetSlotColour)(void *, unsigned) = (void *)0x489b60;
static void (THISCALL *const MP_SetSlotColour)(void *, unsigned, unsigned) = (void *)0x489bc0;

static void net_vcall(void *net, int off)
{
    ((void (THISCALL *)(void *))(*(void ***)net)[off / 4])(net);
}

/* --- roster (msg 0x27): yourSlot, hostSlot, MAXP records --- */

/* replaces DN_BroadcastRoster (0x43d970) */
static void THISCALL broadcast_roster(void *net)
{
    static unsigned char buf[ROSTER_SIZE];
    unsigned slot;
    for (slot = 0; slot < (unsigned)dk_max_players; slot++) {
        if ((int)slot != DN_GetLocalSlot()
            && !DN_FindPlayer((char *)net + 0x68, MP_GetSlotDPID(GS, slot))) {
            MP_ClearSlot(GS, slot);
        }
    }
    memset(buf, 0, sizeof buf);
    buf[1] = (unsigned char)DN_GetLocalSlot();
    MP_CopyLocalRecordToSlot(GS, buf[1]);
    for (slot = 0; slot < (unsigned)dk_max_players; slot++) {
        MP_CopySlotRecordOut(GS, slot, buf + 2 + slot * REC_SIZE);
    }
    for (slot = 0; slot < (unsigned)dk_max_players; slot++) {
        if (MP_IsSlotActive(GS, slot) && (int)slot != DN_GetLocalSlot()) {
            buf[0] = (unsigned char)slot;
            DN_SendCompressed(net, 0x27, MP_GetSlotDPID(GS, slot), buf, ROSTER_SIZE);
        }
    }
    net_vcall(net, 0x74);
}

/* replaces DN_OnRoster (0x43da50) */
static void THISCALL on_roster(void *net, void *msg)
{
    static unsigned char buf[ROSTER_SIZE + 64];
    DWORD len = ROSTER_SIZE;
    unsigned slot;
    memset(buf, 0, sizeof buf);
    *(int *)((char *)net + 0x300) = 2;
    DN_Decompress(net, msg, buf, &len);
    DN_SetLocalSlot(buf[0]);
    DN_SetHostSlot(buf[1]);
    for (slot = 0; slot < (unsigned)dk_max_players; slot++) {
        MP_CopySlotRecordIn(GS, slot, buf + 2 + slot * REC_SIZE);
    }
    MP_MergeSlotRecord(GS, DN_GetLocalSlot());
    net_vcall(net, 0x74);
}

/* --- colours: the game's 8, then orange, purple, lime, pink --- */

static const float g_extra_colours[MAXP - 8][4] = {
    { 1.0f, 0.5f, 0.0f, 1.0f }, { 0.6f, 0.2f, 1.0f, 1.0f }, { 0.6f, 1.0f, 0.2f, 1.0f }, { 1.0f, 0.5f, 0.75f, 1.0f },
};
static const float *g_colours[MAXP];

/* replaces MP_AssignSlotColour (0x489e60): first colour no other player has */
static void THISCALL assign_colour(void *gs, unsigned slot)
{
    char used[MAXP] = { 0 };
    unsigned i;
    for (i = 0; i < (unsigned)dk_max_players; i++) {
        if (MP_IsSlotActive(gs, i) && MP_GetSlotState(gs, i) != 1 && i != slot) {
            unsigned c = MP_GetSlotColour(gs, i);
            if (c < MAXP) {
                used[c] = 1;
            }
        }
    }
    for (i = 0; i < (unsigned)dk_max_players; i++) {
        if (!used[i]) {
            MP_SetSlotColour(gs, slot, i);
            return;
        }
    }
    MP_SetSlotColour(gs, slot, 0);
}

/* --- controllers: network slot s has controller type s+7 (7..14); 15 is another source, so
       slots 8+ get 16+ --- */

void ps_ctrl_assign(void);
void ps_ctrl_dispatch(void);
__asm__(
    ".text\n"
    /* replaces "lea edi,[esi+1]; lea ecx,[edi+6]" in MP_AssignSlotControllers (esi = slot) */
    "_ps_ctrl_assign:\n"
    "  lea 1(%esi), %edi\n"
    "  lea 7(%esi), %ecx\n"
    "  cmp $8, %esi\n"
    "  jb 1f\n"
    "  inc %ecx\n"
    "1:\n"
    "  ret\n"
    /* MP_UpdateCarControllers' switch on the type (eax): 16+ -> the network case as type-1 */
    "_ps_ctrl_dispatch:\n"
    "  cmp $0x10, %eax\n"
    "  jb 2f\n"
    "  cmp $0x14, %eax\n"
    "  jae 3f\n"
    "  dec %eax\n"
    "  push $0x485441\n"
    "  ret\n"
    "2:\n"
    "  jmp *0x4854a4(,%eax,4)\n"
    "3:\n"
    "  push $0x485485\n"
    "  ret\n");

/* --- names --- */

/* the ghost file header's 12-byte name (0x404648): as strcpy did for 12 characters, no terminator then */
static void name12(char *dst, const char *src)
{
    strncpy(dst, src, 12);
}

/* race records' 16-byte name fields (0x48a004, 0x48a246) */
static void name16(char *dst, const char *src)
{
    strncpy(dst, src, 15);
    dst[15] = '\0';
}

static int patch_rel(uint32_t at, const uint8_t *expect, size_t n, uint8_t *code, size_t len, int call_at, void *fn, uint32_t jmp_to)
{
    int32_t rel = (int32_t)((uintptr_t)fn - (at + call_at + 5));
    memcpy(code + call_at + 1, &rel, 4);
    if (jmp_to) {
        rel = (int32_t)(jmp_to - (at + len));
        memcpy(code + len - 4, &rel, 4);
    }
    memset(code + len, 0x90, n - len);
    return dk_patch(at, expect, code, n);
}

static void install_names(int len)
{
    static const uint32_t fields[] = { 0x45abe1, 0x46ccea, 0x46f048 };   /* push 0xc; call SetMaxLength */
    static const uint8_t rail_old[] = {
        0x8b, 0xf8, 0x83, 0xc9, 0xff, 0x33, 0xc0, 0x8d, 0x54, 0x24, 0x10, 0xf2, 0xae, 0xf7, 0xd1, 0x2b,
        0xf9, 0x8b, 0xc1, 0x8b, 0xf7, 0x8b, 0xfa, 0xc1, 0xe9, 0x02, 0xf3, 0xa5, 0x8b, 0xc8, 0x83, 0xe1,
        0x03, 0xf3, 0xa4 };
    static const uint8_t rec1_old[] = {
        0x8b, 0xbd, 0x0c, 0x02, 0x00, 0x00, 0x8d, 0x1c, 0x10, 0x83, 0xc9, 0xff, 0x33, 0xc0, 0xf2, 0xae,
        0xf7, 0xd1, 0x8d, 0xb3, 0xf0, 0x89, 0x01, 0x00, 0x2b, 0xf9, 0x8b, 0xc1, 0x89, 0x74, 0x24, 0x14,
        0x8b, 0xf7, 0x8b, 0x7c, 0x24, 0x14, 0xc1, 0xe9, 0x02, 0xf3, 0xa5, 0x8b, 0xc8, 0x83, 0xe1, 0x03,
        0xf3, 0xa4 };
    static const uint8_t rec2_old[] = {
        0x83, 0xc9, 0xff, 0x33, 0xc0, 0x8d, 0x94, 0x24, 0x5c, 0x01, 0x00, 0x00, 0x8b, 0xbb, 0x0c, 0x02,
        0x00, 0x00, 0xf2, 0xae, 0xf7, 0xd1, 0x2b, 0xf9, 0x8b, 0xc1, 0x8b, 0xf7, 0x8b, 0xfa, 0xc1, 0xe9,
        0x02, 0xf3, 0xa5, 0x8b, 0xc8, 0x83, 0xe1, 0x03, 0xf3, 0xa4 };
    uint8_t code[64];
    unsigned i;
    int ok = 1;

    /* push eax (name); lea edx,[esp+0x14]; push edx; call name12; add esp,8 */
    static const uint8_t rail_new[] = { 0x50, 0x8d, 0x54, 0x24, 0x14, 0x52, 0xe8, 0, 0, 0, 0, 0x83, 0xc4, 0x08 };
    /* lea ebx,[eax+edx]; push edx; push [ebp+0x20c]; lea eax,[ebx+0x189f0]; push eax; call name16;
       add esp,8; pop edx */
    static const uint8_t rec1_new[] = { 0x8d, 0x1c, 0x10, 0x52, 0xff, 0xb5, 0x0c, 0x02, 0x00, 0x00, 0x8d, 0x83,
                                        0xf0, 0x89, 0x01, 0x00, 0x50, 0xe8, 0, 0, 0, 0, 0x83, 0xc4, 0x08, 0x5a };
    /* push [ebx+0x20c]; lea edx,[esp+0x160]; push edx; call name16; add esp,8 */
    static const uint8_t rec2_new[] = { 0xff, 0xb3, 0x0c, 0x02, 0x00, 0x00, 0x8d, 0x94, 0x24, 0x60, 0x01, 0x00,
                                        0x00, 0x52, 0xe8, 0, 0, 0, 0, 0x83, 0xc4, 0x08 };

    memcpy(code, rail_new, sizeof rail_new);
    ok &= patch_rel(0x404648, rail_old, sizeof rail_old, code, sizeof rail_new, 6, (void *)name12, 0);
    memcpy(code, rec1_new, sizeof rec1_new);
    ok &= patch_rel(0x48a004, rec1_old, sizeof rec1_old, code, sizeof rec1_new, 17, (void *)name16, 0);
    memcpy(code, rec2_new, sizeof rec2_new);
    ok &= patch_rel(0x48a23a, rec2_old, sizeof rec2_old, code, sizeof rec2_new, 14, (void *)name16, 0);
    if (!ok) {
        dk_log("players: name copies differ, NameLength not applied");
        return;
    }
    for (i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        static const uint8_t old[] = { 0x6a, 0x0c };
        uint8_t new_[] = { 0x6a, (uint8_t)len };
        dk_patch(fields[i], old, new_, 2);
    }
    dk_log("players: names up to %d characters", len);
}

/* --- player count --- */

static uint32_t new_value(const struct ps_site *s, int n)
{
    uintptr_t slots = (uintptr_t)g_slots, chans = (uintptr_t)g_chans;
    static const int lobby_kind[7] = { PS_LB_CROSS, PS_LB_CARICON, PS_LB_WEAPON, PS_LB_CARNAME, PS_LB_BOX, PS_LB_TRANS, PS_LB_NAME };
    static const uint32_t lobby_base[7] = { 0x534a60, 0x534a80, 0x534b18, 0x534b78, 0x534bc0, 0x534c20, 0x534c50 };
    int i;
    switch (s->kind) {
    case PS_SLOT:
        return s->value - 0x5a0 + (uint32_t)(slots - GAME_SETUP);
    case PS_CHAN:
        return s->value - 0x318 + (uint32_t)(chans - NET);
    case PS_CAR:
        return s->value - 0x511c78 + (uint32_t)(uintptr_t)g_cars;
    case PS_FRAMEIN:
        return s->value - 0x521fd0 + (uint32_t)(uintptr_t)g_framein;
    case PS_FRAMEIN_END:
        return (uint32_t)(uintptr_t)(g_framein + n * 8);
    case PS_COUNT:
        return (uint32_t)n;
    case PS_LB_CROSS_END:
        return (uint32_t)(uintptr_t)&g_lobby[0][n];
    case PS_LB_WEAPON_END:
        return (uint32_t)(uintptr_t)&g_lobby[2][n];
    case PS_LB_NAME_END:
        return (uint32_t)(uintptr_t)&g_lobby[6][n];
    }
    for (i = 0; i < 7; i++) {
        if (s->kind == lobby_kind[i]) {
            return s->value - lobby_base[i] + (uint32_t)(uintptr_t)g_lobby[i];
        }
    }
    return s->value;
}

static int patch_float(uint32_t at, uint32_t expect_addr, const float *value)
{
    uint32_t to = (uint32_t)(uintptr_t)value;
    return dk_patch(at, &expect_addr, &to, 4);
}

/* 12 rows where 8 were: rows 17 px apart (boxes and icons 16 px), the 5 chat lines below them 12 px apart */
static void compact_lobby(void)
{
    static const float spacing = 17.0f, box_top = 410.0f, text_y = 396.0f, chat_spacing = 12.0f, chat_base = 185.0f;
    static const struct { uint32_t at; uint8_t old, new_; } icons[] = {
        { 0x458a5c, 0x16, 0x10 }, { 0x458a5e, 0x16, 0x10 },   /* cross */
        { 0x458b2a, 0x16, 0x10 }, { 0x458b2c, 0x16, 0x10 },   /* weapon */
        { 0x458b8b, 0x16, 0x10 }, { 0x458b8d, 0x16, 0x10 },   /* transmission */
        { 0x458be5, 0x16, 0x10 }, { 0x458be7, 0x2c, 0x20 },   /* car */
    };
    unsigned i;
    patch_float(0x45893c, 0x4f1aec, &spacing);
    patch_float(0x458954, 0x4f1af4, &box_top);
    patch_float(0x458a86, 0x4f1af8, &text_y);
    patch_float(0x458ca6, 0x4f1ae8, &chat_spacing);
    patch_float(0x458cb2, 0x4f1b00, &chat_base);
    for (i = 0; i < sizeof icons / sizeof icons[0]; i++) {
        dk_patch(icons[i].at, &icons[i].old, &icons[i].new_, 1);
    }
}

static void install_players(int n)
{
    static const uint8_t pool_old[] = { 0xc1, 0xe7, 0x06, 0x83, 0xc7, 0x15 };   /* shl edi,6; add edi,0x15 */
    static const uint8_t pool_new[] = { 0xbf, 0x00, 0x10, 0x00, 0x00, 0x90 };   /* mov edi,0x1000 */
    static const uint8_t roster_out[] = { 0x81, 0xec, 0x64, 0x05, 0x00, 0x00 };
    static const uint8_t roster_in[] = { 0x81, 0xec, 0x68, 0x05, 0x00, 0x00 };
    static const uint8_t colour[] = { 0x83, 0xec, 0x08, 0x53, 0xb8 };
    static const uint8_t assign[] = { 0x8d, 0x7e, 0x01, 0x8d, 0x4f, 0x06 };
    static const uint8_t dispatch[] = { 0x83, 0xf8, 0x0f, 0x0f, 0x87, 0xa5, 0x00, 0x00, 0x00 };
    static const char version_old[24] = "Oct 16 1998\0" "22:51:53";
    char version[24] = "Oct 16 1998\0" "dkpatch";
    const uint32_t colour_table = 0x507788;
    unsigned i, bad = 0;

    /* everything is checked first, so a different exe stays untouched */
    for (i = 0; i < sizeof ps_sites / sizeof ps_sites[0]; i++) {
        if (!dk_expect(ps_sites[i].addr, ps_sites[i].bytes, ps_sites[i].len)) {
            dk_log("players: %08x differs", ps_sites[i].addr);
            bad++;
        }
    }
    if (bad || !dk_expect(0x43d208, pool_old, 6) || !dk_expect(0x43d970, roster_out, 6)
        || !dk_expect(0x43da50, roster_in, 6) || !dk_expect(0x489e60, colour, 5) || !dk_expect(0x42a12e, assign, 6)
        || !dk_expect(0x4853d7, dispatch, 9) || !dk_expect(0x489c02, &colour_table, 4)
        || !dk_expect(0x4fe238, version_old, sizeof version_old)) {
        dk_log("players: code differs, MaxPlayers not applied");
        return;
    }

    for (i = 0; i < sizeof ps_sites / sizeof ps_sites[0]; i++) {
        const struct ps_site *s = &ps_sites[i];
        uint32_t v = new_value(s, n);
        dk_write(s->addr + s->off, &v, s->len - s->off == 1 ? 1 : 4);   /* imm8 (cmp/push) or disp32/imm32 */
    }
    dk_net_channels = (uintptr_t)g_chans;
    dk_max_players = n;

    dk_write(0x43d208, pool_new, 6);
    dk_jmp(0x43d970, (void *)broadcast_roster);
    dk_jmp(0x43da50, (void *)on_roster);
    dk_jmp(0x489e60, (void *)assign_colour);
    {
        static const uint8_t nop = 0x90;
        dk_call(0x42a12e, (void *)ps_ctrl_assign);
        dk_write(0x42a133, &nop, 1);
        dk_jmp(0x4853d7, (void *)ps_ctrl_dispatch);
    }
    for (i = 0; i < 8; i++) {
        g_colours[i] = ((const float **)(uintptr_t)colour_table)[i];
    }
    for (i = 8; i < MAXP; i++) {
        g_colours[i] = g_extra_colours[i - 8];
    }
    {
        uint32_t to = (uint32_t)(uintptr_t)g_colours;
        dk_write(0x489c02, &to, 4);
    }
    version[19] = (char)('0' + n / 10);
    version[20] = (char)('0' + n % 10);
    dk_write(0x4fe238, version, sizeof version);
    compact_lobby();
    dk_log("players: up to %d players (%u sites)", n, (unsigned)(sizeof ps_sites / sizeof ps_sites[0]));
}

void dk_players_install(void)
{
    int n = dk_ini_int("Players", "MaxPlayers", MAXP);
    int len = dk_ini_int("Players", "NameLength", 18);
    if (len > 12) {
        install_names(len > 31 ? 31 : len);
    }
    if (n > 8) {
        install_players(n > MAXP ? MAXP : n);
    }
}
