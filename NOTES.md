# NOTES — confirmed API details & measurements

## Sources checked
- https://1541u-documentation.readthedocs.io/en/latest/api/api_calls.html (read 2026-11-23)
- Device API Explorer: _not yet checked_
- Firmware source: `software/network/socket_dma.cc` (master) checked for the Phase 4 DMA socket framing

## Device (probed 2026-11-23)
- `product: "C64 Ultimate"`, firmware 1.2.1 (git 31687dbd), FPGA 126, core 1.50
- Hostname C64-Ultimate-XXXXXX, IP <C64U address>, no network password
- SID setup (`GET /v1/configs/SID%20Addressing` etc.):
  - Socket 1: real **6581** at **$D400**; Socket 2: real **6581** at **$D420**
  - UltiSID 1 **and** UltiSID 2 both at **$D400** (filter curve "8580 Lo", 6581 combined waveforms)
  - ⇒ a write to $D400 currently drives socket 1 + both UltiSIDs at once
  - Mixer: all SID sources 0 dB; sockets panned L3/R3, UltiSIDs centre
  - Auto Address Mirroring: Enabled; range splits off
  - 2026-11-23: set UltiSID 1/2 Address → `Unmapped` via
    `PUT /v1/configs/SID%20Addressing/UltiSID%201%20Address?value=Unmapped` (saved to flash
    2026-11-23). Now only the real 6581 in socket 1 answers at $D400.
  - Config item detail (`GET /v1/configs/<cat>/<item>`) returns `current`, `values` (list of
    allowed strings), `default`. Values are strings, e.g. `"Unmapped"`, `"$D420"`, `" 0 dB"`.

## Phase 0 result — PASSED 2026-11-23
Border red ✅, background green (POST) ✅, A4 beep audible ✅.

## REST API (per docs; ✅ = confirmed on hardware)
| Call | Format | HW |
|---|---|---|
| Auth | Header `X-Password: <pw>` on every route; 403 if wrong. Ignored if no password set. | n/a (no pw) |
| Version | `GET /v1/version` → `{"version": "0.1", "errors": []}` | ✅ |
| Info | `GET /v1/info` → product, firmware_version, git_commit_hash, fpga_version, core_version, hostname, unique_id, ethernet_mac, wifi_mac, errors | ✅ |
| Write (query) | `PUT /v1/machine:writemem?address=D020&data=0504` — uppercase hex, no prefix; data = hex byte string to consecutive addresses; max 128 bytes (per docs) | ✅ |
| Write (binary) | `POST /v1/machine:writemem?address=D021` + raw body (`application/octet-stream`) works. **~186 ms per call** — unusable for real-time. | ✅ |
| Read | `GET /v1/machine:readmem?address=D020&length=2` → raw binary body | ✅ |
| Configs | `GET /v1/configs` → category list; `GET /v1/configs/<cat>` → `{cat: {item: value-string}}` | ✅ |
| Pause/resume | `PUT /v1/machine:pause` / `:resume` — not needed for writemem | — |

**I/O space confirmed:** after `PUT writemem D020=02`, readmem $D020 returned `F2`
(unused upper nibble reads 1s), border turned red. Writes from the BASIC prompt hit the VIC/SID.

**HTTP server behaviour:** every response carries `Connection: close` → **no keep-alive**.
Each request needs a fresh TCP connection (~10 ms connect + ~10 ms request on Wi-Fi).
`requests.Session` therefore gives no reuse benefit. One 1019 ms outlier seen in 600
requests — consistent with a 1 s TCP SYN retransmit on a new connection.

## Latency (ms) — 2026-11-23, Mac on Wi-Fi
ICMP ping: avg 4.6, max 6.1.

| Test | Method | n | mean | median | p95 | max |
|---|---|---|---|---|---|---|
| PUT 1 byte | requests (connect + request) | 200 | 18.1 | 18.0 | 19.2 | 22.0 |
| PUT 25 bytes | requests | 200 | 23.2 | 18.1 | 19.8 | 1019 |
| POST 25 bytes | requests | 200 | 192.7 | 194.9 | 197.6 | 199.0 |
| PUT 1 byte | raw socket, request only (pre-connected) | 200 | 10.2 | 10.0 | 11.5 | 13.1 |
| POST 25 bytes | raw socket, single packet, pre-connected | 200 | 186.3 | 185.9 | 190.1 | 244.6 |
| PUT 1 byte | raw socket, connect + request | 50 | 19.4 | 19.8 | 20.1 | 20.5 |

