#!/usr/bin/env python3
"""
streamcheck.py -- measure what a PC gets from an internet radio station,
in the same terms the Tab5 reports, so the two can be compared directly.

WHY THIS EXISTS

The player showed WNZK starving: a 512 kbit/s AAC stream arriving at a
mean of 466 kbit/s, draining its buffer from 4 s to 1.4 s and dropping
out every 35 seconds. The same station on a PC has no trouble. Either
the station throttles the device, or the device cannot drain the socket
fast enough -- and those have opposite fixes.

This measures the PC side. It does not decode anything: it opens the
stream exactly as the player does, reads bytes, and reports delivery
rate against the station's own declared bitrate. If the PC also gets
0.91x, the station is the problem. If the PC gets 1.00x or better, the
device's transport is, and the prime suspect is already named in the
device's own boot log:

    esp-hosted fw versions: host=3.0.7 coprocessor=0.0.0
    CP without SDIO SW_AGGR; compatible streaming mode enabled

WHAT IT PRINTS

Per window, a line shaped like the firmware's:

    t= 15.0s   462 kbit/s   1.01x   audio  4.12s   read 38 ms

and at the end a verdict. The `audio` column is a simulation of the
player's PCM reserve -- 4 s of preroll, then credited at the declared
bitrate and debited in real time -- so a PC run and a device log can be
put side by side and read the same way. That is the whole point; a bare
throughput number in different units from the firmware's would have to
be converted by hand every time, which is how the byte ring and the
audio reserve got confused in the first place.

DELIBERATELY NOT USED: urllib, requests. SHOUTcast servers answer with
`ICY 200 OK` instead of an HTTP status line, which the standard library
parsers reject outright, and a tool for diagnosing radio streams that
cannot open a SHOUTcast mount is not much of a tool. Raw sockets also
mean the redirect chain is visible hop by hop, which is how the
firmware logs it (`hop 1: ... 302 -> redirect`), and WNZK's problems
have all been on the second hop.

Stdlib only. No pip install.

USAGE

    python3 streamcheck.py                      # the four test stations
    python3 streamcheck.py --duration 300       # five minutes, like the device run
    python3 streamcheck.py URL [URL ...]
    python3 streamcheck.py --m3u stations.m3u
    python3 streamcheck.py --csv wnzk.csv URL   # windows to a file

Ctrl-C stops the current station and moves to the next.
"""

import argparse
import csv
import socket
import ssl
import sys
import time
from urllib.parse import urlsplit, urlunsplit

# The two stations whose behaviour the firmware has measured, so a run
# with no arguments reproduces the comparison that prompted this.
DEFAULT_URLS = [
    "https://26433.live.streamtheworld.com/WUOMFM.mp3",   # 64k MP3 mono
    "https://stream.zeno.fm/erunhwj5lekvv",               # 512k AAC stereo
    "http://ice1.somafm.com/groovesalad-128-mp3",         # 128k MP3 stereo
]

# Matching the firmware so the columns line up.
WINDOW_S = 5.0          # KBPS_WINDOW_US
PREROLL_S = 4.0         # BUFPLAN_START_MS
LOW_S = 1.0             # BUFPLAN_LOW_MS
RESUME_S = 4.0          # BUFPLAN_RESUME_MS

MAX_HOPS = 5
CONNECT_TIMEOUT_S = 10.0
READ_TIMEOUT_S = 15.0
READ_CHUNK = 16384      # the firmware's storage/stream chunk size

# The player sends this, and it changes what the server sends back: with
# metadata interleaved, a fraction of every stream is not audio. Measuring
# with it off would overstate the audio rate by that fraction -- small,
# but the whole question is a 9% shortfall.
USER_AGENT = "streamcheck/1.0 (comparing against DefeatistMusicPlayer)"


class Hop:
    def __init__(self, url):
        self.url = url
        self.status = None
        self.headers = {}
        self.connect_ms = 0.0


