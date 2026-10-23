/* Saves and preferences in the user's Saved Games folder instead of the install folder (docs/saves.md).
 *
 * Folder: <Saved Games>\Beam Software\DethKarz (SHGetKnownFolderPath), [Saves] Folder= overrides it. With
 * the test harness (DK_UNIT) it is <exe dir>\saves_unit<N> instead, never the real Saved Games.
 *
 * Files: IAT hook on CreateFileA to redirect the files the
 * game writes (all relative to the install folder):
 *     data\dk.db              -> dk.db            settings, race records, championship state
 *     data\savegame<N>.dks    -> savegame<N>.dks  saved games
 *     data\ghosts\*           -> ghosts\*         best-lap ghosts
 *     data\replays\*          -> replays\*        replays (the shipped attract-mode replays stay readable
 *                                                 from the install)
 * dk.db and the savegames are copied over from the install when the new folder lacks them (migration).
 * Reading a ghost/replay missing from the new folder falls back to the install; writing one that exists
 * only in the install copies it first when the open does not truncate it.
 *
 * The game's registry settings are not handled here: the renderer choice and DragNet's tunables are in the patch's
 * own settings (opengl.c, net.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlobj.h>
#include "dkpatch.h"

/* FOLDERID_SavedGames {4C5C32FF-BB9D-43B0-B5B4-2D72E54EAAA4}, defined here to avoid needing -luuid */
static const GUID DK_FOLDERID_SavedGames = {0x4C5C32FF, 0xBB9D, 0x43B0, {0xB5, 0xB4, 0x2D, 0x72, 0xE5, 0x4E, 0xAA, 0xA4}};
typedef HRESULT (WINAPI *SHGetKnownFolderPath_fn)(const GUID *, DWORD, HANDLE, PWSTR *);

static CRITICAL_SECTION g_cs;
static int g_state;                 /* 0 = not initialised, 1 = active, -1 = failed (everything passes through) */
static char g_dir[MAX_PATH];        /* destination folder, with trailing backslash */
static char g_game[MAX_PATH];       /* install folder (exe dir), with trailing backslash */
static char g_cwd[MAX_PATH];        /* working directory at startup, with trailing backslash */

/* ---------------------------------------------------------------- folder */

static void add_slash(char *s)
{
    size_t n = strlen(s);
    if (n && s[n - 1] != '\\' && n + 1 < MAX_PATH) {
        s[n] = '\\';
        s[n + 1] = '\0';
    }
}

static void make_dirs(const char *path)   /* creates every missing folder in path (a folder, or a file path's parents) */
{
    char tmp[MAX_PATH], *p;
    lstrcpynA(tmp, path, MAX_PATH);
    for (p = tmp + 3; *p; p++) {          /* skip "C:\" */
        if (*p == '\\') {
            *p = '\0';
            CreateDirectoryA(tmp, NULL);
            *p = '\\';
        }
    }
}

static int saved_games_folder(char *out)
{
    SHGetKnownFolderPath_fn fn;
    HMODULE shell = LoadLibraryA("shell32.dll");
    PWSTR w = NULL;
    int ok = 0;
    fn = shell ? (SHGetKnownFolderPath_fn)(void (*)(void))GetProcAddress(shell, "SHGetKnownFolderPath") : NULL;
    if (fn && fn(&DK_FOLDERID_SavedGames, KF_FLAG_CREATE, NULL, &w) == S_OK && w) {
        ok = WideCharToMultiByte(CP_ACP, 0, w, -1, out, MAX_PATH - 40, NULL, NULL) > 0;
        CoTaskMemFree(w);
    }
    if (ok) {
        add_slash(out);
        strcat(out, "Beam Software\\DethKarz");
    }
    return ok;
}

