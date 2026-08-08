# FMD Router — intercept system audio, process it, hear only the result

A standalone Windows app that takes audio on its way out of another
application — a browser tab, a game, a media player — runs it through the FMD
filter chain, collapses it to mono, and plays only the processed version to your
speakers. The original never reaches an output device.

![The router running](docs/ui-running.png)

Above: running at 48 kHz in and out, 30 ms of buffer, ~52 ms round trip, drift
locked at 1.0000×, zero drops. The OUT meter is lit by the chain's own injected
noise with NOISE at 60%.

## How the interception works

Windows has no supported way for a normal application to reach into another
app's audio and change it in place. There are three ways to get near it, and
this app takes the third:

| Approach | Why not / why yes |
| --- | --- |
| Kernel virtual audio driver (like VB-CABLE itself) | Needs an EV code-signing certificate and WHQL attestation to load on a normal machine. Cannot be shipped from a source checkout. |
| System-effect APO injected into the endpoint | Still a driver-package INF install and still signed, and support varies by audio driver. |
| **Route through a virtual cable and process in user mode** | **No driver, no signing, works today. Costs one free third-party install and one Windows setting.** |

So the app is a router. You point Windows at a virtual audio cable, and the app
captures the far end of that cable, processes it, and renders to your real
device:

```
  Browser / game / player
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
  │  CABLE Output         │   ← FMD Router captures here (WASAPI, event-driven)
  └───────────┬───────────┘
              ▼
     ring buffer + drift-corrected resampler
              ▼
     (L+R)/2  →  FilterCore  →  mono
              ▼
  ┌───────────────────────┐
  │  Your speakers        │   ← FMD Router renders here
  └───────────┬───────────┘
              ▼
             🔊  processed only
```

The unprocessed audio never reaches a speaker because the only device it was
ever sent to is the cable, and the cable has no speaker on it.

## Setup

1. **Install VB-CABLE** — the free VB-Audio Virtual Cable driver, from
   <https://vb-audio.com/Cable/>. Reboot when it asks.
2. **Make it the default output.** Settings → System → Sound → Output →
   *CABLE Input (VB-Audio Virtual Cable)*. Everything now goes silent, which is
   correct: the audio is in the cable and nothing is playing it yet.
3. **Open FMD Router.** It preselects *CABLE Output* as the source as soon as it
   sees one, and your default device as the destination. Set *Play processed to*
   to your real speakers or headphones.
4. **Press Start.**

To send only *one* app through the chain rather than everything, leave the system
default alone and use Settings → System → Sound → Volume mixer → *App volume and
device preferences*, setting just that app's output to *CABLE Input*.

### Trying it without installing anything

Tick **Loopback** and the source list becomes your *playback* devices; the app
taps one of them instead of a capture endpoint. This is good for auditioning the
chain, but it is not interception — you hear the original alongside the
processed version, because the original is still going to a real speaker.

Pointing loopback at the same device you render to is refused rather than
warned about: it is a feedback loop that reaches full scale in well under a
second.

## The chain

```
  L ─┐
     ├─► M = (L+R)/2 ─┬──────────────────────────────────► dry
  R ─┘                │
                      ├─► FilterCore voice 0  (freq × 2^−spread) ─┐
                      └─► FilterCore voice 1  (freq × 2^+spread) ─┴─► wet
                                                                    │
                      out = dry·(1−MIX) + wet·MIX, × OUT ◄──────────┘
```

Three things about this are deliberate:

- **The sum happens before the filter, not after.** So MIX at 0% is already the
  mono downmix — there is no setting at which un-collapsed stereo reaches the
  output.
- **The mono sum is fed through *both* of FilterCore's channels.** SPREAD
  detunes those two channels by up to ±1 octave. On a stereo module that widens
  the image; on a mono output there is no image to widen, so collapsing after
  the filter would leave SPREAD doing nothing. Feeding one mono signal through
  two detuned voices and summing turns it into a second resonant peak instead —
  same knob, same range, still audible in mono. `tests/TestChain.cpp` asserts
  both halves of this: that anti-phase input annihilates even with SPREAD wide
  open, and that SPREAD still changes the output by more than 6 dB.
- **The three products are the three voicings from `fmd-vcv`**, not
  re-implementations. Flower Child is pinned to LP12 with the AGGR switch;
  Shaped Resonator routes grit through the resonance path (CRNCH) and exposes
  BAND/LOW/HIGH; Super Love exposes LP6/LP12/BP/HP with grit as input noise.

### The DSP is still the placeholder

`FmdDsp.hpp` is compiled straight out of the `fmd-vcv` checkout — it is not
vendored or copied, so the two cannot drift apart. That file is still the
stand-in described in `fmd-vcv/README.md`: a TPT state-variable filter, **not**
`FlowerChildFilterCore` from the `soundemoteframework` repository.

