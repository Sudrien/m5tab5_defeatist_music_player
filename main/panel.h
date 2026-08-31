/*
 * panel.h -- the settings panel.
 *
 * Four tabs behind the gear on the transport bar: what is in the card
 * slot, what is on the USB port, what this firmware is, and the one
 * preference that is worth a switch.
 *
 * It is deliberately shaped like browser.c rather than like a settings
 * screen from a phone. Same full-screen takeover, same tab strip, same
 * footer, same open/draw/touch triple driven from ui_task -- because
 * there is exactly one writer to the framebuffer in this program and
 * adding a second screen is not a reason to acquire a second one.
 *
 * WHY THREE OF THE FOUR TABS ARE READ-ONLY
 *
 * Because the questions they answer are the ones actually asked of this
 * player, and none of them had anywhere to be asked. "Is the card
 * running at full speed", "did the drive enumerate", "which build is
 * this" were all logged once at boot and then only existed in a serial
 * console -- which is not where anyone is standing when a track stutters
 * or a drive fails to appear. A panel that shows them is not a
 * diagnostic luxury; it is the difference between a report that says
 * "sometimes it skips" and one that says which card and at what clock.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Put the panel up. Draws nothing itself -- panel_draw() does that, on
 * the task that owns the framebuffer. */
void panel_open(void);
void panel_close(void);
bool panel_is_open(void);

/*
 * Repaint if anything has changed.
 *
 * Cheap to call every frame: it returns immediately unless something
 * has dirtied it. Three things do -- a touch, a mount or unmount (the
 * storage generation), and the passage of a second, because the heap
 * figures and the USB state are live numbers and a panel showing the
 * heap as it was when it opened is worse than one that does not show
 * the heap at all.
 */
void panel_draw(void);

/*
 * A press. Returns true when the panel wants to close, which is the
 * only thing the caller has to act on -- every other press is handled
 * internally and changes nothing outside this file except the one
 * setting.
 *
 * Same edge detection as browser_touch(): down is the raw touch state,
 * and a press is the transition. The caller passes what it already
 * sampled rather than this file polling separately, so the chooser and
 * the panel cannot disagree about whether a finger is down.
 */
bool panel_touch(bool down, int x, int y);

#ifdef __cplusplus
}
#endif
