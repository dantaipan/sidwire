#!/usr/bin/env python3
"""Generate golden vectors: run MIDI scenarios through the Python engine and record
every register write per flush plus the final register state. The C++ core must
reproduce them byte for byte (cpp/core/tests/VectorTests.cpp).

  .venv/bin/python tools/gen_vectors.py            # writes cpp/core/tests/vectors/*.txt
"""
from __future__ import annotations

import copy
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import mido  # noqa: E402

from engine import DEFAULTS, VELOCITY_MODES, Engine, load_presets  # noqa: E402
from profiles import load_profile  # noqa: E402
from sid import SidState  # noqa: E402
from transport import DryRunTransport, Flusher  # noqa: E402

OUT = ROOT / "cpp" / "core" / "tests" / "vectors"
PRESETS = load_presets(ROOT / "presets.toml")


class Scenario:
    def __init__(self, name: str, nsids=2, layout="poly", mode="poly", hard_restart=0,
                 legato=1, strict=1, presets=1, target=0, channel=0, vel="sustain", vamt=100,
                 profile="oxygen-49-mkv"):
        self.name = name
        self.conf = dict(nsids=nsids, layout=layout, mode=mode, hard_restart=hard_restart,
                         legato=legato, strict=strict, presets=presets, target=target, channel=channel,
                         vel=vel, vamt=vamt, profile=profile)
        cfg = copy.deepcopy(DEFAULTS)
        cfg["engine"].update(layout=layout, mode=mode, hard_restart=bool(hard_restart),
                             legato=bool(legato), strict_order=bool(strict),
                             velocity=vel, velocity_amount=vamt / 100)
        cfg["engine"]["knobs_target"] = "all" if target == 0 else target
        prof = load_profile(profile, [ROOT / "tests" / "profiles"]).with_channel(channel)
        self.t = DryRunTransport(echo=False)
        self.fl = Flusher(self.t, [SidState(0xD400 + 0x20 * i) for i in range(nsids)],
                          strict_order=bool(strict))
        self.e = Engine(self.fl, cfg, [dict(p) for p in PRESETS] if presets else [], prof)
        self.fake_time = 0.0
        self.e.now = lambda: self.fake_time
        self.lines = [f"scenario {name}",
                      "config " + " ".join(f"{k}={v}" for k, v in self.conf.items())]

    def ev(self, line: str) -> None:
        self.lines.append(line)
        k, *a = line.split()
        a = [int(x) for x in a]
        e = self.e
        if k == "init":
            e.init_chip()
        elif k == "flush":
            self.fl.flush()
            for addr, data in self.t.writes:
                self.lines.append(f"W {addr:04X} {data.hex().upper()}")
            self.t.writes.clear()
        elif k == "on":
            e.handle(mido.Message("note_on", channel=a[0], note=a[1], velocity=a[2]))
        elif k == "off":
            e.handle(mido.Message("note_off", channel=a[0], note=a[1]))
        elif k == "cc":
            e.handle(mido.Message("control_change", channel=a[0], control=a[1], value=a[2]))
        elif k == "bend":
            e.handle(mido.Message("pitchwheel", channel=a[0], pitch=a[1]))
        elif k == "prog":
            e.handle(mido.Message("program_change", channel=a[0], program=a[1]))
        elif k == "mcc":
            e.handle(mido.Message("control_change", control=a[0], value=a[1]), "controls")
        elif k == "mnote":                 # note on the controls port (vel 0 = off)
            e.handle(mido.Message("note_on", channel=a[0], note=a[1], velocity=a[2]), "controls")
        elif k == "mbend":
            e.handle(mido.Message("pitchwheel", channel=a[0], pitch=a[1]), "controls")
        elif k == "panic":
            e.panic()
        elif k == "clock":                 # fake clock, ms
            self.fake_time = a[0] / 1000.0
        elif k == "service":
            e.service()
        elif k == "velmode":               # velocity mode index, amount in %
            e.set_velocity(VELOCITY_MODES[a[0]], a[1] / 100)
        else:
            raise ValueError(line)

    def run(self, *events: str) -> "Scenario":
        for ev in events:
            self.ev(ev)
        return self

    def finish(self) -> str:
        self.ev("flush")
        for i, st in enumerate(self.fl.states):
            self.lines.append(f"regs {i} {bytes(st.regs).hex().upper()}")
        self.lines.append("end")
        return "\n".join(self.lines) + "\n"