When the real core lands, `FilterCore::process()` is replaced in `fmd-vcv` and
this app inherits it on the next build. Nothing here needs to change:
`MonoChain` only ever talks to `FilterCore` through `fmd::FilterParams`.

## Latency

Round trip is the ring buffer plus the render endpoint's own buffer. On the
machine this was built on, at 48 kHz with a Realtek codec:

| Part | Measured |
| --- | --- |
| Device period, capture / render | 10.0 / 10.0 ms |
| Ring buffer (the `targetBufferMs` setting) | 30.0 ms |
| Render endpoint buffer | 22.0 ms |
| **Round trip** | **~52 ms** |

The ring target is a request, floored at one capture period plus one render
period — below that the ring cannot survive the gap between the render thread
draining a period and the capture thread refilling one. Add a virtual cable in
front and its own period joins the total.

The render stream asks `IAudioClient3` for the driver's minimum shared-mode
period before falling back to the 10 ms default, which is the largest latency
win available without going exclusive-mode. This Realtek driver reports 10 ms as
its minimum, so there was nothing to win here; on hardware that reports 3 ms or
less, it will take it.

### Clock drift

The capture and render endpoints are different devices with different crystals.
Even when both say 48 kHz they are not the same 48 kHz, and a hundred parts per
million of drift will empty or overflow the ring within minutes. The resampler's
ratio is therefore trimmed by a slow proportional controller on the ring's fill
level — hard-smoothed to about 1 Hz so it corrects the trend rather than packet
jitter, and clamped to ±0.5% (about 8 cents) so that a controller misbehaving
degrades to slightly-wrong-speed rather than to a chirp.

The same mechanism handles a genuine rate mismatch, so a 44.1 kHz cable into a
48 kHz card needs no special case.

## Building

Needs Visual Studio 2022 (or Build Tools) with the C++ workload, the Windows
SDK, and CMake 3.20+. Built and tested with MSVC 19.44 / Windows SDK 10.0.26100.

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Produces `build/Release/fmd-router.exe` and `build/Release/fmd-router-tests.exe`.
Compiles clean at `/W4 /permissive-` with no warnings.

If `fmd-vcv` is not the sibling directory, point CMake at it:

```sh
cmake -S . -B build -DFMD_VCV_DIR=path/to/fmd-vcv
```

### Tests

```sh
build/Release/fmd-router-tests.exe        # exit code 0 = all passed
```

27 checks over the chain, the ring and the resampler, none of which touch a
Windows API. They cover the mono collapse, the MIX law, that the filter is
actually in circuit and in the right mode per module, NaN safety at extreme
settings, ring wrap and overrun reporting, sample-exactness of the resampler at
unity ratio, and the drift controller's direction and clamp.

## What has and has not been verified

**Verified on the build machine** (Windows 10 Pro 19045, Realtek HD Audio):

- Builds clean at `/W4`, both targets.
- All 27 offline checks pass.
- Ran Microphone → Headphones for 65 seconds at 48 kHz: **zero drops**, ring
  held at its 30.0 ms target, drift controller converged to 1.0000× and stayed
  there.
- The OUT meter is driven from the samples actually written to the endpoint, and
  it lights when the chain's own noise source is opened — so the DSP output does
  reach the audio device.
- Every slider, the module and mode selectors and the AGGR switch move real
  parameters; the mode list is rebuilt per module so no module can be given a
  mode its panel does not have.

**Not verified:**

- **The VB-CABLE path itself.** No virtual cable is installed on this machine,
  so the capture side was exercised against a physical microphone endpoint —
  the same `IAudioClient` code path, but not the same device. This is the one
  thing worth trying first.
- **Audibility and taste.** Nobody has listened to it. The meters and the tests
  say signal is flowing and being filtered; they say nothing about whether it
  sounds good.
- **Mismatched sample rates end to end.** The resampler is unit-tested at ratios
  1.0 and 2.0, but no live 44.1 → 48 kHz pair was available to run.
- **Other hardware.** One Realtek codec, one machine.

## Known limits

- **Sources with more than two channels lose their surrounds.** Only the first
  two channels are read, which are front L/R in every WAVE layout. Browsers emit
  stereo and Windows puts stereo in the front pair, so this is correct in
  practice, but a genuine 5.1 source is not downmixed properly.
- **Mono output goes to the front pair only**; a centre speaker or LFE is fed
  silence rather than the signal.
- **Settings are not saved.** Device choice and every knob return to their
  defaults on restart.
- **The window is fixed-size** and the layout is absolute.
- **Capture needs microphone permission.** WASAPI capture is gated by Settings →
  Privacy → Microphone even for a virtual cable; if Start fails with access
  denied, that is why, and the error message says so.