def open_stream(url, verbose=True):
    """
    Follow the redirect chain and return (socket, headers, leftover, hops).

    `leftover` is body bytes that arrived in the same packet as the
    headers. Dropping them would undercount the first window, which is
    exactly the window that shows whether a station front-loads.
    """
    hops = []
    for hop_n in range(1, MAX_HOPS + 1):
        parts = urlsplit(url)
        https = parts.scheme == "https"
        host = parts.hostname
        port = parts.port or (443 if https else 80)
        path = urlunsplit(("", "", parts.path or "/", parts.query, ""))

        hop = Hop(f"{parts.scheme}://{host}:{port}{path}")
        hops.append(hop)
        if verbose:
            print(f"  hop {hop_n}: {hop.url}")

        t0 = time.monotonic()
        sock = socket.create_connection((host, port), CONNECT_TIMEOUT_S)
        if https:
            ctx = ssl.create_default_context()
            sock = ctx.wrap_socket(sock, server_hostname=host)
        hop.connect_ms = (time.monotonic() - t0) * 1000.0

        req = (
            f"GET {path} HTTP/1.0\r\n"
            f"Host: {host}\r\n"
            f"User-Agent: {USER_AGENT}\r\n"
            f"Icy-MetaData: 1\r\n"
            f"Accept: */*\r\n"
            f"Connection: close\r\n"
            f"\r\n"
        )
        sock.sendall(req.encode("ascii"))

        # Read to the end of the headers, keeping whatever body came with
        # them.
        sock.settimeout(READ_TIMEOUT_S)
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = sock.recv(4096)
            if not chunk:
                sock.close()
                raise IOError("connection closed before headers")
            buf += chunk
            if len(buf) > 64 * 1024:
                sock.close()
                raise IOError("headers never ended")

        head, leftover = buf.split(b"\r\n\r\n", 1)
        lines = head.decode("latin-1").split("\r\n")

        # `ICY 200 OK` as well as `HTTP/1.x 200 OK`. This is the line the
        # standard library refuses.
        status_line = lines[0]
        bits = status_line.split(None, 2)
        try:
            hop.status = int(bits[1])
        except (IndexError, ValueError):
            sock.close()
            raise IOError(f"unparseable status line: {status_line!r}")

        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                hop.headers[k.strip().lower()] = v.strip()

        if hop.status in (301, 302, 303, 307, 308):
            loc = hop.headers.get("location")
            sock.close()
            if not loc:
                raise IOError(f"{hop.status} with no Location")
            if verbose:
                print(f"          {hop.status} -> redirect, "
                      f"connect {hop.connect_ms:.0f} ms")
            url = loc
            continue

        if hop.status != 200:
            sock.close()
            raise IOError(f"HTTP {hop.status}")

        if verbose:
            print(f"          200 -> play, connect {hop.connect_ms:.0f} ms")
        return sock, hop.headers, leftover, hops

    raise IOError(f"more than {MAX_HOPS} redirects")


def declared_kbps(headers):
    """The station's own claim, from icy-br. See netstream.c's note: this
    is a declaration and not a measurement, nominal on a variable-rate
    mount, and absent on some servers."""
    raw = headers.get("icy-br", "")
    # Multi-rate mounts send a list ("128,64"); the first is this mount.
    first = raw.split(",")[0].strip()
    try:
        v = int(first)
        return v if 0 < v < 10000 else 0
    except ValueError:
        return 0


def metaint(headers):
    try:
        return int(headers.get("icy-metaint", "0"))
    except ValueError:
        return 0


