# MIDI → C64 Ultimate SID bridge

Play the real SID chips in a Commodore 64 Ultimate from a USB MIDI keyboard on another
computer. The computer works out every SID register value and writes it into the C64's
I/O space over the network, through the Ultimate DMA Service (or the REST API if that's
switched off). Nothing runs on the C64, which just sits at the BASIC prompt.

There are two front ends, and both use the same engine:

- **`bridge.py`**: a Python command-line bridge. It's the prototype, and it's the reference
  that the C++ version is checked against.
- **SID Bridge**: a JUCE instrument plugin (VST3 and AU) plus a standalone app, built from
  `cpp/`.

![SID Bridge plugin](docs/sid-bridge-plugin.png)

## What it does

- Polyphony: 3 voices per SID, so 6 voices with two SIDs. A new note takes a free voice,
  and if none is free it steals the oldest note.
- Split layout: MIDI channel 1 plays SID 1 and channel 2 plays SID 2, each with its own patch.
- Any MIDI keyboard: a **controller profile** says what its knobs, faders, pads and buttons do
  (see [Your controller](#your-controller-profiles)). Profiles for a generic keyboard and the
  M-Audio Oxygen 49 MKV are included.
- Live control of cutoff, resonance, pulse width, ADSR and volume.
- Pitch bend, a sustain pedal, presets on program change (`presets.toml`), and a panic.
- Velocity: by default each note's sustain level follows how hard you play it. It can also
  drive the master volume instead, or be switched off (`[engine] velocity`, or the plugin's
  Velocity and Vel Amt controls).
- Pads and buttons can trigger actions: next filter mode, next/previous preset, next waveform,
  panic. With the Oxygen profile, pad 1 steps the filter mode LP → BP → HP → off (set the
  Oxygen's pads to channel 10 first: tap a pad, hold SHIFT and press CHANNEL UP/DOWN).
- Reconnects automatically if the network drops, then rewrites every register.
- Only one bridge can drive a C64U at a time. The script and the plugin share a lock file.

The bridge uses the **Ultimate DMA Service** (C64U menu → Network Settings) by default. A
write lands about 1 ms after it's sent. If the service is off, the bridge falls back to the
REST API, where a write takes about 7 ms (set by the C64U's HTTP server, not the network).
End to end, notes play about 2 ms sooner over DMA. See `NOTES.md`. End to end, from MIDI out to SID sound, the Python bridge and the
plugin measure the same within 1 ms.

## Getting the code

```sh
git clone --recursive https://github.com/dantaipan/sid-bridge.git
# already cloned without --recursive?  git submodule update --init   (JUCE, for the plugin)
```

## Python bridge

```sh
python3.12 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/python bridge.py --list-ports
.venv/bin/python bridge.py --ui            # play; Ctrl-C = panic + quit
.venv/bin/python bridge.py --split --preset "Saw Bass"
.venv/bin/python bridge.py --find          # look for C64 Ultimates on your network
.venv/bin/python bridge.py --sids          # what the C64U has mapped
.venv/bin/python bridge.py --http          # REST API instead of the DMA service
.venv/bin/python bridge.py --list-profiles # controller profiles; --profile NAME|PATH to pick one
.venv/bin/python -m pytest                 # 139 tests
```

Settings live in `config.toml` (C64U address, SID addresses, the controller profile, sound
and velocity options). Put your own changes in `config.local.toml` (same format, read on top,
kept out of git), for example `[device] host = "192.168.1.64"`. With no address set,
`bridge.py` searches the network and uses the C64U if it finds exactly one.

## Plugin and standalone app

```sh
cd cpp
cmake --preset release && cmake --build --preset release   # also installs the AU/VST3
ctest --preset release                                     # core + parity + plugin tests
```

The build produces `cpp/build/release/plugin/SidBridge_artefacts/Release/{VST3,AU,Standalone}`.
A Release build copies the AU and VST3 into `~/Library/Audio/Plug-Ins`.

To use it:

1. Load **SID Bridge** as an instrument (or open the standalone app). The first time, it opens
   on **Setup**:
   - **Your C64 Ultimate**: it searches your network by itself (**Find** does it again) and
     connects when it finds one. You can also type the address. "More settings" has the
     network password, SID addresses, DMA or HTTP, and PAL or NTSC.
   - **Your keyboard**: pick your controller's **Profile**. **Notes in** can open a keyboard
     directly (lowest latency in the standalone app; host MIDI is then ignored). **Controls in**
     is a second input for knobs and buttons; "Auto" uses the one named in the profile.
   - **How you play** (optional): poly or split layout, poly or mono voices, which SID the
     knobs change, and your own presets file. In the standalone app also the **Audio output**
     (only used by Stream C64U audio).
   In the standalone app, an empty **Notes in** ("Auto") opens the keys port named in the
   profile; there is no separate Options / Audio-MIDI dialog.
   When it's connected, **Start playing** takes you to the **Play** tab. From then on it opens
   on Play.
2. **Play** has the preset bar and the sound in four sections: oscillator (light two waveforms
   to combine them), filter (LP / BP / HP, none = off), envelope (with the real times), and
   output (volume and velocity). The knobs are ordinary automatable parameters: MIDI CCs, your
   controller and presets move them too, so the host can record them.
3. **MIDI learn**: right-click a knob, a filter or waveform button, a preset arrow, or Silence,
   choose "MIDI learn...", and move a control on your keyboard. It's saved to your own copy of
   the profile ("... (mine)") and works straight away. The small label under each knob shows
   what controls it; "Forget" in the same menu removes it.
4. **Silence** stops every note on the C64 at once (the volume comes back with the next note):
   for a note stuck because its note-off got lost.
5. **Stream C64U audio** plays the C64U's network audio stream out of SID Bridge, so you hear
   the real SIDs through your audio interface and a DAW can record them on the plugin's track.
   It adds roughly 30–40 ms on top of the C64U's own output, so for playing live the C64U's
   audio out is snappier. The C64U streams to one place at a time: this takes the stream over
   from PS64.

Without **Stream C64U audio** the plugin's own output is silent: the sound comes out of the C64U.

**About SID Bridge** (version and licensing): click the "SID Bridge" title or the "Made with
JUCE" footnote on Setup; in the standalone app also SID Bridge menu → About SID Bridge.

**Updates:** the standalone app asks GitHub once a day whether a newer release is out (it can
be switched off in About) and has SID Bridge → Check for Updates…; the plugin checks only when
you press "Check for updates" in About. Nothing is sent beyond a request for the latest
release of [dantaipan/sid-bridge](https://github.com/dantaipan/sid-bridge/releases).

## Licence

GNU AGPL 3.0 or later (see LICENSE). The plugin and app are built with
[JUCE](https://juce.com), whose modules are dual-licensed under the
[GNU AGPLv3](https://www.gnu.org/licenses/agpl-3.0.en.html) and the commercial
[JUCE licence](https://juce.com/legal/juce-9-licence/); SID Bridge uses JUCE under the AGPL.

## Your controller: profiles

A controller profile is a small TOML file that describes one MIDI keyboard or controller:
its MIDI ports, which channel the keys use, the pitch-bend range, and a `[[control]]` entry for
each knob, fader, wheel, pedal, pad or button. `profiles/generic.toml` explains every option at
the top; copy it to start your own. Both the Python bridge and the plugin read the same files.

```toml
name = "My keyboard"

[keys]
channel = 0                 # 0 = any channel
ignore_channels = [10]      # e.g. pads on channel 10 never play notes

[[control]]                 # an ordinary knob sending CC 74
cc = 74
param = "cutoff"

[[control]]                 # an endless knob on a second "DAW" port
port = "controls"
cc = 16
mode = "relative-signed"
param = "resonance"

[[control]]                 # a pad
note = 36
channel = 10
action = "preset_next"
```

- To see what your controller sends: `.venv/bin/python bridge.py --monitor`.
- **Python bridge:** put the file in `profiles/` and set `[controller] profile = "<name>"` in
  `config.toml`, or pass `--profile path/to/file.toml`.
- **Plugin / app:** Profile → "Open profiles folder..." opens
  `~/Library/Application Support/SID Bridge/Profiles` (with the built-ins as examples). Files
  there show up in the Profile box. A file with a built-in's name replaces that built-in.
- A mistake in a profile names the file and the line. The plugin then falls back to the generic
  profile and says why in its status line.

## Layout

| Path | What |
|---|---|
| `sid.py`, `transport.py`, `engine.py`, `bridge.py` | Python: SID model, HTTP transport and flusher, engine, CLI |
| `presets.toml`, `config.toml` | Patches; bridge settings |
| `profiles/`, `profiles.py` | Controller profiles (built into the plugin too) and their reader; `tests/profiles/test-all.toml` exercises every option |
| `cpp/core` | C++ port of the engine (no JUCE), with Catch2 tests |
| `cpp/core/tests/vectors` | Golden vectors from `tools/gen_vectors.py`. The C++ engine must reproduce the Python engine's register writes byte for byte |
| `cpp/plugin` | JUCE plugin: `BridgeController` (threads, params, lock), processor, editor, headless tests |
| `listen.py` | Records and analyses the C64U's audio stream, for checking sound without ears |
| `tools/e2e_latency.py` | End-to-end test: virtual MIDI → bridge or plugin → C64U → recorded audio |
| `probe.py`, `bench.py` | Phase 0 and 2 hardware probes and benchmarks |
| `NOTES.md` | Confirmed API details, measurements, gotchas |
| `PLAN.md` | The phased plan, its status, and future enhancements |

After changing the Python engine, regenerate the vectors with
`.venv/bin/python tools/gen_vectors.py`. `pytest` fails if you forget.
