# Chapter 7 – Audio

Kern has its own sound engine in C++. It runs in the same WebAssembly module as the renderer and the physics, mixes every voice itself, sample by sample, and hands the browser nothing but finished stereo PCM. Web Audio serves only as the speaker output and as the decoder for imported files.

This chapter describes the mixer with 3D sound, buses, reverb and limiter; the synthesizer from which all built-in sounds are made; the voice of the Micro Duck; the layer through which games and levels use sound; and the connection to the browser.

| Part | Files | Task |
|---|---|---|
| Mixer | `include/kern/audio/audio.h`, `src/audio/audio.cpp` | clips, voices, 3D, buses, reverb, limiter |
| Synthesizer | `include/kern/audio/synth.h`, `src/audio/synth.cpp` | patches, music sequencer, built-in sound library |
| Duck voice | `include/kern/audio/duck_voice.h`, `src/audio/duck_voice.cpp` | vocalizations of the Micro Duck |
| Game sound | `include/kern/game/sound.h`, `src/game/sound.cpp` | `SoundBank` and `GameAudio` |
| Browser | `web/studio/game.mjs` (`KernAudio`) | AudioWorklet/ScriptProcessor, decoding |

## 7.1 Why a mixer of its own?

The header lines of `audio.h` state the reasons between the lines: everything happens "sample by sample and deterministically". A mixer of its own provides three things that a graph of Web Audio nodes does not:

- **Determinism.** The same events produce the same samples on every platform. Built-in sounds are synthesized with fixed seeds, and events arise in the fixed physics steps.
- **One model for all hosts.** Studio, exported games and native tests use the same C++ code. The browser only has to play samples.
- **Full control over volume ramps, voice management and output level.** No click when stopping, no clipping: the output never exceeds ±1.

## 7.2 The mixer

### Initialization

```cpp
audio::Mixer mixer;
mixer.init(48000);          // sample rate of the output device, 8000..192000; max_voices default 64
```

`init(sample_rate, max_voices)` clears everything and creates a fixed pool of voices (1 to `kMaxVoices` = 128). Kern Studio starts the mixer at the rate of the `AudioContext` with 64 voices. The output is always stereo (`kOutputChannels = 2`), interleaved.

### Clips

A clip is PCM at the mixer's rate, mono or stereo.

- `create_clip(samples, frames, channels, rate, name)` copies samples at any rate (3000 to 384,000 Hz) and converts them **once** to the mixer rate. Clips with more samples than 60 seconds of stereo at 384 kHz would have (`60 × 384 000 × 2`) are rejected; non-finite samples are replaced by 0 after resampling.
- `adopt_clip(samples, channels, name, loopable)` takes over samples that are already at the mixer rate – this is how the synthesized sounds get in.
- `destroy_clip` stops all voices of the clip. `clip_info` returns name, frames, channels, duration, peak value and whether the clip is a seamless loop.

Resampling on import is a **windowed-sinc filter**: a sinc kernel with a Blackman window and 16 zero crossings per side. When downsampling, the cutoff frequency is lowered to the lower Nyquist frequency (`cutoff = min(1, ratio)`) so that no aliasing occurs. The weights are normalized per output sample. This is computationally expensive but happens only once at load time; during playback, a cheap interpolation is then sufficient.

### Voices and `PlayParams`

`play(clip, params)` starts a voice and returns a `VoiceId`: `generation << 32 | slot`. The generation increases with every start. An old id whose slot has since been reused therefore no longer matches, and calls with it go nowhere instead of modifying someone else's voice.

| Field | Default | Range | Meaning |
|---|---|---|---|
| `volume` | 1 | 0–16 | linear gain |
| `pitch` | 1 | 0.05–8 | playback speed |
| `pan` | 0 | −1–1 | stereo position (2D only) |
| `loop` | off | | repeat the clip |
| `bus` | `sfx` | | target bus (`master` becomes `sfx`) |
| `fade_in` | 0 | s | fade in |
| `start` | 0 | s | start position in the clip |
| `priority` | 0 | | higher priority survives voice shortage |
| `spatial` | off | | 3D voice |
| `position`, `velocity` | 0 | m, m/s | location and velocity of the source |
| `min_distance` | 1 | ≥ 0.01 | full volume within |
| `max_distance` | 50 | > min | silence (linear) or end point (inverse, exponential) |
| `rolloff` | `inverse` | | `inverse`, `linear`, `exponential` |
| `spread` | 0 | 0–1 | 0 point source, 1 enveloping without panning |
| `doppler` | 1 | 0–4 | 0 disables Doppler for this voice |

