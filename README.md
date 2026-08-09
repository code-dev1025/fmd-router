# Real Mono Sound — stereo to a mono feed that keeps the difference

A standalone Windows app that takes audio on its way out of another
application — a browser tab, a streaming service, a media player — converts it
to a **true mono-compatible** signal, and plays only that to your speakers. The
original stereo never reaches an output device.

![The app running](docs/ui-running.png)

The point is what happens to the difference between the two channels. A plain
`(L+R)/2` downmix cancels it: anything panned wide, anything in anti-phase, the
reverb tail that was only ever in the Side, all of it goes silent the moment
the two channels meet. Real Mono rotates the Side by 90° before the sum, so it
survives instead.

| Content | Plain `(L+R)/2` | Real Mono |
| --- | --- | --- |
| Correlated / centre | present | present |
| Hard panned | −6 dB | −3 dB, in quadrature |
| Pure difference (`R = −L`) | **silent** | recovered at full level |

Those three rows are asserted in `tests/TestRealMono.cpp`, not claimed here.

## The chain

```
  stereo in
    │
    ├─ Stage 0   HQ input trim         −3 dB, off by default
    ├─ Stage 1   Side high-pass        BYPASSED — the client's process has none
    ├─ Stage 2   M/S split             M = (L+R)/2      S = (L−R)/2
    │                                        │               │
    ├─ Stage 3   +90° rotation      delay-aligned      rot₊₉₀(S)
    ├─ Stage 4   mono commit           mono = M + rot₊₉₀(S)
    ├─ Stage 5   look-ahead limiter    ceiling −0.3 dBFS
    └─ dual mono out                   outL = outR
```

Which is, in one line:

```
mono = 0.5 · [ (L + R) + rot₊₉₀(L − R) ]
```

### Where the ±90° went

The reference process describes a Side-only *stereo bus* with +90° on its left
and −90° on its right — MSED into PHA-979, twice. That bus is `(S, −S)`, and a
−90° rotation is the negative of a +90° one, so after rotation it is
`(rot₊₉₀(S), rot₊₉₀(S))`: both channels identical. Its mono downmix is exactly
`rot₊₉₀(S)`, so the two-rotator arrangement collapses to one rotation of S.
Same signal, half the work.

### Levels, and the overshoot

M and S use the 0.5 convention, so correlated content comes out at unity and
pure difference content also comes out at unity. What overshoots is programme
material: `rot₊₉₀(S)` has the same magnitude spectrum as S but a different
waveform, so its peaks land in different places and the sum can exceed
`max(|L|,|R|)`. On the test suite's programme-like material that overshoot
measures **+2.26 dB**, which is the ~3 dB the client reported. Stage 5 catches
it; highest quality mode takes 3 dB at the input instead, so the sum has the
headroom before it needs winning back.

## Interception

Windows has no supported way for a normal application to reach into another
app's audio and change it in place. There are three ways to get near it, and
this app takes the third:

| Approach | Why not / why yes |
| --- | --- |
| Kernel virtual audio driver (like VB-CABLE itself) | Needs an EV code-signing certificate and WHQL attestation to load on a normal machine. Cannot be shipped from a source checkout. |
| System-effect APO injected into the endpoint | Still a driver-package INF install and still signed, and support varies by audio driver. This is the brief's preferred path and remains open — the DSP does not care which side feeds it. |
| **Route through a virtual cable and process in user mode** | **No driver, no signing, works today. Costs one free third-party install and one Windows setting.** |

```
  Browser / streaming service / player
            │
            ▼
  ┌───────────────────────┐
  │  CABLE Input          │   ← set this as the Windows default playback device
  │  (VB-Audio VB-CABLE)  │
  └───────────┬───────────┘
              │  the audio is now inside the cable,
              │  and is NOT going to any speaker
              ▼
  ┌───────────────────────┐
  │  CABLE Output         │   ← captured here (WASAPI, event-driven)
  └───────────┬───────────┘
              ▼
     ring buffer + drift-corrected resampler
              ▼
       the Real Mono chain above
              ▼
  ┌───────────────────────┐
  │  Your speakers        │   ← rendered here, dual mono
  └───────────┬───────────┘
              ▼
             🔊  one coherent feed, every room
```

The unprocessed audio never reaches a speaker because the only device it was
ever sent to is the cable, and the cable has no speaker on it.

## Setup

1. **Install VB-CABLE** — the free VB-Audio Virtual Cable driver, from
   <https://vb-audio.com/Cable/>. Reboot when it asks.
