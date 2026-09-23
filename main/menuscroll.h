/*
 * menuscroll.h -- one vertical scrollbar, for the settings pages.
 *
 * Header-only and arithmetic-only, the way cardtime.h is, so the host
 * suite can test it without building anything that talks to a panel.
 *
 * WHY THIS AND NOT A DRAG ON THE CONTENT. Both pages that use it are
 * made mostly of horizontal sliders whose drag starts anywhere in the
 * row. A content drag would have to decide, from the first few pixels
 * of a gesture, whether a finger that has moved down and left means
 * "scroll" or "quieter", and it would be wrong often enough to notice.
 * A bar at the right edge cannot be confused with anything; browser.c
 * reached the same conclusion for the same reason and this is its
 * idiom, in pixels instead of rows because these pages have rows of
 * five different heights.
 *
 * The viewport is the band between the header and the footer. Content
 * is laid out from `view_y` as though it were unbounded, and drawn
 * shifted up by `off`. There is no clipping in gfx, so a page CLIPS BY
 * OVERDRAW: draw the content, then the header, then the footer, both
 * opaque. The footer already worked that way (5006); the header has to
 * be moved to after the content for this to hold.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MENUSCROLL_H
#define MENUSCROLL_H

#include <stdbool.h>

/* No shorter than a thumb, however long the content is. */
#define MENUSCROLL_MIN_BAR  (72)
/* Drawn this wide at the right edge... */
#define MENUSCROLL_W        (16)
/* ...and hit this wide, browser.c's reason: sixteen pixels is a fine
 * thing to look at and a poor thing to aim for.
 *
 * Much narrower than browser.c's 72, and the width is not a taste: the
 * brightness and timer sliders' knobs reach to 38 px from the right
 * edge, and a strip that overlapped them would turn "drag the
 * brightness to full" into "scroll". 32 clears that by six pixels.
 * menuscrolltest asserts it, which is how 40 was caught. */
#define MENUSCROLL_HIT_W    (32)

typedef struct {
    int  off;       /* pixels of content scrolled off the top */
    bool drag;      /* a press on the bar, held across polls */
} menuscroll_t;

static inline int menuscroll_max(int content_h, int view_h)
{
    const int m = content_h - view_h;
    return m > 0 ? m : 0;
}

static inline int menuscroll_clamp(int off, int content_h, int view_h)
{
    const int m = menuscroll_max(content_h, view_h);
    if (off < 0) return 0;
    if (off > m) return m;
    return off;
}

/*
 * The bar's rectangle, or false when everything fits. False is also the
 * answer to "should anything be drawn" and to "can this be dragged" --
 * one question, so the two cannot disagree.
 */
static inline bool menuscroll_geom(const menuscroll_t *s, int content_h,
                                   int view_y, int view_h,
                                   int *bar_y, int *bar_h)
{
    /* Set before the refusal, not after: a caller that ignores the
     * return value gets a bar of nothing rather than a bar of
     * whatever was on its stack. */
    *bar_y = view_y;
    *bar_h = 0;

    const int m = menuscroll_max(content_h, view_h);
    if (m <= 0 || view_h <= 0) return false;

    int bh = (int)(((long)view_h * view_h) / content_h);
    if (bh < MENUSCROLL_MIN_BAR) bh = MENUSCROLL_MIN_BAR;
    if (bh > view_h) bh = view_h;

    const int travel = view_h - bh;
    const int off = menuscroll_clamp(s->off, content_h, view_h);
    *bar_h = bh;
    *bar_y = view_y + (travel > 0 ? (int)(((long)travel * off) / m) : 0);
    return true;
}

/*
 * A press in the strip, from anywhere in it. The bar CENTRES on the
 * finger rather than keeping the grab point, which is browser.c's
 * choice: it makes press-anywhere and drag the same gesture, so a jab
 * at the bottom of the track goes to the bottom of the content instead
 * of nudging by a bar's height.
 *
 * Returns true when it consumed the press.
 */
static inline bool menuscroll_touch(menuscroll_t *s, bool down, bool tapped,
                                    int x, int y, int w,
                                    int content_h, int view_y, int view_h)
{
    int bar_y, bar_h;
    if (!menuscroll_geom(s, content_h, view_y, view_h, &bar_y, &bar_h)) {
        s->drag = false;
        return false;
    }
    if (!s->drag && !(tapped && x >= w - MENUSCROLL_HIT_W &&
                      y >= view_y && y < view_y + view_h)) {
        return false;
    }
    if (!down) {
        const bool was = s->drag;
        s->drag = false;
        return was;
    }
    const int travel = view_h - bar_h;
    const int m = menuscroll_max(content_h, view_h);
    int pos = y - view_y - bar_h / 2;
    if (pos < 0) pos = 0;
    if (pos > travel) pos = travel;
    s->off = travel > 0 ? (int)(((long)pos * m + travel / 2) / travel) : 0;
    s->drag = true;
    return true;
}

#endif /* MENUSCROLL_H */
