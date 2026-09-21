/*
 * cuesheettest.c -- main/cuesheet.h against the five sheets in
 * test_audio_files (1002), read from disk rather than pasted in, so the
 * bytes under test are the bytes on the card: the BOM, the CRLF, the
 * Windows-1252 accent and the missing final newline all arrive as the
 * files have them.
 *
 * Each block checks what that sheet's row in test_audio_files/README.md
 * says should happen. Those rows were written before this parser was,
 * which is the point of having written them first.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cuesheet.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

#define SEC(s)  ((uint32_t)(s) * CUE_FPS)

/* Static, as on the board: 30 KB is not a local anywhere. */
static cue_sheet_t cs;

/* The decoder's own test, near enough: an audio extension. */
static bool playable(const char *name)
{
    const char *d = strrchr(name, '.');
    return d && (strcasecmp(d, ".flac") == 0 || strcasecmp(d, ".wav") == 0 ||
                 strcasecmp(d, ".mp3") == 0);
}
static char buf[16384];

static size_t slurp(const char *name)
{
    char path[256];
    snprintf(path, sizeof path, "../test_audio_files/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("  FAIL cannot open %s\n", path);
        failures++;
        return 0;
    }
    const size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return n;
}

static int parse(const char *name)
{
    const size_t n = slurp(name);
    return cue_parse(buf, n, &cs);
}