static void ensure_init(void)
{
    char folder[MAX_PATH], ovr[MAX_PATH];
    if (g_state) {
        return;
    }
    EnterCriticalSection(&g_cs);
    if (!g_state) {
        char *slash;
        GetModuleFileNameA(NULL, g_game, MAX_PATH);
        slash = strrchr(g_game, '\\');
        if (slash) {
            slash[1] = '\0';
        }
        GetCurrentDirectoryA(MAX_PATH, g_cwd);
        add_slash(g_cwd);

        ovr[0] = '\0';
        dk_ini_str("Saves", "Folder", ovr, sizeof ovr);
        if (dk_unit) {
            sprintf(folder, "%ssaves_unit%d", g_game, dk_unit);
        } else if (ovr[0]) {
            char exp[MAX_PATH];
            DWORD n = ExpandEnvironmentStringsA(ovr, exp, sizeof exp);
            if (n && n <= sizeof exp) {
                GetFullPathNameA(exp, MAX_PATH - 40, folder, NULL);
            } else {
                folder[0] = '\0';
            }
        } else if (!saved_games_folder(folder)) {
            folder[0] = '\0';
        }
        if (!folder[0]) {
            dk_log("saves: no usable save folder, files stay where they are");
            g_state = -1;
        } else {
            lstrcpynA(g_dir, folder, MAX_PATH - 2);
            add_slash(g_dir);
            make_dirs(g_dir);
            if (GetFileAttributesA(g_dir) == INVALID_FILE_ATTRIBUTES) {
                dk_log("saves: cannot create %s (%lu), files stay where they are", g_dir, GetLastError());
                g_state = -1;
            } else {
                char pat[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
                WIN32_FIND_DATAA fd;
                HANDLE h;
                static const char *const names[] = {"dk.db"};
                size_t i;
                dk_log("saves: folder %s", g_dir);
                lstrcpynA(to, g_dir, MAX_PATH - 10);
                strcat(to, "ghosts\\");
                CreateDirectoryA(to, NULL);
                lstrcpynA(to, g_dir, MAX_PATH - 10);
                strcat(to, "replays\\");
                CreateDirectoryA(to, NULL);
                /* migration: dk.db and savegames, once */
                for (i = 0; i < sizeof names / sizeof names[0]; i++) {
                    snprintf(from, sizeof from, "%sData\\%s", g_game, names[i]);
                    snprintf(to, sizeof to, "%s%s", g_dir, names[i]);
                    if (GetFileAttributesA(to) == INVALID_FILE_ATTRIBUTES && CopyFileA(from, to, TRUE)) {
                        dk_log("saves: migrated %s", names[i]);
                    }
                }
                snprintf(pat, sizeof pat, "%sData\\savegame*.dks", g_game);
                h = FindFirstFileA(pat, &fd);
                if (h != INVALID_HANDLE_VALUE) {
                    do {
                        snprintf(from, sizeof from, "%sData\\%s", g_game, fd.cFileName);
                        snprintf(to, sizeof to, "%s%s", g_dir, fd.cFileName);
                        if (GetFileAttributesA(to) == INVALID_FILE_ATTRIBUTES && CopyFileA(from, to, TRUE)) {
                            dk_log("saves: migrated %s", fd.cFileName);
                        }
                    } while (FindNextFileA(h, &fd));
                    FindClose(h);
                }
                g_state = 1;
            }
        }
    }
    LeaveCriticalSection(&g_cs);
}

/* ---------------------------------------------------------------- path mapping */

static int has_prefix_i(const char *s, const char *prefix)
{
    return _strnicmp(s, prefix, strlen(prefix)) == 0;
}

/* Maps a game path to its place in the save folder. Returns 0 if the path is not one of ours; else 1 and fills
 * dest (new place) and orig (the install copy). */
static int map_path(const char *path, char *dest, char *orig)
{
    char full[MAX_PATH];
    const char *rel;
    const char *name;
    if (!path || !path[0] || (path[0] == '\\' && path[1] == '\\') || path[0] == '@') {
        return 0;
    }
    if (!GetFullPathNameA(path, MAX_PATH, full, NULL)) {
        return 0;
    }
    if (has_prefix_i(full, g_game)) {
        rel = full + strlen(g_game);
    } else if (has_prefix_i(full, g_cwd)) {
        rel = full + strlen(g_cwd);
    } else {
        return 0;
    }
    if (!has_prefix_i(rel, "data\\")) {
        return 0;
    }
    name = rel + 5;
    if (!_stricmp(name, "dk.db") || (has_prefix_i(name, "savegame") && strchr(name, '\\') == NULL)
        || has_prefix_i(name, "ghosts\\") || has_prefix_i(name, "replays\\")) {
        snprintf(dest, MAX_PATH, "%s%s", g_dir, name);
        snprintf(orig, MAX_PATH, "%s%s", g_game, rel);
        return 1;
    }
    return 0;
}

static HANDLE (WINAPI *real_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

static HANDLE WINAPI hk_CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                    DWORD flags, HANDLE tmpl)
{
    char dest[MAX_PATH], orig[MAX_PATH];
    ensure_init();
    if (g_state == 1 && map_path(path, dest, orig)) {
        int write = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE)) != 0
                    || disp == CREATE_ALWAYS || disp == CREATE_NEW || disp == TRUNCATE_EXISTING;
        int have = GetFileAttributesA(dest) != INVALID_FILE_ATTRIBUTES;
        if (!have && write) {
            make_dirs(dest);
            if (disp != CREATE_ALWAYS && disp != CREATE_NEW && disp != TRUNCATE_EXISTING
                && CopyFileA(orig, dest, TRUE)) {
                dk_log("saves: copied %s up", orig + strlen(g_game));
            }
            have = 1;
        }
        dk_log("saves: %s %s -> %s", write ? "write" : "read", path, have ? dest : "(install)");
        if (have) {
            path = dest;
        } else {
            path = orig;
        }
    } else if (g_state == 1 && path && (access & (GENERIC_WRITE | FILE_WRITE_DATA)) && path[0] != '\\' && path[0] != '@') {
        dk_log("saves: write open of %s was not redirected", path);
    }
    return real_CreateFileA(path, access, share, sa, disp, flags, tmpl);
}

/* ---------------------------------------------------------------- install */

void dk_saves_install(void)
{
    if (!dk_ini_int("Saves", "Redirect", 1)) {
        dk_log("saves: Redirect=0, saves and settings stay in the install folder");
        return;
    }
    InitializeCriticalSection(&g_cs);
    real_CreateFileA = dk_hook_import("KERNEL32.dll", "CreateFileA", hk_CreateFileA);
    if (!real_CreateFileA) {
        dk_log("saves: CreateFileA import is missing, redirection is off");
        g_state = -1;
        return;
    }
    dk_log("saves: hooks installed");
}
