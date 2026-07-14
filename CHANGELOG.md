# Changelog

All notable changes to Delta are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versioning
follows [Semantic Versioning](https://semver.org/) adapted for a pre-1.0
beta:

- **PATCH** (0.2.x): bug fixes only, no behavior/feature changes
- **MINOR** (0.x.0): new features or notable user-facing changes
- **MAJOR** (1.0.0+): first stable release, then breaking changes only

## [0.2.1] - 2026-07-15

### Fixed
- Race condition: `prepareToPlay()` could reallocate buffers still being
  read by the background alignment worker or the UI-facing spectrogram
  FIFO, neither of which is covered by JUCE's processBlock/prepareToPlay
  mutual-exclusion guarantee. These buffers are now allocated once and
  never resized after that.

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
