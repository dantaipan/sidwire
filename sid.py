"""SID register model: shadow registers, dirty tracking, pitch maths.

Pure code — no network. One SidState per physical SID chip.

Register map (offsets from the SID base, voice v = 0..2, o = 7*v):
  o+0/o+1  frequency lo/hi
  o+2/o+3  pulse width lo / hi (low nibble) — 12 bits
  o+4      control: GATE $01, SYNC $02, RING $04, TEST $08,
                    TRI $10, SAW $20, PULSE $40, NOISE $80
  o+5      attack (hi nibble) / decay (lo nibble)
  o+6      sustain (hi nibble) / release (lo nibble)
  $15      cutoff lo (bits 0-2)
  $16      cutoff hi (bits 3-10)
  $17      resonance (hi nibble) | routing: FILT1 $01, FILT2 $02, FILT3 $04, FILTEX $08
  $18      mode: LP $10, BP $20, HP $40, 3OFF $80 | volume (lo nibble)
"""
from __future__ import annotations

PAL_CLOCK = 985248
NTSC_CLOCK = 1022727

NUM_REGS = 25
NUM_VOICES = 3

# control register bits
GATE, SYNC, RING, TEST = 0x01, 0x02, 0x04, 0x08
TRI, SAW, PULSE, NOISE = 0x10, 0x20, 0x40, 0x80
WAVE_MASK = 0xF0

# global register offsets
FC_LO, FC_HI, RES_FILT, MODE_VOL = 0x15, 0x16, 0x17, 0x18

# filter modes ($18 high nibble)
LP, BP, HP, VOICE3_OFF = 0x10, 0x20, 0x40, 0x80


# ---------------------------------------------------------------- pitch maths

def note_to_hz(note: float, bend_semitones: float = 0.0) -> float:
    """MIDI note (may be fractional) plus bend → Hz, A4 (69) = 440 Hz."""
    return 440.0 * 2.0 ** ((note + bend_semitones - 69) / 12.0)


def hz_to_sid(hz: float, clock: int = PAL_CLOCK) -> int:
    """Hz → 16-bit SID frequency word, clamped to 0..65535."""
    return max(0, min(0xFFFF, round(hz * 16777216 / clock)))


def note_to_sid(note: float, bend_semitones: float = 0.0, clock: int = PAL_CLOCK) -> int:
    return hz_to_sid(note_to_hz(note, bend_semitones), clock)


def bend_to_semitones(pitch: int, bend_range: float = 2.0) -> float:
    """mido pitchwheel value (-8192..8191) → semitones."""
    return pitch / (8192.0 if pitch < 0 else 8191.0) * bend_range


# ---------------------------------------------------------------- dirty ranges

def merge_ranges(offsets, max_gap: int = 0) -> list[tuple[int, int]]:
    """Merge sorted register offsets into inclusive (start, end) ranges.

    Offsets whose distance is <= max_gap + 1 are joined (the gap bytes get
    rewritten with their unchanged shadow values, which is harmless).
    """
    out: list[list[int]] = []
    for o in sorted(set(offsets)):
        if out and o - out[-1][1] <= max_gap + 1:
            out[-1][1] = o
        else:
            out.append([o, o])
    return [(a, b) for a, b in out]


# ---------------------------------------------------------------- state

