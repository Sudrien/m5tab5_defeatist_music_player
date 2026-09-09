/*
 * clocktest -- the clock only moves forward.
 *
 * settings.c's floor is four lines of int64 arithmetic and a comparison,
 * which is the kind of thing that is either obviously right or wrong in
 * a way nobody notices until a device refuses to sync for a year. It
 * needs no card, no network and no clock, so it is checked here.
 *
 * Two things this covers that the board cannot show cheaply: the
 * behaviour either side of 2038-01-19, since this project's time_t may
 * be 32-bit and the stored fields deliberately are not; and the
 * build-time parse, whose input format is fixed by the C standard and so
 * can be exercised exactly.
 *
 * The floor logic is duplicated here rather than linked. settings.c
 * cannot be compiled on the host -- it pulls in storage_io, cJSON and
 * FreeRTOS -- and the alternative to a duplicate is no test at all. The
 * duplicate is the whole function, kept adjacent to the original by
 * this comment in both files; if they drift, this test is worthless,
 * which is a real cost and is why it is stated rather than buried.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- the copy under test ------------------------------------------- */

static int64_t s_epoch;
static int64_t s_boot_us;

static bool note_time(int64_t epoch, int64_t boot_us)
{
    const int64_t elapsed_s = (boot_us - s_boot_us) / 1000000;
    const int64_t floor_s   = s_epoch + elapsed_s;

    if (epoch < floor_s) return false;

    s_epoch   = epoch;
    s_boot_us = boot_us;
    return true;
}

static void reset_clock(void) { s_epoch = 0; s_boot_us = 0; }

/* settings_now(): the floor as of a given monotonic reading. What gets
 * written to the file on every save. */
static int64_t now_at(int64_t boot_us)
{
    return s_epoch + (boot_us - s_boot_us) / 1000000;
}

/* parse_build_time(), same source, same fixed input format. */
static int64_t parse_build_time(const char *date, const char *time_)
{
    static const char *const mon3[12] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec",
    };
    if (!date || !time_) return 0;

    char mon[4] = {0};
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    if (sscanf(date, "%3s %2d %4d", mon, &day, &year) != 3) return 0;
    if (sscanf(time_, "%2d:%2d:%2d", &hh, &mm, &ss) != 3) return 0;

    int mi = -1;
    for (int i = 0; i < 12; i++) {
        if (strncmp(mon, mon3[i], 3) == 0) { mi = i; break; }
    }
    if (mi < 0 || day < 1 || day > 31 || year < 2020 || year > 2200 ||
        hh > 23 || mm > 59 || ss > 60) {
        return 0;
    }

    /* days_from_civil, same as settings.c. See there for why not
     * timegm(). On the host timegm() does exist, which lets t_civil()
     * below check this arithmetic against it -- the one place the
     * duplication pays for itself. */
    int64_t y = year;
    const int64_t m = mi + 1;
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + doe - 719468;

    return days * 86400 + (int64_t)hh * 3600 + (int64_t)mm * 60 + ss;
}

/* ---- harness ------------------------------------------------------- */

static int g_checks, g_fails;

static void ck(int cond, const char *what)
{
    g_checks++;
    if (!cond) { g_fails++; printf("  FAIL: %s\n", what); }
}

#define SEC     ((int64_t)1000000)          /* one second of boot_us */
#define Y2026   ((int64_t)1772000000)       /* ~Feb 2026 */
#define Y2038   ((int64_t)2147483647)       /* INT32_MAX: the rollover */

/* ---- cases --------------------------------------------------------- */

static void t_seed(void)
{
    printf("the build time is the first floor\n");
    reset_clock();

    ck(note_time(Y2026, 0), "seed accepted from nothing");
    ck(s_epoch == Y2026, "seed stored");

    /* 1970 is what an unset clock offers, and it is exactly what the
     * floor exists to refuse. */
    ck(!note_time(0, 1 * SEC), "1970 refused");
    ck(s_epoch == Y2026, "floor stands after refusal");
}

