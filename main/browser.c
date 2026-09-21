/*
 * browser.c -- the file chooser.
 *
 * SPDX-License-Identifier: MIT
 */

#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "browser.h"
#include "cuedir.h"
#include "favorites.h"
#include "radiobrowser.h"
#include "stations.h"
#include "decoder.h"
#include "gfx.h"
#include "storage.h"

static const char *TAG = "tab5_browser";

#define C_BG        RGB(0x0C, 0x0C, 0x0C)
#define C_ROW       RGB(0x1A, 0x1A, 0x1A)
#define C_ROW_ALT   RGB(0x14, 0x14, 0x14)
#define C_TEXT      RGB(0xEE, 0xEE, 0xEE)
#define C_DIM       RGB(0x77, 0x77, 0x77)
#define C_DISABLED  RGB(0x44, 0x44, 0x44)
#define C_ACCENT    RGB(0xD1, 0x3B, 0x2C)
/*
 * Starred. Gold rather than a second use of C_ACCENT, which already
 * means "this is the one playing" -- two states on the same row need
 * two colours or the row says one thing and means either.
 */
#define C_STAR      RGB(0xE8, 0xB3, 0x2C)
#define C_TAB_ON    RGB(0x22, 0x22, 0x22)
#define C_TAB_OFF   RGB(0x10, 0x10, 0x10)
#define C_BTN       RGB(0x26, 0x26, 0x26)
#define C_RULE      RGB(0x33, 0x33, 0x33)
/* Between C_RULE and C_DIM. The prefix note has to be readable when
 * looked for and easy to look past when not, which neither of the
 * existing greys manages -- one disappears, the other competes with the
 * path above it. */
#define C_FAINT     RGB(0x55, 0x55, 0x55)

/* Layout. Sized for the same 294 PPI the transport bar is: a 88 px row is
 * about 7.5 mm, which is a comfortable thumb target, and the 27 px text in
 * it is the same height as the artist line on the player. */
#define TAB_H       (96)
#define PATH_H      (60)
#define LIST_TOP    (TAB_H + PATH_H)
#define FOOT_H      (120)
#define ROW_H       (88)

/*
 * The scrollbar: how wide it is drawn, and how much wider it is to hit.
 *
 * The two differ on purpose. A 16 px bar is the right weight next to
 * 64 px rows; a 16 px target is not, at 294 PPI with a fingertip. The
 * grab zone is the outer 72 px of the list, which is dead space in every
 * row anyway -- filenames are drawn from the left and the ones long
 * enough to reach the right edge are already being clipped there.
 */
#define SCROLL_W        (16)

/*
 * The star's column, at the right of a station row and clear of the
 * scrollbar.
 *
 * STAR_W is the whole tappable strip, not the glyph: the glyph is
 * STAR_R across and a 72 px strip is 6 mm at 294 PPI, which is what a
 * thumb needs for a target that sits beside a much larger one. The row
 * tap still owns everything to the left of it, so the common action --
 * play this station -- keeps the whole width it had minus this strip,
 * and the name's draw width shrinks by the same amount so a long name
 * cannot run under the star.
 */
#define STAR_W          (72)
#define STAR_R          (18)
#define SCROLL_HIT_W    (72)
#define NAME_SCALE  (3)
#define LABEL_SCALE (2)

/* Entries held for the open directory. Beyond this the listing is
 * truncated, for the same reason PLAYLIST_MAX exists: this is a strdup
 * per name on a touch event. */
#define MAX_ENTRIES (512)

typedef struct {
    char *name;
    /*
     * What to draw instead of the name, or NULL. Set for cue tracks
     * only, whose name is "Album.cue#03" -- the thing that is opened and
     * compared, and nothing anyone wants to read. The prefix elision
     * does not apply to a label; it is already the part that differs.
     */
    char *label;
    bool is_dir;
    /*
     * Starred, resolved when the row was built and not while drawing.
     *
     * favorites_contains() is a scan of up to STATIONLIST_MAX entries.
     * Once per row per list load is 64x64 comparisons; once per row per
     * FRAME is that at the panel's refresh rate, for an answer that
     * cannot change without something calling browser_stations_reloaded()
     * anyway. The URL is not kept -- only the answer -- so this stays
     * true to BROWSER_PLAY_STREAM's rule that stations.c is the only
     * thing that knows what station i is.
     */
    bool fav;
} entry_t;

static bool s_open;
static bool s_dirty;
static bool s_was_down;

/*
 * Which tab is up.
 *
 * A third tab rather than an entry in the file list, and the reason is
 * that a station is not on a volume. An entry would have to live in
 * some directory, which would make it appear and disappear with the
 * card and put it in a sort order next to filenames. The tabs already
 * mean "which source of things to play", which is exactly the
 * distinction.
 *
 * BROWSER_TAB_SD and _USB match storage_id_t numerically so the two
 * that are volumes can still be passed to storage_present() and
 * storage_mount_path() without a mapping table.
 */
typedef enum {
    BROWSER_TAB_SD = STORAGE_SD,
    BROWSER_TAB_USB = STORAGE_USB,
    BROWSER_TAB_RADIO,
    BROWSER_TAB_COUNT
} browser_tab_t;

static browser_tab_t s_tab = BROWSER_TAB_SD;

/*
 * The station list, loaded into the same rows the files use.
 *
 * Copied in rather than drawn from stations.c directly, so that the
 * scrollbar, the row striping, the paging and the clipping all work
 * unchanged -- this tab adds a source of names, not a second list
 * widget. is_dir is false for every one of them.
 *
 * NOTE: the chooser does NOT call stations_load(). That opens a file and
 * ui_task is the one task that must not block on the card; the player
 * loads the list from its idle branch. So this tab shows the list as
 * last read, and a stations.m3u edited with the card still in will not
 * appear until something reloads it.
 */
static bool s_radio;
/*
 * THE RADIO TAB HAS TWO LEVELS AND THIS SAYS WHICH.
 *
 * Menu: the card's list, the two charts, the pinned tags. Stations:
 * whatever list is loaded. It is the folders-then-files gesture the
 * volume tabs already use, against the same rows, which is the whole
 * argument for browsing a directory on a device with no keyboard --
 * the interaction already exists and needed no typing.
 *
 * The menu is the level the tab opens on, always. Opening on the last
 * list would mean the tab shows a tag somebody browsed twenty minutes
 * ago as though it were theirs.
 */