2. **Make it the default output.** Settings → System → Sound → Output →
   *CABLE Input (VB-Audio Virtual Cable)*. Everything now goes silent, which is
   correct: the audio is in the cable and nothing is playing it yet.
3. **Open Real Mono Sound.** It preselects *CABLE Output* as the source as soon
   as it sees one, and your default device as the destination. Set *Play
   processed to* to your real speakers or headphones.
4. **Press Start.**

To send only *one* app through the chain rather than everything, leave the
system default alone and use Settings → System → Sound → Volume mixer → *App
volume and device preferences*, setting just that app's output to *CABLE
Input*.

### Trying it without installing anything

Tick **Loopback** and the source list becomes your *playback* devices; the app
taps one of them instead of a capture endpoint. This is good for auditioning
the chain, but it is not interception — you hear the original alongside the
mono version, because the original is still going to a real speaker.

Pointing loopback at the same device you render to is refused rather than
warned about: it is a feedback loop that reaches full scale in well under a
second.

## The interface

![Idle](docs/ui-idle.png)

Four controls, because that is what the demo has: a preset, the global enable,
highest quality mode, and the limiter ceiling. The global enable is a genuine
stereo pass-through, so it is the A/B.

Everything else is behind **Advanced**, where every one of the five stages has
its own visible bypass — no stage is a code-only flag.

![Advanced](docs/ui-advanced.png)

| Preset | What it is |
| --- | --- |
| `RealMono_Default` | Stages 2–5 on, Stage 1 off, HQ off, ceiling −0.3 dBFS |
| `RealMono_HighestQuality` | The same, plus the −3 dB input trim |
| `Lab_SafeMidOnly` | Rotation off, commit set to Mid only — classic mono, the reference to A/B against |
| `Lab_WithMonoLF` | Default plus Stage 1 at 120 Hz |

Bypassing a stage passes audio through in the domain the graph expects:
Stage 2 off leaves the plain sum (with no Side there is nothing to rotate),
Stage 4 off leaves the rotated result in stereo as `M ± rot₊₉₀(S)`, and the
global bypass is untouched stereo.

Switching any of that mid-programme fades the output through zero for a few
milliseconds first, holds it down while Stage 5's look-ahead buffer flushes the
audio it committed under the old settings, and fades back in. Without the hold,
a switch clicks exactly one look-ahead later — which is what the fade was
supposed to prevent and did not, until it was measured.

## Stage reference

### Stage 0 — highest quality mode
−3 dB at the input, so the Mid + rotated Side sum keeps its dynamics instead of
being handed back by the limiter. Exactly 3 dB, asserted. Off by default; the
output is 3 dB quieter, which is the point.

### Stage 1 — Side high-pass (lab, bypassed)
The client's process has no high-pass, so this ships off. It is kept because
the brief wants it available for LF double-counting experiments, and the
interface bypasses it rather than the code omitting it.

6 / 12 / 24 dB per octave, 40–400 Hz, in two shapes. On a mono output "make the
bass mono" and "high-pass the Side" are the same statement, so the shape
control is what actually differs: Butterworth (−3 dB at fc) or Linkwitz-Riley
(−6 dB at fc, the half of a crossover whose complement is the Mid-only band).

### Stage 2 — M/S split
`M = (L+R)/2`, `S = (L−R)/2`, with a gain on each. Bypassed, there is no Side,
and the chain degrades to the classic downmix.

### Stage 3 — the rotation
Three qualities, and the trade is latency against phase linearity:

| Mode | Delay at 48 kHz | Flat from | Phase |
| --- | --- | --- | --- |
| Linear phase, HQ (1023 taps) | 511 samples, 10.65 ms | ~120 Hz | linear |
| Linear phase, short (255 taps) | 127 samples, 2.65 ms | ~300 Hz | linear |
| Allpass IIR | 1 sample | 20 Hz | non-linear, shared by both paths |

The linear-phase modes are a windowed type-III FIR Hilbert transformer — the
PHA-979 behaviour, every frequency shifted by the same 90° — and the Mid path
is delayed to match, sample-exactly.

The allpass mode is a pair of four-section allpass chains whose outputs stay 90°
apart. Neither branch is linear phase, but *both carry the same phase*, so the
90° between Mid and Side is exact while the latency is one sample. Measured
worst error across 20 Hz–20 kHz: **0.69°**. It is not the default because
transients smear where the FIR's do not.

Measured phase error for the FIR modes across their flat band: **0.00°**.

