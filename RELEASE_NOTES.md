# Bassifier release notes

## v0.1.1

Patch release: new defaults so a new instance starts near-transparent.

- New defaults: Freq 70 Hz (was 100 Hz) and Intensity -24 dB (was 0 dB). Sessions saved with v0.1.0 keep their stored values.
- No DSP change; parameter IDs, ranges and identity are unchanged.
- macOS Universal: AU and VST3, arm64 + x86_64; ad-hoc signed, not notarized.
- Windows x64: VST3, unsigned; requires the Microsoft Visual C++ x64 Redistributable.
- macOS checks: CTest 3/3 passed (DSP evaluation, plug-in smoke, snapshot render); editor snapshot shows FREQ 70.0 Hz and INTENSITY -24.0 dB; arm64 + x86_64 slices confirmed in the AU and VST3 builds.
- Windows: CTest result pending.

## v0.1.0

Bassifier is a psychoacoustic bass enhancer. It rebuilds the bass below a chosen frequency as harmonics so the low end is heard on small speakers.

- macOS Universal: AU and VST3, arm64 + x86_64.
- Windows x64: VST3, built and tested by the native Windows release workflow; requires the Microsoft Visual C++ x64 Redistributable.
- macOS bundles are ad-hoc signed, not notarized. Windows builds are unsigned.
- macOS checks: CTest 3/3 passed (DSP evaluation, plug-in smoke, snapshot render); editor snapshot rendered; arm64 + x86_64 slices, ad-hoc signatures and package integrity verified at packaging.

The output can exceed 0 dBFS; this is not a clip-free guarantee. The harmonics are mono, and a loop-gain cap pins their level at high input levels. The crossover does not fully remove the fundamental when Original Bass is Out.

Each ZIP includes installation instructions, dependency notices and build provenance, with a SHA-256 checksum companion. No DAW-host test has been run.