def measure(url, duration_s, csv_writer=None):
    print(f"\n{'=' * 72}\n{url}")
    try:
        sock, headers, leftover, hops = open_stream(url)
    except (IOError, OSError, ssl.SSLError) as e:
        print(f"  FAILED: {e}")
        return None

    name = headers.get("icy-name", "(no icy-name)")
    ctype = headers.get("content-type", "(none)")
    br = declared_kbps(headers)
    mi = metaint(headers)

    print(f"  icy-name:     {name[:60]}")
    print(f"  content-type: {ctype}")
    print(f"  icy-br:       {br if br else '(not sent)'} kbit/s")
    print(f"  icy-metaint:  {mi if mi else '(none)'}")
    if not br:
        print("  NOTE: no icy-br, so there is nothing to measure the rate")
        print("        against. Throughput is still reported; the x-realtime")
        print("        and audio columns are not.")
    print()

    # Metadata accounting. With Icy-MetaData: 1 the server interleaves a
    # length-prefixed block every `metaint` audio bytes, and counting those
    # as audio would overstate delivery. Tracked rather than stripped --
    # nothing here needs the titles, only the byte count.
    since_meta = 0
    meta_remaining = 0
    meta_len_pending = False
    meta_bytes = 0

    def account(chunk):
        """Split a chunk into (audio_bytes, metadata_bytes)."""
        nonlocal since_meta, meta_remaining, meta_len_pending
        audio = 0
        meta = 0
        i = 0
        n = len(chunk)
        while i < n:
            if meta_remaining > 0:
                take = min(meta_remaining, n - i)
                meta += take
                meta_remaining -= take
                i += take
                continue
            if meta_len_pending:
                meta += 1
                meta_remaining = chunk[i] * 16
                meta_len_pending = False
                i += 1
                continue
            if mi:
                take = min(mi - since_meta, n - i)
                audio += take
                since_meta += take
                i += take
                if since_meta >= mi:
                    since_meta = 0
                    meta_len_pending = True
            else:
                audio += n - i
                i = n
        return audio, meta

    t_start = time.monotonic()
    audio_total = 0
    window_audio = 0
    window_start = t_start
    worst_read_ms = 0.0

    # The simulated reserve, in seconds of audio, to match the device's
    # `audio Xs`. Credited at the declared rate, debited in real time,
    # and held at zero until PREROLL_S like bufplan's gate.
    reserve_s = 0.0
    playing = False
    started = False     # has the first preroll ever completed
    dropouts = 0
    min_reserve = None
    rows = []

    print("   t        rate    vs decl    audio     worst read")
    try:
        while True:
            now = time.monotonic()
            if now - t_start >= duration_s:
                break

            t_read = time.monotonic()
            try:
                chunk = leftover if leftover else sock.recv(READ_CHUNK)
            except socket.timeout:
                print("  read timed out")
                break
            leftover = b""
            read_ms = (time.monotonic() - t_read) * 1000.0
            worst_read_ms = max(worst_read_ms, read_ms)

            if not chunk:
                print("  stream closed by server")
                break

            a, m = account(chunk)
            audio_total += a
            window_audio += a
            meta_bytes += m

            now = time.monotonic()
            if now - window_start >= WINDOW_S:
                span = now - window_start
                kbps = window_audio * 8 / span / 1000.0

                if br:
                    # Credit what arrived, debit what a player would have
                    # consumed -- but ONLY ONCE IT IS PLAYING.
                    #
                    # The first version debited from the first window and
                    # a station pacing at exactly 1.00x therefore never
                    # accumulated anything: credit and debit cancelled and
                    # the reserve sat at 0.00s forever, which is not what
                    # the device does. bufplan does not run the writer
                    # until BUFPLAN_START_MS is queued, so nothing is
                    # consumed during preroll -- which is precisely why
                    # WNZK's first reported figure was 4.09s and not zero.
                    #
                    # Same reason a refill after a dropout works on the
                    # device: the writer stops, so the whole of the
                    # arriving stream becomes lead. That is visible in the
                    # log as the reserve jumping 1.4s -> 3.7s across an
                    # `amplifier off`/`on` pair.
                    reserve_s += (window_audio * 8 / 1000.0) / br
                    if playing:
                        reserve_s -= span
                    if reserve_s < 0:
                        reserve_s = 0.0

                    if not playing and reserve_s >= (PREROLL_S if not started
                                                     else RESUME_S):
                        playing = True
                        started = True
                    elif playing and reserve_s < LOW_S:
                        dropouts += 1
                        playing = False
                    if playing:
                        min_reserve = (reserve_s if min_reserve is None
                                       else min(min_reserve, reserve_s))
                    ratio = f"{kbps / br:5.2f}x"
                    res = f"{reserve_s:6.2f}s"
                else:
                    ratio = "    --"
                    res = "     --"

                t_rel = now - t_start
                print(f"  {t_rel:5.1f}s  {kbps:6.0f} kbit/s  {ratio}  "
                      f"{res}   {worst_read_ms:5.0f} ms")
                rows.append({
                    "t_s": round(t_rel, 2),
                    "kbps": round(kbps, 1),
                    "declared_kbps": br,
                    "ratio": round(kbps / br, 4) if br else "",
                    "reserve_s": round(reserve_s, 2) if br else "",
                    "worst_read_ms": round(worst_read_ms, 1),
                })
                window_audio = 0
                window_start = now
                worst_read_ms = 0.0
    except KeyboardInterrupt:
        print("\n  interrupted; moving on")
    finally:
        try:
            sock.close()
        except OSError:
            pass

    elapsed = time.monotonic() - t_start
    if elapsed <= 0 or audio_total == 0:
        print("  nothing measured")
        return None

    mean_kbps = audio_total * 8 / elapsed / 1000.0
    print(f"\n  {elapsed:.0f} s, {audio_total / 1024:.0f} KB audio"
          + (f" + {meta_bytes / 1024:.1f} KB metadata" if meta_bytes else ""))
    print(f"  mean: {mean_kbps:.0f} kbit/s")

    result = {
        "url": url, "name": name, "declared": br,
        "mean_kbps": mean_kbps, "elapsed": elapsed,
        "dropouts": dropouts, "min_reserve": min_reserve,
    }

    if br:
        ratio = mean_kbps / br
        print(f"  declared: {br} kbit/s  ->  {ratio:.3f}x realtime")
        if min_reserve is not None:
            print(f"  simulated reserve low-water: {min_reserve:.2f}s")
        print(f"  simulated dropouts: {dropouts}")
        print()
        if ratio >= 1.02:
            print("  VERDICT: the PC is served with room to spare. A device")
            print("           that starves on this station is not being")
            print("           throttled -- it is not draining the socket fast")
            print("           enough. Look at the transport, not the station.")
        elif ratio >= 0.99:
            print("  VERDICT: served at about exactly realtime. Enough for a")
            print("           PC with a large buffer, and marginal for anything")
            print("           that has to hold a small one. Run again for longer")
            print("           before concluding either way.")
        else:
            print("  VERDICT: the PC is ALSO short of realtime. The station or")
            print("           the path to it is the limit, and no amount of")
            print("           buffering on the device will fix it.")

    if csv_writer and rows:
        for r in rows:
            r["url"] = url
            csv_writer.writerow(r)

    return result