### Stage 4 — mono commit
`Mid + rotated Side` is the product. `Sum` (the raw reference downmix, ignoring
the Mid gain) and `Mid only` are there to A/B against. `Side energy fold` and
`Polarity matrix` are lab modes and are not the client's process.

### Stage 5 — look-ahead limiter
Exact sliding-maximum peak detection over the look-ahead window, so the gain is
already down before the peak it is reducing arrives; one-pole attack at a fifth
of the window, 100 ms release, and a final clamp so "brickwall" is true rather
than nearly true. Ceiling −3…0 dBFS, look-ahead 5–10 ms. Latency is exactly the
look-ahead.

## Latency

The chain reports its own delay, and the app adds it to the round trip on
screen rather than leaving it out of the number:

| Part | At the shipping defaults, 48 kHz |
| --- | --- |
| Device period, capture / render | 10.0 / 10.0 ms |
| Ring buffer (`targetBufferMs`) | 30.0 ms |
| **Stage 3 rotation** | **10.65 ms** (511 samples) |
| **Stage 5 look-ahead** | **5.00 ms** |
| Render endpoint buffer | 22.0 ms |
| **Round trip** | **~67.6 ms** |

If that is too much for video, the Advanced page's quality control is where the
15.65 ms of chain latency goes: short FIR takes it to 7.65 ms, allpass to
5.02 ms. Add a virtual cable in front and its own period joins the total.

### Clock drift

The capture and render endpoints are different devices with different crystals.
Even when both say 48 kHz they are not the same 48 kHz, and a hundred parts per
million of drift will empty or overflow the ring within minutes. The
resampler's ratio is therefore trimmed by a slow proportional controller on the
ring's fill level — hard-smoothed to about 1 Hz so it corrects the trend rather
than packet jitter, and clamped to ±0.5% (about 8 cents) so a misbehaving
controller degrades to slightly-wrong-speed rather than to a chirp.

The same mechanism handles a genuine rate mismatch, so a 44.1 kHz cable into a
48 kHz card needs no special case.

## The DSP, and RS-MET

The brief asks for RS-MET (`rapt` / `rosic`) for the Hilbert transformer, the
FIR designer and the limiter. No RS-MET checkout is present in this workspace,
so `src/RealMono.h` is the noted fallback: published, small-footprint designs
written to the same interfaces, so substituting the real thing is a change
inside `RealMonoChain` and nowhere else.

| Here | RS-MET equivalent |
| --- | --- |
| `HilbertFir` | `rapt` FIR designer + convolver |
| `QuadratureNetwork` | `RAPT::QuadratureNetwork` |
| `Biquad` / `HighpassChain` | `rosic` cookbook biquads, Linkwitz-Riley |
| `LookaheadLimiter` | `rosic::Limiter` |

If RS-MET is adopted, note the licence question early: it is dual-licensed, and
a commercial Windows product may need a commercial arrangement rather than the
GPL side. Credit to Robin Schmidt either way.

