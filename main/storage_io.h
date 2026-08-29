/*
 * storage_io.h -- one arbiter for the card, because priority is not a
 * device throttle and neither is a delay.
 *
 * WHAT WAS THERE BEFORE
 *
 * Three tasks open files on the same volume: the decode loop (through
 * decoder.c), the decode loop again at a track change (tags, size), and
 * media_task (cover, envelope, and the whole of prefetch). Nothing
 * serialised them. What stood in for it was media_settle() -- a delay,
 * then a ring-occupancy floor -- and prefetch_ok(), which is the same
 * floor checked between stages.
 *
 * That is a throttle, and a throttle is a guess. It says "probably not
 * now" before a read starts and has nothing at all to say once one is in
 * flight. The failure it cannot see is the one that matters: media_task
 * enters a 32 KB fread, FatFs takes the volume lock, and the decoder's
 * next refill blocks behind it. The decode loop stops. Because the decode
 * loop stops, it stops publishing s_ring_pct. The gauge freezes at
 * whatever it last read -- which is above the floor by construction,
 * because that is the gate the background read had to pass to start -- so
 * the abort never fires. player.c already documents this exact loop, and
 * answered it by adding a second publisher on the writer task. That made
 * the gauge honest. It did not stop the read.
 *
 * WHAT THIS IS
 *
 * A lease on the device, taken per operation, granted by class rather
 * than by task priority. Priority arbitrates the CPU; this arbitrates the
 * queue that the CPU is waiting on.
 *
 *   STORAGE_IO_PLAYBACK     the decode loop, and nothing else
 *   STORAGE_IO_PREFETCH     the next track's tags, cover, envelope
 *   STORAGE_IO_BACKGROUND   the playing track's envelope, listings
 *
 * A lower number always wins. When the lease is free, the highest class
 * with somebody waiting takes it; when it is held, everyone else waits.
 *
 * THE POINT IS NOT SHORTER READS, IT IS INTERRUPTIBLE ONES
 *
 * storage_io_fread() does not read less than it was asked for. It reads
 * the same bytes in STORAGE_IO_CHUNK pieces and drops the lease between
 * them, so a playback read that arrives mid-way waits for one chunk
 * rather than for a whole cover or a whole file. That bounds the
 * decoder's worst case at one chunk of the slowest device, which is the
 * number this file exists to make small and knowable:
 *
 *   16 KB on a healthy SD card      ~2 ms
 *   16 KB on the USB drive that
 *   produced the 11 s stalls        unbounded, but one chunk of it
 *
 * The second row is the honest one. This does not fix a device that
 * stalls for eleven seconds; nothing here can. It stops that stall being
 * multiplied by however many background readers happened to be queued.
 *
 * LEAF-LEVEL ONLY
 *
 * Wrap the fread, never the parse. covertag.c and duration.c both funnel
 * every read through their own read_at(), which is why those two files
 * change by one function each: the lease is taken and dropped around each
 * fread inside the parser, not around the parser.
 *
 * Taking it around the parser instead would compile, run, and silently
 * put the starvation back -- one lease held across the whole of
 * covertag_extract_art() is exactly the 512 KB uninterruptible read this
 * replaces. The nesting check below exists to make that mistake loud
 * rather than invisible, because it is the one way to use this file that
 * looks correct and is not.
 *
 * WHAT THIS DOES NOT DO
 *
 * It does not know about volumes. One lease covers the card and the USB
 * port together, which is wrong in principle -- a cover read from USB
 * does not contend with a decode from SD -- and right in practice for
 * now, because nothing plays from one while reading the other. When that
 * stops being true this grows a lease per storage_id_t and the class
 * comparison happens within a volume.
 *
 * It does not replace the abort flags. A lease decides who reads next; it
 * has no opinion on whether a read is still wanted. s_scan_abort and
 * s_prefetch_abort still answer that, and storage_io_should_yield() is
 * the third question -- "should I stop even though I still want this" --
 * which a long background loop should be polling anyway.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lower is more urgent. The order is the whole policy.
 *
 * PREFETCH sits above BACKGROUND because the next track's cover is worth
 * more than the playing track's envelope: the cover is the largest thing
 * on screen and its absence reads as a stall, and the envelope's absence
 * is a plain slider that fills in later. That is the same ranking
 * media_task already applies by running them in order; it is now enforced
 * when they are on different tasks rather than assumed because they were
 * on the same one.
 */
