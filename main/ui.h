/*
 * Transport bar drawn straight into the panel's scan buffer.
 *
 * No LVGL, no M5Canvas. albumart.c already establishes the pattern --
 * esp_lcd_dpi_panel_get_frame_buffer(), write pixels, hand the same
 * pointer back to draw_bitmap -- and the whole UI is five controls, so a
 * widget toolkit would be more code than the thing it draws.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#include "audio_out.h"
#include "levelhist.h"   /* LEVELHIST_COLUMNS, for the level strip */

#ifdef __cplusplus
extern "C" {
#endif

/* What the UI reads. The player owns this; ui_draw() never writes it. */
typedef struct {
    const char *title;      /* falls back to the filename when no tag */
    const char *artist;     /* NULL or "" hides the row */
    const char *album;
    bool     playing;
    int      volume;        /* 0..100 */
    bool     muted;         /* the slider still shows the level it will
                             * return to -- see the note in ui.c */

    /* Where the sound is going. The icon in the volume row's left margin
     * draws this rather than always drawing a speaker, so the panel says
     * which device is playing instead of only which one exists. */
    audio_out_route_t route;
    uint32_t pos_sec;
    uint32_t len_sec;       /* 0 when unknown -- seek bar renders empty */

    /* Whether a drag would do anything. A track can have a known length
     * and still not be seekable: duration.c reads Ogg lengths out of the
     * container, but nothing can seek an Ogg yet. */
    bool can_seek;

    /* Whether the next button would do anything. False greys it out --
     * the last track of a folder in list order, or an empty playlist.
     * True under shuffle even though the next track is unknown: it
     * exists, which is what the button is promising. */
    bool     has_next;
    bool     screen_off;

    /*
     * Whether pos_sec, len_sec and can_seek describe the track named
     * above them.
     *
     * False from the moment a track change is decided until the new
     * track's duration is known -- which on a Xing-less MP3 is a whole-
     * file scan away. The three numbers are stale for all of it, and a
     * seek bar that keeps filling and a clock that keeps counting are
     * the most convincing part of the illusion that nothing has
     * happened. While this is false the clocks read as dashes and the
     * bar is a bare groove.
     */
    bool stats_valid;

    /*
     * State of charge 0-100, or -1 for "no reading". Drawn opposite the
     * speaker on the volume row: an empty outline and no digits at -1,
     * because a gauge that invents a number when it has none is worse
     * than one that admits it.
     */
    int  battery_pct;
    bool battery_charging;

    /*
     * No pack: the board is on the USB-C supply. Drawn as a connector
     * where the battery would be, rather than as an empty battery.
     *
     * The distinction is the whole point. An outline with no fill means
     * "there is a pack and I cannot read it"; a connector means "there
     * is no pack, and the thing keeping this alive is the cable". Those
     * are different situations and only one of them means unplugging is
     * about to switch the player off.
     */
    bool ext_power;

    /*
     * ReplayGain, when a measured gain is in effect for this track.
     *
     * Drawn in its own colour rather than folded into the volume
     * number, because the two are different statements: the slider is
     * what the listener asked for and this is what the file needed on
     * top of it. Showing only their sum would leave a track that plays
     * quieter than the slider says looking like a bug.
     *
     * rg_gain_db is signed and typically a few dB either way.
     */
    bool  rg_active;
    float rg_gain_db;

    /*
     * True only while this play is actually measuring -- no stored
     * loudness, nothing invalidated yet. The listening text is drawn
     * from this rather than from the envelope being absent, because
     * those are different states: a track whose envelope is in its
     * sidecar but whose bar has not been handed it yet is not being
     * listened to, and saying so would be a lie with a five second
     * lifetime.
     */
    bool  rg_measuring;

    /*
     * A live stream rather than a file.
     *
     * Not "there is no duration". A file whose length is unknown already
     * has a state -- stats_valid false, or len_sec 0 -- and it means
     * something is missing that ought to be there: the bar goes to a bare
     * groove and both clocks go to dashes, which is a player admitting a
     * gap in what it knows. A live stream is not missing a position. It
     * does not have one, will never have one, and drawing the
     * unknown-duration state for it reports a fault where there is none.
     *
     * So this is its own flag and it suppresses rows rather than emptying
     * them: no envelope, no groove, no clocks. What goes in their place
     * is the badge that says why they are absent.
     */
    bool live;

    /*
     * What the stream is doing, already rendered, or "" when there is
     * nothing to say. streamplan_status_text() produces exactly this and
     * never returns NULL.
     *
     * A string and not the streamplan_status_t, so that ui.c does not
     * acquire an opinion about a state machine it cannot see: the value
     * is a function of netstream's state AND the buffer phase, the two
     * disagree on purpose, and streamplan.h has the table and the host
     * test for reconciling them. Passing the enum would put the fifth
     * copy of that reasoning in a draw call.
     *
     * It is empty whenever sound is coming out, which is most of the
     * time -- see streamplan_status(). The line is therefore not a label
     * that is always present; it appears when there is a reason.
     */
    const char *stream_status;

    /* 5067: draw a spinner after stream_status -- the stream is waiting
     * for the network, not for the station. */
    bool stream_spinner;

    /*
     * The ICY title -- what is playing on the station right now -- or
     * NULL/"" when the station is not sending one, or is sending its own
     * name as one, which several do for hours at a time.
     *
     * Separate from `title` rather than folded into it. `title` is the
     * station, and the station is the thing the listener chose and the
     * thing that stays put; this changes every few minutes underneath it,
     * and the two swapping places on the same row would read as the
     * player having switched station.
     */
    const char *stream_title;

    /*
     * A minute of output level, oldest column first, and how much of the
     * strip ahead of the mark is audio that has arrived and not been
     * heard.
     *
     * Only read when `live`. A file has a seek bar and an envelope, both
     * of which say more than this would: where you are in a thing of
     * known length beats how loud the last minute was. A stream has
     * neither, and what it has instead is a reserve that can run out.
     *
     * `strip_valid` false means the writer held the strip when this
     * frame was assembled. The bar leaves the last one up rather than
     * drawing an empty minute -- a strip that blinks to silence
     * whenever the mutex is busy would look exactly like a dropout,
     * which is the thing it is supposed to report.
     */
    bool        strip_valid;
    uint8_t     strip[LEVELHIST_COLUMNS];
    /*
     * The reserve's own columns, nearest-first, in the same units as
     * `strip` and already scaled by the applied gain so the two halves
     * meet at the mark without a step.
     *
     * Separate from `strip` and not the spare end of it: see
     * LEVELHIST_HISTORY_OFFSET -- the columns past the mark in `strip`
     * are the newest twenty seconds of history, not spare.
     *
     * Zero means no measurement rather than silence, and draws the flat
     * band the reserve used to be.
     */
    uint8_t     strip_ahead[LEVELHIST_AHEAD_COLUMNS];
    int         strip_ahead_cols;
    bool        strip_clipped;

    /*
     * The star, on the transport row between `next` and the moon.
     *
     * "Not shown" is not the same as "not starred": the button is
     * absent when there is nothing playing to star, and an empty
     * outline there would be a control that does nothing. Since 1208 a
     * local file has a star too (starred.h), and FOLDER is the fourth
     * state. UI_FAV_HIDDEN draws no glyph and ui_touch() does not
     * test the box, so the space belongs to nobody rather than to a
     * dead button.
     *
     * The player resolves this from the URL of the station playing, so
     * it is the panel's answer to the same question the chooser's gold
     * row answers -- one source, favorites_contains(), and two places
     * that show it.
     */
    enum {
        UI_FAV_HIDDEN = 0,  /* not a stream: no button */
        UI_FAV_OFF,         /* a stream, not starred */
        UI_FAV_ON,          /* starred: a stream, or this file */
        /*
         * A file that is not starred but whose folder -- or a folder
         * above it -- is. A thick gold ring: the star is not this
         * track's, but it is not nothing either. Files only; a station
         * has no folder.
         */
        UI_FAV_FOLDER,
    } fav;

    /* 5106: the record button's state. While true the player also puts
     * the recording's name and length where the track's text goes. */
    bool recording;
} ui_state_t;

/* What a touch produced. The player acts on these; the UI never acts. */
typedef enum {
    UI_ACTION_NONE = 0,
    UI_ACTION_PLAY_PAUSE,
    UI_ACTION_CHOOSE_FILE,   /* folder button: open the chooser */
    UI_ACTION_SETTINGS,      /* gear button: open the settings panel */
    UI_ACTION_SCREEN_OFF,
    UI_ACTION_SCREEN_ON,
    UI_ACTION_PREV,         /* start of track, or the track before it */
    UI_ACTION_PREV_AGAIN,   /* double tap: back through play history */
    UI_ACTION_NEXT,
    UI_ACTION_SEEK,         /* value = target percent 0..100 */
    UI_ACTION_VOLUME,       /* value = target percent 0..100 */
    UI_ACTION_MUTE,         /* speaker icon, or the headset's mute key */
    /*
     * The star: star or unstar what is playing.
     *
     * A request, like everything else here -- the UI never acts. The
     * player owns it because it writes to the card, and the panel does
     * not change until the player has done it and published a new
     * state. A star that lit on the press and went out on the failure
     * would be the only optimistic control on this screen.
     *
     * Never produced while `fav` is UI_FAV_HIDDEN.
     */
    UI_ACTION_FAVORITE,
    /*
     * The notice card was tapped and should go.
     *
     * Only ever produced for a dismissible card -- see ui_notice_hit().
     * The player repaints the artwork, because clearing the card is
     * uncovering the cover and ui.c does not own that.
     */
    UI_ACTION_DISMISS_NOTICE,
    /* 5106: the record button, row 8's fifth icon. Start or stop; the
     * player decides which from recorder_active(), not from the icon. */
    UI_ACTION_RECORD,
} ui_action_kind_t;

/* Name of an action, for logging. Never NULL. Lives beside the enum so a
 * new action cannot be added without a name -- the switch has no default,
 * so the compiler asks. */
const char *ui_action_name(ui_action_kind_t k);

typedef struct {
    ui_action_kind_t kind;
    int value;
} ui_action_t;

esp_err_t ui_init(esp_lcd_panel_handle_t panel, int w, int h);

/* Blank the area above the bar.
 *
 * Needed once tracks follow one another: the cover is drawn by
 * albumart_show() and never cleared, so a track with no art inherited the
 * previous track's cover -- which reads as the player having ignored the
 * choice rather than as the file having no picture in it. */
void ui_clear_art(void);

/*
 * Fill the artwork square with lines of text, centred.
 *
 * For a file with no picture in it. The alternative -- and what this
 * replaces -- is 720x720 of black, which looks like a cover that has not
 * arrived yet rather than one that does not exist. What goes there
 * instead is the thing the file can always say about itself: its format.
 *
 * lines[0] is drawn largest and the rest step down, so the container
 * name reads as a heading and the details under it as detail. Draws and
 * blits immediately; not part of ui_draw().
 */
void ui_show_art_info(const char *const *lines, int n);

/* ------------------------------------------------------------------ */
/* The notice card                                                      */
/* ------------------------------------------------------------------ */

/*
 * A card over most of the artwork, for things the listener has to be
 * told while the transport stays usable.
 *
 * WHY IT IS OVER THE ART AND NOT A ROW IN THE BAR. The bar is eight
 * rows of controls and every one of them is doing something; an error
 * that needed a ninth would push the transport, and the transport is
 * where every finger goes. The artwork is 720x720 of decoration -- the
 * largest thing on the panel and the only part of it that can be
 * covered without taking a control away. Playback is not interrupted,
 * so a stream keeps running behind an address somebody is typing into
 * a phone.
 *
 * NOT part of ui_draw(). ui_draw() owns the bottom UI_BAR_H rows and
 * never the cover; this blits the card itself, like ui_show_art_info()
 * and ui_clear_art() already do. The cost is that the caller has to
 * repaint the artwork when the card goes away, which is the same thing
 * it already does after the format card.
 *
 * TWO KINDS, AND THE DIFFERENCE IS WHO CLEARS THEM.
 *
 * `dismissible` false is for a condition that is TRUE RIGHT NOW and
 * will stop being true on its own -- the web server is running, and
 * when it stops the card should go with it. There is no close button,
 * because a card the listener can dismiss while the thing it describes
 * is still happening leaves them with no way back to the address.
 *
 * `dismissible` true is for something that HAPPENED -- no media, a
 * station that would not play. Nothing will clear it but the person
 * reading it, so it draws a close box and ui_notice_hit() answers for
 * it.
 *
 * `head` is one line, larger. `body` is up to four lines under it.
 * Neither is copied: this draws immediately and keeps nothing but the
 * card's geometry.
 */
void ui_show_notice(const char *head, const char *const *body, int n,
                    bool dismissible);

/*
 * Is a card up, and is it one the listener can dismiss?
 *
 * The caller needs both: a notice that is up at all suppresses the
 * artwork repaint that would paint over it, and only a dismissible one
 * should be closed by a tap.
 */
bool ui_notice_active(void);
bool ui_notice_dismissible(void);

/*
 * Did this tap land on the card?
 *
 * The WHOLE CARD and not just the close box. A card is the only thing
 * on that part of the screen and a person dismissing it aims at the
 * text as readily as at the corner; a 40 px target inside a 600 px one
 * is a hit box that is technically discoverable and practically not.
 * The close box is drawn so that it is obvious the card CAN be
 * dismissed, not so that it is the only way to.
 *
 * False when no card is up, or when the one that is cannot be
 * dismissed -- so the caller does not have to ask first.
 */
bool ui_notice_hit(int x, int y);

/*
 * Forget the card without painting anything.
 *
 * For the caller that is about to repaint the artwork anyway, which is
 * every caller: clearing and then blitting the cover twice would show
 * the card's background for a frame.
 */
void ui_notice_clear(void);

/*
 * ANYTHING THAT PAINTS THE ARTWORK SQUARE MUST DROP THE CARD.
 *
 * ui_clear_art() and ui_show_art_info() do it themselves. albumart_show()
 * is not in this file and cannot, so the caller that blits a cover has
 * to call ui_notice_clear() first -- otherwise the card is gone from
 * the screen while ui_notice_hit() still answers for it, and every tap
 * on the cover is swallowed by a card that is not there.
 */

/* Repaint the bar. Cheap enough to call at 20 Hz: it touches only the
 * bottom UI_BAR_H rows, never the cover art above them. */
void ui_draw(const ui_state_t *st);

/* Height of the envelope standing on the seek baseline.
 *
 * 64 px of upper sideband. The mirrored version needed twice this and
 * could only get it in the artwork area; half the shape carries all of
 * the information, so it fits here. */
#define UI_WAVE_H   (72)

/*
 * Whether anything on the bar is mid-animation.
 *
 * Exactly one thing is: a title too long for the panel, sliding. The UI
 * task polls at 10 Hz when no finger is down, which is right for a
 * seconds-resolution clock and visibly wrong for something moving --
 * 10 Hz reads as a title jumping in steps rather than travelling. Asking
 * costs nothing and means the faster rate is paid for only while there is
 * something to see.
 */
bool ui_animating(void);

/* Feed one poll of the touch controller. down=false means no finger.
 * Returns the action this poll produced, if any.
 *
 * Drags are tracked across calls, which is why this takes raw state
 * rather than taps: the sliders need continuous movement, and the finger
 * bubble has to follow it. */
ui_action_t ui_touch(const ui_state_t *st, bool down, int x, int y);

/* Height of the bar at the bottom of the screen. Everything above it
 * belongs to the cover art. */
/*
 * Grew by 40 px when album moved off the artist line onto its own. That
 * is one scale-3 row (24 px) plus the gap that keeps the three rows from
 * reading as a block of text.
 *
 * It comes out of the artwork, which is the only place it can come from.
 * At 1280 the cover had 1004 rows and now has 964; a 500x500 cover is
 * unaffected either way, and one large enough to be cropped loses 20
 * rows top and bottom.
 */
/*
 * THE CONTROL BLOCK IS A 720x720 SQUARE, and the artwork gets what is
 * left of the long edge: 1280 - 720 = 560.
 *
 * Derived rather than chosen, and derived twice. The bar used to be
 * sized to its contents and the artwork got the remainder, so the cover
 * was 964 rows in a 720 px column and albumart.c letterboxed it with 122
 * rows of black above and below. Making the cover square handed those
 * rows to the controls: 720 art, 560 bar.
 *
 * This is the reverse of that, for landscape. The panel is 720 on its
 * short edge, so a control block that is to be the same block at every
 * angle can be at most 720x720 -- and if it is to be that at 90 and 270
 * as well, it must be exactly 720x720, because 720 is the whole short
 * edge. That fixes the artwork at 1280-720 = 560. There is no freedom in
 * the split; naming UI_SQUARE first and subtracting is what says so.
 *
 * So the artwork band is 560x720 in landscape and 720x560 in portrait --
 * the same rectangle turned -- and the controls are one square laid out
 * from its own origin, which is at x=560 in landscape and y=560 in
 * portrait. The ROWS do not turn: reading order is top to bottom at
 * every angle, and the envelope and the volume slider are horizontal
 * controls at every angle. What is shared is the square's extent, the
 * row offsets inside it and the hit grid -- not a rotated bitmap.
 *
 * UI_ART_H is the portrait name and stays, because media_task's band
 * ownership is written in terms of it: it owns rows 0..UI_ART_H-1. In
 * landscape the art is not a band of rows at all, and ui_art_band()
 * is what callers ask instead.
 */
#define UI_SQUARE   (720)
#define UI_ART_H    (1280 - UI_SQUARE)
#define UI_BAR_H    (UI_SQUARE)

/*
 * Where the artwork lives at the current angle, in logical coordinates.
 *
 * Portrait: x=0, w=720, y=0, h=560 -- rows 0..559, still a band, and
 * still the band media_task owns.
 * Landscape: x=0, w=560, y=0, h=720 -- a column. A caller blitting it
 * must blit rows 0..719, which is every row, because a column is not a
 * band. ui_blit_art() does that so no caller has to know.
 */
void ui_art_band(int *x, int *y, int *w, int *h);
void ui_blit_art(void);

/* Same, returning the panel's error. albumart.c wants it: a cover that
 * failed to reach the glass must not be reported as shown, which is the
 * same reason gfx_blit_err() exists. */
esp_err_t ui_blit_art_err(void);

/* The control square's own blit. In landscape it sends every row,
 * because the square is a column -- see ui.c. */
void ui_blit_bar(void);

/* True when a logical point is over the artwork rather than the square.
 * Replaces the `y < s_bar_top` test, which is only the artwork in
 * portrait. */
bool ui_in_art(int x, int y);

/*
 * Re-read gfx_w()/gfx_h() and lay the square out again.
 *
 * Called after gfx_set_rotation(), because a turn between portrait and
 * landscape swaps the logical extent and every bound in this file is
 * derived from it. Does not draw; the caller repaints.
 */
void ui_relayout(void);

/* True when the square is to the right of the artwork rather than below
 * it. A few callers lay out per orientation; they ask this. */
bool ui_landscape(void);

#ifdef __cplusplus
}
#endif
