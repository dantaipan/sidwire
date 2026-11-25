# PLAN: MIDI → C64 Ultimate SID bridge (network register writes)

## Goal
A program on a computer reads a USB MIDI controller and plays the SID chip(s) in a
Commodore 64 Ultimate (firmware 1.2.1) over the LAN. The computer works out every
SID register value and writes it straight into the C64's I/O space ($D400–$D418)
through the Ultimate's network API. Nothing has to run on the C64; it just sits
idle at the BASIC prompt.

Known limit: writes are not cycle-exact. Every update costs a network round trip,
so expect a few ms of latency plus jitter. That's fine for playing notes and
sweeping knobs. Tight ADSR tricks may misbehave.

## Instructions for Claude Code
- Work through the phases in order. Don't start a phase until the one before it
  is verified on real hardware. Ask me to run hardware tests and report what I
  hear or see.
- **Don't trust the API details below blindly.** Check them against the device's
  own API Explorer (linked from the C64U web interface, added in 1.2.0), against
  https://1541u-documentation.readthedocs.io/en/latest/api/api_calls.html, or
  against the 1541ultimate firmware source on GitHub. Update this file with
  whatever you confirm.
- Keep a `NOTES.md` with the confirmed endpoint formats and the measured latencies.

## Stack (prototype)
- Python 3.11+
- `mido` + `python-rtmidi` for MIDI input
- `requests` (HTTP) at first; raw TCP socket in Phase 4
- Config in `config.toml`: host/IP, optional password, PAL/NTSC, SID base
  address(es), MIDI port name, CC map
- Possible later port: a C++/JUCE app or plugin (Phase 6)

## Device setup (manual, before any code)
1. C64U menu → network services: enable **Web Remote Control** (HTTP API) and
   **Ultimate DMA Service** (raw socket, used in Phase 4).
2. Use wired Ethernet if possible, and note the IP address.
3. If a network password is set, every HTTP request needs the header
   `X-Password: <password>`.
4. Note the SID configuration (UltiSID or real chips, and the addresses, e.g.
   $D400 and a second SID at $D420/$D500). It can also be read via
   `GET /v1/configs`, which needs verifying.

---

## Phase 0 — Prove the write path (no MIDI yet)
Script: `probe.py`
1. `GET /v1/version` and `GET /v1/info`, then print both. This confirms
   connectivity, the product name and the firmware version.
2. Write the border colour: `PUT /v1/machine:writemem?address=D020&data=02`.
   The border should turn red. This proves the writes reach **I/O space**, not
   just the RAM underneath it.
   - Verify the parameter format: hex without `$`? Is `data` a hex byte string?
     There is also a `POST` variant that takes a binary body for multi-byte
     writes. Confirm which one exists and what it's called.
3. Make a beep:
   - $D418 = $0F (volume max, no filter)
   - $D405 = $09 (attack 0, decay 9), $D406 = $00 (sustain 0, release 0)
   - $D400/$D401 = frequency lo/hi for A4 (formula below)
   - $D404 = $11 (triangle + gate), then $D404 = $10 after 500 ms
4. Measure the round-trip time of 200 single-byte writes. Report the average,
   p95 and maximum, and compare Wi-Fi with Ethernet if possible.

Exit criterion: an audible beep, and latency numbers recorded in NOTES.md.

## Phase 1 — SID model (pure code, unit-tested, no network)
Module: `sid.py`
- A `SidState` class holding a 25-byte shadow register array ($00–$18), with a
  dirty-range tracker.
- Register map for each voice v (offset = 7*v):
  `+0/+1` freq lo/hi, `+2/+3` pulse width lo/hi (12-bit),
  `+4` control (bit0 gate, bit1 sync, bit2 ring, bit3 test, $10 tri, $20 saw,
  $40 pulse, $80 noise), `+5` attack/decay, `+6` sustain/release.
- Filter registers: $15 cutoff lo (3 bits), $16 cutoff hi (8 bits), $17
  resonance (high nibble) plus filter routing bits, $18 mode (LP $10, BP $20,
  HP $40, voice-3 off $80) and volume (low nibble).
- Frequency: `Fn = round(f_hz * 16777216 / clock)`, where
  clock = 985248 Hz (PAL) or 1022727 Hz (NTSC). Clamp to 0–65535.