typedef enum {
    STORAGE_IO_PLAYBACK   = 0,
    STORAGE_IO_PREFETCH   = 1,
    STORAGE_IO_BACKGROUND = 2,
    STORAGE_IO_CLASSES
} storage_io_class_t;

/*
 * How much a background reader may hold the lease for at once.
 *
 * This is the decoder's worst-case wait, so it wants to be small. It is
 * also the read size the card sees, so it wants to be large -- 16 KB is
 * 32 sectors and comfortably past the point where per-read overhead
 * dominates on this bus.
 *
 * framewalk.c reads 32 KB at a time into its own buffer and now gets that
 * as two leases rather than one. Its buffer size is unchanged; only the
 * number of times it lets go is.
 */
#define STORAGE_IO_CHUNK    (16 * 1024)

/*
 * THE REQUEST SIZE, AND TWO THINGS IT IS NOT
 *
 * Throughput has now been measured at 577, 576, 571 and 569 KB/s across
 * six windows, two files, two bus clocks and both with and without DMA
 * staging. It has not moved. Two explanations are therefore dead:
 *
 *   0102  SDMMC_FREQ_DEFAULT -> SDMMC_FREQ_HIGHSPEED, 20 to 40 MHz.
 *         The card accepted it (the banner reads 40000 kHz) and
 *         throughput moved about 4%.
 *   0103  Staging every read through an internal DMA-capable buffer,
 *         on the theory that sdmmc_read_sectors() was falling back to
 *         its sector-at-a-time path. The `bounced` counter confirmed
 *         the staging happened -- 77 of 77 reads, 94 of 94 -- and
 *         throughput moved 1 KB/s.
 *
 * Both of those live at or below the driver. So the constraint is above
 * it: not how fast a request moves bytes, but how large the requests are
 * and therefore how many there are.
 *
 * 576 KB/s is 1127 requests per second at 512 bytes, or 0.89 ms per
 * single-sector round trip through the VFS lock, FatFs and the driver.
 * That is what one sector per request looks like, and it looks the same
 * at any clock and with any destination -- which is precisely the
 * invariance the last two patches measured.
 *
 * WHY THE REQUESTS ARE THAT SIZE
 *
 * newlib's fread() does not hand a large request to the layer below.
 * Unlike glibc, which bypasses its buffer for reads larger than it,
 * newlib loops on __srefill_r and refills fp->_bf._size bytes at a time,
 * so everything reaches f_read() one stdio buffer at a time no matter
 * what the caller asked for. minimp3 asks for 128 KB; the arbiter
 * faithfully measured 111 KB per lease; FatFs underneath saw neither.
 *
 * newlib sizes _bf from st_blksize, which for this VFS comes from
 * CONFIG_FATFS_VFS_FSTAT_BLKSIZE -- a value sdkconfig.defaults does not
 * set. storage_io_open() logs the st_blksize it found on the first open,
 * so this stops being an inference.
 *
 * WHAT THIS DOES
 *
 * fopen(), then setvbuf() with a buffer from a small preallocated pool of
 * internal, cache-aligned, DMA-capable blocks. That sets the request size
 * AND makes the destination the driver finally sees DMA-capable, which is
 * what 0103 was trying to do from the wrong end -- with stdio buffering
 * the bytes land in the stdio buffer first, so the caller's PSRAM pointer
 * was never what the driver was given. 0103's staging is removed here
 * rather than kept: on top of a buffered stream it is a third copy of
 * every byte to solve a problem it has already been measured not to
 * solve.
 *
 * SAME SHAPE OF GUESS AS THE ONE THAT JUST FAILED
 *
 * "A layer below is chunking smaller than we think" is exactly the class
 * of hypothesis 0103 was, so it gets the same explicit test: if the
 * stdio buffer goes to 16 KB and throughput does not move, then the
 * constraint is the card's own per-request latency and no amount of
 * restructuring above it will help. The st_blksize line settles it
 * either way, and it is worth having even if the fix does nothing.
 *
 * Only decoder.c is converted here, which is where 99% of the bytes are.
 * The rest follow once this is measured rather than before -- the lesson
 * of 0103 being that the cheap test comes first.
 */
/*
 * Safe to call before this is initialised: every function degrades to
 * calling straight through to stdio, which is exactly the old behaviour.
 * That is deliberate -- settings.c reads before app_main() has got here,
 * and a boot-order mistake should cost arbitration rather than the read.
 */
void storage_io_init(void);