static bool s_radio_menu;
/* Set by the player task while a fetch is in flight, or after one
 * failed. Cleared when a list arrives. */
static char s_radio_status[96];
static char s_dir[512];
static char s_result[512];

static entry_t *s_entries;
static int s_count;
static int s_top;               /* first visible row */

/*
 * A drag on the scrollbar, in progress.
 *
 * The bar was drawn from the first version and has never been touchable:
 * eight pixels of decoration reporting a position on a list that could
 * only be moved a page at a time from the footer. On a 9-track folder
 * that is a curiosity. On a folder with two hundred files in it, it is
 * twenty-two presses to reach the end of something the bar has been
 * showing you the shape of the whole time.
 *
 * Held across polls because a drag is not a press: browser_touch() is
 * otherwise built entirely on the down edge, which is right for buttons
 * and useless for anything that has to follow a finger.
 */
static bool s_scroll_drag;

/* The shared leading text of this folder's filenames, and its length.
 * Declared up here rather than beside find_common_prefix() because
 * entries_free() has to be able to drop it. */
static char s_prefix[128];
static int  s_prefix_len;

static play_order_t s_order = PLAY_ORDER_ALL;
static uint32_t s_seen_generation = UINT32_MAX;

play_order_t browser_order(void) { return s_order; }
bool browser_is_open(void) { return s_open; }

static int rows_visible(void)
{
    return (gfx_h() - FOOT_H - LIST_TOP) / ROW_H;
}

/*
 * The scrollbar's geometry, in one place.
 *
 * Drawing and hit-testing computed this independently in the first
 * version of this patch and it was wrong within a day -- the thumb was
 * drawn from LIST_TOP and grabbed from the top of the screen, so the
 * finger led the bar by the height of the tab strip. One function, two
 * callers, no chance to drift.
 *
 * Returns false when the list fits, which is also when there is nothing
 * to drag.
 */
static bool scroll_geom(int *track_y, int *track_h, int *bar_h, int *max_top)
{
    const int rows = rows_visible();
    if (s_count <= rows) return false;

    *track_y = LIST_TOP;
    *track_h = rows * ROW_H;
    *bar_h = (*track_h * rows) / s_count;
    if (*bar_h < 24) *bar_h = 24;
    *max_top = s_count - rows;
    return true;
}

/* ------------------------------------------------------------------ */
/* Directory listing                                                   */
/* ------------------------------------------------------------------ */

static void entries_free(void)
{
    for (int i = 0; i < s_count; i++) {
        free(s_entries[i].name);
        free(s_entries[i].label);
        s_entries[i].label = NULL;
    }
    s_count = 0;
    /* The prefix belonged to those names. A volume going away without a
     * reload -- which is what calls this -- would otherwise leave the
     * header claiming an elision over an empty list. */
    s_prefix[0] = '\0';
    s_prefix_len = 0;
}

/* Folders first, then files, each run sorted case-insensitively. Mixing
 * them alphabetically buries a disc subfolder in the middle of the track
 * list, and the two are different kinds of thing to tap. */
static int cmp_entry(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

/* Defined below, next to the rules it enforces; called from the bottom
 * of load_dir() because it can only run once the listing is complete
 * and sorted. */
static void find_common_prefix(void);

static void load_dir(const char *dir)
{
    if (!s_entries) {
        s_entries = heap_caps_calloc(MAX_ENTRIES, sizeof(entry_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_entries) {
            ESP_LOGE(TAG, "out of memory for the listing");
            return;
        }
    }

    entries_free();
    s_top = 0;
    snprintf(s_dir, sizeof(s_dir), "%s", dir);
    s_dirty = true;

    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGW(TAG, "cannot open %s", dir);
        return;
    }

    /* The same cue view the playlist builds, so a row here is a track
     * there. See cuedir.h. */
    cuedir_t *cues = cuedir_load(dir, STORAGE_IO_BACKGROUND);

    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_count < MAX_ENTRIES) {
        const bool is_dir = (e->d_type == DT_DIR);
        /* Everything the decoder cannot open is hidden rather than
         * greyed. A card root is mostly System Volume Information and
         * stray .txt files, and a list where two thirds of the rows are
         * untappable is a worse list. */
        if (storage_is_hidden(e->d_name)) continue;   /* . .. and dotfiles */
        if (!is_dir && !decoder_supports(e->d_name)) continue;
        if (!is_dir && cuedir_hides(cues, e->d_name)) continue;

        s_entries[s_count].name = strdup(e->d_name);
        if (!s_entries[s_count].name) break;
        s_entries[s_count].is_dir = is_dir;
        s_count++;
    }
    closedir(d);

    for (int i = 0; i < cuedir_count(cues) && s_count < MAX_ENTRIES; i++) {
        s_entries[s_count].name = strdup(cuedir_name(cues, i));
        s_entries[s_count].label = strdup(cuedir_label(cues, i));
        if (!s_entries[s_count].name) break;
        s_entries[s_count].is_dir = false;
        s_count++;
    }
    cuedir_free(cues);

    qsort(s_entries, (size_t)s_count, sizeof(entry_t), cmp_entry);
    find_common_prefix();
}

/*
 * The bit of every filename in this folder that is not telling you
 * anything.
 *
 * Ripped albums come off a CD as
 *
 *   Advent_Chamber_Orchestra_-_04_-_Mozart_-_Eine_Kleine_Nachtmusik.mp3
 *
 * and a folder of them is nine rows that agree for the first
 * twenty-seven characters. At NAME_SCALE the row holds about forty, so
 * the artist and the album spend two thirds of every line repeating
 * what the folder name above them already says, and the part that
 * differs -- the track number and the title, the only reason to look at
 * the list -- is pushed off the right edge and clipped.
 *
 * So it is found once per folder and dropped from the drawing. THE
 * ENTRIES ARE NOT MODIFIED: s_entries[i].name stays the real filename,
 * because it is what gets opened, what is compared against the playing
 * track, and what the playlist is built from. Only the row's starting
 * offset moves.
 *
 * WHERE IT STOPS
 *
 * At a separator, never mid-word. The raw common prefix of 04_ and 05_
 * includes the "0", which would leave rows reading "4_-_Mozart" and
 * "5_-_Handel" -- technically shorter and actively worse, because a
 * truncation that lands inside a number reads as data loss rather than
 * as tidying. Backing up to the last _ - . or space costs a couple of
 * characters and keeps every row starting on something whole.
 *
 * WHEN IT DECLINES
 *
 * Fewer than two files, a prefix shorter than MIN_PREFIX, or any file
 * left with almost nothing after it. That last one is the case that
 * matters: a folder holding 01.mp3 through 09.mp3 has a common prefix
 * of "0", and hiding it turns a legible list into the digits 1 to 9.
 * The point is to remove what is redundant, not to remove as much as
 * possible.
 *
 * Directories are neither counted nor shortened. A subfolder is not part
 * of the album's naming scheme and the prefix rarely applies to it.
 */