static void t_forward(void)
{
    printf("forward always, backward never\n");
    reset_clock();
    note_time(Y2026, 0);

    ck(note_time(Y2026 + 3600, 10 * SEC), "an hour later, accepted");
    ck(note_time(Y2026 + 7200, 20 * SEC), "later still");

    /* One second before the floor is as refused as twenty years before
     * it: there is no tolerance band to fall inside. */
    ck(!note_time(s_epoch - 1, 30 * SEC), "one second back refused");
    ck(!note_time(Y2026 - 86400 * 365, 40 * SEC), "a year back refused");

    /* The revoked-certificate jump, which is the whole point. */
    ck(!note_time(Y2026 - 86400 * 700, 50 * SEC), "a two-year jump back refused");
}

static void t_monotonic_carry(void)
{
    printf("the floor rises with monotonic time, unasked\n");
    reset_clock();
    note_time(Y2026, 0);

    /* After 100 s of uptime the floor is 100 s higher, so a reply that
     * merely repeats the old epoch is now behind it. This is what stops
     * a replayed capture of a genuine earlier reply. */
    ck(!note_time(Y2026, 100 * SEC), "replayed old reply refused");
    ck(note_time(Y2026 + 100, 100 * SEC), "the same instant, accepted");
    ck(note_time(Y2026 + 500, 200 * SEC), "ahead of the floor, accepted");

    /* Monotonic time cannot run backwards, but the arithmetic should not
     * do anything wild if a caller passes a stale reading. */
    ck(note_time(Y2026 + 500, 150 * SEC), "stale boot_us does not crash");
}

static void t_2038(void)
{
    printf("across 2038, where a 32-bit time_t would not go\n");
    reset_clock();

    /* Each claim must also clear the floor, which rises by the elapsed
     * uptime between calls -- so the epochs step forward at least as
     * fast as boot_us does. Getting this wrong the other way is what
     * this test caught on its first run. */
    ck(note_time(Y2038 - 10, 0), "just before the rollover");
    ck(note_time(Y2038, 10 * SEC), "INT32_MAX itself");
    ck(note_time(Y2038 + 20, 20 * SEC), "past it, clearing the floor");
    ck(s_epoch == Y2038 + 20, "stored past INT32_MAX intact");

    /* The value a 32-bit time_t would wrap to. It is far below the
     * floor, so the floor refuses it -- which is the useful property:
     * a truncating layer elsewhere cannot poison the stored belief. */
    ck(!note_time(-(int64_t)2147483648LL, 30 * SEC), "the wrapped value refused");
    ck(s_epoch == Y2038 + 20, "floor survives the wrapped value");

    /* Well past it, where the sleep timer and file dates still have to
     * work. */
    ck(note_time(Y2038 + 86400 * 365 * 20, 40 * SEC), "twenty years past 2038");
}

static void t_parse(void)
{
    printf("__DATE__ and __TIME__ as the standard fixes them\n");

    /* The exact strings esp_app_desc_t carried on the board. */
    const int64_t t = parse_build_time("Sep  9 2026", "10:38:28");
    ck(t > 0, "the real build stamp parses");

    struct tm g;
    const time_t tt = (time_t)t;
    gmtime_r(&tt, &g);
    ck(g.tm_year == 126, "year 2026");
    ck(g.tm_mon == 8, "September");
    ck(g.tm_mday == 9, "the 9th");
    ck(g.tm_hour == 10 && g.tm_min == 38 && g.tm_sec == 28, "the time");

    /* A day below 10 is space-padded by __DATE__; a two-digit day is
     * not. Both spellings occur and both must parse. */
    ck(parse_build_time("Dec 25 2026", "00:00:00") > 0, "two-digit day");
    ck(parse_build_time("Jan  1 2027", "23:59:59") > 0, "space-padded day");

    /* Ordering must survive the parse, or the seed is worse than none. */
    ck(parse_build_time("Jan  1 2027", "00:00:00") >
       parse_build_time("Dec 25 2026", "00:00:00"), "across a year end");
    ck(parse_build_time("Sep  9 2026", "10:38:29") >
       parse_build_time("Sep  9 2026", "10:38:28"), "one second apart");

    /* Rubbish returns 0, which settings_init() reads as "no seed" rather
     * than as 1970 -- the one value that would defeat the floor. */
    ck(parse_build_time(NULL, "10:38:28") == 0, "null date");
    ck(parse_build_time("Sep  9 2026", NULL) == 0, "null time");
    ck(parse_build_time("", "") == 0, "empty");
    ck(parse_build_time("Xyz  9 2026", "10:38:28") == 0, "bad month");
    ck(parse_build_time("Sep  9 1999", "10:38:28") == 0, "before this project");
    ck(parse_build_time("Sep  9 2026", "99:99:99") == 0, "impossible clock");
    ck(parse_build_time("Sep 32 2026", "10:38:28") == 0, "no 32nd");

    /* A leap second is 60 and is allowed through rather than refused;
     * timegm normalises it. */
    ck(parse_build_time("Dec 31 2026", "23:59:60") > 0, "leap second tolerated");
}

