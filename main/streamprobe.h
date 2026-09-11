/*
 * streamprobe.h -- a throwaway measurement, not a feature.
 *
 * v0.4.0 is internet radio, and before any of the stream path is written
 * three things want measuring on this board: what a real station's HTTP
 * looks like (redirects, headers, ICY), what HTTPS costs in time and
 * internal heap, and what the SDIO-attached C6 actually delivers in bytes
 * per second. The first stream anyone plans to use is WNZK on Zeno.FM.
 *
 * IT NO LONGER OPENS A CONNECTION (0105)
 *
 * netstream.c does that now, and this file's job has changed from
 * measuring the network to **saying whether netstream handles it
 * correctly.** It asks netstream to play STREAMPROBE_URL and then drains
 * the ring exactly as phase 2's decoder will -- netstream_read() with a
 * timeout, in a loop -- for STREAMPROBE_SECONDS.
 *
 * That moves the measurement to the far side of the parts that are new
 * and unproven. The old probe counted ADTS frames off the socket and saw
 * zero bytes lost hunting for a sync; this one counts them after the ICY
 * demultiplexer and after the ring, so a desynchronised demuxer, a ring
 * that drops, or a reconnect that loses its place all show up as lost
 * bytes and a falling x-real-time ratio against a figure we already have
 * for the same station.
 *
 * It also logs what only this side can see: every state transition with
 * its timing, how full the ring gets, how often a reader that is faster
 * than the network finds it empty, and how long netstream_stop_wait()
 * takes -- which is the number phase 3 needs for its pause.
 *
 * It decodes nothing and draws nothing, and playback carries on beside it
 * -- which is part of what is being measured.
 *
 * Empty STREAMPROBE_URL compiles it to nothing. It goes when the stream
 * path lands; until then it is the only thing that exercises netstream
 * on hardware.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define STREAMPROBE_URL      "https://stream.zeno.fm/erunhwj5lekvv"
#define STREAMPROBE_SECONDS  (60)
/*
 * Seconds after the first address before the probe starts. Long enough
 * to start a track from the card by hand, so the second run measures the
 * download beside playback, which is what the stream path will have to do.
 */
#define STREAMPROBE_DELAY_S  (45)

/* Called when the station has an address. Starts the probe the first
 * time only; cheap and safe from the event task. */
void streamprobe_kick(void);

#ifdef __cplusplus
}
#endif