#define MIN_PREFIX      (8)     /* below this it is not worth the elision */
#define MIN_REMAINDER   (4)     /* what every row must still have left */

static bool is_sep(char c)
{
    return c == '_' || c == '-' || c == '.' || c == ' ';
}

static void find_common_prefix(void)
{
    s_prefix[0] = '\0';
    s_prefix_len = 0;

    const char *first = NULL;
    int files = 0, lcp = 0;

    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].is_dir || s_entries[i].label) continue;
        const char *n = s_entries[i].name;
        if (!first) { first = n; lcp = (int)strlen(n); files = 1; continue; }
        files++;

        int k = 0;
        while (k < lcp && first[k] && n[k] == first[k]) k++;
        lcp = k;
        if (lcp < MIN_PREFIX) return;   /* cannot recover; nothing to do */
    }

    if (files < 2 || lcp < MIN_PREFIX) return;

    /* Back up to a separator, and include it -- the row should start on
     * the next real character, not on the underscore before it. */
    while (lcp > 0 && !is_sep(first[lcp - 1])) lcp--;
    if (lcp < MIN_PREFIX) return;

    /* Every row has to be left with something worth reading. */
    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].is_dir || s_entries[i].label) continue;
        if ((int)strlen(s_entries[i].name) - lcp < MIN_REMAINDER) return;
    }

    if (lcp >= (int)sizeof(s_prefix)) lcp = (int)sizeof(s_prefix) - 1;
    memcpy(s_prefix, first, (size_t)lcp);
    s_prefix[lcp] = '\0';
    s_prefix_len = lcp;
    ESP_LOGI(TAG, "hiding a shared prefix on %d files: \"%s\"", files, s_prefix);
}

/* Path of the volume root for the active tab. */
static const char *tab_root(void)
{
    /* RADIO has no root. Callers guard on s_radio before using this;
     * returning "" rather than NULL keeps a stray use from crashing. */
    if (s_tab == BROWSER_TAB_RADIO) return "";
    return storage_mount_path((storage_id_t)s_tab);
}

static bool at_root(void)
{
    return strcmp(s_dir, tab_root()) == 0;
}

static void go_up(void)
{
    if (at_root()) return;
    char up[512];
    snprintf(up, sizeof(up), "%s", s_dir);
    char *slash = strrchr(up, '/');
    if (!slash) return;
    if (slash == up) return;
    *slash = '\0';
    load_dir(up);
}

/*
 * An absent volume's tab is still selectable, which is the one place
 * this differs from a normal tab strip. The grey means "nothing here
 * yet", not "not a button".
 *
 * It used to mean more than that: selecting the USB tab was what powered
 * the port, so the tab HAD to be selectable or there was no way to ask.
 * The port comes up at boot now -- a USB audio device cannot announce
 * itself through a dark port, and waiting for someone to open the
 * chooser was never a sensible gate on that -- so this is back to being
 * an ordinary reason rather than a load-bearing one. The tab stays
 * selectable anyway: a tab that ignores taps while the drive spins up
 * reads as a lost tap.
 */
/* A tab has something in it. For the volumes that is a mount; for RADIO
 * it is a station list that has been read. */
static bool tab_present(browser_tab_t t)
{
    if (t == BROWSER_TAB_RADIO) return stations_count() > 0;
    return storage_present((storage_id_t)t);
}

/*
 * Fill the rows from the menu table.
 *
 * is_dir is true for every row, and that is not a lie told to get a
 * folder icon: each one opens into a list of things to play, which is
 * what is_dir means everywhere else in this file. The card row included
 * -- it opens into the card's stations.
 */
static void load_radio_menu(void)
{
    entries_free();
    if (!s_entries) {
        s_entries = heap_caps_calloc(MAX_ENTRIES, sizeof(entry_t),
                                     MALLOC_CAP_SPIRAM);
        if (!s_entries) {
            ESP_LOGE(TAG, "no PSRAM for the radio menu");
            return;
        }
    }
    for (int i = 0; i < RADIOBROWSER_MENU_ROWS && s_count < MAX_ENTRIES; i++) {
        s_entries[s_count].name = strdup(radiobrowser_menu_label(i));
        if (!s_entries[s_count].name) break;
        s_entries[s_count].is_dir = true;
        s_count++;
    }
    s_top = 0;
    s_dirty = true;
}

void browser_set_radio_status(const char *line)
{
    snprintf(s_radio_status, sizeof(s_radio_status), "%s", line ? line : "");
    s_dirty = true;
}

/* Fill the rows from the station list. Same entry_t the files use, so
 * everything downstream of here is unchanged. */
static void load_stations(void)
{
    entries_free();
    if (!s_entries) {
        s_entries = heap_caps_calloc(MAX_ENTRIES, sizeof(entry_t),
                                     MALLOC_CAP_SPIRAM);
        if (!s_entries) {
            ESP_LOGE(TAG, "no PSRAM for the station rows");
            return;
        }
    }
    const int n = stations_count();
    for (int i = 0; i < n && s_count < MAX_ENTRIES; i++) {
        station_t st;
        if (!stations_get(i, &st)) continue;
        s_entries[s_count].name = strdup(st.name);
        if (!s_entries[s_count].name) break;
        s_entries[s_count].is_dir = false;
        s_entries[s_count].fav = favorites_contains(st.url);
        s_count++;
    }
    /* NOT sorted. The file's order is the listener's order -- it is what
     * next and previous move through, and what stations_index() counts
     * in. Sorting the display would make the first row and the first
     * station different things. */
    s_top = 0;
    s_dirty = true;
    ESP_LOGI(TAG, "radio: %d stations", s_count);
}