def handwritten() -> list[str]:
    S = []
    S.append(Scenario("chord_and_release").run(
        "init", "flush", "on 0 60 100", "on 0 64 100", "on 0 67 100", "flush",
        "off 0 64", "flush", "off 0 60", "off 0 67").finish())
    S.append(Scenario("six_voices_then_steal").run(
        "init", "flush", *[f"on 0 {n} 100" for n in range(60, 66)], "flush",
        "on 0 70 100", "flush", "on 0 71 90").finish())
    S.append(Scenario("retrigger_same_window").run(
        "init", "flush", "on 0 60 100", "flush", "off 0 60", "on 0 60 100", "flush").finish())
    S.append(Scenario("velocity_zero_noteoff").run(
        "init", "flush", "on 0 60 100", "on 0 60 0", "flush").finish())
    S.append(Scenario("adsr_and_gate_split").run(
        "init", "flush", "cc 0 73 127", "on 0 48 100").finish())
    S.append(Scenario("no_strict_order", strict=0).run(
        "init", "flush", "cc 0 73 127", "on 0 48 100").finish())
    S.append(Scenario("bend_retunes").run(
        "init", "flush", "on 0 60 100", "on 0 64 100", "flush", "bend 0 8191", "flush",
        "bend 0 -8192", "on 0 67 100", "flush", "bend 0 1234").finish())
    S.append(Scenario("cc_map").run(
        "init", "flush", "cc 0 74 0", "cc 0 74 127", "cc 0 71 127", "cc 0 1 64", "flush",
        "cc 0 73 127", "cc 0 75 0", "cc 0 76 64", "cc 0 72 32", "cc 0 7 40", "cc 0 99 10").finish())
    S.append(Scenario("presets_cycle").run(
        "init", "flush", "on 0 60 100", *[f"prog 0 {i}" for i in range(10)], "flush",
        "prog 0 7", "on 0 64 100").finish())
    S.append(Scenario("waveform_programs_no_presets", presets=0).run(
        "init", "flush", "on 0 60 100", "prog 0 1", "flush", "prog 0 10").finish())
    S.append(Scenario("panic_and_unmute").run(
        "init", "flush", "on 0 60 100", "on 0 64 100", "flush", "cc 0 123 0", "flush",
        "on 0 67 100", "flush", "panic", "cc 0 7 127").finish())
    S.append(Scenario("sustain_pedal").run(
        "init", "flush", "cc 0 64 127", "on 0 60 100", "off 0 60", "on 0 62 100", "off 0 62",
        "flush", "on 0 60 100", "cc 0 64 0").finish())
    S.append(Scenario("mono_legato", mode="mono").run(
        "init", "flush", "on 0 60 100", "flush", "on 0 64 100", "flush", "on 0 67 100",
        "off 0 67", "flush", "off 0 60", "flush", "off 0 64").finish())
    S.append(Scenario("mono_no_legato", mode="mono", legato=0).run(
        "init", "flush", "on 0 60 100", "flush", "on 0 64 100", "flush", "off 0 64").finish())
    S.append(Scenario("hard_restart", hard_restart=1).run(
        "init", "flush", "on 0 60 100", "flush", "on 0 60 100", "flush", "off 0 60").finish())
    S.append(Scenario("split_channels", layout="split").run(
        "init", "flush", "on 0 60 100", "on 1 60 100", "on 5 62 100", "flush",
        "bend 1 8191", "cc 0 74 0", "prog 1 1", "flush", "off 1 60").finish())
    S.append(Scenario("split_mono", layout="split", mode="mono").run(
        "init", "flush", "on 0 60 100", "on 0 64 100", "on 1 67 100", "flush").finish())
    S.append(Scenario("mackie_encoders").run(
        "init", "flush", *["mcc 16 65"] * 20, "flush", *["mcc 17 1"] * 9, "mcc 16 3",
        "mcc 18 66", "mcc 40 1", "flush", "prog 0 1", "mcc 16 1").finish())
    S.append(Scenario("mackie_target_2", layout="split", target=2).run(
        "init", "flush", "mcc 16 65", "mcc 23 65", "mbend 0 -8192", "flush", "mbend 0 8191").finish())
    S.append(Scenario("mackie_faders").run(
        "init", "flush", "mbend 0 -8192", "flush", "mbend 0 0", "mbend 5 0", "flush",
        "panic", "mbend 0 8191").finish())
    S.append(Scenario("channel_filter", nsids=1, channel=2).run(
        "init", "flush", "on 0 60 100", "on 1 61 100", "flush").finish())
    S.append(Scenario("mute_after_release").run(
        "init", "flush", "clock 1000", "on 0 60 100", "on 0 64 100", "flush", "clock 1500",
        "off 0 60", "flush", "clock 1600", "service", "flush", "clock 1690", "service", "flush",
        "bend 0 4000", "flush", "off 0 64", "cc 0 72 127", "clock 2000", "service", "flush",
        "clock 30000", "service", "flush", "on 0 60 100", "flush").finish())
    S.append(Scenario("velocity_sustain").run(
        "init", "flush", "on 0 60 127", "on 0 64 64", "on 0 67 1", "flush", "cc 0 76 127", "flush",
        "off 0 64", "on 0 72 100", "flush", "prog 0 2", "on 0 74 20").finish())
    S.append(Scenario("velocity_amount_half", vamt=50).run(
        "init", "flush", "on 0 60 1", "on 0 61 50", "on 0 62 126", "flush").finish())
    S.append(Scenario("velocity_volume", vel="volume").run(
        "init", "flush", "on 0 60 64", "flush", "on 0 64 127", "cc 0 7 64", "flush", "panic",
        "on 0 60 10", "flush", "mbend 0 8191", "on 0 62 90").finish())
    S.append(Scenario("velocity_volume_split", vel="volume", layout="split").run(
        "init", "flush", "on 0 60 127", "on 1 60 32", "flush").finish())
    S.append(Scenario("velocity_off", vel="off").run(
        "init", "flush", "on 0 60 1", "on 0 64 127", "flush").finish())
    S.append(Scenario("velocity_mode_changes").run(
        "init", "flush", "on 0 60 10", "on 0 64 90", "flush", "velmode 2 100", "flush",
        "on 0 67 30", "flush", "velmode 0 100", "flush", "velmode 1 40", "on 0 69 1", "flush").finish())
    S.append(Scenario("velocity_hard_restart_mono", mode="mono", hard_restart=1, legato=0).run(
        "init", "flush", "on 0 60 30", "flush", "on 0 64 127", "flush", "off 0 64", "flush").finish())
    S.append(Scenario("pads_filter_cycle").run(
        "init", "flush", "on 0 60 100", "flush", *[f"on 9 40 100" for _ in range(3)], "flush",
        "on 9 36 100", "on 9 40 0", "off 9 40", "on 0 40 100", "flush", "on 9 40 127", "flush").finish())
    S.append(Scenario("pads_split_target", layout="split", target=2).run(
        "init", "flush", "on 9 40 100", "flush", "prog 1 1", "on 9 40 100", "flush").finish())
    T = "test-all"
    S.append(Scenario("profile_cc_params_and_channels", profile=T).run(
        "init", "flush", "cc 0 74 20", "flush", "cc 1 20 127", "cc 0 20 0", "cc 2 20 0", "flush",
        "on 0 60 100", "bend 2 8191", "bend 0 8191", "flush", "on 8 61 100", "on 9 62 100", "flush").finish())
    S.append(Scenario("profile_actions_cc", profile=T).run(
        "init", "flush", "cc 0 80 127", "flush", "cc 0 80 0", "cc 0 80 64", "cc 0 81 127", "cc 0 81 127",
        "flush", "cc 0 82 100", "cc 0 82 100", "cc 0 84 127", "flush", "on 0 60 100", "cc 0 64 127",
        "off 0 60", "flush", "cc 0 64 0", "flush", "on 0 62 100", "cc 0 83 127", "flush").finish())
    S.append(Scenario("profile_preset_prev_first", profile=T).run(
        "init", "flush", "cc 0 81 127", "flush").finish())
    S.append(Scenario("profile_note_button", profile=T).run(
        "init", "flush", "on 0 48 100", "off 0 48", "flush", "on 1 48 100", "flush", "on 0 48 0").finish())
    S.append(Scenario("profile_controls_port", profile=T).run(
        "init", "flush", *["mcc 10 1"] * 3, "mcc 10 127", "mcc 11 66", "mcc 11 63", "flush", "mcc 12 30",
        "mcc 40 1", "mnote 0 86 127", "mnote 0 86 0", "mnote 0 60 100", "flush", "on 0 60 100",
        "mcc 13 127", "off 0 60", "flush", "mcc 13 0", "mbend 4 0", "flush").finish())
    S.append(Scenario("profile_split_targets", profile=T, layout="split", target=2).run(
        "init", "flush", "cc 0 74 0", "cc 1 74 127", "mcc 12 0", "cc 0 84 127", "mnote 0 86 127",
        "flush", "on 0 60 100", "on 1 60 100", "cc 1 64 127", "off 1 60", "off 0 60", "flush").finish())
    S.append(Scenario("profile_keys_channel", profile=T, channel=2).run(
        "init", "flush", "on 0 60 100", "on 1 61 100", "cc 0 74 0", "cc 1 74 0", "flush").finish())
    S.append(Scenario("one_sid", nsids=1).run(
        "init", "flush", *[f"on 0 {n} 100" for n in (60, 64, 67, 72)], "flush").finish())
    return S


