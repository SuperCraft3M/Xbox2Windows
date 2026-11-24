"""Check partial log writes and the independent storage and presentation progress summaries.

The debugger and the stand-in DLLs append to files.txt while the viewer polls it. A path with a
non-ASCII character can be flushed in two pieces, so one poll ends between the bytes of one UTF-8
character. The line must still show the character, whichever way the log ends its lines, including
a CRLF split between reads; invalid UTF-8 bytes must be replaced without stalling later lines. The
control is an ASCII path split the same way, which has to stay unchanged. A second run passes a
package root and checks which opens are shown: NtCreateFile and NtOpenFile, with or without the
NT object prefix, on the G: mount or under the root in any case, with spaces kept and a failed
status appended; a sibling folder that only starts with the root's name and a system path are not
shown. No GPU and no game files are involved.
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import time

VIEW = Path(__file__).resolve().parents[1] / "tools/startup/live_view.py"
sys.path.insert(0, str(VIEW.parent))
from live_progress import LiveProgress

TAIL = b" from 0x1000 -> 0x00000000"


def run_two_writes(first_bytes, second_bytes, *args):
    """Run the viewer on files.txt written in two chunks after the ready sentinel; return "file" lines."""
    with tempfile.TemporaryDirectory() as run:
        log = Path(run, "files.txt")
        log.write_bytes(first_bytes)
        with subprocess.Popen([sys.executable, "-B", str(VIEW), run, *args], stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, text=True, encoding="utf-8") as viewer:
            try:
                for line in viewer.stdout:  # the first poll has consumed first_bytes once this shows
                    if "ready" in line:
                        break
                time.sleep(0.6)  # more than one poll interval
                with log.open("ab") as handle:
                    handle.write(second_bytes)
                time.sleep(0.6)
                Path(run, "finished.txt").write_text("done", encoding="utf-8")
                lines = []
                for line in viewer.stdout:
                    if "Run ended" in line:
                        break
                    lines.append(line)
                return [line for line in lines if " file " in line]
            finally:
                viewer.kill()
                viewer.wait()


def shown_files(first, second, newline):
    """Run the viewer on a log whose last line is written in two parts; return the "file" lines."""
    return run_two_writes(
        b"1.0 NtCreateFile G:\\ready" + TAIL + newline + b"2.0 NtCreateFile G:\\" + first,
        second + TAIL + newline,
    )


def check_progress():
    activity = LiveProgress(100)
    assert activity.describe(100) == ("waiting for a storage sample", "waiting for a presentation sample")
    activity.observe("x2w-dstorage heartbeat 10 s: requests done=1 inflated=2 failed=0 bcpack-blank=0", 100, historical=True)
    activity.observe("x2w-dstorage heartbeat 20 s: requests done=3 inflated=4 failed=0 bcpack-blank=0", 100, historical=True)
    activity.observe("x2w-d3d12 heartbeat: 10 frames presented in 1.0 s (10.0 fps overall)", 100, historical=True)
    assert all("observing for an increase" in summary for summary in activity.describe(100))

    activity.observe("x2w-dstorage heartbeat 30 s: requests done=3 inflated=7 failed=0 bcpack-blank=1", 110)
    activity.observe("x2w-render presented surface 128x128 format 28, readback 40, mean level 36.00/255", 111)
    active, present = activity.describe(111)
    assert "active" in active and "(3 plain, 7 compressed, 0 failed, 1 blank BCPack)" in active, active
    assert "observing" in present and "last surface mean 36.00/255" in present, present

    # A memory heartbeat, failed request, status enqueue, or new frame cannot renew the storage activity.
    activity.observe("x2w-dstorage heartbeat 60 s: requests done=3 inflated=7 failed=0 bcpack-blank=1; commit peak=12 MiB", 138)
    activity.observe("x2w-dstorage Queue::EnqueueStatus index=0 (requests done=80 inflated=900 failed=0)", 139)
    activity.observe("x2w-dstorage heartbeat 70 s: requests done=3 inflated=7 failed=1 bcpack-blank=1", 140)
    activity.observe("x2w-d3d12 heartbeat: 30 frames presented in 3.0 s (10.0 fps overall)", 141)
    storage, presenting = activity.describe(142)
    assert "no increase for 32 s" in storage and "1 failed" in storage, storage
    assert "active" in presenting and "30 frames" in presenting, presenting
    assert "no increase for 31 s" in activity.describe(172)[1]

    activity.observe("x2w-dstorage heartbeat 90 s: requests done=4 inflated=7 failed=1 bcpack-blank=1", 175)
    assert "active" in activity.describe(176)[0]
    print("live status distinguishes observed storage progress, failed requests, quiet storage and presentations")


def check_status_console():
    with tempfile.TemporaryDirectory() as run:
        Path(run, "stderr.txt").write_text(
            "x2w-dstorage heartbeat 10 s: requests done=3 inflated=4 failed=1 bcpack-blank=1\n"
            "x2w-d3d12 heartbeat: 12 frames presented in 2.0 s (6.0 fps overall)\n",
            encoding="utf-8",
        )
        with subprocess.Popen([sys.executable, "-B", str(VIEW), "--status", run], stdout=subprocess.PIPE,
                              text=True, encoding="utf-8") as viewer:
            lines = []
            try:
                for line in viewer.stdout:
                    lines.append(line)
                    if " present " in line:
                        break
            finally:
                viewer.kill()
                viewer.wait()
    assert any("3 plain, 4 compressed, 1 failed, 1 blank BCPack" in line for line in lines), lines
    assert any("12 frames, 6.0 fps overall" in line for line in lines), lines


def main():
    check_progress()
    check_status_console()
    for newline in (b"\n", b"\r\n"):
        shown = shown_files(b"caf\xc3", b"\xa9.bin", newline)
        if len(shown) != 1 or not shown[0].rstrip().endswith("G:\\caf\u00e9.bin"):
            sys.exit(f"character split between two reads was damaged ({newline!r}): {shown!r}")
        control = shown_files(b"caf", b"e.bin", newline)
        if len(control) != 1 or not control[0].rstrip().endswith("G:\\cafe.bin"):
            sys.exit(f"plain split line changed ({newline!r}): {control!r}")

    edge = run_two_writes(
        b"1.0 NtCreateFile G:\\ready" + TAIL + b"\r\n"
        + b"2.0 NtCreateFile G:\\split-crlf.bin" + TAIL + b"\r",
        b"\n"
        + b"3.0 NtCreateFile G:\\bad\xff.bin" + TAIL + b"\r\n"
        + b"4.0 NtCreateFile G:\\after.bin" + TAIL + b"\r\n",
    )
    expected = ("G:\\split-crlf.bin", "G:\\bad\ufffd.bin", "G:\\after.bin")
    if len(edge) != len(expected) or any(
        not line.rstrip("\r\n").endswith(suffix) or "\r" in line.rstrip("\n")
        for line, suffix in zip(edge, expected)
    ):
        sys.exit(f"split CRLF or invalid UTF-8 line failed: {edge!r}")

    # The package root is matched with a trailing separator and without regard to case, so the log
    # spells it in upper case, and "<root>-other" is a sibling folder that must not be shown.
    with tempfile.TemporaryDirectory() as package:
        root = str(Path(package).absolute())
        opens = run_two_writes(
            b"1.0 NtCreateFile G:\\ready" + TAIL + b"\n",
            b"2.0 NtCreateFile \\??\\G:\\nt prefix.bin from game.exe+0x1a2b -> 0x00000000\n"
            + b"3.0 NtOpenFile G:\\missing.bin from 0x1000 -> 0xc0000034\n"
            + b"4.0 NtCreateFile " + root.upper().encode() + b"\\Data\\a.bin" + TAIL + b"\n"
            + b"5.0 NtCreateFile " + root.encode() + b"-other\\b.bin" + TAIL + b"\n"
            + b"6.0 NtCreateFile C:\\Windows\\c.dll" + TAIL + b"\n",
            root,
        )
    shown = [line.rstrip("\r\n").split(None, 2)[2] for line in opens]  # drop the time and the tag
    expected = ["G:\\nt prefix.bin", "G:\\missing.bin   (failed 0xc0000034)", root.upper() + "\\Data\\a.bin"]
    if shown != expected:
        sys.exit(f"file opens shown wrongly:\n  got      {shown!r}\n  expected {expected!r}")
    print("live view keeps split UTF-8, split CRLF, and replaces invalid UTF-8 without stalling")
    print("live view shows G:\\ and package-root opens with their status, and skips other paths")


if __name__ == "__main__":
    main()
