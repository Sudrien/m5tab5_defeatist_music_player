# The media index

A plan, not a description. Nothing here is built. `ARCHITECTURE.md` is
for code that exists; this is the reasoning behind a v0.5.0 target so
that the decisions already argued out do not have to be argued again,
and so the ones still open are visibly open.

## What it is for

Three consumers, one library:

- **MPD compatibility**, which is the v0.5.0 API target. A control
  client asks for a library and expects to browse and search it.
- **The web UI**, which wants the same answers over a different
  transport.
- **`build file lists faster`**, already on the README's "what could
  happen" list, which is the local benefit that justifies the cost even
  if neither of the above ships.

## What exists now, and why it is not enough

Nothing on the device knows the whole card. The chooser walks one
directory per view -- `readdir` into `MAX_ENTRIES` (512), `qsort`, plus
every cue sheet in the folder parsed to expand its tracks -- and throws
the result away on the way out. `playlist.c` holds one directory's
tracks, `PLAYLIST_MAX` 1024, in PSRAM.

That is the right shape for a chooser and the wrong shape for a library.
MPD's `search` and `find` are whole-card questions, and answering them
by walking the card per query means a full card walk inside a protocol
timeout.

The saving grace is that `lsinfo` is per-directory, and `listallinfo` is
deprecated upstream. The protocol's supported shape is therefore already
a directory at a time, which is the shape a directory-keyed index
answers in one seek. Search is the query that needs the whole file;
browsing is not.

## Storage: a JSONL catalog beside a fixed-width index

Two files, both dotfiles at the volume root, alongside `.defeatist.dat`:

- **the catalog**, JSONL, one record per track: path, tags, duration,
  ReplayGain, cue parentage, `deleted_at`. Variable length because tags
  are variable length, and a format that can be appended to and read
  line-at-a-time without a parser holding the whole file.
- **the index**, fixed-width records sorted by path, each holding the
  key and the catalog offset of its record.

Fixed width is the whole point of the second file: it makes the index
binary-searchable by seeking, so a lookup is `log2(n)` seeks of one
record each rather than a scan. At 20 000 tracks that is 15 seeks. The
catalog is never searched, only seeked into.

**Append-only, with compaction.** This is `settings.c`'s pattern and it
should be copied deliberately rather than reinvented: `write_file()`
opens `"a"`, appends one record, and compacts when the file would pass
`SETTINGS_MAX_FILE_BYTES` (64 KB). It is the right pattern here for the
same reasons -- an append is one write with no window where the file is
half-rewritten, and a card pulled mid-append loses the last line and
nothing else. The index cannot be appended to in sorted order, so it is
rebuilt from the catalog rather than updated in place; that is a reason
to keep it small and derived, and never to treat it as the truth.

**`deleted_at` is a timestamp, not a boolean.** A boolean cannot expire
a tombstone, cannot be reconciled between two volumes that disagree, and
cannot tell "deleted before the clock was trusted" from "deleted last
Tuesday" -- which matters on a device whose clock starts wrong (see
below). The cost of the timestamp is eight bytes and the cost of the
boolean is a schema change later.

## Memory

PSRAM is not free here. It is already carrying a 256 KB netstream ring
plus its working set, the framebuffer, two cover-art entries at ~142 KB
each, the playlist's 1024 pointers, and the decoders' scratch.

So **the index is not held in memory**. It lives on the card and is read
by seek and binary search, with at most a small hot window resident. A
design that wants the index in RAM is a design that has to argue with
`netstream.c` for the space, and it will lose -- 20 000 records at even
64 bytes of key is 1.2 MB before any tags.

## Reconcile: a merge-join, and why there is no shortcut

The card and the index are both sorted by path, so bringing them into
agreement is a merge-join: walk both in order, and

- present in both, same mtime and size -- nothing to do;
- present in both, changed -- re-read tags, append a new record;
- on the card, not in the index -- new, append;
- in the index, not on the card -- **set `deleted_at`**;
- in the index with `deleted_at` set, and back on the card -- clear it.

That last case is why tombstones are kept rather than dropped: a card
that comes back has a library that comes back with it, without re-reading
every tag.

### What cannot be used to detect change

This is the part most likely to be got wrong later, because the
shortcuts all look plausible and three of them were proposed and
withdrawn while this was being discussed.

- **The filesystem has no volume-level timestamp.** FAT32 and exFAT
  record no "last modified" for the volume itself. There is nothing to
  read that says "this card changed".
- **Directory mtimes do not bubble.** A directory's timestamp moves when
  an entry is added to or removed from THAT directory. Adding a track to
  `/sd/Artist/Album/` moves `Album`'s mtime and leaves `Artist`'s and the
  root's untouched. A cheap check of the root detects nothing.