static void t_seed_then_card(void)
{
    printf("a card record raises the floor, never lowers it\n");
    reset_clock();

    const int64_t built = parse_build_time("Sep  9 2026", "10:38:28");
    ck(built > 0, "have a build time");
    ck(note_time(built, 0), "seeded from the build");

    /* A card written by a later run of a later firmware. */
    ck(note_time(built + 86400 * 30, 1 * SEC), "a newer card raises it");

    /* A card from an older device, or an edited one. Both are the same
     * case and both are refused. */
    ck(!note_time(built - 86400 * 30, 2 * SEC), "an older card ignored");
    ck(s_epoch == built + 86400 * 30, "the newer value still stands");
}

/*
 * The property the updated-at adds: uptime survives reboots even with no
 * network ever. Each session writes its floor; each boot seeds from the
 * later of the build stamp and that written value.
 */
static void t_updated_at(void)
{
    printf("uptime carries across reboots without a network\n");

    const int64_t built = parse_build_time("Sep  9 2026", "10:38:28");

    /* Session one: boot, seed from the build, run for an hour, save. */
    reset_clock();
    note_time(built, 0);
    const int64_t saved1 = now_at(3600 * SEC);
    ck(saved1 == built + 3600, "an hour of uptime is in the saved value");

    /* Session two: fresh boot. Monotonic time restarts at 0, the build
     * stamp is the same firmware, and the card holds session one's
     * value. The card must win. */
    reset_clock();
    note_time(built, 0);
    ck(note_time(saved1, 0), "the card's later value is taken");
    ck(s_epoch == saved1, "floor is now an hour past the build");

    const int64_t saved2 = now_at(1800 * SEC);
    ck(saved2 == saved1 + 1800, "session two adds its own uptime");
    ck(saved2 > saved1, "strictly forward across the reboot");

    /* Session three, with the clock never having been set by anything.
     * Three sessions of uptime are now folded in. */
    reset_clock();
    note_time(built, 0);
    note_time(saved2, 0);
    ck(now_at(0) == built + 3600 + 1800, "all of it accumulated");

    /* And a card that is behind still loses, however it got that way. */
    ck(!note_time(built, 1 * SEC), "the bare build stamp no longer raises it");
}

/*
 * What the file records must be something this player would itself
 * accept on the next boot. If settings_now() and the floor ever
 * disagreed, a record would be refused by the device that wrote it.
 */
static void t_written_is_acceptable(void)
{
    printf("what is written is what would be accepted back\n");
    reset_clock();
    note_time(parse_build_time("Sep  9 2026", "10:38:28"), 0);

    for (int64_t up = 0; up <= 10000; up += 997) {
        const int64_t written = now_at(up * SEC);
        int64_t ke = s_epoch, kb = s_boot_us;   /* a reboot */
        s_epoch = 0; s_boot_us = 0;
        ck(note_time(written, 0), "a written value is accepted next boot");
        s_epoch = ke; s_boot_us = kb;
    }
}

/* fmt_epoch(), same source as settings.c. */
static void fmt_epoch(int64_t t, char *out, size_t out_len)
{
    int64_t days = t / 86400;
    int64_t rem  = t % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }

    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp  = (5 * doy + 2) / 153;
    const int64_t d   = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m   = mp + (mp < 10 ? 3 : -9);
    const int64_t y   = yoe + era * 400 + (m <= 2);

    snprintf(out, out_len, "%04lld-%02lld-%02lld %02lld:%02lld:%02lldZ",
             (long long)y, (long long)m, (long long)d,
             (long long)(rem / 3600), (long long)(rem % 3600 / 60),
             (long long)(rem % 60));
}