- MIDI note to Hz: `440 * 2**((note-69)/12)`. Apply pitch bend (±2 semitones
  by default) before converting.
- Unit tests: frequency table spot checks, register packing, dirty-range merging.

## Phase 2 — Transport layer
Module: `transport.py`, with an interface `write(address:int, data:bytes)`.
- `HttpTransport`: uses writemem. Prefer one call per *contiguous dirty range*
  over one call per byte. Use a persistent `requests.Session` (keep-alive).
- `DryRunTransport`: logs writes to the console, for testing without hardware.
- A **flush loop** on its own thread: every N ms (default 5, configurable) it
  sends whatever is dirty in the shadow registers. Changes made between flushes
  merge together. This stops a fast knob sweep from flooding the device.
- **Ordering rule:** within one flush, write freq/PW/ADSR *before* the control
  register, so a gate-on always plays the new pitch. If a note-off and a
  note-on hit the same voice within one flush window, send the gate-off and
  force a flush before the gate-on.

## Phase 3 — MIDI engine (`bridge.py`, the main entry point)
- List MIDI ports (`--list-ports`) and open the port from the config or `--port`.
- **Voice allocation:** 3 voices per SID. Use a free voice first, otherwise
  steal the oldest. Optionally a mono/legato mode that only uses voice 1.
- Note on: set the frequency, then set the gate bit. Note off: clear the gate.
- Velocity: ignore it at first. Optionally map it to the master volume or the
  sustain level later.
- Default CC map, editable in config.toml:
  - CC1 (mod wheel): pulse width
  - CC74: filter cutoff (11-bit, scaled)
  - CC71: resonance
  - CC72/73/75/76: release / attack / decay / sustain
  - CC7: master volume
  - Program change: waveform presets (tri / saw / pulse / noise / combos)
- Pitch bend: retune every sounding voice.
- Panic: on Ctrl-C or CC123 (all notes off), clear every gate bit and write
  $D418 = 0.
- Hard-restart option: before re-gating a voice, write gate-off and zero ADSR,
  then wait one flush. This works around the SID's ADSR delay bug. Off by
  default, toggled in the config.

Exit criterion: I can play 3-note chords from my controller and sweep the
filter live with acceptable feel.

## Phase 4 — Faster transport: raw DMA socket (optional, if HTTP feels laggy)  *(done, hardware-tested 2026-11-24: ~2 ms faster notes, 2.7× sweep throughput — see NOTES.md)*
- The Ultimate DMA Service listens on a raw TCP port (believed to be 64). It
  takes binary commands: a 16-bit little-endian command word, a 16-bit length,
  then the payload. One command writes bytes to a C64 address.
- **Verify the exact command codes and framing in the 1541ultimate firmware
  source** (the socket DMA handler) before implementing. Don't guess.
- Implement `DmaSocketTransport` with one persistent TCP connection and
  `TCP_NODELAY`.
- Re-run the Phase 0 latency benchmark and compare it with HTTP in NOTES.md.

## Phase 5 — Second SID + polish  *(done, verified on hardware 2026-11-23)*
- If a second SID is configured, support 6 voices, or split by MIDI channel
  (channel 1 → SID 1, channel 2 → SID 2).
- Small preset file (JSON/TOML) for patches: waveform, ADSR, PW, filter.
- Reconnect logic if the C64U reboots or the network drops.
- Optional: a simple terminal UI (e.g. `rich`) showing the voices, gates and CC
  values.

## Phase 6 — (Later) JUCE port  *(built 2026-11-23: cpp/ — see NOTES.md; needs your DAW test)*
- Port the SID model and transport to C++. Build a JUCE standalone app and/or a
  VST3/AU instrument plugin that takes MIDI from the DAW and outputs to the
  C64U, with parameters exposed for automation.
- Reuse the Python unit-test vectors to confirm both versions behave the same.

## Phase 7 — DMA by default  *(done 2026-11-24)*
- The DMA service is the default transport and falls back to HTTP when it's off (Python + plugin).
- An optional SIDlink C64 player was built and tested the same day, then **removed at the user's
  request** ("not what I had in mind"). The full project with it is archived in
  `attic/with-sidlink-2026-11-24.tar.gz`; results are in NOTES.md.