def main():
    ap = argparse.ArgumentParser(
        description="Measure internet radio delivery rate from this PC, "
                    "in the same terms the Tab5 firmware reports.")
    ap.add_argument("urls", nargs="*", help="stream URLs to test")
    ap.add_argument("--m3u", help="read URLs from a stations.m3u")
    ap.add_argument("--duration", type=float, default=60.0,
                    help="seconds per station (default 60; use 300 to match "
                         "the device run that found the problem)")
    ap.add_argument("--csv", help="write per-window rows to this file")
    args = ap.parse_args()

    urls = list(args.urls)
    if args.m3u:
        with open(args.m3u, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    urls.append(line)
    if not urls:
        urls = DEFAULT_URLS

    print("streamcheck -- no decoding, just delivery rate")
    print(f"{len(urls)} station(s), {args.duration:.0f} s each, "
          f"{WINDOW_S:.0f} s windows")

    csv_file = None
    writer = None
    if args.csv:
        csv_file = open(args.csv, "w", newline="", encoding="utf-8")
        writer = csv.DictWriter(csv_file, fieldnames=[
            "url", "t_s", "kbps", "declared_kbps", "ratio",
            "reserve_s", "worst_read_ms"])
        writer.writeheader()

    results = []
    for url in urls:
        r = measure(url, args.duration, writer)
        if r:
            results.append(r)

    if csv_file:
        csv_file.close()
        print(f"\nwindows written to {args.csv}")

    if len(results) > 1:
        print(f"\n{'=' * 72}\nSUMMARY\n")
        print(f"  {'station':<34} {'decl':>6} {'mean':>6} {'ratio':>7} {'drops':>6}")
        for r in results:
            ratio = f"{r['mean_kbps'] / r['declared']:.2f}x" if r["declared"] else "--"
            decl = r["declared"] or "--"
            print(f"  {r['name'][:34]:<34} {decl:>6} "
                  f"{r['mean_kbps']:>6.0f} {ratio:>7} {r['dropouts']:>6}")
        print()
        print("  Compare `ratio` against the device. The firmware's own line")
        print("  reads e.g. `434 kbit/s, bytes 0% (2047), audio 3.02s` -- the")
        print("  device measured 466 kbit/s mean on a 512 kbit/s station,")
        print("  which is 0.91x, with the reserve sawtoothing 4.1s -> 1.4s and")
        print("  a dropout roughly every 35 s.")

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print()
        sys.exit(130)
