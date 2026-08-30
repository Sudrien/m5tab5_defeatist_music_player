/*
 * player_diag.h -- the one-build diagnostics, in one place.
 *
 * These are not options. Each one disables something the player needs in
 * order to answer a question about the cyan flash, and each comes out
 * again once it has. They live here rather than in player.c because
 * decoder.c has to see them too, and a flag defined in a .c file that
 * another .c file silently does not see is a way to run an experiment
 * that did not happen.
 *
 * The rule for everything in this file: default 0, and a build with any
 * of them at 1 is a build that is measuring, not one that is working.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

/*
 * Skip minimp3's up-front seek index.
 *
 * At a track boundary the next track's decoder_open() reads the whole
 * file to build it -- 6.4 MB at ~8.6 MB/s for this material -- while the
 * outgoing track still has up to twenty seconds in its ring. That is the
 * "T-20" the flash has been timed against since the first note, and it
 * is the one candidate every previous patch throttled rather than
 * removed: 0403 paced the decode, 0408 paced the reads, 0411 changed
 * where they landed. None of them stopped the open happening.
 *
 * Set to 1 and MP3D_DO_NOT_SCAN replaces MP3D_SEEK_TO_SAMPLE. A boundary
 * becomes an open of a few kilobytes.
 *
 * Still flashes: the open is not it, and what remains at a boundary is
 * the ring handoff.
 *
 * Stops flashing: it is the index build, and there is a real fix --
 * build it lazily on the first seek, or cache it in the sidecar that
 * already carries the envelope and the loudness.
 *
 * Cost while set: no duration and no seek bar, for every track.
 */
#define BOUNDARY_NO_INDEX       (0)