Invalid values are clamped, not rejected.

**Voice stealing.** If no slot is free, `play()` looks for the least important voice with at most equal priority: first the lowest priority, then the quietest (audibility of the last block), then the oldest. If there are only voices of higher priority, the start fails (`"all voices busy with higher priority"`). `stats().stolen` counts the takeovers.

**Controlling running voices:** `stop(voice, fade)` fades out – at least 5 ms (`kStopGuard`), because a hard cut in the middle of a waveform produces an audible click. `set_volume(voice, v, ramp)` moves the volume linearly over `ramp` seconds (default 30 ms). There are also `set_pitch`, `set_pan`, `set_position`, `set_paused`, `set_all_paused` (game pause, positions are preserved), `stop_all(bus, fade)`, `playing`, `position_of`.

### The mixing process

`render(out, frames)` works in blocks of 128 frames (`kBlock`). Per block and voice, `spatialize()` computes the target gains left and right, a Doppler factor and a low-pass cutoff frequency. Within the block, the gains are **interpolated linearly from the old to the new value**. When a source moves, the level therefore does not jump from block to block but glides – without "zipper" noise.

Then, per frame:

1. Advance the envelope (fade in, or fade out when stopping) and the volume ramp.
2. Read the clip at the fractional position, with **4-point Hermite interpolation** (Catmull-Rom):
   ```cpp
   inline f32 hermite(f32 x0, f32 x1, f32 x2, f32 x3, f32 t) {
     const f32 c1 = 0.5f * (x2 - x0);
     const f32 c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
     const f32 c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
     return ((c3 * t + c2) * t + c1) * t + x1;
   }
   ```
   For loops the index wraps modulo the clip length; for one-shots, anything outside yields silence.
3. Apply a one-pole low-pass (air absorption, see 7.3): `y += k · (x − y)` with `k = 1 − exp(−2π f_c / rate)`.
4. Add into the bus buffer with envelope × volume × channel gain.
5. Advance the position by `pitch × doppler` (paused: 0).

Mono clips are distributed to both channels.

**Virtual voices.** If the loudness of a 3D voice is below 10⁻⁵ (`kSilence`) – for instance because it is beyond `max_distance` – it is not mixed. Its position keeps advancing nonetheless. When the listener comes back, the hum of a machine, say, continues at the point where it would have been instead of starting over. `stats().virtual_voices` counts such voices.

## 7.3 3D sound

`Mixer::set_listener({position, forward, up, velocity})` sets the listener. The game runtime places it at the camera (7.7). For every 3D voice, `spatialize()` computes:

### Distance

With `d = clamp(distance, min_distance, max_distance)`:

| `rolloff` | Gain g |
|---|---|
| `inverse` | `min_distance / d` |
| `linear` | `1 − (d − min) / (max − min)` |
| `exponential` | `(d / min_distance)^−1.5` |

Inverse and exponential curves never quite reach zero. The mixer therefore additionally fades them linearly to zero over the last 20 % before `max_distance`. That way every source beyond `max_distance` actually becomes silent and can turn virtual.

### Panning

The direction to the source is projected onto the listener's right axis (`cross(forward, up)`), which gives a pan value in [−1, 1]. Near sources and wide sources pan less:

```
proximity = clamp(dist / max(0.25, min_distance / 2), 0, 1)
pan      *= (1 − spread) · proximity
```

A source right at the head thus does not jump hard from left to right. Then comes **equal-power panning**, normalized to unity gain in the center:

```
a     = (pan + 1) · π/4
left  = cos(a) · √2 · g
right = sin(a) · √2 · g
```

Equal power means: `left² + right²` is the same for all pan values. A source moving from left to right does not get quieter in the middle, as would happen with a linear crossfade. The same formula (without g) applies to 2D voices with `pan`.

### Air absorption and direction

High frequencies are attenuated more strongly over distance than low ones. Kern approximates this with a low-pass whose cutoff frequency falls with distance; behind the listener, the sound additionally becomes duller:

```
f_c = 20000 / (1 + dist / 120)
if front < 0:  f_c *= 1 + 0.6 · front        // directly behind the listener: 40 %
```

`front` is the dot product of the direction to the source and the view direction. Without HRTF, this difference is the most important cue for telling "front" from "back".

### Doppler

```
f' = f · (c − v_l) / (c − v_s)
```

