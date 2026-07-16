# Changelog

All notable changes to Delta are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versioning
follows [Semantic Versioning](https://semver.org/) adapted for a pre-1.0
beta:

- **PATCH** (0.2.x): bug fixes only, no behavior/feature changes
- **MINOR** (0.x.0): new features or notable user-facing changes
- **MAJOR** (1.0.0+): first stable release, then breaking changes only

## [0.3.1] - 2026-07-16

### Fixed
- The ALIGN button gave no feedback while a correlation search was running
  in the background -- clicking it just did nothing visible until the
  offset changed. It now reads "ALIGNING" and disables itself for the
  duration, so repeated clicks can't queue up redundant requests.

### Changed
- The null-depth dB readout now has light ballistics smoothing instead of
  jumping instantly between frames, closer to how a real meter settles.

## [0.3.0] - 2026-07-16

### Changed
- UI pass on the instrument display: a bezel frame with corner ticks now
  encloses the spectrogram/peak-strip/legend as a single panel, a faint
  sub-grid fills the frequency axis between the labeled lines so the
  display reads as graph paper rather than mostly-empty black, the
  heatmap gradient gained a third warmer stage (amber -> red-orange ->
  white) instead of a flat amber-to-white blend, and the HUD title/footer
  text got slightly wider letter-spacing for a more technical readout feel.
- The null-depth dB reading -- the one number this plugin exists to
  produce -- moved out of the shared HUD text row into its own inset
  digital-readout box, so it reads as the dominant element instead of
  sharing type weight with the branding.
- The peak-hold contour now has a soft glow pass beneath the crisp line,
  matching the spectrogram's existing phosphor glow.
- Added a faint, fixed (non-animated) grain texture across the display
  panel for a bit of screen texture instead of perfectly flat vector fills.
  No DSP or parameter changes.

## [0.2.2] - 2026-07-15

### Fixed
- File-comparison mode (loaded file paths) was not saved/restored across
  session save-reload -- only the TEST SIGNAL toggle was, so reopening a
  project with an active file comparison silently reverted to live
  sidechain mode. Both file paths now round-trip through
  get/setStateInformation, same as TEST SIGNAL.

## [0.2.1] - 2026-07-15

### Fixed
- Race condition: `prepareToPlay()` could reallocate buffers still being
  read by the background alignment worker or the UI-facing spectrogram
  FIFO, neither of which is covered by JUCE's processBlock/prepareToPlay
  mutual-exclusion guarantee. These buffers are now allocated once and
  never resized after that.
- File-mode loading could hit undefined behaviour (division by zero, then
  casting +/-inf to `int`) if a malformed file reported a zero or negative
  sample rate, or read out of bounds if it reported zero channels. Both are
  now validated before use.
- File-mode loading had no cap on file length, so an accidentally-selected
  multi-hour file could exhaust memory. Capped at 30 minutes.

## [0.2.0] - 2026-07-14

### Added
- Background alignment worker thread: the correlation search behind Align
  no longer runs on the audio thread (was up to ~1M operations,
  synchronous; now the audio thread only takes a cheap snapshot and hands
  it off, measured ~0.02ms).
- Resizable window (520x340 up to 1600x1000).
- Offline file-comparison mode: LOAD A / LOAD B load two audio files
  (auto-resampled to the project rate) that loop through the same
  alignment/spectrogram/null-depth pipeline as live sidechain use.

### Fixed
- Processing latency (256 samples) is now reported to the host (PDC) --
  was previously introducing an unreported timing offset of its own.
- Spectrogram could show the main input's own content while the HUD read
  NO SIDECHAIN / -inf dB; both are now consistent.
- Align requests made before the capture window had filled now stay
  pending instead of running against near-empty data.
- The TEST SIGNAL toggle now persists across session save/reload.
- Data race on the file-mode playhead when a new file was loaded mid-block.

## [0.1.0] - 2026-07-14

### Added
- First public beta: sidechain-based null test, one-click cross-correlation
  auto-alignment, live scrolling spectrogram with peak-hold and dB colour
  legend, built-in synthetic test signal, VST3/AU/Standalone on macOS and
  Windows.