int main(void)
{
    /* ---- 22: the ordinary case -------------------------------------- */
    CHECK(parse("22 cue-flac-image.cue") == 6, "22: %d tracks, not 6", cs.ntracks);
    CHECK(!cs.cp1252, "22: UTF-8 read as 1252");
    CHECK(strcmp(cs.title, "Six Marks") == 0,
          "22: album title '%s' -- the BOM left in front of REM?", cs.title);
    CHECK(strcmp(cs.performer, "Landmark Ensemble") == 0, "22: performer '%s'", cs.performer);
    CHECK(cs.nfiles == 1 && strcmp(cs.files[0], "22 cue-flac-image.flac") == 0,
          "22: file '%s' -- a CR on the end?", cs.files[0]);
    for (int i = 0; i < cs.ntracks; i++) {
        CHECK(cs.tracks[i].number == i + 1, "22: track %d numbered %d", i, cs.tracks[i].number);
        CHECK(cs.tracks[i].start == SEC(i * 10), "22: track %d at %u", i + 1, cs.tracks[i].start);
        CHECK(cs.tracks[i].file == 0, "22: track %d file %d", i + 1, cs.tracks[i].file);
    }
    CHECK(strcmp(cs.tracks[4].title, "F\xc3\xbcnf Signalt\xc3\xb6ne") == 0,
          "22: track 5 title '%s'", cs.tracks[4].title);
    CHECK(strcmp(cs.tracks[0].title, "One Beep") == 0, "22: track 1 title '%s'", cs.tracks[0].title);
    CHECK(cue_track_end(&cs, 0) == SEC(10), "22: track 1 does not end at 10");
    CHECK(cue_track_end(&cs, 5) == 0, "22: last track does not run to the end");
    CHECK(cs.dropped == 0, "22: dropped %d", cs.dropped);

    /* ---- 23: index lines that are not starts, and 1252 -------------- */
    CHECK(parse("23 cue-pregap-htoa.cue") == 4, "23: %d tracks, not 4", cs.ntracks);
    CHECK(cs.cp1252, "23: a 0xE9 byte read as UTF-8");
    CHECK(strcmp(cs.performer, "Caf\xc3\xa9 Tones") == 0,
          "23: performer '%s', not Caf\\u00e9 Tones", cs.performer);
    CHECK(strcmp(cs.tracks[0].performer, "Caf\xc3\xa9 Tones") == 0,
          "23: track 1 did not inherit the album performer");
    CHECK(cs.tracks[0].start == SEC(3), "23: track 1 at %u, not 3 s -- INDEX 00 taken as the start?",
          cs.tracks[0].start);
    CHECK(cs.tracks[0].pregap, "23: track 1 hidden track not noted");
    CHECK(cs.tracks[1].start == SEC(10), "23: track 2 at %u, not 10 s", cs.tracks[1].start);
    CHECK(cs.tracks[1].pregap, "23: track 2 pregap not noted");
    CHECK(cue_track_end(&cs, 0) == SEC(10),
          "23: track 1 ends at %u -- the pregap belongs to it", cue_track_end(&cs, 0));
    CHECK(cs.tracks[2].start == SEC(25) + 37, "23: track 3 at %u", cs.tracks[2].start);
    CHECK(cue_frames_to_cs(cs.tracks[2].start) == 2549,
          "23: 00:25:37 is %u cs, not 25.49 s", cue_frames_to_cs(cs.tracks[2].start));
    CHECK(cue_frames_to_samples(cs.tracks[2].start, 44100) == 25 * 44100 + 37 * 588,
          "23: track 3 not sample-exact at 44.1 kHz");
    CHECK(cs.tracks[3].start == SEC(40) - 1, "23: track 4 at %u, not one frame before 40",
          cs.tracks[3].start);
    CHECK(cs.tracks[3].number == 4, "23: INDEX 02 made a track");
    CHECK(cs.dropped == 0, "23: dropped %d", cs.dropped);

    /* ---- 24: names a file that is not there ------------------------- */
    CHECK(parse("24 cue-filename-mismatch.cue") == 3, "24: %d tracks", cs.ntracks);
    CHECK(strcmp(cs.files[0], "CDImage.wav") == 0,
          "24: file '%s' -- the parser is not the place to resolve it", cs.files[0]);
    CHECK(cs.tracks[1].start == SEC(20) && cs.tracks[2].start == SEC(40),
          "24: starts %u %u", cs.tracks[1].start, cs.tracks[2].start);

    /* ---- 25: one FILE per track, a pregap in the previous file ------ */
    CHECK(parse("25 cue-multifile.cue") == 3, "25: %d tracks", cs.ntracks);
    CHECK(cs.nfiles == 3, "25: %d files", cs.nfiles);
    CHECK(cs.tracks[0].file == 0 && cs.tracks[1].file == 1 && cs.tracks[2].file == 2,
          "25: files %d %d %d -- track 3 is in the file its INDEX 01 is under",
          cs.tracks[0].file, cs.tracks[1].file, cs.tracks[2].file);
    CHECK(cs.tracks[2].start == 0, "25: track 3 at %u in 25c", cs.tracks[2].start);
    CHECK(cs.tracks[2].pregap, "25: track 3's pregap in 25b not noted");
    CHECK(cue_track_end(&cs, 1) == 0,
          "25: track 2 ends at %u -- it runs to the end of 25b, pregap and all",
          cue_track_end(&cs, 1));
    CHECK(strcmp(cs.files[2], "25c cue-multifile.flac") == 0, "25: file 3 '%s'", cs.files[2]);

    /* ---- 26: broken on purpose -------------------------------------- */
    CHECK(parse("26 cue-malformed.cue") == 5,
          "26: %d tracks from the parser, not 5 (01 04 06 08 09)", cs.ntracks);
    CHECK(cs.dropped == 3, "26: parser dropped %d, not 3 (02 03 07)", cs.dropped);
    CHECK(strcmp(cs.title, "Unclosed quote") == 0, "26: unclosed title '%s'", cs.title);
    /* 08 starts at 70 s in a 60 s file; only the length can say so. And
     * it has to go before the order is checked, or 09 is lost to it. */
    CHECK(cue_finish(&cs, 0, SEC(60)) == 3, "26: %d tracks after finishing, not 3", cs.ntracks);
    CHECK(cs.dropped == 5, "26: dropped %d in all, not 5", cs.dropped);
    CHECK(cs.tracks[0].number == 1 && cs.tracks[1].number == 4 && cs.tracks[2].number == 9,
          "26: kept %d %d %d, not 1 4 9",
          cs.tracks[0].number, cs.tracks[1].number, cs.tracks[2].number);
    CHECK(cue_track_end(&cs, 0) == SEC(20) && cue_track_end(&cs, 1) == SEC(50) &&
          cue_track_end(&cs, 2) == 0, "26: spans are not 0-20, 20-50, 50-end");
    CHECK(strlen(cs.tracks[1].title) == CUE_TEXT_MAX - 1,
          "26: 300-char title cut to %zu", strlen(cs.tracks[1].title));
    for (int i = 0; i < cs.ntracks; i++) {
        const uint32_t end = cue_track_end(&cs, i);
        CHECK(end == 0 || end > cs.tracks[i].start,
              "26: track %d has zero or negative length", cs.tracks[i].number);
    }

    /* Unknown length still enforces order. */
    CHECK(parse("26 cue-malformed.cue") == 5, "26 again");
    cue_finish(&cs, 0, 0);
    CHECK(cs.ntracks == 3 && cs.tracks[2].number == 8,
          "26 with no length: kept %d tracks, last %d -- expected 01 04 08, and 09 behind 08",
          cs.ntracks, cs.tracks[cs.ntracks - 1].number);

    /* 22-25 lose nothing when finished at their real lengths. */
    CHECK(parse("22 cue-flac-image.cue") == 6 && cue_finish(&cs, 0, SEC(60)) == 6,
          "22 lost a track to cue_finish");
    CHECK(parse("25 cue-multifile.cue") == 3 && cue_finish(&cs, 0, SEC(20)) == 3 &&
          cue_finish(&cs, 1, SEC(20)) == 3 && cue_finish(&cs, 2, SEC(20)) == 3,
          "25 lost a track to cue_finish");

    /* ---- the edges, as literals ------------------------------------- */
    CHECK(cue_parse("", 0, &cs) == 0, "empty sheet had tracks");
    CHECK(cue_parse(NULL, 5, &cs) == 0, "NULL sheet had tracks");
    CHECK(cue_parse("\xef\xbb\xbf", 3, &cs) == 0, "BOM alone had tracks");

    /* A TRACK before any FILE has nowhere to be. */
    static const char nofile[] = "TRACK 01 AUDIO\nINDEX 01 00:00:00\n";
    CHECK(cue_parse(nofile, sizeof nofile - 1, &cs) == 0 && cs.dropped == 1,
          "a track with no FILE was kept");

    /* Lower case, tabs, CR alone as the line end (old Mac), minutes past
     * 99 in a long image. */
    static const char odd[] = "file \"a.flac\" wave\r\ttrack 1 audio\r"
                              "\t\tindex 01 00:00:00\r\ttrack 2 audio\r"
                              "\t\tindex 01 120:00:00";
    CHECK(cue_parse(odd, sizeof odd - 1, &cs) == 2, "odd spelling: %d tracks", cs.ntracks);
    CHECK(cs.tracks[1].start == SEC(7200), "120 minutes read as %u", cs.tracks[1].start);

    /* 60 seconds is not a second, and a fourth digit is not a number. */
    CHECK(cue_msf("00:60:00", "00:60:00" + 8) < 0, "00:60:00 accepted");
    CHECK(cue_msf("0000:00:00", "0000:00:00" + 10) < 0, "four-digit minutes accepted");
    CHECK(cue_msf("00:00:00 junk", "00:00:00 junk" + 13) < 0, "trailing junk accepted");

    /* A title cut must not split a character: 127 bytes of room, and
     * 'é' (2 bytes) straddling the edge. */
    char big[200], out[CUE_TEXT_MAX];
    memset(big, 'a', 126);
    memcpy(big + 126, "\xc3\xa9", 2);
    cue_text(out, sizeof out, big, 128, false);
    CHECK(strlen(out) == 126, "split a character at the cut: %zu bytes", strlen(out));
    CHECK(cue_is_utf8(out, strlen(out)), "the cut left invalid UTF-8");

    /* The 1252 punctuation that is not Latin-1. */
    cue_text(out, sizeof out, "\x93quoted\x94 \x80", 10, true);
    CHECK(strcmp(out, "\xe2\x80\x9cquoted\xe2\x80\x9d \xe2\x82\xac") == 0,
          "1252 quotes and euro: '%s'", out);

    /* Overlong and surrogate UTF-8 are not UTF-8. */
    CHECK(!cue_is_utf8("\xc0\xaf", 2), "overlong '/' accepted");
    CHECK(!cue_is_utf8("\xed\xa0\x80", 3), "surrogate accepted");

    /* ---- virtual paths ---------------------------------------------- */
    size_t sl = 0;
    CHECK(cue_vpath_split("/sdcard/A/Disc.cue#03", &sl) == 3 && sl == 18,
          "vpath: %zu", sl);
    CHECK(cue_vpath_split("/sdcard/A/Disc.CUE#99", NULL) == 99, "upper-case .CUE");
    CHECK(cue_vpath_split("/sdcard/A/Disc.cue#00", NULL) == 0, "track 0 accepted");
    CHECK(cue_vpath_split("/sdcard/A/Disc.cue#100", NULL) == 0, "track 100 accepted");
    CHECK(cue_vpath_split("/sdcard/A/Disc.cue#3x", NULL) == 0, "trailing junk accepted");
    CHECK(cue_vpath_split("/sdcard/A/Disc.cue#", NULL) == 0, "no number accepted");
    CHECK(cue_vpath_split("/sdcard/A/Track #3.flac", NULL) == 0,
          "a real file with a # in its name taken for a cue track");
    CHECK(cue_vpath_split("/sdcard/A/Disc.cue", NULL) == 0, "the sheet itself");
    CHECK(cue_is_sheet("x.cue") && cue_is_sheet("X.CUE") && !cue_is_sheet(".cue") &&
          !cue_is_sheet("x.cue.flac"), "cue_is_sheet");

    /* ---- resolving FILE lines against a directory ------------------- */
    static const char *const dir[] = {
        "22 cue-flac-image.cue", "22 cue-flac-image.flac",
        "24 cue-filename-mismatch.cue", "24 cue-filename-mismatch.flac",
        "25 cue-multifile.cue", "25a cue-multifile.flac",
        "25b cue-multifile.flac", "25c cue-multifile.flac",
        "CDImage.flac", "notes.txt",
    };
    const int nd = (int)(sizeof dir / sizeof dir[0]);
    CHECK(cue_resolve("22 cue-flac-image.flac", "22 cue-flac-image.cue", 1, dir, nd, playable) == 1,
          "exact name not found");
    CHECK(cue_resolve("22 CUE-FLAC-IMAGE.FLAC", "22 cue-flac-image.cue", 1, dir, nd, playable) == 1,
          "case-insensitive name not found");
    CHECK(cue_resolve("CDImage.wav", "x.cue", 3, dir, nd, playable) == 8,
          "compressed after the sheet: CDImage.wav should find CDImage.flac");
    CHECK(cue_resolve("Range.wav", "24 cue-filename-mismatch.cue", 1, dir, nd, playable) == 3,
          "24: the sheet's own stem not used");
    CHECK(cue_resolve("Range.wav", "25 cue-multifile.cue", 3, dir, nd, playable) == -1,
          "the sheet's stem used for a multi-file sheet");
    CHECK(cue_resolve("C:\\Rips\\25b cue-multifile.flac", "25 cue-multifile.cue", 3, dir, nd, playable) == 6,
          "a Windows directory in FILE not stripped");
    CHECK(cue_resolve("notes.wav", "z.cue", 1, dir, nd, playable) == -1,
          "resolved to something that does not play");
    CHECK(cue_resolve("nothing.flac", "22 cue-flac-image.cue", 2, dir, nd, playable) == -1,
          "resolved a name that is not there, on a two-file sheet");

    printf("%s: %d checks, %d failures\n",
           failures ? "FAILURES" : "all passed", checks, failures);
    return failures ? 1 : 0;
}
