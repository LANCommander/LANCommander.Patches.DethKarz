/* DragNet connection-stability fixes (docs/network_stability.md).
 *
 * 1. Retries: the default number of NACKs before the "Connection timeout / Waiting on %s"
 *    modal opens (15, i.e. about 1.9 s of silence at the 125 ms Timeout). Host-side: clients
 *    adopt the host's config block (msg 0x2e).
 * 2. The modal's callbacks only receive, so once every waiting peer is inside its modal nothing
 *    is sent any more and the race never recovers by itself. The lockstep modal's callback is
 *    replaced: it keeps NACKing, and on the host it also closes once every client's input block
 *    is in (with 3+ players the healthy clients stall silently waiting for the host's next block,
 *    so "heard from everyone recently" never becomes true while the host sits in the modal).
 *    The keep-alive modal's DN_NetPoll call is redirected to a wrapper that keeps pinging.
 * 3. DirectPlay player loss: DESTROYPLAYER left the lockstep slot active (the host then stalls),
 *    SESSIONLOST was ignored. The host now drops the slot, as for a client's quit message
 *    (0x25); a client treats losing the host like "Host has quit the game" (0x24). */
#include <string.h>
#include "dkpatch.h"

#define THISCALL __attribute__((thiscall))

#define NET          0x522248u   /* DN_g_Net */
#define GAME_SETUP   0x7045c8u   /* gGameSetup (slot table) */
#define NET_STATE    (*(volatile int *)(NET + 0x188c))
#define NET_HOSTDPID (*(volatile DWORD *)(NET + 0x16c))
#define NET_HOSTQUIT (*(volatile char *)(NET + 0x314))
#define NET_TIMEOUT  (*(volatile DWORD *)(NET + 0x1864))   /* config Timeout (ms) */
#define NET_SENDIDX  (*(volatile int *)(NET + 0x1894))     /* host: next ring index to merge */
#define SLOT(i)      (dk_net_channels + (i) * 0x2a8)          /* lockstep channel (DN_GetSlot) */
#define SLOT_WRITE(i) (*(volatile int *)(SLOT(i) + 4))      /* ring write index */
#define HOST_SLOT    (*(volatile unsigned *)0x524c38)      /* DN_g_HostSlot */
#define NET_THIS     ((void *)(uintptr_t)NET)

static char (*const DN_NetPoll)(int) = (void *)0x43f480;
static char (THISCALL *const DN_IsHost)(void *) = (void *)0x4454f0;
static char (THISCALL *const DN_IsRemoteActiveSlot)(void *, unsigned) = (void *)0x43ef10;
static char (THISCALL *const DN_WaitDialogReady)(void *) = (void *)0x43e730;
static char (THISCALL *const DN_SlotTimedOut)(void *, unsigned, DWORD) = (void *)0x43e800;
static char (THISCALL *const DN_SendResendNack)(void *, unsigned) = (void *)0x43e520;  /* 0 = sent (bool in al) */
static int (THISCALL *const DN_PlayerCount)(void *) = (void *)0x4477c0;                /* this = NET+0x68 */
static DWORD *(THISCALL *const DN_PlayerAt)(void *, int) = (void *)0x4479f0;          /* [0] = DPID */
static char (THISCALL *const DN_KeepAliveExpired)(void *, DWORD *, DWORD) = (void *)0x4455f0;
static void (THISCALL *const DN_SendPing)(void *, DWORD, DWORD) = (void *)0x445700;
static void (THISCALL *const DN_DropSlot)(void *, unsigned, int) = (void *)0x43e640;
static void (THISCALL *const DN_OnHostQuit)(void *) = (void *)0x43dcd0;               /* __fastcall */
static unsigned (THISCALL *const MP_DPIDToSlot)(void *, DWORD) = (void *)0x4899b0;
static void (THISCALL *const DN_OnDestroyPlayer)(void *, DWORD *) = (void *)0x4457e0;

static DWORD g_last_nack, g_last_ping;
static long g_nacks;

/* poll, then NACK every peer we are stalled on, once per Timeout */
static void wait_dialog_poll(void)
{
    DWORD now = timeGetTime();
    DWORD interval = NET_TIMEOUT < 125 ? 125 : NET_TIMEOUT;
    if (now - g_last_nack >= interval) {
        unsigned slot;
        g_last_nack = now;
        if (DN_IsHost(NET_THIS)) {
            for (slot = 0; slot < (unsigned)dk_max_players; slot++) {
                if (DN_IsRemoteActiveSlot(NET_THIS, slot) && DN_SlotTimedOut(NET_THIS, slot, now)
                    && DN_SendResendNack(NET_THIS, slot) == 0) {
                    g_nacks++;
                }
            }
        } else if (DN_SendResendNack(NET_THIS, HOST_SLOT) == 0) {
            g_nacks++;
        }
        if (g_nacks <= 3 || g_nacks % 40 == 0) {
            dk_log("net: wait dialog, %ld NACKs sent so far", g_nacks);
        }
    }
}

/* host: every remote client has an input block queued, so DN_HostMergeAndBroadcast can run
   (its own condition, 0x43eb90) */
static int host_can_merge(void)
{
    unsigned slot;
    if (SLOT_WRITE(HOST_SLOT) == NET_SENDIDX) {
        return 0;
    }
    for (slot = 0; slot < (unsigned)dk_max_players; slot++) {
        if (slot != HOST_SLOT && DN_IsRemoteActiveSlot(NET_THIS, slot) && SLOT_WRITE(slot) == NET_SENDIDX) {
            return 0;
        }
    }
    return 1;
}