class SidState:
    """Shadow copy of one SID's 25 writable registers plus a dirty set."""

    def __init__(self, base: int = 0xD400, clock: int = PAL_CLOCK):
        self.base = base
        self.clock = clock
        self.regs = bytearray(NUM_REGS)
        self.committed = bytearray(NUM_REGS)  # values already handed to the transport
        self.dirty: set[int] = set()

    # --- raw access
    def set_reg(self, off: int, value: int, force: bool = False) -> None:
        if not 0 <= off < NUM_REGS:
            raise IndexError(f"SID register offset {off:#x} out of range")
        value &= 0xFF
        if force or self.regs[off] != value:
            self.regs[off] = value
            self.dirty.add(off)

    def get_reg(self, off: int) -> int:
        return self.regs[off]

    def _update(self, off: int, mask: int, bits: int) -> None:
        self.set_reg(off, (self.regs[off] & ~mask) | (bits & mask))

    def mark_all_dirty(self) -> None:
        self.dirty = set(range(NUM_REGS))

    def take_dirty(self) -> set[int]:
        """Return and clear the dirty offsets."""
        d, self.dirty = self.dirty, set()
        return d

    def chunks(self, offsets, max_gap: int = NUM_REGS, src=None) -> list[tuple[int, bytes]]:
        """(absolute address, bytes) chunks for the given offsets, read from src (default regs)."""
        src = self.regs if src is None else src
        return [(self.base + a, bytes(src[a:b + 1]))
                for a, b in merge_ranges(offsets, max_gap)]

    def gate_off_pending(self, voice: int) -> bool:
        """True if a gate-off has been set but not yet committed for this voice."""
        c = self.voff(voice) + 4
        return bool(self.committed[c] & GATE) and not (self.regs[c] & GATE)

    def snapshot(self, strict_order: bool = True) -> list[tuple[int, bytes]]:
        """Turn the dirty set into an ordered list of (address, bytes) writes.

        Normally one contiguous write — a single writemem is applied in
        ascending address order, so each voice's freq/PW already precede its
        control register. ADSR ($+5/$+6) come *after* control, so when a voice
        has both a dirty control register and dirty ADSR (and strict_order),
        the batch is split: first everything with the old control values,
        then the control registers.
        """
        d = self.take_dirty()
        if not d:
            return []
        ctrl = {7 * v + 4 for v in range(NUM_VOICES)} & d
        split = strict_order and any(c + 1 in d or c + 2 in d for c in ctrl)
        if not split:
            writes = self.chunks(d)
        else:
            staged = bytearray(self.regs)
            for c in ctrl:
                staged[c] = self.committed[c]
            writes = self.chunks(d - ctrl, src=staged) if d - ctrl else []
            writes += self.chunks(ctrl)
        self.committed[:] = self.regs
        return writes

    # --- voice helpers
    @staticmethod
    def voff(voice: int) -> int:
        if not 0 <= voice < NUM_VOICES:
            raise IndexError(f"voice {voice} out of range")
        return 7 * voice

    def set_freq(self, voice: int, fn: int) -> None:
        o = self.voff(voice)
        fn = max(0, min(0xFFFF, int(fn)))
        self.set_reg(o, fn & 0xFF)
        self.set_reg(o + 1, fn >> 8)

    def get_freq(self, voice: int) -> int:
        o = self.voff(voice)
        return self.regs[o] | self.regs[o + 1] << 8

    def set_note(self, voice: int, note: float, bend_semitones: float = 0.0) -> None:
        self.set_freq(voice, note_to_sid(note, bend_semitones, self.clock))

    def set_pulse_width(self, voice: int, pw: int) -> None:
        o = self.voff(voice)
        pw = max(0, min(0xFFF, int(pw)))
        self.set_reg(o + 2, pw & 0xFF)
        self.set_reg(o + 3, pw >> 8)

    def get_pulse_width(self, voice: int) -> int:
        o = self.voff(voice)
        return self.regs[o + 2] | (self.regs[o + 3] & 0x0F) << 8

    def set_control(self, voice: int, value: int) -> None:
        self.set_reg(self.voff(voice) + 4, value)

    def get_control(self, voice: int) -> int:
        return self.regs[self.voff(voice) + 4]

    def set_gate(self, voice: int, on: bool) -> None:
        self._update(self.voff(voice) + 4, GATE, GATE if on else 0)

    def set_waveform(self, voice: int, wave: int) -> None:
        """Waveform bits plus SYNC/RING/TEST (everything but GATE)."""
        self._update(self.voff(voice) + 4, 0xFE, wave)

    def set_adsr(self, voice: int, a: int | None = None, d: int | None = None,
                 s: int | None = None, r: int | None = None) -> None:
        o = self.voff(voice)
        ad, sr = self.regs[o + 5], self.regs[o + 6]
        if a is not None: ad = (ad & 0x0F) | (a & 0xF) << 4
        if d is not None: ad = (ad & 0xF0) | (d & 0xF)
        if s is not None: sr = (sr & 0x0F) | (s & 0xF) << 4
        if r is not None: sr = (sr & 0xF0) | (r & 0xF)
        self.set_reg(o + 5, ad)
        self.set_reg(o + 6, sr)

    def get_adsr(self, voice: int) -> tuple[int, int, int, int]:
        o = self.voff(voice)
        ad, sr = self.regs[o + 5], self.regs[o + 6]
        return ad >> 4, ad & 0xF, sr >> 4, sr & 0xF

    # --- filter / global
    def set_cutoff(self, fc: int) -> None:
        fc = max(0, min(0x7FF, int(fc)))
        self.set_reg(FC_LO, fc & 0x07)
        self.set_reg(FC_HI, fc >> 3)

    def get_cutoff(self) -> int:
        return (self.regs[FC_LO] & 0x07) | self.regs[FC_HI] << 3

    def set_resonance(self, res: int) -> None:
        self._update(RES_FILT, 0xF0, (res & 0xF) << 4)

    def set_filter_route(self, voice: int, on: bool) -> None:
        bit = 1 << voice
        self._update(RES_FILT, bit, bit if on else 0)

    def set_filter_ext(self, on: bool) -> None:
        self._update(RES_FILT, 0x08, 0x08 if on else 0)

    def set_filter_mode(self, mode: int) -> None:
        """Any OR of LP/BP/HP/VOICE3_OFF."""
        self._update(MODE_VOL, 0xF0, mode)

    def set_volume(self, vol: int) -> None:
        self._update(MODE_VOL, 0x0F, max(0, min(15, int(vol))))

    def get_volume(self) -> int:
        return self.regs[MODE_VOL] & 0x0F
