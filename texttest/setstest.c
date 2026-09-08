/*
 * setstest.c -- which volume gets which field.
 *
 * The rule is two sentences and it was wrong in two different ways
 * before: settings written to one volume only (so the other lost them),
 * and one track shared by both (so returning to a volume resumed a file
 * that was not on it). Both directions are checked.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

enum { SD = 0, USB = 1, NVOL = 2 };

/* --- the model, mirroring settings.c --- */
static char  track[NVOL][64];
static bool  seen[NVOL];
static bool  loaded;
static int   volume;
static int   crossfade;
static bool  present[NVOL];
static int   last = NVOL;

/* what is actually on each card, before anything reads it */
static char  disk_track[NVOL][64];
static int   disk_volume[NVOL], disk_crossfade[NVOL];
static bool  disk_has[NVOL];

static void ensure_loaded(int id, bool may_adopt)
{
    const bool want_settings = may_adopt && !loaded;
    const bool want_track = !seen[id];
    if (!want_settings && !want_track) return;
    if (want_settings) loaded = true;
    seen[id] = true;
    if (!disk_has[id]) return;
    if (want_track) strcpy(track[id], disk_track[id]);
    if (want_settings) { volume = disk_volume[id]; crossfade = disk_crossfade[id]; }
}

static bool note_path(int id)
{
    if (!present[id]) return false;
    const bool first = !loaded;
    ensure_loaded(id, true);
    last = id;
    return first;
}

static void set_track(int id, const char *p) { strcpy(track[id], p); }

static void flush(void)
{
    for (int v = 0; v < NVOL; v++) {
        if (!present[v]) continue;
        ensure_loaded(v, false);
        disk_volume[v] = volume;
        disk_crossfade[v] = crossfade;
        strcpy(disk_track[v], track[v]);
        disk_has[v] = true;
    }
}

static void reboot(void)
{
    memset(track, 0, sizeof track); memset(seen, 0, sizeof seen);
    loaded = false; volume = 40; crossfade = 0; last = NVOL;
}

static int fails;
static void ck(const char *what, bool ok)
{ printf("%-58s %s\n", what, ok ? "ok" : "FAIL"); if (!ok) fails++; }

int main(void)
{
    memset(disk_has, 0, sizeof disk_has);
    present[SD] = true; present[USB] = true;
    reboot();

    /* Session 1: boot with both, play from USB, set a crossfade. */
    note_path(USB);
    set_track(USB, "/usb/a.mp3");
    crossfade = 12;
    flush();
    ck("both volumes get the crossfade", disk_crossfade[SD] == 12 &&
                                         disk_crossfade[USB] == 12);
    ck("only USB gets the USB track", strcmp(disk_track[USB], "/usb/a.mp3") == 0 &&
                                      disk_track[SD][0] == '\0');

    /* Session 2: reboot, SD is read first (it is present). The crossfade
     * must survive -- this is the bug that started all of it. */
    reboot();
    note_path(SD);
    ck("crossfade survives a boot that adopts the other volume",
       crossfade == 12);

    /* Play from SD, then flush. USB's track must not be replaced. */
    set_track(SD, "/sd/b.mp3");
    note_path(SD);
    flush();
    ck("SD now remembers its own track", strcmp(disk_track[SD], "/sd/b.mp3") == 0);
    ck("USB still remembers its own",   strcmp(disk_track[USB], "/usb/a.mp3") == 0);

    /* Session 3: reboot, ask each volume what it remembers. */
    reboot();
    note_path(SD);
    ck("SD resumes the SD track", strcmp(track[last], "/sd/b.mp3") == 0);
    note_path(USB);
    ck("USB resumes the USB track", strcmp(track[last], "/usb/a.mp3") == 0);

    /* A volume that appears later still gets the settings. */
    reboot();
    present[USB] = false;
    note_path(SD);
    volume = 77;
    flush();
    present[USB] = true;
    flush();
    ck("a volume inserted later receives the settings",
       disk_volume[USB] == 77);
    ck("  without clobbering its remembered track",
       strcmp(disk_track[USB], "/usb/a.mp3") == 0);

    /* No volume present: nothing is written, nothing is lost. */
    reboot();
    present[SD] = present[USB] = false;
    volume = 55;
    flush();
    ck("no volume present writes nothing", disk_volume[SD] == 77);
    present[SD] = true;
    flush();
    ck("  and the value survives to the next one", disk_volume[SD] == 55);

    printf("\n%s\n", fails ? "FAILURES" : "all passed");
    return fails != 0;
}
