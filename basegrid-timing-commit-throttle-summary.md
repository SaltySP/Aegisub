# Throttle BaseGrid repaints triggered by timing commits

## Problem

With "automatically commit all changes" enabled in the audio spectrum
display, dragging a line marker made the entire UI outside the subtitle
grid render at very low FPS or freeze outright, on Windows builds.
The subtitle grid itself appeared unaffected.

## Root cause

Dragging a marker fires an `AssFile::COMMIT_DIAG_TIME` commit on
(almost) every mouse-move event while auto-commit is on - potentially
dozens of times per second during a fast drag.

`BaseGrid::OnSubtitlesCommit` handled that commit type with an
unconditional, full-window `Refresh(false)`:

```cpp
if (type & AssFile::COMMIT_DIAG_TIME)
    Refresh(false);
```

`BaseGrid::OnPaint` has column-level clipping (it checks
`GetUpdateRegion()`'s X-extent to skip columns that aren't dirty), but
**no row-level clipping**: the row-drawing loop always redraws every
currently visible row, regardless of how small or large the invalidated
rect was. So every single commit during a drag triggered a full redraw
of the entire visible grid - confirmed directly via profiling
(`BaseGrid::OnPaint` accounted for ~45-57% of total sampled time during
a fast marker drag, dwarfing everything happening in the audio display
itself). Because each `OnPaint` call does essentially the same fixed
amount of work no matter what changed, the only effective lever is
reducing how often it's invoked, not how much of it is dirty per call.

(The grid appearing visually "fine" during the freeze is consistent
with this: it's the one thing actually getting CPU time on the UI
thread each cycle, while everything else - video, audio panel, other
controls - is starved waiting its turn.)

## Fix

Coalesce timing-commit-triggered repaints to roughly 60 Hz instead of
requesting one on every commit:

- The first timing commit in a burst repaints immediately (or, when a
  single changed line is known, just that row's rect - not that it
  currently changes `OnPaint`'s cost, but it's the more correct
  invalidation regardless), so a single time edit remains instant.
- Further timing commits arriving while a ~16ms cooldown is active are
  merged rather than triggering another repaint immediately. If two
  different rows are touched within one cooldown window, the pending
  repaint is escalated to a full-grid refresh (this shouldn't happen
  during a single marker drag, which only ever touches one line).
- When the cooldown timer fires, any owed repaint is flushed and a new
  cooldown starts if the grid is still receiving timing commits.

This caps the number of full-grid redraws per second regardless of how
fast the underlying commit rate is, which is what actually matters
given `OnPaint`'s cost is roughly independent of the invalidated area.

## Scope / what this doesn't fix

- This only affects `COMMIT_DIAG_TIME`. Other commit types are
  untouched.
- `COMMIT_DIAG_TEXT` has the same underlying issue (targeted
  `RefreshRect` calls via `text_refresh_rects`, but `OnPaint` ignores
  the Y-extent regardless) - it just isn't normally fired at a high
  enough frequency to be symptomatic. Not addressed here since it
  wasn't reproducible as a user-facing problem; flagged for awareness.
- The separate, smaller freeze that occurs during marker drags with
  auto-commit *off* (no commit fires at all in that case, so `BaseGrid`
  was never involved) is not addressed by this patch. That one traces
  to a mix of `AudioDisplay`'s own repaint area/frequency and a
  wxWidgets-level cost on MSW (`wxMSWDCImpl::DoStretchBlit` taking the
  `StretchDIBits` path because `wxAutoBufferedPaintDC`'s shared buffer
  is explicitly 24bpp, and therefore DIB-backed, on MSW) - addressed
  separately in the `audio_display`/`audio_renderer` patches, which are
  smaller, secondary contributors by comparison.

## Files changed

- `src/base_grid.h` - new throttle state (`timing_commit_paint_timer`,
  pending-rect/pending-full flags) and two new private methods.
- `src/base_grid.cpp` - `OnSubtitlesCommit`'s `COMMIT_DIAG_TIME` branch
  now routes through `RequestTimingCommitRepaint()` instead of calling
  `Refresh`/`RefreshRect` directly; `RequestTimingCommitRepaint()` and
  `OnTimingCommitPaintTimer()` implement the coalescing.