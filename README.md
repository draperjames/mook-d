# Mook D — Model D-style synth module for Ableton Move (Schwung)

A monophonic Minimoog Model D-style **sound_generator** module for the
[Schwung](https://github.com/charlesvestal/schwung) framework. Three oscillators
into a 4-pole transistor ladder low-pass filter, with independent filter and
loudness contour envelopes, glide, and osc-3/noise modulation.

Written natively to the Schwung Plugin ABI (`move_plugin_init_v2`,
44100 Hz / 128-frame blocks, stereo interleaved int16). No vendor source reused.

## Signal path
```
Osc1 ┐
Osc2 ┼─ Mixer ─→ Ladder LPF ─→ VCA ─→ out
Osc3 ┘           (cutoff/emphasis/    (loudness
Noise┘            contour env)         env)
```

## DSP notes
- **Oscillators:** PolyBLEP band-limited saw/pulse/square + naive triangle.
  Ranges LO/32'/16'/8'/4'/2' map to octave multipliers; osc 3 can free-run
  (`o3_kbd = Off`) to act as a modulation oscillator.
- **Filter:** Huovilainen-style nonlinear 4-pole ladder, 2× oversampled, with
  per-stage `tanh` saturation and resonance up to self-oscillation.
- **Envelopes:** exponential ADSR. The Model D **Decay switch** (`decay_sw`)
  makes release track the decay time; Off gives a short release.
- **Voice:** mono, last-note priority with an 8-deep held-note stack, glide.
- **Mod:** `mod_mix` crossfades osc 3 ↔ noise; route to pitch and/or cutoff.
- **LFO:** dedicated modulation oscillator — Rate (0.05–40 Hz, log), Shape
  (Tri / Ramp Up / Square / Ramp Dn / S&H), independent depths to pitch
  (vibrato) and filter cutoff. **Tempo sync** (`lfo_sync`) locks the rate to
  host BPM via `get_bpm()` at a musical division (`lfo_div`, 4 bars…1/32 incl.
  triplets); free-runs otherwise. Separate from the osc-3/noise mod bus.
- **Hard sync:** `osc_sync` makes Osc 1 the master and resets Osc 2 (and
  optionally Osc 3) phase on each master cycle — the classic sync-lead tear.
- **Filter FM:** `filt_fm` routes Osc 3 into the ladder cutoff at audio rate
  for growl/formant tones (inharmonic when Osc 3 free-runs, `o3_kbd = Off`).
- **Presets:** 32 native patches (`preset` param) authored for this module's
  ranges, spanning bass/lead/pad-drone/bell/FX character. Nothing copied from
  other projects — their values are calibrated to different DSP and wouldn't
  map — though the category vocabulary and naming take inspiration from
  open-source Minimoog emulators (e.g. `stevebarakat/Minimoog`'s Bass/Lead/
  Pad/Experimental preset bank).

## Build (ARM64, requires Docker)
```bash
./scripts/build.sh
# → dist/mook-d/{dsp.so,module.json,help.json}
# → dist/mook-d-module.tar.gz
```
The Dockerfile pins `gcc-aarch64-linux-gnu`; flags mirror the reference modules
(`-O2 -shared -fPIC -ffast-math -lm`) and avoid `shm_open` so the GLIBC
requirement stays at the Move's runtime level.

## Install on the Move
Copy the built folder to the Move's modules directory (via schwung-manager or
scp), so it lands at `.../modules/mook-d/` containing `dsp.so` + `module.json`,
then load **Mook D** from the sound-generator list. It's `chainable`, so it also
works as a Signal Chain generator.

## Starting patch — classic fat bass/lead
| Section | Setting |
|---|---|
| Osc 1 | 8′, Saw |
| Osc 2 | 8′, Saw, Tune −0.07 st |
| Osc 3 | 16′, Saw, Tune +0.05 st, Kbd On |
| Mixer | O1 0.8 · O2 0.7 · O3 0.6 · Noise 0 |
| Filter | Cutoff 0.45 · Emphasis 0.35 · Contour 0.65 |
| Filter env | A 0.005 · D 0.35 · S 0.15 |
| Loudness env | A 0.005 · D 0.5 · S 0.8 |
| Controllers | Glide 0 · Master Tune 0 · Decay Sw On |

For the pitch-wobble variant: Osc 3 → LO range, `o3_kbd` Off, `mod_mix` 0,
`mod_osc` On.

## Presets
**Original 16:** Init · Fat Bass · Sub Bass · Rubber Bass · Classic Lead ·
Brass · Funk Lead · Wobble Bass · Vibrato Lead · Sci-Fi Sweep · Bright Pluck ·
Detuned Stab · Whistle · Noise Sweep · Sync Lead · FM Growl.

**Expansion 16:** Taurus Sub · Slap Bass · Dub Bass · Growl Bass · Warm Pad ·
Glass Pad · Cathedral Drone · Prog Solo · Screamer Lead · Watery Lead ·
Simple Lead · Crystal Bells · Wind Chimes · Submarine Sonar · Alien Signal ·
Cosmic Drone.

Wobble/Sci-Fi/Vibrato/Whistle/Noise/Wind Chimes/Submarine Sonar/Cosmic Drone
use the LFO (Wobble and Cosmic Drone are tempo-synced); Sync Lead and Alien
Signal use hard sync; FM Growl, Crystal Bells, Growl Bass and Alien Signal
use filter FM.

## Contributing upstream
See `PR_CHECKLIST.md`. Schwung requires disclosing AI
tooling in the PR (this was built with Claude) and does not accept Grok work.

## Third-party
The Schwung Plugin API header (`src/dsp/host/plugin_api_v1.h`) is
MIT © Charles Vestal — see `THIRD_PARTY.md`. Everything else is original.

## License
MIT.
