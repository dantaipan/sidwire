"""The C++ parity vectors must match what the current Python engine produces.
If this fails after a deliberate engine change: .venv/bin/python tools/gen_vectors.py
(then rebuild/run the C++ tests)."""
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def test_vectors_up_to_date():
    spec = importlib.util.spec_from_file_location("gen_vectors", ROOT / "tools" / "gen_vectors.py")
    gv = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gv)
    assert (gv.OUT / "handwritten.txt").read_text() == "".join(gv.handwritten())
    assert (gv.OUT / "randomized.txt").read_text() == "".join(gv.all_randomized())