/*
 * fopen() with a large internal DMA-capable stdio buffer attached.
 *
 * Falls back to a plain fopen() when the pool is empty or setvbuf()
 * refuses, so the result is always usable and never NULL for a reason
 * this file invented. Close with storage_io_close() -- a plain fclose()
 * still works and still closes the file, it just strands a pool slot
 * until reboot, which is logged.
 */
FILE *storage_io_open(const char *path, const char *mode);
int   storage_io_close(FILE *f);

/*
 * Take and drop the lease.
 *
 * Not recursive by design. A second acquire from a task that already
 * holds it is a bug in the caller -- it means a lease is being held
 * across something bigger than one read -- and it is logged as one. It is
 * then honoured with a depth count rather than deadlocking, because a
 * hang here takes the audio with it and a log line does not.
 */
void storage_io_acquire(storage_io_class_t cls);
void storage_io_release(void);

/*
 * Is somebody more urgent queued behind me?
 *
 * For loops that read for a long time and can stop cleanly. The lease
 * itself is dropped between chunks, so this is not needed to be polite;
 * it is needed to give up entirely. framewalk.c does not poll it -- its
 * caller's abort flag already covers the case that matters -- but a
 * future whole-file pass that has no abort flag should.
 */
bool storage_io_should_yield(storage_io_class_t cls);

/*
 * fread(), in chunks, with the lease dropped between them.
 *
 * Same return as fread(dst, 1, len, f): bytes actually read, short only
 * at EOF or on error. Do not call while holding a lease.
 */
size_t storage_io_fread(void *dst, size_t len, FILE *f,
                        storage_io_class_t cls);

/*
 * fseek() then storage_io_fread(), with the seek inside the first lease
 * so nothing can move the file position in between.
 *
 * Returns true only on a full-length read, which is what covertag.c and
 * duration.c both already tested for.
 */
bool storage_io_read_at(FILE *f, long off, void *dst, size_t len,
                        storage_io_class_t cls);

/*
 * Longest any class has waited for the lease, in milliseconds, and how
 * many times it has taken one. Reset by the read.
 *
 * PLAYBACK's number is the one that matters: it is the decoder's observed
 * worst case, and if it is not roughly one chunk of the current device
 * then somebody is holding a lease across a parse. That is what this
 * exists to make visible -- the failure mode it replaces was invisible
 * precisely because the number that would have shown it was frozen.
 */
/*
 * Everything the arbiter knows about one class, for one window.
 *
 * A struct rather than six out-parameters because the list stopped being
 * memorable at four, and because the interesting figures are ratios
 * between members rather than any one of them:
 *
 *   bytes / held_ms       throughput while actually inside a read. THIS
 *                         is the card's speed. Bytes over wall-clock is
 *                         not, and 0101 reported the latter and called
 *                         it the former -- 8551 KB over a 15.4 s open
 *                         read as 555 KB/s, but the open is a read and a
 *                         parse taking turns and that number is their
 *                         average, not the card's rate.
 *   held_ms / phase_ms    how much of the window was spent in I/O at
 *                         all. Low means the time went somewhere else,
 *                         and on the open the only somewhere else is
 *                         minimp3 walking frame headers.
 *   worst_hold_ms         the longest single uninterruptible read. This
 *                         is the floor on control latency, because the
 *                         decode loop cannot look at a button while it
 *                         is inside one -- which is what the 229 ms
 *                         "seek waited for the decode loop" was.
 *   worst_wait_ms         the longest anything waited for the lease.
 *                         This is the arbiter's own cost, and the only
 *                         number here that 0100 can be blamed for.
 *
 * Reading resets, so two reports bracket a window.
 */
typedef struct {
    uint32_t reads;
    uint64_t bytes;
    uint32_t held_ms;           /* summed time inside fread() */
    uint32_t worst_hold_ms;     /* longest single lease */
    uint32_t worst_wait_ms;     /* longest wait to be granted one */
} storage_io_stat_t;

void storage_io_stats(storage_io_class_t cls, storage_io_stat_t *out);

/* Wall-clock milliseconds since the last report, and reset. The
 * denominator for held_ms. */
uint32_t storage_io_phase_ms(void);

/*
 * Log every class that did anything, then reset.
 *
 * `phase` names the window being closed, so two calls bracket a span:
 * report("open") right after decoder_open() attributes everything the
 * open did to the open, and report("track") at the end attributes the
 * rest to playback. Without the phase the two are one undifferentiated
 * total and the expensive half cannot be told from the cheap one.
 */
void storage_io_report(const char *phase);

#ifdef __cplusplus
}
#endif