`c` is `speed_of_sound` (default 343 m/s); `v_l` and `v_s` are the velocities of listener and source along the line source → listener, each scaled by `doppler_factor × PlayParams::doppler` and limited to ±0.9 c. The factor is clamped to 0.5–2 and smoothed toward the new value by 25 % per block, so that fast fly-bys do not sound stepped.

## 7.4 Buses, reverb and master

### Buses

Every voice plays on a bus: `music`, `sfx`, `ambience`, `ui` or `voice`. All buses feed into `master`.

```cpp
struct BusSettings {
  f32 volume = 1;
  bool muted = false;
  f32 lowpass_hz = 0;     // 0 = off
  f32 reverb_send = 0;    // 0..1 to the master reverb
};
```

`init()` sets initial values: music 0.7, ambience 0.8, reverb send from `sfx` 0.12 and from `voice` 0.08. Games override this via the level settings (7.7).

Per block and bus:

- Volume and mute are **ramped across the block** (from the previous block's gain to the new one). Muting therefore does not click.
- Optionally a low-pass: an RBJ biquad (Audio EQ Cookbook), Q 0.7071 (Butterworth), cutoff frequency between 10 Hz and 0.45 × rate.
- The signal goes into the master and, weighted by `reverb_send`, into the reverb input.
- Peak and RMS levels are measured per bus.

### Reverb (Freeverb)

The master reverb is Freeverb by Jezar at Dreampoint: per channel, **eight parallel damped comb filters**, followed by **four all-pass filters in series**.

- A comb filter is a delay line with feedback. It produces a series of ever quieter echoes spaced by its length. The damping in the feedback path (`store = out · (1 − damp) + store · damp`, a low-pass) makes high frequencies decay faster – as in a real room.
- Eight combs with slightly different lengths (1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 samples at 44.1 kHz) overlap into a dense cloud of echoes.
- The all-pass filters (556, 441, 341, 225) smear the remaining structure without coloring the frequency response.
- The right channel has lines that are 23 samples longer. This decorrelates the channels and makes the reverb wide.

The lengths are scaled to the actual rate. `set_reverb(room_size, damping, wet)` sets `feedback = 0.7 + 0.28 · room_size` and `damp = 0.4 · damping`; the input gets Freeverb's fixed gain of 0.015 on the sum of both channels, and the reverb signal is added to the master with `wet × 3`.

### Limiter and soft clip

At the end sits a peak limiter:

```
peak   = max(|L|, |R|)
needed = peak · gain > 0.92 ? 0.92 / peak : 1
gain   = needed < gain ? needed : 1 − (1 − gain) · release
```

The attack is instantaneous: the gain drops before a peak exceeds the 0.92 threshold. The release follows an exponential curve with a 120 ms time constant (`release = exp(−1 / (0.12 · rate))`). After that comes a soft clip with `tanh` above the threshold. With the gain in front of it, it is normally not reached; but it catches any remainder, including NaN. Every output sample thus stays within [−1, 1].

`stats()` returns voices (active, audible, virtual, stolen), clips, rendered frames, the limiter's strongest gain reduction in dB, peak and RMS levels of the master and the buses, and a peak-hold value that falls at 20 dB per second like the needle of a level meter. The Studio status bar shows the level meter from this.

## 7.5 The synthesizer

Kern ships no sound files. All built-in sounds are created from code at load time: a small subtractive synthesizer in the tradition of sfxr, extended with resonant filters, noise colors, FM and layering, plus a step sequencer for music loops (`synth.h`). Everything is rendered offline into clips, with fixed seeds and therefore identical on every platform.

### `SynthPatch`

| Group | Fields |
|---|---|
| Waveform | `wave`: `square`, `saw`, `sine`, `triangle`, `noise`, `pink`, `pulse`; `volume` |
| Envelope (s) | `attack`, `sustain`, `punch`, `release` |
| Pitch | `frequency`, `slide` (octaves/s), `slide_accel` (octaves/s²), `min_frequency`, `vibrato_depth` (semitones), `vibrato_rate`, `arp_ratio`, `arp_time` |
| Pulse width | `duty`, `duty_sweep` |
| FM | `fm_ratio`, `fm_depth` (sine only) |
| Filter | `lowpass` (Hz), `lowpass_sweep` (octaves/s), `resonance` (0–0.95), `highpass` (Hz) |
| Modulation | `tremolo_depth`, `tremolo_rate`, `repeat` (s), `bitcrush` (0–1) |
| Noise | `seed` |

### How `synthesize()` computes

- **Control-rate modulation.** Slide, vibrato, tremolo and filter sweep are only recomputed every 16 samples. According to the comment, `exp2`/`sin` per sample would dominate the cost for long notes; 16 samples are far below audible steps.
- **Oscillators** via a phase in [0, 1): square and pulse at ±0.6, sawtooth, triangle, sine (with optional phase modulation by a second sine at the ratio `fm_ratio`).
- **Noise** is re-rolled, as in sfxr, at the rate of the oscillator frequency (at eight times the frequency; every sample from a start frequency of 10 kHz upward): low notes sound darker. Pink noise is made from white noise with Paul Kellet's economy filter (three one-pole low-passes).
- **Low-pass:** Chamberlin state-variable filter with resonance (`q = 1 − resonance`), cutoff coefficient `2 · sin(π · f / rate)`. **High-pass:** one-pole.
- **Bitcrush** as sample-and-hold.
- **Envelope:** linear attack, sustain phase with optional "punch" (an overshoot that falls linearly back to 1), linear release.
- `repeat` restarts the pitch envelope periodically (power-up effect), `arp_time` jumps once by `arp_ratio` (pickup effect).

Helper functions: `mix_into` (layering with time offset), `make_seamless` (the last milliseconds are laid over the beginning with an equal-power crossfade, after which the buffer is a seamless loop), `normalize`.

### Modal synthesis for strikes

For impact sounds of small solid bodies the library has a second method, `struck()`: a few exponentially decaying sine partials (modes of the body) plus a short, low-pass-filtered noise burst for the contact itself, with a 2 ms soft onset. The code comments justify the choice of modes: a solid plastic part sounds neither low nor long – a low, long-ringing partial sounds like a hollow box. That is why the modes of `plastic` lie in the middle range (760, 1650, 3100 Hz) and decay within a few milliseconds.

### Music

The two music loops are scores: every bar puts its notes into a queue; identical notes are rendered only once and reused.

- `music_drive`: 120 BPM, Am–F–C–G, four-on-the-floor kick drum, snare on 2 and 4, eighth-note bass, sixteenth-note arpeggio, pads. 8 bars of 2 s each.
- `music_calm`: 80 BPM, Fmaj7–Cmaj7–Am7–G6, pads, bass and a pentatonic bell melody. 8 bars of 3 s each.

Both are stereo and seamless.

### `BuiltinJob`: rendering in time slices

Rendering music takes a few hundred milliseconds. `BuiltinJob` spreads this out: `start(name, rate)`, then `step(budget_ms)` – for music roughly one bar, or as many notes as fit into the budget – until `done()`, then `take(channels)`. Short effects finish in one step. According to `docs/studio.md`, a level with music therefore does not stutter while loading.

### The built-in library

Sounds are referenced with `"builtin:<name>"`. `builtin_sounds()` lists, in display order:

| Category | Names |
|---|---|
| Gameplay | `pickup`, `powerup`, `jump`, `land`, `goal`, `hazard`, `checkpoint`, `lose`, `laser` |
| Combat | `gunshot`, `smg`, `pistol`, `sniper`, `launcher`, `reload`, `hit`, `down` |
| Physics | `explosion`, `impact`, `impact_light`, `break`, `step`, `plastic`, `plastic_step` |
| Vehicle | `engine`*, `hover`*, `boost` |
| Ambience | `wind`*, `city`*, `hum`* |
| Robot | `servo`*, `quack`, `beep` |
| UI | `click`, `confirm`, `error` |
| Music | `music_drive`*, `music_calm`* |

Entries marked with * are loops. In addition there are the Micro Duck's sounds (7.6), which are not in this list but can be reached via a naming scheme.

Two examples from `render_builtin` to show how the sounds are built:

```cpp
} else if (name == "pickup") {
  SynthPatch a = patch(Wave::pulse, 988, 0.002f, 0.05f, 0.28f, 0.42f);
  a.duty = 0.35f;
  a.arp_ratio = 1.5f;          // a fifth higher after 65 ms
  a.arp_time = 0.065f;
  out = synthesize(a, rate);
  add(out, bell(rate, 100, 0.35f), 0.35f);
} else if (name == "impact" || name == "impact_light") {
  // A body landing on something: a thud, not a crash - the noise is kept below 2 kHz.
  ...
```

## 7.6 The voice of the Micro Duck

The Micro Duck speaks with a small synthesizer of its own (`duck_voice.h`), a port of the `sounds` crate from pollen-robotics/microduck (Rust, Apache-2.0). There are no samples.

![The Micro Duck in the level: its sounds come from its head, on the voice bus (duck shown in this book's own design)](../buch/bilder/duck-level.png)

### Personality from a seed

A single `u32` seed deterministically determines a `DuckPersonality`: pitch and register, glissando tendency, overtone tilt, nasality, formant, vibrato, jitter, breath, "quackiness" (amplitude modulation at 18–55 Hz), trill and tempo (0.82–1.22). Two seeds sound like two different animals; the same seed always sounds the same.

As on the robot, the seed is derived from an identifier: the first four bytes of the SHA-256 of the identifier, big-endian (`duck_seed_from_id`; Kern contains a small SHA-256 implementation of its own for this). The robot uses its serial number, Kern the name and the actor id (`name + "#" + id`, in `src/game/walker.cpp`).

### How a sound is made

- **Body of the sound:** seven harmonics with weights from the personality:
  ```
  base      = 1 / n^tilt
  high_lift = brightness · (n/7)^1.5
  nasal     = nasal · 0.6       (2nd and 3rd harmonics only)
  skew      = even/odd emphasis according to harmonic_skew
  formant   = formant_gain · base · 1.5   (only for n = formant_n)
  w_n = max(base + high_lift + nasal + skew + formant, 0),  w_1 ≥ 0.7
  ```
- Each sound's pitch curve is multiplied by vibrato and jitter and integrated into the phase.
- **Quacking:** amplitude modulation `1 − am_d · (0.5 + 0.5 · sin(2π · am_rate · t))`.
- **Breath:** pink noise, mixed in with `breath`.

### Sounds and variants

Seven tags: `alarm`, `greet`, `inquire`, `peck`, `chirp`, `coo`, `wheee`. `greet` and `chirp` have 12 variants each, `wheee` 6, all others 10 (`duck_variant_count`). A random variant is played so that the duck does not sound like a stuck recording. `wheee` is split into start, loop and end (`render_duck_wheee`) so that a player can hold the loop for as long as a ride lasts.

### Bit-exact like the Rust original

The port is designed for sample equality at 48 kHz: the same xoshiro256++/splitmix64 stream including the Box-Muller reserve, the same order of random draws, the same constants, `f32` and `f64` exactly where the Rust original uses them. Fused multiply-add is disabled by pragma so that `a*b + c` rounds twice as in Rust. According to `docs/walker.md`, a comparison of all 328 buffers against the Rust crate yields the same length and a maximum deviation of 0.

As on the robot, each sound then exists as a 16-bit buffer (× 32767, clamped, truncated). Only then is it brought to the mixer rate with cubic interpolation. Finally comes a second-order high-pass at 300 Hz: the robot's small speaker reproduces almost nothing below that (`SPEAKER_ROLLOFF_HZ`), and Kern emulates this.

### Integration

A sound is an ordinary sound reference following the scheme

```
builtin:duck_<tag>_<seed>_<variant>
builtin:duck_wheee-start_<seed>_<variant>    (likewise -loop, -end)
```

`find_builtin` and `render_builtin` recognize these names. The runtime sends them as a `SoundCue` with `voice = true` on the `voice` bus, with the duck's head as the position (that is where the speaker sits). A new sound cuts off the current one – like a single speaker. During `wheee`, other sounds are discarded.

Because synthesis on a phone can cost a few tens of milliseconds per sound (long enough to starve the audio output), `GameWorld::warm_walker_voices()` pre-warms each duck's sounds: one synthesis per frame, most frequent first (`duck_voice_warm`, cached process-wide). When the duck calls which sound is described in [Chapter 13](13-micro-duck.md) and in `docs/walker.md`.

## 7.7 Sound in the game: `SoundBank` and `GameAudio`

The game runtime uses the mixer through two classes from `include/kern/game/sound.h`. The header comment shows the usage:

```cpp
audio::Mixer mixer;  mixer.init(48000);
SoundBank bank(mixer);
GameAudio audio(mixer, bank);
world.start(level, SimMode::play);  audio.begin(world);
// every frame:
bank.prepare(4); audio.update(world, camera, dt);   // the host pulls mixer.render()
world.stop();  audio.end();
```

### Sound references

Wherever a level names a sound, there is a string:

- `"builtin:<name>"` – a synthesized sound,
- the id of an imported `SoundAsset` – a file stored in the level as a data URL (WAV, MP3, OGG, M4A/AAC, FLAC – whatever the browser decodes),
- `""`, `"none"` – silent; `"auto"` – the default of the respective action.

`sound_reference_ok(level, ref)` checks a reference for validity.

### `SoundBank`

The bank turns references into mixer clips.

- `clip(ref)` returns the clip, or 0 as long as it is not ready – and requests it in the process.
- **Built-in sounds** go into a queue. `prepare(budget_ms)` renders them with `BuiltinJob` in time slices and hands the result over with `adopt_clip`.
- **Imported sounds** are "requested". The host fetches the list with `pending_imports()` and the data URL with `source(id)`, decodes it and returns the PCM with `set_pcm(id, samples, frames, channels, rate)`; from that, the bank creates a clip with `create_clip` (windowed-sinc resampling). If decoding fails, the host reports `set_failed(id, reason)`.
- `request_level(level)` requests in advance everything a level may need: music, ambience, impact and break sounds, the controllers' engine, jump and boost sounds, footsteps and landings depending on the movement type, weapon and hit sounds, pickup and trigger sounds (with the defaults `goal`, `hazard`, `checkpoint`, `confirm`), audio sources, `goal`/`lose` if the level can be won or lost, and `impact_light` if `impact` is used anywhere. Play then starts with everything in place.
- `sync_assets(level)` discards clips of assets that have been removed or replaced (recognized by the identity of the source).

### `GameAudio`

`GameAudio` plays back a running `GameWorld`.

**`begin(world)`** takes music and ambience from the World Settings, determines the player's engine loop and collects all audio sources with `autoplay`.

**`apply_settings(w)`** maps the level's World Settings onto the mixer (`update` calls it every frame):

| Level | Mixer |
|---|---|
| `master_volume` | master bus |
| `music_volume`, `ambience_volume` | buses `music`, `ambience` |
| `reverb` r (0–1) | reverb send: `sfx` 0.7 r, `voice` 0.5 r, `ambience` 0.3 r, `music`/`ui` 0; `set_reverb(0.55 + 0.4 r, 0.45, 0.12 + 0.5 r)` |
| `doppler` | `Mixer::doppler_factor` |

**`update(world, camera, dt)`** every frame:

1. **Pause** follows the world (`set_all_paused`).
2. **Listener** at the camera; its velocity from the eye's motion since the last frame. Above 200 m/s it counts as a camera cut, and the velocity becomes 0 – otherwise every cut would produce an absurd Doppler jump.
3. **Music and ambience** start as 2D loops (priority 100, 0.6 s fade-in) as soon as their clips are ready.
4. **Player engine:** a 3D loop at the player, without Doppler (the camera follows the player; self-Doppler would be wrong), priority 90. Volume and pitch follow speed and throttle, with exponential smoothing:
   ```
   speed   = clamp(hud.speed / top_speed, 0, 1.2)
   volume → 0.32 + 0.45 · speed + 0.18 · throttle     (max. 0.95; not playing: 0.12)
   pitch   = 0.72 + 0.95 · speed + 0.08 · throttle    (0.5–2.2)
   ```
   For the walking duck (`walker`), the "engine" is the servo noise: quiet, louder with the work of the motors (torque × velocity), higher with their speed.
5. **Audio sources** follow their actors; their velocity is estimated from the change in position (for Doppler). Loops start with priority 40 and a 0.3 s fade-in, one-shots with priority 20, both after their start delay. With `pitch_by_speed`, the pitch rises with the actor's speed: `pitch · (1 + pitch_by_speed · speed · 0.1)`. If the actor dies, the voice fades out in 0.15 s.
6. **Cues from the simulation** (7.8) are played. If a cue's clip is not ready yet, it waits up to 0.25 s and is then discarded – a sound that comes too late is worse than none. More than 64 waiting cues are cut off.

Cue voices have `min_distance` 3 m, `max_distance` 80 m and priority 50 (bus `ui`) or 10 (everything else). An effect therefore never displaces an important continuous sound (music, engine, loops) when voices are scarce – but the reverse can happen, since `play()` only steals voices with at most equal priority. `cues_played()` and `cues_dropped()` keep count.

**`end(fade)`** fades out everything from the game. `set_muted` mutes the master and persists across `begin`/`end` (the editor's mute switch).

### Which sounds controllers use

With `engine_sound = "auto"`, `engine_sound_for(controller)` picks the loop by movement type:

| Controller | Loop |
|---|---|
| Hover Car, Flyer | `builtin:hover` |
| Vehicle | `builtin:engine` |
| Walker (Micro Duck) | `builtin:servo` |
| Rolling, Character, Soldier | none |

Further fields on the controller: `jump_sound` (`builtin:jump`), `boost_sound` (`builtin:boost`). Weapons have `fire_sound` (`builtin:smg`) and `reload_sound` (`builtin:reload`), health has `hit_sound` (`builtin:hit`) and `death_sound` (`builtin:down`), pickups `sound` (`builtin:pickup`), triggers `sound` (`"auto"`: matching the action).

## 7.8 Cues and impact sounds

### `SoundCue`

The simulation does not play anything itself. During the fixed physics steps it produces **cues** and collects them; `GameAudio` empties the list every frame:

```cpp
struct SoundCue {
  std::string sound;              // "builtin:<name>" or imported id
  Vec3 position;
  f32 volume = 1, pitch = 1;
  bool spatial = true;
  audio::Bus bus = audio::Bus::sfx;
  ActorId actor = 0;
  bool voice = false, silence = false;   // actor's voice: cuts off / only falls silent
};
```

Because cues arise in the fixed steps, they are deterministic. Without a consumer (headless, for example in the Lehrer), `GameWorld::cue` keeps only the newest 64. Over the network, sounds are transmitted unreliably, whereas events such as collected pickups are transmitted reliably ([Chapter 10](10-networking.md)).

### Impact sounds

Every rigid body has `impact_sound` (default `builtin:impact`) and `impact_threshold` (2.5 m/s approach speed). Every collider additionally has `impact_sound`, default `"auto"` = the sound of its body. One part of a body can thus sound different from another: the duck's sole makes `plastic_step`, its housing `plastic`.

The sequence in `GameWorld` (`src/game/runtime.cpp`):

1. Only shapes that have a sound (or are needed by the player or the logic) report Box3D hit events. Box3D's global hit threshold starts at `kHitSpeed` (2 m/s; from here on a hit counts for the logic and camera shake) and is lowered to the smallest threshold of all sounding bodies, but never below 0.05 m/s (`kMinImpactThreshold`).
2. At most 6 impact sounds per physics step – the mixer would steal more anyway.
3. Below the body's threshold it stays silent.
4. **Debouncing:** a sole rolls from heel to toe and settles in small hops, which Box3D reports as individual hits. A footstep (a sound with "step" in its name) therefore sounds at most every 0.5 s per actor, a knock every 0.09 s.
5. **Strength:** `clamp((v − threshold) / (5 · threshold) + 0.2, 0.2, 1)` becomes the volume.
6. **Light impact:** for `builtin:impact` below 2.2 times the threshold, `builtin:impact_light` is played.
7. **Pitch by mass:** `clamp(1.05 − log10(max(0.1, mass)) · 0.1, 0.7, 1.15)`. Heavy things sound lower; light ones only slightly higher, because strongly pitched-up knocks sound glassy.

The Micro Duck sets its threshold to 0.12 m/s so that even light touches are audible. Breaking joints play `break_sound` (`builtin:break`) at the position of their actor.

## 7.9 Level settings and the audio source

### World Settings

```cpp
f32 master_volume = 1;
std::string music;            // 2D loop on the music bus ("" = none)
f32 music_volume = 0.6f;
std::string ambience;         // 2D loop on the ambience bus
f32 ambience_volume = 0.6f;
f32 reverb = 0.25f;           // master reverb 0..1
f32 doppler = 1;              // Doppler scale for all 3D sounds
```

### `AudioComponent`

The "Audio Source" component turns an actor into a sound source – machine hum, a radio, wind, a one-off sound:

| Field | Default | Meaning |
|---|---|---|
| `sound` | `builtin:hum` | reference |
| `volume` | 1 | linear, 0–4 |
| `pitch` | 1 | playback rate |
| `loop` | on | |
| `autoplay` | on | starts with Play/Simulate |
| `spatial` | on | 3D at the actor; off: 2D on the bus |
| `min_distance`, `max_distance` | 2 m, 40 m | full volume inside, silent beyond |
| `rolloff` | `inverse` | |
| `bus` | `sfx` | |
| `doppler` | 1 | 0 = no Doppler |
| `pitch_by_speed` | 0 | additional pitch per m/s of the body |
| `delay` | 0 | seconds after the start |

The Studio catalog contains ready-made variants: Audio Source (machine hum), Ambient Wind (audible everywhere), City Ambience (with falloff) and Radio (plays `music_calm` in 3D). For operation in the editor, see [Chapter 11](11-studio.md).

## 7.10 The browser connection

The mixer renders PCM; the browser plays it. This task is handled by `KernAudio` in `web/studio/game.mjs`, which Studio and exported games share.

### Starting

```js
export function startAudio(engine) {
  if (!KernAudio.supported()) return null;
  const audio = new KernAudio(frames => engine.audioRender(frames));
  const rate = audio.create();
  if (!rate || !engine.audioInit(rate)) return null;
  setInterval(() => { audio.pump(); engine.audioPrepare(5); }, 20);
  return audio;
}
```

- `create()` creates the `AudioContext` (`latencyHint: "interactive"`) and, where available, sets `navigator.audioSession.type = "playback"`. On iOS this ensures that the mute switch does not silence games.
- Browsers only start audio after a user gesture. The first `pointerdown`, `keydown`, `touchend` or `mousedown` action resumes the context and connects the output. When the page becomes visible again after an interruption, `KernAudio` resumes an "interrupted" context.
- The mixer receives the device's rate (`kern_studio_audio_init(rate)`). Nothing has to be resampled in the browser.
- A timer every 20 ms refills the output and renders built-in sounds with a 5 ms budget (`kern_studio_audio_prepare`). Sound thus keeps flowing when frames are slow or the tab is in the background, and music gets finished between frames too.

### The AudioWorklet path

Preferably, output runs through an AudioWorklet. The processor source is a string in `game.mjs` and is loaded as a blob URL, or as a fallback as a `data:` URL (in case a Content Security Policy blocks blobs).

- The main thread renders chunks of 128 to 2048 frames (`kern_studio_audio_render`, at most 16,384 frames per call), copies them and sends them to the worklet via `postMessage` with a transferred buffer.
- The worklet plays the queue and reports back every 4 blocks how many frames it has consumed and how often it ran dry.
- `pump()` keeps the queue `latency` seconds ahead, rounded to 128 frames. The initial value is 70 ms. **Adaptive:** if the queue runs dry (the main thread stalled longer than the queue lasted), 30 ms are added, up to at most 200 ms. After 15 seconds without dropouts, the lead decreases again by 5 ms per step toward the initial value.
- **Dropouts without clicks:** if the queue is empty, the worklet does not jump to silence. The last sample glides to zero over about 2 ms (96 samples), and when data arrives again, it is faded in. A gap remains audible, but without a pop.
- After a very long stall (more than one second behind), counting starts over.

Where no AudioWorklet is available, `KernAudio` uses a `ScriptProcessor` with 1024 frames that renders directly in its callback.

### Decoding imported sounds

Imported sounds are stored in the level as data URLs. `pollSounds(audio)` asks the engine for pending imports (`kern_studio_pending_sounds`), fetches the source (`kern_studio_sound_source`), decodes it with `decodeAudioData` and hands over the PCM with `kern_studio_sound_pcm` (interleaved `f32`, 1 or 2 channels, any rate). More than two channels are first downmixed to stereo (even channels left, odd right). If something fails, `kern_studio_sound_failed` reports the reason; cues with this sound are then discarded instead of waiting forever.

## 7.11 Notes for your own games

- **References instead of files.** The built-in sounds cost no download and are deterministic out of the box. Your own files are worthwhile for music and characteristic sounds.
- **Use the buses.** Music on `music`, continuous sounds on `ambience`, effects on `sfx`, interface on `ui`, voices on `voice`. Then volume controls and reverb behave as expected; `music` and `ui` get no reverb.
- **Set `max_distance` honestly.** Beyond it, voices become virtual and cost no mixing time.
- **Priorities.** Important continuous sounds get a high priority, short effects a low one. Otherwise, with 64 voices and a lot of action, the wrong ones get stolen.
- **Thresholds for impacts.** An `impact_threshold` that is too low makes rolling or resting bodies knock constantly; debouncing only helps so much.
- **No sound without a gesture.** In the browser, nothing can be heard until the first interaction. That is a browser rule, not a bug.

The mixer itself is independent of the game runtime. If you want sound in your own `kern::App` ([Chapter 12](12-your-own-games.md)), you can use `audio::Mixer` and `audio::render_builtin` directly. For browser output, however, the app then has to export a render function itself and connect an output like `KernAudio`; `web/app.mjs` contains no audio connection.

---

© 2026 Christian Heinrich Hohlfeld · All rights reserved · [Imprint and rights](00-imprint.md)