/*
 * The list has been re-read. Same rows, rebuilt.
 *
 * load_stations() already does everything needed -- it frees the rows,
 * refills them from stations.c in the file's order, resets the scroll and
 * marks the screen dirty -- so this is a guard and a call. The guard is
 * the whole of what is new: this arrives asynchronously, from a load
 * another task performed, and by then the tab showing may not be RADIO
 * any more.
 */
void browser_stations_reloaded(void)
{
    if (!s_open || !s_radio) return;
    /*
     * A list arriving is what opens the second level. Both ways in lead
     * here -- the card row and a directory row -- so there is one place
     * that decides the tab is showing stations, rather than one per
     * request kind.
     */
    s_radio_menu = false;
    s_radio_status[0] = '\0';
    load_stations();
}

static void select_tab(browser_tab_t id)
{
    const bool same = (id == s_tab);
    s_tab = id;

    if (id == BROWSER_TAB_RADIO) {
        s_radio = true;
        /* The prefix elision is a filename thing and station names are
         * not filenames. Left set, "all start with" would be computed
         * from the previous folder and applied to station rows. */
        s_prefix[0] = '\0';
        s_prefix_len = 0;
        s_dir[0] = '\0';
        s_radio_status[0] = '\0';
        s_radio_menu = true;
        load_radio_menu();
        return;
    }

    s_radio = false;

    if (!storage_present((storage_id_t)id)) {
        entries_free();
        s_dir[0] = '\0';
        s_dirty = true;
        return;
    }

    if (same && s_count > 0) return;
    load_dir(storage_mount_path((storage_id_t)id));
}

void browser_open(const char *start)
{
    s_open = true;
    s_was_down = false;
    /* A drag cannot survive the screen it was on. Left set, the first
     * press after reopening would be treated as the continuation of a
     * gesture that ended somewhere else. */
    s_scroll_drag = false;
    s_dirty = true;
    s_seen_generation = storage_generation();

    /*
     * A VOLUME TAB, WHICH MEANS NOT THE RADIO TAB.
     *
     * Every path below sets s_tab to a storage id or leaves both tabs
     * grey with no rows; none of them can open on RADIO. But none of
     * them went through select_tab() either, and select_tab() is the
     * only thing that had ever cleared s_radio -- so once the radio tab
     * had been visited, every REOPEN of the chooser showed a volume's
     * files with s_radio still true.
     *
     * The board found it and the log is unambiguous:
     *
     *   button: row 3 (station) "Advent_Chamber_Orchestra_-_04_-_Mozart..."
     *   station 4 of 4: ice1.somafm.com
     *
     * A file row, named as a station, playing whatever station happened
     * to sit at that index -- and for rows past the end of a four-entry
     * station list, playing nothing at all while the press logged as
     * received. The footer's slot 0 was RLOD over a directory too, and
     * `radio: 4 stations` was drawn over a nine-file listing.
     *
     * Reset here rather than by routing browser_open() through
     * select_tab(): that function loads, and this one has three
     * different ideas about what to load and which volume to load it
     * from. What was missing was one line of state, and this is that
     * line.
     */
    s_radio = false;

    /* Reopen where the current track lives, when that volume is still
     * there. Coming back to the root of the card every time is the thing
     * that makes a chooser tedious on an album you are picking through. */
    storage_id_t want = STORAGE_COUNT;
    if (start && *start) want = storage_of_path(start);

    if (want < STORAGE_COUNT && storage_present(want)) {
        s_tab = want;
        char dir[512];
        snprintf(dir, sizeof(dir), "%s", start);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) *slash = '\0';
        load_dir(dir);
        return;
    }

    for (int i = 0; i < STORAGE_COUNT; i++) {
        if (storage_present((storage_id_t)i)) {
            s_tab = (storage_id_t)i;
            load_dir(storage_mount_path(s_tab));
            return;
        }
    }

    /* Nothing mounted. Both tabs grey, no rows. */
    entries_free();
    s_dir[0] = '\0';
}