/* replaces DN_WaitDialogCallback (0x43e9c0); returning 1 closes the modal (and resets the retries) */
static char wait_dialog_callback(void)
{
    DN_NetPoll(1);
    wait_dialog_poll();
    if (DN_WaitDialogReady(NET_THIS)) {
        dk_log("net: wait dialog closed, connection back (%ld NACKs)", g_nacks);
        return 1;
    }
    if (DN_IsHost(NET_THIS) && host_can_merge()) {
        dk_log("net: wait dialog closed, all input in (%ld NACKs)", g_nacks);
        return 1;
    }
    return 0;
}

/* callback 0x43f0c0 of the keep-alive modal: poll, then ping every silent DragNet player once a second. */
static char keepalive_dialog_poll(int arg)
{
    char r = DN_NetPoll(arg);
    DWORD now = timeGetTime();
    if (now - g_last_ping >= 1000) {
        void *players = (void *)(uintptr_t)(NET + 0x68);
        int i, n = DN_PlayerCount(players);
        g_last_ping = now;
        for (i = 0; i < n; i++) {
            DWORD *p = DN_PlayerAt(players, i);
            if (p && DN_KeepAliveExpired(NET_THIS, p, now)) {
                DN_SendPing(NET_THIS, p[0], 0);
            }
        }
    }
    return r;
}

static void host_lost(const char *why)
{
    if (NET_HOSTQUIT) {
        return;
    }
    dk_log("net: %s (state %d), leaving as if the host had quit", why, NET_STATE);
    DN_OnHostQuit(NET_THIS);
}

/* game vtable +0x6c (DPSYS_DESTROYPLAYERORGROUP), msg[2] = DPID */
static void THISCALL on_destroy_player(void *self, DWORD *msg)
{
    DWORD dpid = msg[2];
    if (DN_IsHost(self)) {
        unsigned slot = MP_DPIDToSlot((void *)(uintptr_t)GAME_SETUP, dpid);
        if (slot < (unsigned)dk_max_players && DN_IsRemoteActiveSlot(self, slot)) {
            dk_log("net: DirectPlay removed player %08lx (slot %u, state %d), dropping the slot", dpid, slot, NET_STATE);
            DN_DropSlot(self, slot, 0);     /* removes the DragNet player too (vt+0x70) */
            return;
        }
    } else if (dpid == NET_HOSTDPID) {
        DN_OnDestroyPlayer(self, msg);
        host_lost("DirectPlay removed the host");
        return;
    }
    DN_OnDestroyPlayer(self, msg);
}

/* game vtable +0x80 (DPSYS_SESSIONLOST), originally a bare ret */
static void THISCALL on_session_lost(void *self)
{
    if (DN_IsHost(self)) {
        dk_log("net: DirectPlay reported SESSIONLOST on the host (ignored)");
        return;
    }
    host_lost("DirectPlay session lost");
}

static void patch_vtable(uint32_t addr, uint32_t expect, void *fn)
{
    uint32_t to = (uint32_t)(uintptr_t)fn;
    dk_patch(addr, &expect, &to, 4);
}

void dk_net_install(void)
{
    int retries = dk_ini_int("Net", "Retries", 40);
    if (retries > 0) {
        static const uint8_t old[] = { 0xc7, 0x41, 0x10, 0x0f, 0x00, 0x00, 0x00 };   /* DN_ConfigDefaults */
        uint8_t new_[7] = { 0xc7, 0x41, 0x10 };
        memcpy(new_ + 3, &retries, 4);
        if (dk_patch(0x440a73, old, new_, 7)) {
            dk_log("net: Retries default %d", retries);
        }
    }
    if (dk_ini_int("Net", "FixClientSkipThresh", 1)) {
        /* DN_LoadRegistryConfig stored ClientSkipThresh as (value != 0); store the value */
        static const uint8_t old[] = { 0x33, 0xd2, 0x85, 0xc0, 0x0f, 0x95, 0xc2 };
        static const uint8_t new_[] = { 0x89, 0xc2, 0x90, 0x90, 0x90, 0x90, 0x90 };
        dk_patch(0x440d05, old, new_, 7);
    }
    if (dk_ini_int("Net", "ResendWhileWaiting", 1)) {
        static const uint8_t lockstep[] = { 0x6a, 0x01, 0xe8, 0xb9, 0x0a, 0x00, 0x00 };  /* push 1; call DN_NetPoll */
        static const uint8_t keepalive[] = { 0xe8, 0xb9, 0x03, 0x00, 0x00 };
        if (dk_expect(0x43e9c0, lockstep, 7) && dk_expect(0x43f0c2, keepalive, 5)) {
            dk_jmp(0x43e9c0, wait_dialog_callback);
            dk_call(0x43f0c2, keepalive_dialog_poll);
            dk_log("net: wait dialogs keep resending");
        } else {
            dk_log("net: wait dialog callbacks differ, ResendWhileWaiting skipped");
        }
    }
    if (dk_ini_int("Net", "HandlePlayerLoss", 1)) {
        patch_vtable(0x4f126c, 0x4457e0, (void *)on_destroy_player);
        patch_vtable(0x4f1280, 0x417870, (void *)on_session_lost);
        dk_log("net: DirectPlay player/session loss handled");
        if (dk_ini_int("Net", "DirectPlayKeepAlive", 1)) {
            /* DN_HostCreateSession: DPSESSION_CLIENTSERVER | DIRECTPLAYPROTOCOL, + DPSESSION_KEEPALIVE
               so DirectPlay notices players that vanish (crash, cable) and reports DESTROYPLAYER */
            static const uint8_t old[] = { 0x68, 0x00, 0x30, 0x00, 0x00 };
            static const uint8_t new_[] = { 0x68, 0x40, 0x30, 0x00, 0x00 };
            if (dk_patch(0x43fd72, old, new_, 5)) {
                dk_log("net: hosted sessions use DirectPlay keep-alive");
            }
        }
    }
}