Convolution with a client-supplied IR (Stage 3's `convolution_ir` in the brief)
is **not** implemented: no IR was supplied, and a mode that cannot be exercised
is worse than an absent one. The FIR path is where it would go.

## Building

Needs Visual Studio 2022 (or Build Tools) with the C++ workload, the Windows
SDK, and CMake 3.20+. Built and tested with MSVC 19.44 / Windows SDK 10.0.26100.
No third-party dependency.

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Produces `build/Release/fmd-router.exe` and `build/Release/fmd-router-tests.exe`.
Compiles clean at `/W4 /permissive-` with no warnings.

### Tests

```sh
build/Release/fmd-router-tests.exe        # exit code 0 = all passed
```

71 checks over the chain, the ring and the resampler, none of which touch a
Windows API. The ones worth knowing about:

- Anti-phase input is recovered at its original level, and the app's own
  classic-mono mode cancels the same input to −240 dB.
- The rotation is measured, not derived: Side leads Mid by 90° across each
  mode's band, and all three modes rotate the same way. A sign error here would
  still sound like Real Mono on most material.
- The Mid path is exactly the reported delay — deviation `1.5e-08` — so the two
  paths cannot silently drift apart.
- The limiter holds its ceiling on a 4× transient, and the reported latency
  matches the parts it is made of at 48 and 44.1 kHz.
- Global bypass is bit-exact stereo, and the normal output is dual mono sample
  for sample.
- A NaN from the capture device never reaches the endpoint, and the chain
  recovers once it has flushed through.

## Hearing it without a cable: `realmono-wav`

Judging the chain by ear through the live path needs a virtual cable and two
endpoints. `realmono-wav` does not: it runs a WAV file through the *same*
`RealMonoChain` the audio thread runs, so what it writes is what the app would
have played. This is the harness for the A/B against the client's before/after
references.

```sh
build/Release/realmono-wav --demo source.wav                      # a test signal
build/Release/realmono-wav source.wav realmono.wav                # the product
build/Release/realmono-wav source.wav classic.wav --preset midonly  # to A/B against
```

The output is latency-compensated by default, so the files line up sample for
sample and can be dropped onto adjacent DAW tracks without nudging anything.
Every stage bypass, quality mode and gain from the Advanced page has a flag;
`--help` lists them.

`--demo` writes twelve seconds in four sections, which is the fastest way to
hear what the product does. Measured on those sections, dB RMS:

| Section | Source | Real Mono | Classic mono |
| --- | --- | --- | --- |
| Correlated | −12.50 | −12.50 | −12.50 |
| **Pure Side** (`R = −L`) | −12.50 | **−12.50** | **−240.00** |
| Hard panned left | −13.47 | −16.48 | −19.49 |
| Widened mix | −11.65 | −11.65 | −13.47 |

The second row is the product: a plain downmix takes it to digital silence,
Real Mono returns it at its original level. The third is the −3 dB quadrature
sum against the −6 dB linear one. The fourth is the case that matters on real
programme material — a mono bass with a widened top, where the top is what goes
missing.

The client's test file is an MP3, so convert first:

```sh
ffmpeg -i "stereo flute and mono guitar.mp3" -c:a pcm_s24le test.wav
```

## What has and has not been verified

**Verified on the build machine** (Windows 10 Pro 19045, Realtek HD Audio):

- Builds clean at `/W4`, both targets.
- All 71 offline checks pass.
- The app starts, runs the new chain live at 48 kHz for 6 s with **zero drops**,
  keeps the drift controller inside 0.01% of unity, and reports 15.6 ms of chain
  latency as part of the round trip. The ring readout is sampled from the GUI
  thread rather than at a block boundary, so it swings by up to one render
  block either side of its 30 ms target; the drops counter is the number that
  says whether that mattered.
- Every control on both pages is bound to a parameter the audio thread reads,
  and the stage bypasses grey out the controls they disable. The enable, HQ and
  Stage 3 switches were clicked while the engine was running, and the chain
  reported the state change each time.
- End to end on a file: the `--demo` signal through `realmono-wav` gives the
  table above, dual mono on every frame, and latency compensation exact to
  0.00000.

**Not verified:**

- **Audibility.** Nobody has listened to it. The numbers say the Side is
  recovered at the right level and the right phase; they say nothing about
  whether the client agrees it is their process.
- **Against the client's material.** `stereo flute and mono guitar.mp3` and the
  before/after references were not available here, so the A/B against Alex's
  MSED + PHA-979 chain has not been run. That is the first thing to do, and
  `realmono-wav` is the tool for it.
- **The VB-CABLE path itself.** No virtual cable is installed on this machine,
  so the capture side was exercised against a physical endpoint — the same
  `IAudioClient` code path, but not the same device.
- **A live signal through the chain.** The 6 s run was on a silent microphone,
  so it proves the path runs, not that it sounds right. The DSP is covered
  offline instead.
- **Mismatched sample rates end to end.** The resampler is unit-tested at ratios
  1.0 and 2.0, but no live 44.1 → 48 kHz pair was available to run.
- **Other hardware.** One Realtek codec, one machine.

## Known limits

- **Sources with more than two channels lose their surrounds.** Only the first
  two channels are read, which are front L/R in every WAVE layout. Browsers emit
  stereo and Windows puts stereo in the front pair, so this is correct in
  practice, but a genuine 5.1 source is not downmixed properly.
- **Output goes to the front pair only**; a centre speaker or LFE is fed
  silence rather than the signal. For multi-room the front pair is what the
  zones take, but a centre channel would want the same feed.
- **Settings are not saved.** Device choice, preset and every control return to
  their defaults on restart.
- **Stage 3's HQ mode rolls off below ~120 Hz** — a windowed FIR Hilbert cannot
  hold 90° all the way down without more taps, and more taps is more latency.
  Side content below that is attenuated rather than rotated. The allpass mode
  holds 90° to 20 Hz if that matters more than linear phase.
- **Capture needs microphone permission.** WASAPI capture is gated by Settings →
  Privacy → Microphone even for a virtual cable; if Start fails with access
  denied, that is why, and the error message says so.