def randomized(seed: int, profile: str = "oxygen-49-mkv") -> str:
    rnd = random.Random(seed)
    test_all = profile == "test-all"
    conf = dict(nsids=rnd.choice([1, 2, 2]), layout=rnd.choice(["poly", "poly", "split"]),
                mode=rnd.choice(["poly", "poly", "poly", "mono"]), hard_restart=rnd.choice([0, 0, 0, 1]),
                legato=rnd.choice([0, 1]), strict=rnd.choice([0, 1, 1]), presets=rnd.choice([0, 1, 1]),
                target=rnd.choice([0, 0, 1, 2]), vel=rnd.choice(["sustain", "sustain", "volume", "off"]),
                vamt=rnd.choice([100, 100, 50, 0]), profile=profile)
    sc = Scenario(f"random_{seed}", **conf)
    sc.ev("init"); sc.ev("flush")
    held: list[tuple[int, int]] = []
    chans = [0, 1] if conf["layout"] == "split" else [0]
    t_ms = 0
    for _ in range(250):
        r = rnd.random()
        ch = rnd.choice(chans + [rnd.randrange(16)] if rnd.random() < 0.05 else chans)
        if r < 0.30:
            n = rnd.randrange(36, 84)
            held.append((ch, n))
            sc.ev(f"on {ch} {n} {rnd.choice([0, 1, 64, 100, 127]) if rnd.random() < 0.1 else rnd.randrange(1, 128)}")
        elif r < 0.52:
            if held and rnd.random() < 0.9:
                c, n = held.pop(rnd.randrange(len(held)))
            else:
                c, n = ch, rnd.randrange(36, 84)
            sc.ev(f"off {c} {n}")
        elif r < 0.64:
            pool = ([74, 20, 64, 64, 80, 81, 82, 84, 1, 99] if test_all else
                    [1, 7, 71, 72, 73, 74, 75, 76, 64, 64, 99, 123])
            cc = rnd.choice(pool if rnd.random() < 0.97 else ([83] if test_all else [120]))
            sc.ev(f"cc {ch} {cc} {rnd.randrange(128)}")
        elif r < 0.70:
            sc.ev(f"bend {rnd.choice([ch, 2]) if test_all else ch} {rnd.randrange(-8192, 8192)}")
        elif r < 0.73:
            sc.ev(f"prog {ch} {rnd.randrange(12)}")
        elif r < 0.83:
            if test_all and rnd.random() < 0.3:
                sc.ev(f"mnote {rnd.randrange(2)} {rnd.choice([86, 86, 60])} {rnd.choice([0, 127])}")
            else:
                num = rnd.choice([10, 11, 12, 13, 40]) if test_all else rnd.randrange(16, 25)
                sc.ev(f"mcc {num} {rnd.choice([1, 2, 3, 65, 66, 67, 1, 65, 127, 63, 64, 0])}")
        elif r < 0.85:
            sc.ev(f"mbend {rnd.randrange(3)} {rnd.randrange(-8192, 8192)}")
        elif r < 0.855:
            sc.ev("panic")
        elif r < 0.86:
            sc.ev(f"velmode {rnd.randrange(3)} {rnd.choice([0, 30, 50, 100])}")
        elif r < 0.87:
            sc.ev(f"on 9 {rnd.choice([40, 40, 36])} {rnd.choice([0, 100])}")
        elif r < 0.88:
            t_ms += rnd.choice([5, 50, 200, 1000, 5000])
            sc.ev(f"clock {t_ms}")
            sc.ev("service")
        else:
            sc.ev("flush")
    return sc.finish()


def all_randomized() -> list[str]:
    return [randomized(seed) for seed in range(40)] + [randomized(seed, "test-all") for seed in range(40, 52)]


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    hand = handwritten()
    (OUT / "handwritten.txt").write_text("".join(hand))
    rand = all_randomized()
    (OUT / "randomized.txt").write_text("".join(rand))
    n_writes = sum(t.count("\nW ") for t in hand + rand)
    print(f"wrote {len(hand)} handwritten + {len(rand)} randomized scenarios, {n_writes} expected writes -> {OUT}")


if __name__ == "__main__":
    main()