void browser_close(void)
{
    s_open = false;
    entries_free();
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static const char *status_line(void)
{
    if (s_radio) {
        static char line[96];
        /* Whatever the player task last said, ahead of everything: a
         * fetch in flight or a fetch that failed is the only thing on
         * this tab worth the row while it is true. */
        if (s_radio_status[0]) return s_radio_status;
        if (s_radio_menu) return "radio - the card, the charts, or a tag";
        const int n = stations_count();
        if (n <= 0) {
            /* Names the file, because the fix is to make one. */
            return "no " STATIONS_FILENAME " on the card";
        }
        snprintf(line, sizeof(line), "%d station%s from %s", n,
                 n == 1 ? "" : "s", stations_source());
        return line;
    }
    if (s_dir[0]) return s_dir;
    if (s_tab == BROWSER_TAB_USB) {
        /* The false branch is a few milliseconds of bring-up at boot,
         * not a state anyone can tap their way into any more. */
        return storage_usb_powered() ? "USB port on - waiting for a drive"
                                     : "USB port coming up";
    }
    return "no card in the slot";
}

static void draw_tab(browser_tab_t id, int x, int w)
{
    const bool present = tab_present(id);
    /* Selected is selected even with nothing mounted: the USB tab can be
     * the active one while the port is still coming up, and a strip with
     * no underline at all would read as a lost tap. */
    const bool active = (id == s_tab);

    gfx_fill_rect(x, 0, w, TAB_H, active ? C_TAB_ON : C_TAB_OFF);
    if (active) gfx_fill_rect(x, TAB_H - 5, w, 5, present ? C_ACCENT : C_DISABLED);

    /* storage_label() reports the volume's own label -- "SD8G" -- which
     * RADIO has no equivalent of and should not borrow. */
    const char *label = (id == BROWSER_TAB_RADIO) ? "RADIO"
                                                  : storage_label((storage_id_t)id);
    const int tw = gfx_text_w(label, NAME_SCALE);
    gfx_draw_text(x + (w - tw) / 2, (TAB_H - GFX_GLYPH_H(NAME_SCALE)) / 2, label,
                  NAME_SCALE, w - 16,
                  present ? (active ? C_TEXT : C_DIM) : C_DISABLED);
}

/* A folder, drawn the same way ui.c draws the one on the transport bar so
 * the button and the rows it produces are visibly the same idea. */
static void draw_folder_icon(int cx, int cy, uint16_t c)
{
    gfx_fill_rect(cx - 20, cy - 14, 16, 5, c);
    gfx_fill_rect(cx - 20, cy - 9, 40, 25, c);
    gfx_fill_rect(cx - 16, cy - 5, 32, 17, C_ROW);
}

/*
 * The star: filled when starred, a ring when not.
 *
 * IT USED TO BE TWO OVERLAPPING TRIANGLES, which is a hexagram -- six
 * points -- while the comment above it said "five-pointed". No pair of
 * triangles makes a five-pointed star; it is a ten-vertex polygon, and
 * gfx_fill_star() draws one.
 *
 * The unfilled case is the same star again at 58% in the row's own
 * colour, which leaves a rim. `bg` and not C_ROW: the rows alternate
 * C_ROW and C_ROW_ALT, and a hole punched in the wrong one is a star
 * with a shadow on every second line.
 *
 * 58% rather than the 60% the triangles used because a star's arms are
 * thinner than a triangle's edge at the same fraction, and at r=18 the
 * rim wants the two or three pixels this leaves it.
 */
static void draw_star(int cx, int cy, int r, bool filled, uint16_t c,
                      uint16_t bg)
{
    gfx_fill_star(cx, cy, r, c);
    if (!filled) gfx_fill_star(cx, cy, (r * 58) / 100, bg);
}

/* A note: stem and head. Two rectangles and a circle is enough to read as
 * "this one is a track" at a glance, which is all the icon is for. */
static void draw_note_icon(int cx, int cy, uint16_t c)
{
    gfx_fill_rect(cx + 6, cy - 16, 4, 24, c);
    gfx_fill_rect(cx + 6, cy - 16, 14, 5, c);
    gfx_fill_circle(cx + 1, cy + 9, 7, c);
}

/*
 * The track being heard, as told by the player. See browser_set_playing().
 *
 * Held here rather than asked for, because the answer is not derivable
 * on this side: playlist_current() is where the DECODER is, and at a
 * boundary that is twenty seconds ahead of the speaker. 0513 dirtied
 * the list when that changed, which worked and was too early by exactly
 * one ring.
 */
static char s_playing[512];

/*
 * The station being heard, as an index, or -1 for none.
 *
 * An index and not a name, because a station list is a position: two
 * entries with the same name are two stations and stations_index() is
 * what next and previous move through. Same reason BROWSER_PLAY_STREAM
 * hands back an index rather than a URL.
 *
 * Told rather than read from stations_index(), which is always set to
 * something: it keeps the last choice after a stream ends, so reading it
 * directly would leave a station marked as playing when nothing is.
 * That distinction is the player's to make and this is how it says it --
 * exactly as s_playing works for files.
 */
static int s_playing_station = -1;

void browser_set_station(int index)
{
    if (index == s_playing_station) return;
    s_playing_station = index;
    s_dirty = true;     /* same reason as browser_set_playing()'s */
}

void browser_set_playing(const char *path)
{
    const char *p = (path && *path) ? path : "";
    if (strcmp(s_playing, p) == 0) return;

    snprintf(s_playing, sizeof(s_playing), "%s", p);

    /* The redraw goes with it. Every other thing that moves the marker
     * is a press, which dirties the list on its own; this one arrives
     * from another task and has nothing else to ride on. */
    s_dirty = true;
}

/* Is this row the track that is playing? Compared as a whole path: a
 * same-named track in a different folder is a different track, and on a
 * card full of "01 Intro.mp3" that is not a hypothetical. */
static bool is_current(const char *name)
{
    const char *path = s_playing[0] ? s_playing : NULL;
    if (!path || !s_dir[0]) return false;

    char full[512];
    /* A name too long to join is a name that cannot be the current track
     * either, since the current track was opened through the same join. */
    if (!storage_join_path(full, sizeof(full), s_dir, name)) return false;
    return strcmp(full, path) == 0;
}

static void draw_button(int x, int y, int w, int h, const char *label, bool on)
{
    gfx_fill_rect(x, y, w, h, on ? C_BTN : C_TAB_OFF);
    const int tw = gfx_text_w(label, LABEL_SCALE);
    gfx_draw_text(x + (w - tw) / 2, y + (h - GFX_GLYPH_H(LABEL_SCALE)) / 2, label,
                  LABEL_SCALE, w - 8, on ? C_TEXT : C_DISABLED);
}

static const char *order_label(void)
{
    switch (s_order) {
    case PLAY_ORDER_ONE:     return "ONE";
    case PLAY_ORDER_SHUFFLE: return "RND";
    case PLAY_ORDER_REPEAT_ONE: return "RPT";
    default:                 return "ALL";
    }
}

/* Six buttons across the bottom, equal width. The arithmetic is done from
 * the panel width rather than written out, because a 720 px panel divides
 * evenly and nothing here should assume that twice. */
#define FOOT_BUTTONS (6)

static void foot_box(int i, int *x, int *w)
{
    const int bw = gfx_w() / FOOT_BUTTONS;
    *x = i * bw;
    *w = (i == FOOT_BUTTONS - 1) ? (gfx_w() - *x) : bw;
}

void browser_draw(void)
{
    if (!s_open) return;

    /* A card going in or a drive coming out while the chooser is up has
     * to be visible without a touch, so the generation counter is the
     * other thing that dirties the screen. */
    const uint32_t gen = storage_generation();
    if (gen != s_seen_generation) {
        s_seen_generation = gen;
        s_dirty = true;
        /*
         * The radio tab has no volume behind it, so none of the
         * mount-chasing below applies to it -- and all of it would be
         * actively wrong: `!storage_present(s_tab)` on
         * BROWSER_TAB_RADIO reads past the end of the volume array, and
         * the adopt-what-appeared branch would silently switch the tab
         * out from under a station list and replace the rows with a
         * directory. The station rows are only rebuilt by select_tab().
         *
         * Scoped rather than an early return: s_dirty has just been set
         * and returning here would leave the repaint a frame late, so
         * the tab strip would not redraw until the next poll.
         */
        if (!s_radio) {
        /* The tab stays where the user put it. Jumping to whatever else
         * is mounted would undo the tap that powered this port about a
         * second before the drive it was waiting for turned up. */
        if (!storage_present((storage_id_t)s_tab)) {
            if (s_dir[0]) {                 /* it was there and went away */
                entries_free();
                s_dir[0] = '\0';
            } else {
                /*
                 * Never landed anywhere, so there is no tap to respect.
                 *
                 * This is the boot case: the chooser comes up before
                 * anything is mounted, defaults to the SD tab, and with
                 * no card there is nothing behind it. A drive turning
                 * up a second later left the user looking at an empty
                 * SD listing with their music one untapped tab away.
                 *
                 * Adopting the volume that just appeared is not
                 * overriding a choice, because none has been made.
                 */
                for (int i = 0; i < STORAGE_COUNT; i++) {
                    if (!storage_present((storage_id_t)i)) continue;
                    s_tab = (browser_tab_t)i;
                    load_dir(tab_root());
                    break;
                }
            }
        } else if (s_count == 0 || !s_dir[0]) {
            load_dir(s_dir[0] ? s_dir : tab_root());
        }
        }
    }

    if (!s_dirty) return;
    s_dirty = false;

    const int w = gfx_w(), h = gfx_h();
    gfx_fill_rect(0, 0, w, h, C_BG);

    /*
     * Three tabs, and the arithmetic is written so they tile exactly:
     * the last one takes whatever the two divisions left over rather
     * than each being w/3 and leaving a seam of background at the right
     * edge on a width that is not a multiple of three. 720 is not.
     */
    {
        const int t1 = w / 3, t2 = (2 * w) / 3;
        draw_tab(BROWSER_TAB_SD, 0, t1);
        draw_tab(BROWSER_TAB_USB, t1, t2 - t1);
        draw_tab(BROWSER_TAB_RADIO, t2, w - t2);
    }

    /* Current directory, tail kept -- the end of the path is the part
     * that says where you are. With nothing mounted it says why, because
     * "no drive" and "no power" are the difference between waiting and
     * tapping. */
    /*
     * One line normally, two when something is being hidden -- and the
     * second line is not optional politeness. A list quietly showing
     * names that are not the names on the card is a list you cannot
     * trust; saying what came off the front makes it an abbreviation
     * rather than a discrepancy.
     *
     * Both fit in PATH_H at LABEL_SCALE without moving LIST_TOP, so the
     * list does not lose a row on folders that happen to be tidy.
     */
    if (s_prefix_len) {
        const int gh = GFX_GLYPH_H(LABEL_SCALE);
        const int pad = (PATH_H - 2 * gh) / 3;
        gfx_draw_text_tail(16, TAB_H + pad, status_line(), LABEL_SCALE,
                           w - 32, C_DIM);

        char note[160];
        snprintf(note, sizeof(note), "all start with  %s", s_prefix);
        /* Head kept, not tail: the front of the prefix is what identifies
         * it, and it is the front that the rows are missing. */
        gfx_draw_text(16, TAB_H + 2 * pad + gh, note, LABEL_SCALE,
                      w - 32, C_FAINT);
    } else {
        gfx_draw_text_tail(16, TAB_H + (PATH_H - GFX_GLYPH_H(LABEL_SCALE)) / 2,
                           status_line(), LABEL_SCALE, w - 32, C_DIM);
    }
    gfx_fill_rect(0, LIST_TOP - 2, w, 2, C_RULE);

    const int rows = rows_visible();
    for (int r = 0; r < rows; r++) {
        const int i = s_top + r;
        const int y = LIST_TOP + r * ROW_H;
        if (i >= s_count) {
            gfx_fill_rect(0, y, w, ROW_H, C_BG);
            continue;
        }
        const uint16_t row_bg = (r & 1) ? C_ROW_ALT : C_ROW;
        gfx_fill_rect(0, y, w, ROW_H, row_bg);

        /*
         * On the radio tab the marker is the station index, because
         * is_current() cannot answer there: it joins s_dir with the row
         * name and compares against a file path, and s_dir is empty on
         * RADIO -- so it returned false for every station and NOTHING
         * was ever marked. A file was correctly highlighted on the SD
         * tab in the same session, which is what made it look like the
         * marker working rather than the marker being absent.
         *
         * Row i IS station i: load_stations() builds the rows in the
         * list's order and does not sort, for exactly this kind of
         * reason.
         */
        /* And not on the menu level, where row i is a tag rather than
         * station i -- the marker would land on whichever genre happens
         * to sit at the playing station's index. */
        const bool playing = s_radio
            ? (!s_radio_menu && i == s_playing_station)
            : (!s_entries[i].is_dir && is_current(s_entries[i].name));

        if (s_entries[i].is_dir) {
            draw_folder_icon(48, y + ROW_H / 2, C_DIM);
        } else {
            draw_note_icon(48, y + ROW_H / 2, playing ? C_ACCENT : C_DIM);
        }
        /* Files only. See find_common_prefix() -- the name itself is
         * untouched; this is where the elision happens and the only
         * place it happens. */
        const char *label = s_entries[i].label ? s_entries[i].label
                          : s_entries[i].name
                          + (s_entries[i].is_dir ? 0 : s_prefix_len);

        /*
         * The star, on station rows only -- not on the menu level,
         * where a row is a chart or a tag and there is nothing to star,
         * and not on the volume tabs, where a row is a file.
         *
         * An empty outline on every unstarred row rather than nothing
         * at all: a control that appears only once it has been used is
         * one nobody finds. It is drawn dim so that a list with none
         * starred does not read as a column of decorations.
         */
        const bool starrable = s_radio && !s_radio_menu;
        const int  name_w = w - 112 - SCROLL_W - (starrable ? STAR_W : 0);
        gfx_draw_text(96, y + (ROW_H - GFX_GLYPH_H(NAME_SCALE)) / 2, label,
                      NAME_SCALE, name_w, playing ? C_ACCENT : C_TEXT);

        if (starrable) {
            draw_star(w - SCROLL_W - STAR_W / 2, y + ROW_H / 2, STAR_R,
                      s_entries[i].fav,
                      s_entries[i].fav ? C_STAR : C_DIM, row_bg);
        }
    }

    /*
     * Scroll position, as a bar down the right edge -- and, since 0606,
     * the way to change it. A number of pages would need a font; a bar
     * says the same thing in sixteen pixels and can be dragged.
     *
     * The call IS the test. s_count > rows is the same question
     * scroll_geom() already answers, and asking it separately while
     * discarding the answer is what let the outputs be read on a path
     * the compiler could not prove they had been written on. One
     * condition, and it is the one that fills the variables.
     */
    int track_y, track_h, bar_h, max_top;
    if (scroll_geom(&track_y, &track_h, &bar_h, &max_top)) {
        const int bar_y = track_y + (max_top > 0 ? ((track_h - bar_h) * s_top) / max_top : 0);

        /*
         * Wider than it was, because it is a control now rather than a
         * readout. Eight pixels is legible; it is not something a finger
         * aims at. SCROLL_W is still narrow enough to leave the row taps
         * the whole of the rest of the width.
         */
        gfx_fill_rect(w - SCROLL_W, track_y, SCROLL_W, track_h, C_ROW);
        gfx_fill_rect(w - SCROLL_W, bar_y, SCROLL_W, bar_h,
                      s_scroll_drag ? C_TEXT : C_DIM);
    }

    const int fy = h - FOOT_H;
    gfx_fill_rect(0, fy, w, FOOT_H, C_BG);
    gfx_fill_rect(0, fy, w, 2, C_RULE);

    int bx, bw;
        /*
     * Slot 0 is UP on a volume and RLOD on the radio tab.
     *
     * Reused rather than given a seventh button. UP is a volume
     * operation and has nothing to do on RADIO, so the slot was drawn
     * disabled and refused -- a dead button in the corner the eye goes
     * to first. Reload is the operation that tab actually wants and had
     * nowhere to be.
     *
     * Always enabled on RADIO, including with no stations at all. That
     * is the case it is most for: the status line says
     * "no stations.m3u on the card", and the point of reading it is to
     * go and make one.
     */
    foot_box(0, &bx, &bw);
    if (s_radio) {
        /*
         * UP on the second level, RLOD on the first, which is the same
         * reuse of this slot the tab already makes and for the same
         * reason. Going back up from a list of stations is exactly what
         * UP means on a volume; reloading is what the menu level has to
         * offer, and the two never want the slot at once.
         */
        draw_button(bx + 4, fy + 8, bw - 8, FOOT_H - 16,
                    s_radio_menu ? "RLOD" : "UP", true);
    } else {
        draw_button(bx + 4, fy + 8, bw - 8, FOOT_H - 16, "UP",   !at_root() && s_dir[0]);
    }
    foot_box(1, &bx, &bw); draw_button(bx + 4, fy + 8, bw - 8, FOOT_H - 16, "FLDR", !s_radio && s_dir[0] != '\0');
    foot_box(2, &bx, &bw); draw_button(bx + 4, fy + 8, bw - 8, FOOT_H - 16, "UP^",  s_top > 0);
    foot_box(3, &bx, &bw); draw_button(bx + 4, fy + 8, bw - 8, FOOT_H - 16, "DN",   s_top + rows < s_count);
    foot_box(4, &bx, &bw); draw_button(bx + 4, fy + 8, bw - 8, FOOT_H - 16, order_label(), true);
    foot_box(5, &bx, &bw); draw_button(bx + 4, fy + 8, bw - 8, FOOT_H - 16, "X",    true);

    gfx_blit(0, h);
}

/* ------------------------------------------------------------------ */
/* Touch                                                               */
/* ------------------------------------------------------------------ */

browser_result_t browser_touch(bool down, int x, int y)
{
    browser_result_t res = { BROWSER_NONE, NULL, -1 };
    const bool tapped = down && !s_was_down;
    s_was_down = down;

    if (!s_open) return res;

    const int w = gfx_w(), h = gfx_h();
    const int rows = rows_visible();

    /*
     * The scrollbar, before anything else and outside the tapped test.
     *
     * Outside it because a drag is a sequence of downs with one edge at
     * the front, and everything else in this function wants only that
     * edge. Before the rows because the grab zone overlaps them: a press
     * inside it is a scroll, not a file, and a press that starts a drag
     * must not also open whatever it happened to land on.
     */
    if (s_scroll_drag || (tapped && x >= w - SCROLL_HIT_W)) {
        int track_y, track_h, bar_h, max_top;
        if (down && scroll_geom(&track_y, &track_h, &bar_h, &max_top) &&
            y >= track_y && y < track_y + track_h) {

            /*
             * The thumb centres on the finger rather than keeping the
             * offset it was grabbed at.
             *
             * Grab-offset is the desktop behaviour and it is the right
             * one for a mouse, where the pointer is a pixel and the
             * thumb is visible under it. Here the thumb is under a
             * fingertip that covers it entirely, so preserving an offset
             * preserves something nobody can see, and a press on the
             * track above the thumb would do nothing at all instead of
             * going there. Centring makes press-anywhere and drag the
             * same gesture.
             */
            const int span = track_h - bar_h;
            int pos = y - track_y - bar_h / 2;
            if (pos < 0) pos = 0;
            if (pos > span) pos = span;

            const int want = span > 0 ? (pos * max_top + span / 2) / span : 0;
            if (want != s_top) {
                s_top = want;
                s_dirty = true;
            }
            /* Set after the move, so the first frame of a drag already
             * draws the thumb in its held colour. */
            s_scroll_drag = true;
            return res;
        }

        if (!down && s_scroll_drag) {
            s_scroll_drag = false;
            s_dirty = true;          /* back to the resting colour */
            ESP_LOGI(TAG, "scrolled to row %d of %d", s_top, s_count);
            return res;
        }
    }

    if (!tapped) return res;

    if (y < TAB_H) {
        /* Same thirds as the drawing, and derived the same way so a tap
         * lands on the strip it looks like it landed on. */
        const int t1 = w / 3, t2 = (2 * w) / 3;
        const browser_tab_t want = (x < t1) ? BROWSER_TAB_SD
                                 : (x < t2) ? BROWSER_TAB_USB
                                            : BROWSER_TAB_RADIO;
        static const char *const tab_name[BROWSER_TAB_COUNT] = {
            "SD", "USB", "RADIO"
        };
        ESP_LOGI(TAG, "button: tab %s", tab_name[want]);
        select_tab(want);
        return res;
    }

    if (y >= h - FOOT_H) {
        const int bw = w / FOOT_BUTTONS;
        int which = x / bw;
        if (which >= FOOT_BUTTONS) which = FOOT_BUTTONS - 1;

        /* One line per footer press, named, before the switch acts on
         * it -- so a press that turns out to do nothing (page up at the
         * top of the list, play-folder on an empty folder) still shows
         * up as having been received. A button that is working and a
         * button that is not both look like silence otherwise. */
        static const char *const foot_name[FOOT_BUTTONS] = {
            "up", "play folder", "page up", "page down", "order", "cancel"
        };
        static const char *const foot_name_radio[FOOT_BUTTONS] = {
            "reload stations", "play folder", "page up", "page down",
            "order", "cancel"
        };
        static const char *const foot_name_menu[FOOT_BUTTONS] = {
            "back to the menu", "play folder", "page up", "page down",
            "order", "cancel"
        };
        ESP_LOGI(TAG, "button: %s",
                 !s_radio ? foot_name[which]
                          : s_radio_menu ? foot_name_radio[which]
                                         : foot_name_menu[which]);

        /*
         * UP and FLDR are volume operations and the radio tab has
         * neither a parent nor a folder to play. Refused here as well as
         * drawn disabled, because a disabled button that still acts is
         * worse than one that is not drawn at all.
         */
        /*
         * Slot 0 on RADIO is the reload, not UP. Before the refusal
         * below, which still owns slot 1.
         */
        if (s_radio && which == 0) {
            if (!s_radio_menu) {
                /* Back to the menu, and done here: the rows are this
                 * task's to rebuild and there is nothing to ask the
                 * player for. The list stays loaded, so a station
                 * playing out of it keeps playing and next still moves
                 * through it -- going up a level is a change of view
                 * and not a change of what is on. */
                s_radio_menu = true;
                s_radio_status[0] = '\0';
                load_radio_menu();
                return res;
            }
            res.kind = BROWSER_RELOAD_STATIONS;
            return res;
        }
        if (s_radio && which == 1) {
            ESP_LOGI(TAG, "not on the radio tab");
            return res;
        }

        switch (which) {
        case 0:
            go_up();
            break;
        case 1:
            if (s_dir[0]) {
                snprintf(s_result, sizeof(s_result), "%s", s_dir);
                res.kind = BROWSER_PLAY_FOLDER;
                res.path = s_result;
            }
            break;
        case 2:
            if (s_top > 0) {
                s_top -= rows;
                if (s_top < 0) s_top = 0;
                s_dirty = true;
            }
            break;
        case 3:
            if (s_top + rows < s_count) {
                s_top += rows;
                s_dirty = true;
            }
            break;
        case 4:
            /* ONE -> ALL -> RND -> RPT -> ONE. RPT sits after RND so
             * the two whole-folder modes stay adjacent and the two
             * single-track modes bookend the cycle. */
            s_order = (s_order == PLAY_ORDER_ONE)     ? PLAY_ORDER_ALL
                    : (s_order == PLAY_ORDER_ALL)     ? PLAY_ORDER_SHUFFLE
                    : (s_order == PLAY_ORDER_SHUFFLE) ? PLAY_ORDER_REPEAT_ONE
                                                      : PLAY_ORDER_ONE;
            ESP_LOGI(TAG, "play order now %s", order_label());
            s_dirty = true;
            break;
        default:
            res.kind = BROWSER_CANCELLED;
            break;
        }
        return res;
    }

    const int r = (y - LIST_TOP) / ROW_H;
    if (r < 0 || r >= rows) return res;
    const int i = s_top + r;
    if (i >= s_count) return res;

    ESP_LOGI(TAG, "button: row %d (%s) \"%s\"", i,
             s_radio ? "station" : s_entries[i].is_dir ? "dir" : "file",
             s_entries[i].name);

    if (s_radio && s_radio_menu) {
        /*
         * A menu row. Row 0 is the card and is a reload; everything
         * else is a request to the directory. What the row MEANS is
         * radiobrowser.h's business -- this asks whether it is a fetch
         * and passes the number on.
         */
        if (!radiobrowser_menu_kind(i, NULL, NULL)) {
            /* Three rows are not fetches, and they are three different
             * actions. radiobrowser.h names them so this does not
             * hard-code which number is which. */
            res.kind = (i == RADIOBROWSER_MENU_ADD) ? BROWSER_ADD_STATION
                     : (i == RADIOBROWSER_MENU_FAV) ? BROWSER_LOAD_FAVORITES
                                                    : BROWSER_RELOAD_STATIONS;
            return res;
        }
        res.kind = BROWSER_FETCH_STATIONS;
        res.index = i;
        return res;
    }

    if (s_radio) {
        /*
         * The star's strip, tested before the row: it is inside the
         * row's box and has to win, which is the rule row 7 of the
         * panel already follows. The scrollbar has had its own test
         * long before this point, so this strip never reaches it.
         */
        if (x >= gfx_w() - SCROLL_W - STAR_W && x < gfx_w() - SCROLL_W) {
            res.kind = BROWSER_TOGGLE_FAVORITE;
            res.index = i;
            return res;
        }
        /* The index, not the URL. See BROWSER_PLAY_STREAM in browser.h:
         * the rows were built from the station list in its own order, so
         * row i IS station i, and stations.c stays the only thing that
         * knows what station i is. */
        res.kind = BROWSER_PLAY_STREAM;
        res.index = i;
        return res;
    }

    if (s_entries[i].is_dir) {
        char sub[512];
        if (!storage_join_path(sub, sizeof(sub), s_dir, s_entries[i].name)) {
            ESP_LOGW(TAG, "path too long: %s/%s", s_dir, s_entries[i].name);
            return res;
        }
        load_dir(sub);
        return res;
    }

    if (!storage_join_path(s_result, sizeof(s_result), s_dir, s_entries[i].name)) {
        ESP_LOGW(TAG, "path too long: %s/%s", s_dir, s_entries[i].name);
        return res;
    }
    res.kind = BROWSER_PLAY_FILE;
    res.path = s_result;
    return res;
}
