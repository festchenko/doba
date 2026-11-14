"""tools/doba_card.py: copies what changed, leaves builds in progress behind,
writes a manifest, and verify notices a file that does not match."""
import os
import subprocess
import sys

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "doba_card.py")


def run(*a):
    return subprocess.run([sys.executable, TOOL, *a], capture_output=True, text=True)


def test_write_then_verify(tmp_path):
    data, card = tmp_path / "data", tmp_path / "card"
    (data / "lib" / "water").mkdir(parents=True)
    (data / "lib" / "water" / "index.dti").write_bytes(b"x" * 32)
    (data / "lib" / "water" / "info.txt").write_text("name=Water\n")
    (data / "lib" / "draft.new").mkdir()
    (data / "lib" / "draft.new" / "index.dti").write_bytes(b"y")
    (data / "dem-pack").mkdir()
    (data / "dem-pack" / "N1E1.dem").write_bytes(b"z" * 10)
    card.mkdir()
    r = run("write", str(data), str(card))
    assert r.returncode == 0, r.stdout + r.stderr
    assert (card / "doba" / "lib" / "water" / "index.dti").exists()
    assert not (card / "doba" / "lib" / "draft.new").exists()
    assert (card / "doba" / "dem" / "N1E1.dem").exists()
    assert (card / "doba" / "manifest.json").exists()
    again = run("write", str(data), str(card)).stdout
    assert "books: 2 files, 0 copied" in again and "heights: 1 files, 0 copied" in again   # nothing changed
    (card / "doba" / "lib" / "water" / "index.dti").write_bytes(b"x" * 31 + b"!")
    r = run("verify", str(data), str(card))
    assert r.returncode == 1 and "DIFFERS lib/water/index.dti" in r.stdout