## Phase 8 — Hear the C64 through the plugin  *(built 2026-11-24)*
- "C64 audio" option: the plugin starts the C64U audio stream towards itself (UDP, port 11003 or
  any free one), resamples it to the host rate with drift control and plays it on its output.
  Stops the stream when switched off / disconnected.

## Enhancements done 2026-11-24
- Velocity → each note's sustain level (default), master volume, or off, with an amount setting
  (Python + plugin parameters). Hardware-checked: see NOTES.md.
- Pad 1 on MIDI channel 10 steps the filter mode LP → BP → HP → off (the user sets the Oxygen's
  pads to channel 10). Hardware-checked.

## Controller profiles — done 2026-11-24
- The user asked for the app to be easy to set up for other controllers, or a different keyboard
  of their own. Everything controller-specific moved out of the code into profiles
  (`profiles/*.toml`): ports, keys channel, bend range, ignored channels, and a list of controls
  (CC / note / pitch bend → param or action, absolute or 3 relative knob types).
- Shipped profiles: `generic` (the default) and `oxygen-49-mkv`. The Python bridge picks one in
  config.toml or with `--profile`; the plugin has a Profile box, a user profiles folder, and a
  presets file of your own.
- Next, with the UI redesign: MIDI learn (click a control, move a knob, saved into your profile).

## Plugin UI redesign — done 2026-11-24
- The user picked option A from mockups (a design canvas): **Play** and **Setup** tabs. Setup
  shows three numbered steps with a "Ready to play / Start playing" bar. The window opens on
  Setup until it has connected once, then on Play.
- **Find**: searches the local network for C64 Ultimates like PS64 does (also `bridge.py
  --find`). It runs by itself on first run and uses the result when exactly one is found.
- **MIDI learn** (right-click) saves into the user's copy of the profile.
- **Silence** (was Panic) and **Stream C64U audio** (was "C64 audio"), renamed by the user.

## Voice modes and arpeggiator — done 2026-11-24
- Like LukHash's SID SYNTH (found in the search for similar apps): poly, mono, unison
  (detuned), octave / fifth / chord stacks, and a tempo-locked arpeggiator (rates, patterns,
  octaves, gate; MIDI clock, else host tempo or BPM). Python + C++ (parity vectors), plugin
  parameters + Voice mode bar on Play, a "voice_mode_next" profile action.

## Renamed to SIDwire — 2026-11-24
- "C64 Bridge" (a remote-control tool for the C64U) was too close a name. The plugin and app are
  now **SIDwire** (repo `dantaipan/sidwire`), from release 0.2.0. Plugin codes and bundle id are
  unchanged, so DAW projects still open; settings and profiles are copied over on first run.

## Future enhancements (noted 2026-11-23)
- Stereo balance in poly mode (notes alternate L/R in threes because the sockets are panned
  L3/R3). The user is deciding between centring the mixer, a fill-SID-1-first allocator, or split.
- Save the current knob state as a new preset in presets.toml.
- Mod wheel (CC1) → pulse width is mapped but untested: nothing was seen in captures.
- DMA: merge both SIDs' writes into one command per flush (cost is per command; halves sweep cost in 6-voice mode).
- A program on the C64 side: removed for now (see Phase 7). Ask the user what they have in
  mind before building anything there again.
- Plugin "C64 audio": report a latency to the host (measured round trip) so recorded tracks
  line up without manual nudging.

---

## Risks / open questions to resolve
- [x] Confirm that writemem reaches I/O registers ($D020 test) and doesn't just
      write RAM. → Confirmed (readback F2, border red).
- [x] Exact writemem parameter formats (PUT query vs POST binary body).
      → Both work; POST costs ~186 ms/call, so use PUT. Server sends
      `Connection: close` (no keep-alive). See NOTES.md.
- [ ] Does the network write pause or stall the C64 CPU (DMA)? Does that cause
      audible glitches?
- [ ] Real-world latency and jitter over Ethernet vs Wi-Fi.
- [ ] Behaviour of UltiSID vs real 6581/8580 chips (filter curves differ).
- [ ] Keep the C64 idle so no running program writes to the SID at the same
      time.

## Definition of done (v1)  *(met 2026-11-23)*
`python bridge.py --port "<my controller>"` plays the C64U's SID polyphonically
from a USB MIDI controller on another computer, with live filter/PW/ADSR control,
a working panic, and documented latency.
