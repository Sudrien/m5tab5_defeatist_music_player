# texttest

Host-side tests for the variable-width text layout in `main/gfx.c`.

    cd texttest && make

Builds the **real** `main/gfx.c` and the **real** generated
`components/ark12/ark12.c` against a small ESP-IDF shim, under
`-fsanitize=address,undefined -fno-sanitize-recover=all`, and runs a set
of property checks. Exit status is non-zero on any failure.

## Why it compiles the real file

`CLAUDE.md` on `seektest/`: *"Generated test files verify the logic
against the format as understood; they cannot verify the understanding."*
The same trap is open here in a different shape. A reimplementation of
`gfx_text_w()` in the harness would share the author's misunderstanding by
construction, pass, and prove nothing.

So nothing in this directory reimplements any layout function. `shim.h`
and `fake/` supply the ESP-IDF surface `gfx.c` includes; `shim.c` backs it
with `malloc` so ASan sees every write into the shadow framebuffer. The
functions under test are the ones that ship.

`gfx.c` keeps `s_fb` private, and it should. Rather than add a test-only
accessor to shipping code, `shim.c` remembers the largest allocation it
handed out — which is the framebuffer, since `gfx_init()` is the only
thing allocating. `main()` verifies that pointer by writing a pixel and
reading it back before trusting it.

## What it checks

1. **`gfx_text_w()` matches what the draw path walks.** When every glyph
   was one cell these could not disagree. They are now separate code paths
   over separate width lookups, and every caller that centres or
   right-aligns text trusts them to agree.
2. **`max_w` is a hard budget** for both `gfx_draw_text()` and
   `gfx_draw_text_tail()`, across budgets chosen to straddle the
   dots-plus-one-glyph bail-out.
3. **`gfx_draw_text_clipped()` never draws outside its window**, swept
   across the full marquee travel plus overshoot at both ends.
4. **The tail walk keeps the tail**, including when the string is longer
   than the ring buffer that replaced the old byte-offset walk.
5. **Degenerate input is a no-op, not a crash**: NULL, empty, zero and
   negative budgets, and origins far off every edge of the panel.

The corpus is real-shaped strings rather than random bytes — Latin,
accented Latin, Cyrillic, kana, CJK, mixed-script runs, fullwidth
parentheses, codepoints Ark has no glyph for, soft hyphen, no-break
space — plus malformed UTF-8 (truncated sequences, lone continuations,
overlongs, surrogates, 5-byte leads) because a tag can contain anything.

## Mutation-checked

A suite that cannot fail proves nothing, so four deliberate bugs were
introduced into `gfx.c` and the suite was confirmed to catch each:

| mutation | result |
| --- | --- |
| `gfx_text_w()` assumes every glyph is halfwidth | 67 failures |
| off-by-one in the tail ring's oldest-entry index | UBSan: null deref |
| right-edge clip removed from `gfx_draw_text_clipped()` | 468 failures |
| ellipsis budget check drops the next glyph's advance | 90 failures |

## ctrltest: the USB control-transfer handoff

`make` also builds and runs `ctrltest`, which covers a different bug in a
different file: `hid.c`'s report-descriptor request freed its transfer
and its completion semaphore when the 1 s wait timed out, while the URB
was still in flight. The completion then ran against a dead stack frame
and gave a deleted semaphore, which is a panic inside
`usb_host_client_handle_events`:

    assert failed: xQueueGenericSend queue.c:936 (pxQueue)

It only reproduced with the device already plugged in at boot, where
enumeration lands on top of the SD mount and the first cover prefetch --
so it was a boot loop on ordinary use and a clean run on a bench test.

Unlike the text tests, this one **reproduces** the logic rather than
compiling `hid.c`, because the real function is welded to the USB host
API and the interesting part is twenty lines. That is a genuine weakness:
the file and the test can drift. What it verifies is the *shape* of the
ownership handoff -- heap context, an `abandoned` flag set under a lock,
and whichever side finishes last doing the free -- and the shape is what
was wrong.

A fourth case covers the ordering fix in 0903: a single-threaded event
loop, a wait that runs *inside* the dispatch (never completes -- this is
the deadlock that made the timeout universal) and a wait that runs after
it returns (always completes).

A fifth covers the close-after-release ordering in 0904: a claim count
and a gone flag driven from two tasks with randomised delays, asserting
the device is closed exactly once and never while an interface is still
claimed. Closing immediately on DEV_GONE -- the old shape -- fails it.

The fake host's completion delay is a knob, so the three cases are the
three orderings: completion inside the timeout (waiter frees), completion
long after (callback frees), and completion right at the deadline, run
400 times because a handoff that is only usually right is a flake.

Confirmed to fail on the old shape: reverting to the stack context and
freeing on timeout gives
`AddressSanitizer: heap-use-after-free ... WRITE of size 4`, freed by the
waiter, written by the completion thread.

## What it does *not* verify

Everything `CLAUDE.md` says host testing cannot see, which has been right
every time: the DSI panel, PSRAM timing and bandwidth, the ESP-IDF build
itself, and how any of this looks at 294 PPI. **This is not a substitute
for a flash.** It catches layout arithmetic, which is what changed, and
nothing else.
