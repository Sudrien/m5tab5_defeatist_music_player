/*
 * cardtime.h -- a clock floor read off the card, for the boot before NTP.
 *
 * The floor starts at the build timestamp, which is a true lower bound by
 * construction and can be months stale on a device that never reaches a
 * network. What is on the card is often much fresher: an album copied on
 * last week, a stations.m3u edited on a desktop yesterday. Those are real
 * wall-clock readings taken by a machine that knew the time, and they are
 * available before the radio is up.
 *
 * So: one directory read per volume at mount, the entries' mtimes through
 * the filter below, and the survivors offered to settings_note_ntp_time()
 * like any other claim. Nothing here sets a clock. It proposes, and the
 * floor in settings.c disposes.
 *
 * ONLY THE ROOT, AND ONLY ONE LEVEL
 *
 * Directory mtimes DO NOT BUBBLE. A directory's timestamp moves when an
 * entry is added to or removed from THAT directory, and not when a file
 * somewhere beneath it changes -- so adding a track to
 * /sd/Boa/Twilight/ moves Twilight's mtime and leaves Boa's and the
 * root's alone. This is therefore not a change detector and must never
 * be used as one; a card can be edited from top to bottom without a
 * single root entry moving.
 *
 * It does not need to be. For a floor, one plausible recent reading is
 * worth as much as ten thousand, and the root is where the cheapest ones
 * are: the player's own files (defeatist.dat, stations.m3u,
 * favorites.m3u, starred.m3u) plus whatever top-level folders the music
 * is in. That is one opendir and a handful of stats.
 *
 * WHOSE CLOCK WROTE IT
 *
 * The player's own files are stamped from the floor that this is trying
 * to raise, so they are circular and can never exceed what settings.c
 * already knows. They cost nothing to include and cannot help. The ones
 * that CAN help are the ones a computer touched: stations.m3u, which
 * exists to be edited in a text editor, and the album folders, which
 * were created by whatever copied the music on.
 *
 * That asymmetry is also the hazard. Foreign timestamps are foreign
 * input, the floor is a permanent one-way latch that accepts forward
 * jumps without limit, and a single file dated 2107 -- which FAT can
 * represent and bad tools do produce -- would put the clock there
 * forever and make every subsequent NTP reply fail the floor. Hence the
 * ceiling in cardtime_filter(), which is the most important line here.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A day of slack, for the same reason settings.c gives the build stamp a
 * day: FAT records local wall clock with no offset, and ESP-IDF's FAT
 * VFS hands it back through stat() already converted to a time_t as
 * though it were UTC. A card written in UTC+13 therefore reads thirteen
 * hours ahead of the truth.
 *
 * Which direction that error runs decides everything. A floor a day LOW
 * is harmless -- it is still a floor, and still far better than a build
 * stamp from six months ago. A floor thirteen hours HIGH refuses correct
 * NTP for thirteen hours. So the margin is subtracted, always, and it is
 * wider than any offset on Earth (UTC-12 to UTC+14).
 *
 * exFAT does carry a real UTC offset per entry, with a validity bit, and
 * on an exFAT volume this margin is unnecessary for entries that set it.
 * It is applied anyway: that byte is not exposed through POSIX stat(),
 * which is the only interface this file has, so reaching it would mean
 * going under the VFS to f_stat() and FILINFO. Worth doing one day;
 * noted here so the next reader knows the margin is a limitation of the
 * interface and not of the filesystem.
 */
#define CARDTIME_TZ_MARGIN_S    (86400)

/*
 * How far past the build stamp a card's timestamp may claim to be.
 *
 * THIS IS THE GUARD THAT MATTERS. Without it one corrupt dirent or one
 * file stamped 2107 by a camera with a dead battery raises the floor by
 * eighty years, permanently, and the device refuses every real NTP reply
 * for the rest of its life with no way back short of wiping settings.
 *
 * Ten years is the useful shape: far longer than any firmware will
 * plausibly go unflashed, far shorter than the decades a garbage
 * timestamp overshoots by. A card genuinely older than the firmware
 * fails the floor anyway and never reaches this test.
 */
#define CARDTIME_MAX_AHEAD_S    ((int64_t)10 * 365 * 86400 + 3 * 86400)

/*
 * One candidate, filtered. Returns the epoch to offer, or 0 to discard.
 *
 * `mtime` is what stat() reported. `ref` is the floor's CURRENT value,
 * not specifically the build stamp: by the time this runs a card's own
 * settings record may already have raised it, and using the live value
 * means a candidate is measured against the best time known rather than
 * against the oldest. Either way it is a true lower bound, which is all
 * the ceiling needs -- a ceiling of ref+10y refuses a 2107 timestamp
 * whether ref is the build stamp or a real NTP sync.
 *
 * The ceiling is tested against the RAW mtime rather than the margined
 * one, so that subtracting a day can never duck a poisoned value under
 * the bar.
 *
 * Header-only and free of any filesystem call, so texttest can compile
 * and exercise it directly -- the same bargain tailplan.h and favmatch.h
 * make. Every decision that could brick the clock is in these few lines
 * and is checked on the host.
 */
static inline int64_t cardtime_filter(int64_t mtime, int64_t ref)
{
    /* No reference means no ceiling, and no ceiling means no guard. A
     * build stamp that failed to parse is the one case where doing
     * nothing is strictly better than guessing. */
    if (ref <= 0) return 0;
    if (mtime <= 0) return 0;

    /* Absurdly far ahead: discard, loudly, at the caller. */
    if (mtime > ref + CARDTIME_MAX_AHEAD_S) return 0;

    const int64_t cand = mtime - CARDTIME_TZ_MARGIN_S;

    /* Not newer than what is already known. Not an error -- it is the
     * common case for a card untouched since before this build -- and it
     * cannot raise the floor, so it is dropped here rather than being
     * offered and refused. */
    if (cand <= ref) return 0;

    return cand;
}

/*
 * Scan one mounted volume's root and return the best candidate, or 0.
 *
 * One opendir, one stat per entry, no descent. Bounded by
 * CARDTIME_SCAN_MAX so that a root with thousands of loose files cannot
 * stall a mount: the entries are unordered, but for a floor any
 * sufficiently large sample is as good as all of them.
 */
#define CARDTIME_SCAN_MAX       (256)

int64_t cardtime_root_candidate(const char *mount, int64_t ref);

/*
 * Scan every mounted volume and offer what it finds to the floor.
 *
 * Call it when storage_generation() moves -- a volume that has just
 * appeared is exactly a card whose root has not been read yet -- and
 * before NTP, which needs a network that may never arrive.
 */
void cardtime_note_volumes(void);

#ifdef __cplusplus
}
#endif