/*
 * fmt_epoch against gmtime_r, and against the parser it inverts.
 *
 * The log is the only place a person sees any of this, so a formatter
 * that is quietly wrong produces a log that is believed and misleads --
 * which is worse than no log, and is why it is checked rather than
 * eyeballed once.
 */
static void t_fmt(void)
{
    printf("fmt_epoch inverts the parse and matches gmtime_r\n");

    char buf[32];

    /* The build stamp from the board, formatted back. */
    fmt_epoch(parse_build_time("Sep  9 2026", "15:20:47"), buf, sizeof(buf));
    ck(strcmp(buf, "2026-09-09 15:20:47Z") == 0, "the real build stamp");

    /* Round trip across the same span the parser was swept over, on an
     * offset that lands on odd times of day. */
    int mismatches = 0, tested = 0;
    for (int64_t t = 1600000000; t < 7000000000LL; t += 999983) {
        fmt_epoch(t, buf, sizeof(buf));

        const time_t tt = (time_t)t;
        struct tm g;
        char ref[32];
        gmtime_r(&tt, &g);
        snprintf(ref, sizeof(ref), "%04d-%02d-%02d %02d:%02d:%02dZ",
                 g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
                 g.tm_hour, g.tm_min, g.tm_sec);

        tested++;
        if (strcmp(buf, ref) != 0) {
            if (mismatches < 3) printf("  got %s want %s\n", buf, ref);
            mismatches++;
        }
    }
    ck(tested > 5000, "swept a real span");
    ck(mismatches == 0, "agrees with gmtime_r everywhere, including past 2038");

    /* Zero is 1970 and should print as such rather than as anything
     * clever -- it is the value the floor refuses and wants to be
     * recognisable in a log. */
    fmt_epoch(0, buf, sizeof(buf));
    ck(strcmp(buf, "1970-01-01 00:00:00Z") == 0, "the epoch itself");
}

/*
 * The civil-date arithmetic against timegm(), which the host has and the
 * target does not. Every day across a span that includes the leap-year
 * rules that actually differ: 2100 is not a leap year, 2000 was.
 */
static void t_civil(void)
{
    printf("days_from_civil agrees with timegm across 180 years\n");

    static const char *const mon3[12] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec",
    };

    int mismatches = 0, tested = 0;
    for (int year = 2020; year <= 2199; year++) {
        for (int mon = 0; mon < 12; mon++) {
            for (int day = 1; day <= 31; day++) {
                struct tm probe = {
                    .tm_year = year - 1900, .tm_mon = mon, .tm_mday = day,
                    .tm_hour = 12, .tm_min = 34, .tm_sec = 56,
                };
                struct tm norm = probe;
                const time_t ref = timegm(&norm);
                /* timegm normalises 31 Feb into March; skip those rather
                 * than compare against a different date. */
                if (norm.tm_mday != day || norm.tm_mon != mon) continue;

                char ds[32], ts[16];
                snprintf(ds, sizeof(ds), "%s %2d %4d", mon3[mon], day, year);
                snprintf(ts, sizeof(ts), "12:34:56");
                const int64_t got = parse_build_time(ds, ts);

                tested++;
                if (got != (int64_t)ref) mismatches++;
            }
        }
    }
    ck(tested > 60000, "swept a real span");
    ck(mismatches == 0, "no disagreement with timegm");
    if (mismatches) printf("  (%d of %d disagreed)\n", mismatches, tested);

    /* The two centuries that catch a naive leap rule. */
    ck(parse_build_time("Mar  1 2100", "00:00:00") -
       parse_build_time("Feb 28 2100", "00:00:00") == 86400,
       "2100 is not a leap year");
    ck(parse_build_time("Mar  1 2024", "00:00:00") -
       parse_build_time("Feb 28 2024", "00:00:00") == 2 * 86400,
       "2024 is");
}

int main(void)
{
    t_seed();
    t_forward();
    t_monotonic_carry();
    t_2038();
    t_parse();
    t_civil();
    t_fmt();
    t_seed_then_card();
    t_updated_at();
    t_written_is_acceptable();

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
