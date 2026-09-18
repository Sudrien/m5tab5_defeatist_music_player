/*
 * bench.h -- how fast can this player actually pull bytes?
 *
 * WHY THIS EXISTS, AND WHY IT IS NOT A SPEEDTEST.
 *
 * A stream that will not play has three possible culprits and the logs
 * cannot separate them: the network, the transport in this player, or
 * the decode loop consuming too slowly to keep the ring draining. An
 * evening was spent guessing between them. A phone speedtest ruled out
 * the network and told us nothing about the other two, because it
 * measured a different radio on a different band.
 *
 * A BULK FILE DOWNLOAD WOULD HAVE THE SAME PROBLEM in a subtler form.
 * Bulk transfer runs flat out with large reads and nothing consuming.
 * Streaming here is 2048-byte reads through TLS, an ICY demux, a 256 KB
 * ring with backpressure, and a decode loop competing for the same CPU
 * and the same SDIO link to the co-processor. Those fail in different
 * places, and a bulk number that looked healthy would be a green light
 * that meant nothing.
 *
 * So this measures the REAL path with exactly one variable removed.
 * netstream connects to a real station exactly as it would to play it
 * -- same URL, same TLS, same reads, same demux, same ring -- and the
 * bytes are read and thrown away instead of decoded. What comes back is
 * how fast the transport can deliver when nothing is consuming.
 *
 * Reading the result:
 *
 *   drain is fast, playback is slow   the transport is fine; look at
 *                                     the decode loop and the ring
 *   drain is slow too                 it is the transport or below,
 *                                     and the decoder is innocent
 *
 * A CEILING, NOT A SPEED. A server that paces itself cannot be made to
 * go faster by not decoding it: SomaFM at 128 kbit/s will read 128
 * kbit/s here and that is the server's answer, not this player's. The
 * number means something only against a station that sends as fast as
 * it is taken -- the lossless ones do, and WNZK has previously outrun
 * the ring entirely. `declared_kbps` is carried in the result so the
 * reading can be judged against what the station said it needs.
 *
 * NOTHING MAY BE PLAYING. Serviced only from player_loop(), the idle
 * loop, where no track is decoding and no stream is open. That is a
 * structural guarantee rather than a check: a measurement taken while
 * the decoder competes for the same link would measure the two of them
 * together, which is the thing it exists to take apart.
 */
#ifndef BENCH_H
#define BENCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How long the drain runs, once connected. Long enough for TCP to open
 * its window and for a slow start to stop dominating the average, short
 * enough that nobody walks away from it. */
#define BENCH_RUN_MS        (12000)

/* Given up on if the station will not connect in this. Shorter than
 * netstream's own retry ladder on purpose: a measurement that spends
 * thirty seconds failing to connect has measured the station, not the
 * player, and should say so quickly. */
#define BENCH_CONNECT_MS    (8000)

typedef struct {
    bool     running;       /* a run is in progress right now */
    bool     have;          /* a finished result is in here    */

    char     name[48];      /* the station measured */

    /* Mean over the whole drain, and the best one-second window in it.
     * Both, because they answer different questions: the mean is what a
     * stream would live on, and the peak is what the path can do when
     * it is having a good moment. A large gap between them is a link
     * that stalls rather than one that is simply slow. */
    int      kbps_avg;
    int      kbps_peak;

    /* What the station says it needs, when it says anything -- the ICY
     * header, or the decoder's own figure from a previous play. 0 when
     * unknown, which is common and not an error. */
    int      declared_kbps;

    uint32_t bytes;         /* drained in total */
    uint32_t ms;            /* actually spent draining */
    int      connect_ms;    /* how long the connect and TLS took */

    /* Empty on success. Why it stopped otherwise, in words meant for
     * the panel rather than for a log. */
    char     note[64];
} bench_result_t;

/*
 * Ask for a run against the currently selected station.
 *
 * Returns false and sets nothing if there is no station to measure, or
 * if a run is already in progress. Safe from ui_task: this only raises
 * a flag, and the work happens on the player task.
 */
bool bench_request(void);

/* A snapshot, filled into the caller's storage -- portal.h's reasoning
 * about values rather than pointers applies here for the same reason. */
void bench_state(bench_result_t *out);

/* PLAYER TASK ONLY, and only from player_loop(). Blocks for up to
 * BENCH_CONNECT_MS + BENCH_RUN_MS while a run is pending; returns
 * immediately when none is. */
void bench_service(void);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_H */