- **`readdir` carries no timestamps.** FatFs `f_readdir` fills a
  `FILINFO` that has them, but the project reads through POSIX
  `readdir`, which does not -- so a walk that wants mtimes pays a
  `stat()` per file, and the walk is the expensive part, not the compare.
  (5019: the walk now reads `FILINFO` through FatFs, `mediadir.c`.)

Together these mean there is **no cheap probe for "has anything
changed"**. The full walk is the only correct answer, and the design
should spend its effort on making the walk cheap and rare rather than on
looking for a probe that does not exist.

### `cardtime.c` is not a change detector

It reads timestamps off up to `CARDTIME_SCAN_MAX` (256) entries to find
a floor for the clock before NTP can run, and it stops at the first five
useful ones. The shape is close enough to a change detector that someone
will eventually try to use it as one. It cannot be: it is a floor, it is
one-way, it deliberately refuses anything more than
`CARDTIME_MAX_AHEAD_S` out, and it looks at a handful of entries rather
than all of them. Reusing it would produce an index that silently missed
changes in every folder it did not happen to sample.

What it *is* good for here is the clock the index writes with, which is
the next section.

## The clock problem

The device boots with no RTC and possibly no network. Records written
before NTP carry a timestamp derived from the card's own contents, which
is a floor and not the time.

Consequences the format has to live with:

- a record's write time may be **earlier than the truth**, never later,
  by construction;
- two records written either side of an NTP sync are not comparable by
  timestamp alone;
- `deleted_at` inherits all of this, which is the second reason it is a
  timestamp: a tombstone written pre-NTP is identifiable as such.

The simplest answer, and the recommended one, is to record which clock
each record was written against -- floor or synced -- as a field, and
never compare across the two. One character in the JSON.

## When the index is built

On mount, and on demand (the REINDEX button, 5015; on mount, 5018).
**Not periodically.** A full walk competes with
playback for the storage arbiter, and the failure mode of a periodic
scan is a card that stutters every few minutes for no reason the
listener can see.

The arbiter already has the vocabulary for this -- the walk is
background work by definition, and should take `STORAGE_IO_BACKGROUND`
throughout, the way `cuedir_load()` does.

## Decided since

Settled after this note was written, some by the code already in the
tree and some by asking. `main/mediaindex.h` (5010) is the first code.

1. **Cue sheets: the tracks.** A cue track is `<sheet>.cue#NN`
   everywhere a path goes already (`cuesheet.h`), `NN` is two digits so
   path order is track order, and `cuedir.h` hides the image a sheet
   covers. The parent is recoverable from the path by
   `cue_vpath_split()`, so it need not be a field. A cue track's stamp
   is the sheet's and the audio's together (5013), so a re-ripped
   image under an untouched sheet is still a change.
2. **Search: needed, and from a derived search file.** MPD asks first,
   a web UI with a keyboard later. One plain line per track, lowercased
   tags and the catalog offset, scanned without a JSON parser; rebuilt
   from the catalog like the index.
3. **Two volumes: one merged library, SD preferred.** Paths are
   relative to the volume root, so the same relative path on both is
   one entry and the SD's copy is the one shown; folders present on
   both list the union. With one volume mounted, it is the library.
   Matching is byte-exact, so `ABBA/` and `Abba/` are two folders.
4. **Record size: a path prefix, not a hash.** The merged listing needs
   both indexes in path order, which a hash key cannot give. The key is
   the first 104 bytes of the path, with the catalog offset and the
   stamp, 128 bytes a record (5011 has the layout); a prefix tie is
   settled by reading the full path out of the catalog. Paths stay good
   to the 512 bytes the rest of the player allows.
5. **A version bump rebuilds the index.** The index and search file are
   derived, and the sidecar (`replaygain.h`) already showed derived data
   needs no migration path. The catalog follows `settings.h`: a key a
   build does not know is skipped, not fatal.

**The order is not strcmp.** Found writing 5010, and worth having here
because it is the mistake the obvious implementation makes: the index
must be in the same order as a depth-first walk with sorted folders,
and whole-path `strcmp()` is not that order, because `' '`, `'-'`, `'.'`
and every other byte below `'/'` sort before it. `mediaindex.h` makes
`'/'` sort lowest, and the walk sorts each folder by the same function
-- not the chooser's folders-first, case-insensitive order.

## What would make this not worth building

Worth writing down: if the web UI does not need whole-card search and
MPD support stays at "browse and control what is playing", then
`lsinfo` against a live `readdir` answers every query the device
actually receives, and the index is a file format to maintain for no
gain. The index is justified by **search**, not by browsing.