Takeaways:
- Payload size barely matters (1 vs 25 bytes) → always write the whole dirty range in one PUT.
- POST slowness is server-side (one-packet send didn't help) → use PUT only.
- Transport idea: keep one **pre-opened** socket ready, so the connect cost overlaps
  idle time → ~10 ms per flush instead of ~19 ms. Add connect timeout + retry for SYN loss.
- Ethernet numbers: see Phase 2 section (C64U wired, 2026-11-23).

## Phase 2 — transport measurements (2026-11-23, Mac on Wi-Fi, `bench.py`)
Raw-socket `HttpTransport`, 200 PUT writes to $D41B (read-only OSC3 → harmless).

| Mode | mean | median | p95 | max |
|---|---|---|---|---|
| no preconnect, back-to-back | 13.96 | 14.13 | 15.06 | 15.53 |
| no preconnect, 20 ms gaps | 10.26 | 10.14 | 10.81 | 24.0 |
| **preconnect**, back-to-back | 13.98 | 14.21 | 15.07 | 15.58 |
| **preconnect, 20 ms gaps** | **6.96** | **6.96** | **7.63** | **8.04** |

- Back-to-back writes cost ~14 ms each regardless → device-side cycle limit, **≈70 writes/s max**.
- With idle time between writes, the pre-opened socket saves ~3 ms → **~7 ms** per write.
  That's the typical case for a note-on after a pause.
- Idle pre-opened sockets survive ≥ 8 s (tested 0.5–8 s, all hits). `max_idle` default = 5 s.
- Filter sweep via Flusher: 3148 cutoff changes in 4 s coalesced into 282 writes (70/s).
- Write ordering: one writemem is *assumed* (not verified) to be applied in ascending address order — a
  DMA burst of ≤25 bytes takes only µs anyway. So freq/PW precede
  each voice's control byte. Only when control **and** ADSR of a voice change together is the
  batch split (ADSR first, then control) — costs one extra round trip in that rare case.
- Retrigger (gate off+on inside one window) is preserved by committing the gate-off as its
  own batch — no blocking in the MIDI thread.

### Wired LAN (C64U on Ethernet, Mac on en7), 2026-11-23
ICMP ping avg 1.8 ms (vs 4.6 ms Wi-Fi), but HTTP write latency is **unchanged**:

| Mode | mean | median | p95 | max |
|---|---|---|---|---|
| no preconnect, back-to-back | 13.96 | 14.22 | 15.01 | 15.29 |
| no preconnect, 20 ms gaps | 10.14 | 10.07 | 10.88 | 11.37 |
| preconnect, back-to-back | 13.97 | 14.26 | 15.01 | 15.84 |
| **preconnect, 20 ms gaps** | **7.00** | **6.98** | **7.58** | **8.60** |

⇒ latency is dominated by the C64U's HTTP handling (~7 ms/request, ~14 ms cycle), not the
network. Phase 4 (raw DMA socket) is the only way to go lower. User reports latency is
not a problem in practice (Phase 3 play test).

## Audio capture for self-verification (`listen.py`)
- C64U audio stream: `PUT /v1/streams/audio:start?ip=<me>:<port>` / `PUT /v1/streams/audio:stop`.
  UDP packets = u16 LE sequence + 192 stereo frames s16 LE, ~47983 Hz (PAL). Format taken from
  the PS64 project (`src/audiostream.h`), confirmed working here.
- `listen.py` uses port 11002 (PS64 listens on 11001). Only one audio destination exists on the
  C64U, so a capture redirects the stream away from PS64 while it runs.
- Verified 2026-11-23: A4 beep measured 440.1 Hz (+0 cents), 0 packets lost. A ~6581 click is
  visible at the start (volume write). Knob (K1 encoder) filter sweep on a C3 chord: centroid
  3600 → 315 → 3760 Hz, smooth; level steady at −22 dB. Captures in `captures/`.

## Oxygen 49 MKV (DAW / Mackie mode) — observed 2026-11-23
- Keys, pitch wheel: `Oxygen 49 MKV USB MIDI` port.
- 8 knobs: `Oxygen 49 MKV Mackie/HUI` port, ch1 CC16–23, relative: 1 = +1 (cw), 65 = −1 (ccw).
- Fader(s): pitchwheel on the Mackie port (fader 1 = channel 1).
- Mod wheel: nothing seen yet in a 45 s all-port capture (untested?).

## Phase 5 — second SID, presets, reconnect, UI (2026-11-23)
- `bridge.py --sids` reads `SID Addressing` + `SID Sockets Configuration`: `$D400: socket 1 (6581)`,
  `$D420: socket 2 (6581)`. Startup warns if a configured base has nothing mapped.
  `[sid].bases = "auto"` uses whatever is mapped.
- Layouts (`[engine].layout`): `poly` = one part, 6 voices over both SIDs; `split` = MIDI ch1 → SID 1,
  ch2 → SID 2 (own patch, bend, pedal, presets per part). `[mackie].target` = "all" or a part number.
- Audio-verified: split → low chord only in L (socket 1, pan Left 3), high chord in R (socket 2);
  6-voice poly → notes 1–3 on SID 1, 4–6 on SID 2. 0 packets lost.
  Level check (C4 triangle on each SID alone): pans are symmetric (~10 dB to the far side);
  socket 2 was 2.7 dB quieter. Set `Audio Mixer / Vol Socket 2` → `+3 dB` via
  `PUT /v1/configs/Audio%20Mixer/Vol%20Socket%202?value=%2B3%20dB` → now within 0.2 dB.
  Saved with `PUT /v1/configs:save_to_flash` (2026-11-23) → response `"written": ["Audio Mixer",
  "Speaker Mixer", "SID Addressing"]`, so the UltiSID unmapping is persistent too.
- `presets.toml`: 8 patches, program change N → preset N % 8. All 8 measured audible at C3 (130.8 Hz).
  Gotcha found: voices routed into the filter with **no filter mode** are silent on the SID →
  `filter_mode = "off"` now also un-routes the voices.
- Reconnect: a failed flush marks all registers dirty, flusher retries with backoff 0.25 → 2 s,
  logs once on loss and once on recovery, then resyncs all 25 registers per SID. Unit-tested with
  a flaky transport, and verified on hardware: Ethernet cable pulled and replugged while playing → sound
  resumed normally.
- `--ui`: rich live view (connection, writes/s, median latency, per-part patch + voices).
- Gotcha: two bridge processes running at once both receive the Oxygen's notes and fight over
  the SID voices (heard as "only one chord plays"). The bridge now holds `.bridge.lock` (flock) and a
  second instance refuses to start.
- **Phase 5 verified on hardware 2026-11-23:** two 3-note chords (6 voices over both 6581s) play
  correctly; reconnect after a cable pull works.

## v1 definition of done — MET (2026-11-23)
`bridge.py` plays both 6581s polyphonically (6 voices) from the Oxygen 49 MKV over the LAN, with
live filter/PW/ADSR control (Mackie encoders + CC map), presets, a working panic, reconnect, and
documented latency (~7 ms per write, device-bound).
Open items at the time: mod wheel (CC1) never seen in capture; Phase 4 (DMA socket) skipped; Phase 6 (JUCE) not started.
(Phase 4 and 6 have since been done — see below.)

## Phase 6 — C++ core + JUCE plugin (2026-11-23, overnight)
- `cpp/core` (`msid::`): port of sid.py / transport.py (Flusher, HttpTransport with pre-connect) /
  engine.py, plus a TOML-subset presets reader. `pyRound()` = `std::nearbyint` (ties-to-even, like
  Python's `round`) — needed for byte-exact parity.
- **Parity:** `tools/gen_vectors.py` records 22 hand-written + 40 randomized scenarios (4,747 register
  writes) from the Python engine; `VectorTests.cpp` replays them and requires identical writes per
  flush. Passed first time; a deliberately flipped byte is caught. `tests/test_vectors_current.py`
  fails if the Python engine changes without regenerating.
- JUCE 9.0.4 (local clone of roughstomp's pinned commit 3fb5cbc) in `cpp/external/JUCE`.
- Plugin "SID Bridge" (VST3/AU/Standalone, instrument, silent output). `BridgeController` thread owns
  engine/flusher/transport; audio thread only pushes MIDI into a lock-free FIFO. Optional direct
  MIDI input ports (notes / Mackie encoders) bypass the host. `host:port` accepted.
- Validation: `auval -v aumu Sidb Msid` PASS; pluginval strictness 10 (VST3 + AU, incl. editor) SUCCESS.
  ctest: 22 core + 9 plugin integration test cases (fake C64U on localhost) pass.
- **Bug found by hardware test:** parameter write-back (engine → host param, via message thread)
  lagged, and the stale parameter value was read as a host change, dragging the engine back during a
  fast encoder sweep (centroid only 4088 → 3337 Hz vs Python's 4097 → 1689). Fixed by tracking the
  parameter's last *seen* value and ignoring our own echoes; now 3993 → 1813 Hz. Covered by the
  "fast CC sweep" plugin test.
- **End-to-end latency** (`tools/e2e_latency.py`: virtual MIDI note-on → onset in the C64U audio
  stream, 30 notes; includes the audio stream's own buffering, identical for both):
  bridge.py median 42.1 ms, plugin standalone median 43.1 ms (stdev 0.7 ms). The plugin's 1 ms
  bridge-thread poll accounts for the difference. How much of the ~42 ms is the stream's buffering
  (vs. what you hear from the C64 directly) is not known — the measurement compares targets, it
  doesn't give the acoustic latency.
- Shared instance lock: `/tmp/midi-sid-bridge-<host>.lock` (flock), used by bridge.py and the plugin.

## Phase 4 — DMA socket transport (hardware-tested 2026-11-24)
- Ultimate DMA Service framing confirmed from firmware `software/network/socket_dma.cc` (master):
  TCP **64**, u16 LE command (high byte 0xFF), u16 LE length, payload. `DMAWRITE = 0xFF06`,
  payload = u16 LE address + data, **no reply**. `IDENTIFY 0xFF0E` replies (1 length byte +
  string), `AUTHENTICATE 0xFF1F` (1 byte reply) required first if a network password is set.
  Other codes: DMA 0xFF01, DMARUN 0xFF02, KEYB 0xFF03, RESET 0xFF04, …
- The user enabled **Ultimate DMA Service** on 2026-11-24. IDENTIFY → `*** C64 Ultimate (V1.50) 1.2.1 ***`.
  Writes to $D020 land (checked by readback).
- **Measured on the C64U (wired LAN)** — timed as write(s) + IDENTIFY, since the service answers in order:
  | What | Time |
  |---|---|
  | IDENTIFY alone (round trip) | 1.7–2.1 ms |
  | 1 write + IDENTIFY, after a pause | 2.4 ms (→ a write lands ≈ 0.8 ms after sending) |
  | 1-byte vs 1000-byte write | same (cost is per command, not per byte) |
  | burst of N writes after a pause | ≈ 0.8–1.0 ms each up to N≈16; 24 → 43 ms, 32 → 85 ms |
  | sustained writes | fine to ~143/s (lag 1.7 ms); 200/s → 64 ms lag after 2 s; 333/s → 1.6 s |
  | back-to-back writes, no pause | ≈ 5.3 ms each (the service's slow mode) |
- **Gotcha 1 — idle close:** the service closes a connection after **exactly 1.0 s** without a
  command (measured 997–999 ms). A DMAWRITE sent into the closed socket "succeeds" locally and is
  **silently lost**: the first test run dropped the first note after every pause. Fix: `idle()`
  keepalive (an IDENTIFY every 0.4 s while idle), plus reopen-before-write once a connection has
  been quiet for 0.9 s or the server's EOF is seen.
- **Gotcha 2 — no backpressure:** writes are fire-and-forget, so a fast knob sweep could queue
  more than the service handles (see the sustained rates) and the lag would grow without bound.
  Fix: an IDENTIFY after every flush acts as an acknowledgement, and the next flush waits until the
  previous one is acknowledged (`max_in_flight = 1`). Edits made meanwhile coalesce. The ack is
  also the only way to notice a dead link (sends into a broken TCP connection keep succeeding), so
  a missing reply for 1 s raises an error → the Flusher resyncs all registers. A transport never
  silently retries after a failure if unacknowledged writes could have been lost.
- Both fixes in Python and C++ (Transport hooks `begin_flush` / `end_flush` / `idle`, no-ops for
  HTTP), with fake-service tests for idle close, keepalive, backpressure and a frozen link.
- **End-to-end** (`tools/e2e_latency.py`, 40 notes, median MIDI → onset in the audio stream):
  | | HTTP | DMA |
  |---|---|---|
  | bridge.py | 42.1 ms | **40.2 ms** |
  | plugin standalone | 43.1 ms | **41.1 ms** |
  40/40 notes found in every run; chord, encoder sweep and preset checks pass on both.
  (The occasional ~7–9 ms "early" outlier appears with HTTP too — a measurement artefact.)
- **Worst-case knob sweep** (both SIDs' cutoff, one edit per ms for 3 s): HTTP 36 flushes/s, last
  value on the chip 58–79 ms after the last edit; DMA 96 flushes/s, 24–27 ms. Backlog bounded, no errors.
- SID registers are write-only: `readmem` of $D415 returns 0 after a write via either transport,
  so SID writes can't be verified by readback (use audio, or RAM/VIC registers).
- Network-timed arpeggio (Mac busy-waits, writes the frequency each step), DMA vs HTTP: no better —
  step stdev 5–9 ms and holes of 70–120 ms with both. The jitter comes from the C64U's network
  handling, not the transport, so frame-exact effects still need SIDlink (player: max 25 ms at
  20 ms steps, no holes, same session).
- To use: `bridge.py --dma`, `[device] transport = "dma"`, or plugin "Via: DMA service". The
  config default is still HTTP.

## Companion player on the C64 (SIDlink) — prototype 2026-11-23 (REMOVED 2026-11-24, archived in attic/)
- See `companion/DESIGN.md`. 938-byte raster-IRQ player at $C000, voice images at $CE00/$CE80,
  installed via writemem + `SYS49152` typed into the keyboard buffer ($0277/$C6) — no settings needed.
- Hardware: installed, ran (51 frames/s), uninstalled cleanly (screen `READY.`, IRQ vector $EA31).
- C64-timed vs Mac-timed (network) arpeggio, 1 ms analysis: at 60 ms steps both ~0.45 ms spread;
  at **20 ms steps C64 0.31 ms vs network 1.76 ms spread (p1–p99 1 vs 7.8 ms), network dropped
  3 steps/3 s with a 70 ms hole**.
- UCI network target ($03) researched: client-only TCP/UDP (no listen), needs Command Interface;
  not pursued.

## Phase 7 — SIDlink integrated (optional) + DMA default — 2026-11-24 (SIDlink part REMOVED the same day)
- User decisions 2026-11-24: make DMA the default; integrate SIDlink if it can be optional.
- **DMA default:** `[device] transport = "dma"` (DEFAULTS too). `bridge.py` probes with IDENTIFY
  at start and falls back to HTTP with a warning; `--http` forces HTTP. Plugin: "Via" defaults to
  DMA, same fallback, status shows "via HTTP (DMA service not reachable)".
- **Design:** `CompanionTransport` (sidlink.py / Sidlink.cpp) sits under the Flusher: register
  writes → voice images (bytes 0-6 = SID voice registers, globals at +60), each 0→1 gate edge
  bumps the image's trigger counter, `SidState.ext` (10 bytes/voice) fills bytes 8-17. One image
  write per changed SID per flush (DMA: both images in one 193-byte command). Any send error
  invalidates the "sent" copy, so whole images are resent. Direct mode ignores `ext`, so its
  writes and the old vectors are unchanged.
- Engine (companion only): `arp_speed` > 0 → all held notes of a part on one voice (lowest = base,
  up to 3 more as the arp, no retrigger when the chord changes, pitch bend retunes the arp);
  `pwm_speed` → player sweep; `hard_restart` → player flag instead of the network sequence.
  `select_preset` resets both keys unless the preset sets them.
- Lifecycle: install refuses unless the IRQ vector is $EA31 (idle at BASIC). Watchdog: HTTP
  ident check every 2 s on its own thread; if gone → passthrough + full resync. SIGTERM/SIGHUP
  now shut bridge.py down like Ctrl-C (before, a kill skipped panic and left SIDlink installed).
  Plugin: env overrides (MSID_*) are no longer saved into the standalone's settings.
- **Hardware (one 6581, DMA):**
  - Chord Arp, hold C-E-G: 127 steps cycling C E G, 20.10 ms/step (frame 19.95), min 17.5 max 22.5
    at 2.5 ms resolution — no holes.
  - PWM Lead: 2nd/1st harmonic ratio sweeps 0.03-0.97 on a held note.
  - e2e via SIDlink: 40/40 (Python) and 30/30 (plugin) notes, chord/sweep/preset checks pass.
    Median 50.9 ms (Python) / 49.9 ms (plugin) vs 40.2 / 41.1 direct: notes wait for the next
    frame (+10 ms average, as DESIGN.md predicted).
  - Fallback: player stopped behind the bridge's back → detected in ≤2 s, notes keep sounding
    (same level) through direct writes.
  - Exit: SIGTERM (Python) and Cmd-Q (plugin) remove the player; IRQ back to $EA31. A plain
    start removes a leftover player.
- Tests: Python 96 (12 new SIDlink + fallback); C++ 47 (Sidlink unit tests, installer against a
  fake C64U that emulates the player, plugin companion + DMA-fallback tests); vectors 28 + 60
  scenarios (26 companion) byte-identical; auval + pluginval strictness 10 pass.

### SIDlink removed — 2026-11-24
The user tried `--companion`, said it was not what they had in mind and asked to remove it for
now. Everything SIDlink was taken out (Python, C++ core, plugin, presets "Chord Arp"/"PWM Lead",
`companion/`, tests, vectors back to 22 + 40 scenarios / 4,747 writes). Kept: DMA default with
HTTP fallback, SIGTERM/SIGHUP graceful shutdown, MSID_* env overrides not saved by the standalone,
the fake C64U serving connections concurrently. The whole project as it was with SIDlink:
`attic/with-sidlink-2026-11-24.tar.gz`. After removal: Python 82 tests, C++ 40, auval + pluginval pass.
The player was uninstalled from the C64 (IRQ vector back at $EA31).

### DMA service serves one connection at a time — 2026-11-24
- A second DMA connection is accepted but gets no reply until the first one is dropped (after
  its 1.0 s idle close): IDENTIFY on B answered after 999 ms while A stayed open. A fresh
  connection right after closing is served in ~8 ms.
- This made the plugin's startup probe (1.0 s timeout) fall back to HTTP. Fix (Python + plugin):
  probe on its own connection with a 2.5 s timeout, then open the real transport (1 s ack timeout).
  Verified: probe with another client holding the service → DMA after 999 ms.
- Standalone app: the "Notes in" / "Knobs in" lists were filled only when the window opened and
  came up empty for the user; they now refresh about once a second (also catches hot-plugging).

## Phase 8 — C64 audio through the plugin — 2026-11-24
- User request: hear the C64 through the Scarlett (plugin output) instead of only the C64U's out.
- Stream on the wired LAN (8 s): 47982.9 Hz, 0 lost, inter-arrival median 4.00 ms (192 frames),
  arrival vs ideal clock p99 0.23 ms, p99.9 2.1 ms, max 10 ms → 15 ms playback buffer.
- `AudioStreamReceiver` (plugin): UDP thread → lock-free ring → audio thread resamples with
  `juce::LagrangeInterpolator`; ratio = 47983/host × (1 + PI correction on the smoothed buffer
  level, clamped ±0.5% ≈ ±9 cents max, normally a few hundredths of a percent). Underrun → silence
  until refilled; a buffer far above target (audio was paused) jumps back to the target.
  `PUT /v1/streams/audio:start?ip=<our address>:<port>` on connect, `audio:stop` on disconnect.
- Tests: fake C64U streams a 1 kHz sine at 47983 Hz in real time → plugin output at 48 kHz:
  1 kHz ±10 Hz, rms 0.34, no loss; silence after the stream stops; audio:stop sent when turned off.
  Hidden hardware test: `MSID_HW_HOST=<C64U address> sidbridge-tests "[.hardware]"`.
- Hardware (2026-11-24, DMA, plugin rendered in real time at 48 kHz / 512): A4 → 440.4 Hz,
  C5 → 521.7 Hz (autocorrelation lag resolution), 2,924 packets / 0 lost / 0 underruns over ~12 s,
  buffer steady ≈ 20 ms, clock correction −0.06 %.

## 6581 leak hum after release — fixed 2026-11-24
- User: with "C64 audio" on, a low hum keeps sounding after each note. Measured on the C64U stream
  (C3, init patch, release 4): note -25.9 dBFS; 0.5-1.5 s and 2.5-3.5 s after release still
  -66.4 dBFS at 131/393/654 Hz (the note's harmonics), never fading; idle floor -72 dBFS
  (digital noise at 3849/15394 Hz only). It is the 6581's oscillator bleed-through with the
  envelope at zero — also present on the C64U's own output, just easy to miss there.
- Fix (Python + C++, `[engine] mute_after_release = true`): `Engine.service()` (bridge.py main
  loop every 30 ms, plugin bridge thread every 20 ms) sets a released voice's frequency to 0 once
  1.2 × the datasheet release time + 50 ms has passed, and forgets its note. Engine now has a
  lock (MIDI callbacks vs service). Vectors: `clock`/`service` events (1 hand-written scenario,
  189 in the randomized ones); C++ identical.
- After: 0.5-1.5 s and 2.5-3.5 s after release -72.3 / -72.5 dBFS, only the digital noise.

## Velocity response — 2026-11-24
- `[engine] velocity` = `sustain` (default) | `volume` | `off`, `velocity_amount` 0..1:
  scale = 1 − amount × (1 − velocity/127).
  - sustain: each voice's sustain nibble = round(patch sustain × scale). It's written in the same
    flush as, and before, the gate-on.
  - volume: the part's $D418 volume = round(patch volume × scale) on each note-on.
  - Plugin: parameters `velocity` (choice) and `velocity_amount` (0–100 %), host → engine only.
- Vectors: config keys `vel`/`vamt`, event `velmode <0-2> <pct>`. 7 hand-written scenarios, plus
  random velocities and modes in the randomized ones. C++ is identical.
- Hardware (A3, init patch: pulse, A0 D9 S10 R4, through bridge.py over DMA). Columns are the
  attack peak and the held level (1.0–1.4 s), dBFS:

  | velocity | sustain mode | volume mode | off |
  |---|---|---|---|
  | 127 | −21.9 / −25.9 | −21.6 / −25.9 | −21.9 / −25.9 |
  | 90 | −21.8 / −28.3 | **−18.4** / −27.8 | −21.9 / −26.0 |
  | 60 | −21.6 / −31.2 | **−15.1** / −31.3 | −21.8 / −26.0 |
  | 30 | −24.5 / −40.5 | **−17.4** / −35.6 | −24.3 / −28.6 |
  | 10 | −24.8 / −45.8 | **−15.7** / −46.3 | −24.4 / −28.7 |

  - Sustain mode gives about 20 dB of range on the held level, with the same attack peak (as
    designed).
  - Volume mode gives the same range, but every volume change makes a 6581 click: a step in its
    DC offset, louder than the note itself (bold). That's why sustain is the default.
  - Notes 4 and 5 are about 2.6 dB lower in every mode, including off. They land on SID 2 (poly
    allocation), and with this pulse patch socket 2 is still 2.7 dB quieter than socket 1, even
    with Vol Socket 2 at +3 dB. That +3 dB was matched with a triangle wave; the two 6581s don't
    scale evenly across waveforms. The mixer config was checked and is unchanged.
- Captures: `captures/velocity_{sustain,volume,off}.wav`.

## Pads → filter mode — 2026-11-24
- Oxygen 49 MKV pads (DAW mode, factory setting): Note On/Off 36–43 on MIDI channel 1 on the
  "USB MIDI" port, the same channel as the keys, so they can't be told apart from keys C2–G2.
  The pads' channel can be changed on the keyboard: tap a pad, then hold SHIFT and press CHANNEL
  UP/DOWN, the keys above the keybed (Oxygen Series MKV User Guide v1.2, pp. 12 and 30). Pad
  bank 2 is SHIFT + pad 1/2. After the change (user, 2026-11-24): pads on channel 10; pads 1-4
  send notes 40-43 and pads 5-8 send 36-39, so pad 1 is **note 40**. In DAW (Mackie) mode, on the
  Mackie/HUI port: Loop = note 86 (Mackie "cycle"), the button under fader 1 = note 0 (rec arm
  ch 1). Either could serve as a trigger later.
- `[pads]`: `channel = 10` (0 = none). Notes on it are pad actions and never play the SID.
  `filter_mode = 40` (pad 1) steps through `filter_modes = ["lp","bp","hp","off"]` on the knobs' target
  parts; a mode not in the list goes to the first one. In the plugin it updates the Filter Mode
  parameter, so the host sees it.
- Hardware (Saw Bass, cutoff 600, reso 10, A2 held, pad hit every 1.2 s), level and spectral
  centroid: LP −24.8 dBFS / 687 Hz → BP −27.6 / 1848 → HP −32.1 / 4326 → off −19.3 / 3847 →
  LP −24.6 / 686. The held note was unaffected. Capture: `captures/pad_filter_cycle.wav`.

## Controller profiles — 2026-11-24
- `profiles.py` / `cpp/core/src/Profile.cpp` read the same TOML format (documented at the top of
  `profiles/generic.toml`). Unknown keys and bad values are errors that name the file and line.
  The C++ side uses a small TOML-subset reader (`cpp/core/src/Toml.cpp`).
- Engine: `handle(msg, port)` with port "notes" or "controls" replaces `handle_mackie`.
  - Note controls are checked first; a matching note never plays the SID. Then the profile's
    ignore_channels drop notes, then the part/channel routing applies.
  - On the controls port, anything unmapped is ignored. Pitch bend on the notes port sets a param
    if one is mapped, and bends the notes otherwise.
  - CC 120/123 always panic.
  - Targets: params from the notes port go to the part that plays that channel. Params from the
    controls port, and all actions except sustain_pedal, go to `[engine] knobs_target`.
- Refactor check: with the Oxygen profile, the regenerated golden vectors were **byte-identical**
  to the old hard-wired engine's, before any new scenarios were added (72 scenarios, 5,120
  writes).
  - New: `tests/profiles/test-all.toml` (every trigger, port, mode and action), 7 hand-written
    and 12 randomized scenarios using it, and the `mnote` event.
  - Now 39 + 52 scenarios, 6,755 writes. The C++ engine is identical.
- Plugin:
  - Built-in profiles come from BinaryData. User profiles live in
    `~/Library/Application Support/SID Bridge/Profiles`; a file with a built-in's stem replaces it.
  - "Open profiles folder..." writes a README plus `*.toml.example` copies of the built-ins.
  - Settings `profile` and `presetsFile` are saved with the plugin's state. A broken or missing
    profile or presets file falls back (generic / built-in) and shows a notice in the status line.
  - Settings saved before profiles existed load as `oxygen-49-mkv`, which is exactly the old
    hard-wired behaviour; new installs default to `generic`.
  - Env override `MSID_PROFILE`.
- Two plugin bugs found and fixed on the way:
  1. JUCE 9 lists and opens MIDI devices on the message thread only (JUCE_ASSERT_MESSAGE_THREAD
     in MidiDeviceListConnectionBroadcaster). The bridge thread used to open the direct ports
     itself. It now asks (`requestDirectPorts`), and the message thread opens and closes them
     (`openDirectPorts`, through a WeakReference callAsync). In tests the old way stalled the bridge
     thread ("killing thread by force").
  2. Host MIDI waiting while disconnected was discarded at the very end of connect(), after the
     status already said "connected", so a note played in that window was lost. It's now
     discarded before the status changes.
- Hardware (Saw Bass, cutoff 600, A2 held, virtual ports → Oxygen profile; bridge.py and the app
  gave the same results):

  | Step | Level | Spectral centroid |
  |---|---|---|
  | LP (start) | −24.8 dBFS | 687 Hz |
  | pad 1 → BP | −27.5 dBFS | 1817 Hz |
  | pad 1 → HP | −32.1 dBFS | 4312 Hz |
  | pad 1 twice → LP | −24.7 dBFS | 693 Hz |
  | knob 1 (controls port, relative) +60 ticks | −26.7 dBFS | 3058 Hz |

  0 packets lost.
- Tests: Python 134, C++ 57. auval and pluginval (strictness 10, VST3 + AU) pass.

## UI redesign, Find, MIDI learn — 2026-11-24
- **Window:** 960×600, Play / Setup tabs.
  - New files: `Ui.{h,cpp}` (look and widgets), `PlayPage`, `SetupPage`; `PluginEditor` is the
    header and shell.
  - The user chose option A from mockups and renamed Panic → **Silence** and "C64 audio" →
    **Stream C64U audio**.
  - It opens on Setup until `setupDone`, which is set the first time a connection succeeds and
    saved with the state. Older saved state with an address counts as set up.
  - The filter toggles allow all three on, so `lp+bp+hp` is now a filter mode (Python + C++,
    appended to the list so parameter indices stay the same).
- **Find** (`cpp/core/src/Finder.cpp`, `finder.py`): PS64's method (src/discover.c).
  - Connect to port 80 on the local /24 (plus the /24 of the last address), 64 at a time with a
    600 ms window, then `GET /v1/info` on the hosts that answered. It keeps products containing
    "Ultimate"; a 403 is listed as password-protected.
  - On the user's LAN: found `C64-Ultimate-XXXXXX` at <C64U address> (firmware 1.2.1) in 2.6 s.
  - The plugin runs it on a background thread (`BridgeController::startFind`). On first run
    with no address, Setup starts it by itself and uses the result if exactly one usable C64U
    was found.
- **MIDI learn** (`BridgeController::startLearn`, bridge thread):
  - The next CC or pitch bend (for a param), or note-on or pressed CC (for an action), is
    consumed instead of played. Knobs: after the first message it collects 0.4 s of values.
    Repeated identical values in 1..15 mean relative-signed, in 65..79 relative-offset;
    otherwise absolute (absolute knobs only send on change, endless ones repeat their step).
  - The learned control replaces every other control for that target and any other use of
    that trigger.
  - It's saved as a user profile: a built-in `<id>` is copied to `<id>-mine` / "<name> (mine)"
    via `profileToToml`, and used at once through `Engine::setProfile` (no reconnect).
  - Gives up after 20 s with nothing. "Forget" removes a target's controls.
- **Fixes on the way:** a `PlayPage` member-order crash (a slider destroyed before its
  attachment); and `Card::content()` now returns the parent's coordinates (Setup's controls had
  all landed in the first card).
- **Tests:** Python 139; C++ 70 (tabs and first run, Find and auto-use, learn absolute /
  relative / action / cancel / forget, Silence, profile TOML round trip, `setProfile`, finder on
  localhost). auval and pluginval (strictness 10) pass. Hardware with the standalone app: same
  results as before the redesign, 0 lost. Snapshots in `docs/`.

## Standalone app: own window, setup in one place — 2026-11-24
- The user asked why JUCE's generic **Options → Audio/MIDI Settings** dialog sat apart from
  Setup. It belongs to JUCE's standalone wrapper. With all three Oxygen ports ticked there, the
  Mackie/HUI port also arrived as keys: fader 1 bent the pitch, and the Loop / fader buttons
  played notes.
- Fix: `JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1` (Standalone target only) plus
  `Source/StandaloneApp.cpp`.
  - It uses the same `StandalonePluginHolder` and the same settings file (`SID Bridge.settings`,
    same PropertiesFile options), in a plain window with no Options bar.
  - It switches the holder's MIDI inputs off; SID Bridge opens its own ports.
  - `StandaloneHooks.h` gives Setup the device manager, which adds an **Audio output** choice
    (standalone only).
  - `BridgeController::setStandalone`: an empty Notes in means "Auto": the profile's notes port
    (or the first input that isn't the controls port), shown as "Auto: <port>".
- Tests 71 C++ / 139 Python; auval and pluginval pass. The user's app relaunched with the new
  window: it connected, and the settings were kept.

## About screen — 2026-11-24
- `Source/About.{h,cpp}`: an in-window panel with the version (`JucePlugin_VersionString`,
  0.1.0 from the CMake project), a one-line description, and the JUCE statement (dual-licensed
  AGPLv3 / commercial JUCE licence, per JUCE's LICENSE.md) with links to juce.com, the AGPLv3 and
  the JUCE 9 licence.
- It opens from the window title, from the Setup footnote ("Made with JUCE · About SID
  Bridge"), and in the standalone app from the SID Bridge app menu (`MenuBarModel::setMacMainMenu`
  with an extra Apple-menu item). Escape, Close or a click outside closes it.
- Tests: 72 C++ (adds an About test) / 139 Python; auval and pluginval pass.

## Update check and going public — 2026-11-24
- `Source/Updates.{h,cpp}` is a port of RoughTrack's update checker (`updates::` namespace) for
  `api.github.com/repos/dantaipan/sid-bridge/releases/latest`.
  - Versions compare part by part. Drafts and prereleases are ignored. The download is a .dmg,
    else a .zip, else the release page. Release notes show without the download section, images
    and checksum.
  - `UpdateChecker` (a SharedResourcePointer) keeps the preferences in
    `~/Library/Application Support/SID Bridge.updates`: automatic on/off, the last check (as
    "version time"), and a skipped version.
  - Standalone app: an automatic check once a day per version, plus SID Bridge → Check for
    Updates…. Plugin: only from About ("Check for updates"). The update panel has Download /
    Skip This Version / Later. A 404 (no release yet) reads "No SID Bridge release has been
    published yet."
- The user chose for the public repo `dantaipan/sid-bridge`, AGPL-3.0-or-later (as RoughTrack;
  JUCE under the AGPL).
  - Left out: `captures/`, raw `bench_*.json` and `attic/` (they stay on the Mac).
  - The C64U's LAN address and hostname were replaced: `config.toml` has an empty host; your own
    settings go in a git-ignored `config.local.toml`, read on top. `bridge.py` with no address
    runs Find. Docs and tests use neutral examples.
  - JUCE is a git submodule (`cpp/external/JUCE`, juce-framework/JUCE tag 9.0.4, commit 3fb5cbc).

## Voice modes and arpeggiator — 2026-11-24
- The user asked for SID SYNTH-style voice modes. `[engine] mode` / `set_voice_mode`: poly, mono,
  unison, octave, fifth, chord, arp.
  - Every part now always has all its voices (3 per SID). The mode only decides how notes use
    them, so it can change while playing; sounding notes are released on a change.
  - Each voice has an `offset` (semitones): a stack interval (+12 / +7 / +7 and +12) or the
    unison detune (+/- `unison_detune` cents, alternating: 0, +d, -d, +2d, ...). Bend keeps the
    offsets.
  - Stacks take a free voice per interval (released longest ago, else the oldest stolen), so
    chord polyphony = voices / 3.
- Arpeggiator (`[arp]` rate / pattern / octaves / gate / bpm):
  - The held notes (pedal-held too) play on voice 1 of the part, re-gated each step. The first
    note plays at once.
  - Patterns: up, down, up-down (bounces without repeating the ends), random (LCG
    `x = (x*1103515245 + 12345) & 0x7FFFFFFF`, seeded 1 at each start, so Python and C++ match),
    and as played. 1-3 octaves.
  - Steps run from `service()` (bridge.py every 3 ms in arp, the plugin every 2 ms) at
    60/bpm/steps-per-beat. A stall over 1 s skips ahead instead of bursting.
  - MIDI clock: 24 pulses per quarter note; a step every 24/steps-per-beat pulses, and the gate
    off after round(pulses x gate). Start resets the pattern. While clock arrives (a pulse in the
    last 0.5 s), time-based stepping waits.
  - Plugin tempo: the host's BPM from the playhead, else the `arp_bpm` parameter.
- Vectors: events `mclock`, `mstart`, `vmode`, `arpset`; config keys `detune`, `arate`, `apat`,
  `aoct`, `agate`, `bpm`. Every earlier scenario stayed byte-identical (mono included, despite
  the voice-allocation change).
  - Now 53 hand-written + 76 randomized scenarios, 10,414 writes. The C++ engine is identical.
- Plugin:
  - Parameters `voice_mode` (two-way, so the profile action shows in the host), `arp_rate`,
    `arp_pattern`, `arp_octaves`, `arp_gate`, `arp_bpm`.
  - The Play tab has a Voice mode bar (arp controls in arp mode, a one-line description
    otherwise). Mono moved there from Setup.
  - Host MIDI clock and start reach the engine.
- Hardware (C64U, bridge.py over DMA):
  - Arp 1/8 at 120 BPM, C4 E4 G4 held: it played C4 E4 G4 repeating, steps 249.5 ms apart
    (expected 250, sd 15 ms).
  - Chord on A2: 110 / 164 / 220 Hz (A2 E3 A3).
- Tests: Python 155, C++ 79; auval and pluginval pass.

## Latency write-up and first release — 2026-11-24
- README "Latency: what to expect" summarizes the measurements above.
  - A write lands in ≈ 1 ms over DMA, ≈ 7 ms over HTTP.
  - Note → sound in the network audio stream is ≈ 40 / 42 ms. The C64U's analog output is
    estimated at a few ms: it isn't measured, since that needs an analog loopback.
  - Stream C64U audio adds ≈ 20 ms; knob changes take 25 ms in the worst case; the DAW adds up
    to one buffer; and there's the jitter.
- `tools/make_release.sh`, modelled on RoughTrack's:
  1. The `dist` preset builds universal (arm64 + x86_64), macOS 13+, with no tests.
  2. It signs the app, the AU and the VST3 with Developer ID (hardened runtime, timestamp),
     notarizes them in one zip and staples each.
  3. It builds the DMG: the app, an Applications link, Plug-ins/ with links to
     /Library/Audio/Plug-Ins/{Components,VST3}, and "Read Me.txt" (from tools/dmg-readme.txt).
  4. It signs, notarizes and staples the DMG, then runs spctl and stapler checks.
  - Credentials: the `rifftrack-notary` keychain profile, shared with RoughTrack.
- The app's Info.plist now has `NSLocalNetworkUsageDescription`, for macOS 15's local-network
  prompt (via PLIST_TO_MERGE); HARDENED_RUNTIME_ENABLED is on.
- v0.1.0: dist/SID-Bridge-0.1.0.dmg, 19 MB; notarization accepted for the bundles and the DMG;
  spctl reports "Notarized Developer ID". Notes are in docs/releases/v0.1.0.md.

## App icon — 2026-11-24
- The user designed it (in Claude.ai): direction A, "SID chip with a rainbow sawtooth", kept as
  `docs/icon.svg` / `docs/icon.png` (1024 px). An earlier green-wave version is in attic/ (not
  in git).
- `ICON_BIG docs/icon.png` in juce_add_plugin: JUCE writes `AppIcon.icns` into the app, the AU
  and the VST3. **JUCE generates it when CMake configures**, not when it builds: after changing
  the icon, run `cmake --preset release` again. The release script configures fresh every time.
  The release DMG gets it as its volume icon (`.VolumeIcon.icns` + `SetFile -a C`).
- Small sizes: at 32 px the chip, "SID" and the coloured wave read; at 16 px it's a chip shape
  with a coloured line.

## Renamed SID Bridge → SIDwire — 2026-11-24
- Why: "C64 Bridge" (a remote-control/MCP tool for the C64U, not music) has a confusingly close
  name. The user chose SIDwire; no product of that name was found, and the GitHub name was free.
  Older entries above keep the old name.
- What changed: `PRODUCT_NAME "SIDwire"`, so the bundles are SIDwire.app / .component / .vst3
  and `JucePlugin_Name` is "SIDwire"; every visible string; the update URL
  (`api.github.com/repos/dantaipan/sidwire/releases/latest`; GitHub redirects the old one, so
  0.1.x apps still find new releases); User-Agent `SIDwire/<ver>`; release script and DMG
  `SIDwire-<ver>.dmg`; README screenshots `docs/sidwire-{plugin,setup}.png`.
- **Kept on purpose:** `PLUGIN_MANUFACTURER_CODE Msid`, `PLUGIN_CODE Sidb`, `BUNDLE_ID
  com.midisid.sidbridge`, `COMPANY_NAME "MidiSid"`. The VST3 class IDs in moduleinfo.json are
  identical to 0.1.1's (`ABCDEF019182FAEB4D73696453696462` processor), and the AU is still
  `aumu Sidb Msid` (now named "MidiSid: SIDwire"), so DAW projects reopen with their state.
  Internal names (CMake target `SidBridge`, `SidBridgeProcessor`, `SidBridge_artefacts`,
  `sidbridge-tests`) are unchanged: invisible to users. The lock file stays
  `/tmp/midi-sid-bridge-<host>.lock`, so 0.1.x, SIDwire and bridge.py still exclude each other.
- Migration (`cpp/plugin/Source/Migration.h`): on first use SIDwire copies
  `~/Library/Application Support/SID Bridge.settings`, `SID Bridge.updates` and
  `SID Bridge/Profiles` to the SIDwire names, only when the new one doesn't exist; the old files
  stay, so going back to 0.1.x works.
- Setup shows "Old SID Bridge still installed: show it" (reveals it in Finder) while an old
  SID Bridge.app / .component / .vst3 is in /Applications, ~/Applications or
  {~,}/Library/Audio/Plug-Ins: both have the same codes, so a DAW may load either. Rechecked
  about once a second while Setup is open. Tests point the search at an empty folder.
- Version 0.2.0. Tests: C++ 81 (new: migration, old-install notice), auval, pluginval 10.
