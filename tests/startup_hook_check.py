"""Check that the startup debugger's function hooks see every concurrent call (#1281).

usage: startup_hook_check.py                              the step-window bookkeeping
       startup_hook_check.py <startup_launch_probe.exe>  two threads through a hooked function

A hook is one patched byte. While one thread steps over it the original byte is back, so a second
thread entering the function at that moment ran untraced. tools/startup/hook_window.py holds the
other threads for the step. The first form checks that bookkeeping with fake threads and needs no
Windows. The second runs the source-built probe, whose two threads open one file 32 times each,
entering CreateFileW together, under tools/startup/debug_launch.py: every one of the 64 opens must
appear in files.txt with its result, and the run reports no unprotected window. Control:
--suspend-others 0 must lose opens and say so.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

TOOLS = Path(__file__).resolve().parents[1] / "tools/startup"
sys.path.insert(0, str(TOOLS))
from hook_window import FAILED, GONE, HELD, StepWindow  # noqa: E402

NAME = "x2w_hook_overlap.dat"
OPENS = 64


def fakes(results=None):
    log = {"held": [], "released": []}
    results = results or {}

    def hold(tid):
        result = results.get(tid, HELD)
        if result == HELD:
            log["held"].append(tid)
        return result

    return log, hold, lambda tid: log["released"].append(tid)


def bookkeeping():
    # Every other thread is held before the step and resumed when the owner finishes; the owner never is.
    log, hold, release = fakes()
    window = StepWindow(hold, release)
    window.open(1, [1, 2, 3], now=0)
    assert log["held"] == [2, 3] and not window.defer(1) and window.defer(2) and window.deferred == 1
    assert not window.finish(2) and log["released"] == [], "only the owner closes a window"
    assert window.finish(1) and sorted(log["released"]) == [2, 3] and not window.defer(2)
    assert window.unprotected == 0 and window.timeouts == 0
    window.open(2, [1], now=1)  # a closed window can be opened again
    window.abandon()

    # A thread created during the step is held too; one that never joins the window is not.
    log, hold, release = fakes()
    window = StepWindow(hold, release)
    window.hold(9)
    assert log["held"] == []
    window.open(1, [2], now=0)
    window.hold(9)
    window.hold(9)
    window.hold(1)
    assert log["held"] == [2, 9]
    window.finish(1)
    assert sorted(log["released"]) == [2, 9]

    # A thread that is gone needs nothing; one that cannot be held leaves the window unprotected.
    log, hold, release = fakes({2: GONE, 3: FAILED})
    window = StepWindow(hold, release)
    window.open(1, [2, 3, 4], now=0)
    assert log["held"] == [4] and window.unprotected == 1
    window.finish(1)
    assert log["released"] == [4]

    # A window that outlives its limit is closed, resumed and counted, once.
    log, hold, release = fakes()
    window = StepWindow(hold, release, limit=2.0)
    window.open(1, [2], now=10)
    assert not window.expire(11.9) and log["released"] == []
    assert window.expire(12.0) and log["released"] == [2]
    assert window.timeouts == 1 and window.unprotected == 1
    assert not window.expire(99) and not window.finish(1) and window.timeouts == 1

    # With protection off nothing is held or deferred, and a window with company is reported.
    log, hold, release = fakes()
    window = StepWindow(hold, release, protect=False)
    window.open(1, [2], now=0)
    assert log["held"] == [] and not window.defer(2) and window.unprotected == 1
    window.finish(1)
    window.open(1, [], now=0)
    assert window.windows == 2 and window.unprotected == 1

    # A second window while one is open is a bug in the caller, not a silent overwrite.
    window = StepWindow(hold, release)
    window.open(1, [], now=0)
    try:
        window.open(2, [], now=0)
    except RuntimeError:
        pass
    else:
        raise AssertionError("a second window was opened")

    # The summary is always there, with zeros.
    line = StepWindow(hold, release).summary(0)
    assert line == "hook windows: entries=0 windows=0 unprotected=0 deferred=0 timeouts=0 protection=on", line


def run(probe, scratch, name, options):
    logdir = scratch / name
    data = scratch / NAME
    data.write_bytes(b"x2w")
    env = {k: v for k, v in os.environ.items() if not k.startswith("X2W_")}
    env.update(X2W_LAUNCH_PROBE_MODE="overlap", X2W_LAUNCH_PROBE_FILE=str(data))
    command = [sys.executable, "-B", str(TOOLS / "debug_launch.py"), str(probe), str(scratch), str(logdir),
               "--timeout", "60", "--console", "0", "--trace-files", "1", "--continue-asserts", "0", *options]
    result = subprocess.run(command, capture_output=True, text=True, timeout=180, env=env)
    files = (logdir / "files.txt").read_text(encoding="utf-8", errors="replace")
    traced = sum(1 for line in files.splitlines()
                 if " NtCreateFile " in line and NAME in line.lower() and line.endswith("-> 0x00000000"))
    stdout = (logdir / "stdout.txt").read_text(encoding="utf-8", errors="replace")
    summary = re.search(r"hook windows: entries=\d+ windows=(\d+) unprotected=(\d+) deferred=\d+ timeouts=(\d+) "
                        r"protection=(on|off)", result.stdout + files)
    assert summary, result.stdout
    return traced, stdout, [int(summary.group(i)) for i in (1, 2, 3)], summary.group(4)


def launcher(probe, scratch):
    for attempt in range(3):
        traced, stdout, (windows, unprotected, timeouts), protection = run(probe, scratch, f"held{attempt}", [])
        assert "overlap opens 64 of 64" in stdout, stdout
        assert traced == OPENS, f"attempt {attempt}: {traced} of {OPENS} opens traced"
        assert windows >= OPENS and unprotected == 0 and timeouts == 0 and protection == "on", (windows, unprotected, timeouts)
    # Control: without holding the other thread the same run loses opens, and the report says so.
    for attempt in range(8):
        traced, stdout, (windows, unprotected, timeouts), protection = run(probe, scratch, f"free{attempt}",
                                                                          ["--suspend-others", "0"])
        assert "overlap opens 64 of 64" in stdout, stdout
        assert protection == "off"
        if traced < OPENS:
            assert unprotected > 0, "opens were lost but the run reported no unprotected window"
            return traced
    raise AssertionError(f"the control never lost an open in 8 runs: the overlap was not exercised ({traced} of {OPENS})")


if __name__ == "__main__":
    bookkeeping()
    if len(sys.argv) == 1:
        print("startup_hook_window: PASS (hold, defer, finish, new thread, gone/failed thread, deadline, protection off, summary)")
        sys.exit(0)
    with tempfile.TemporaryDirectory() as temporary:
        lost = launcher(Path(sys.argv[1]).resolve(), Path(temporary))
    print(f"startup_hook_overlap: PASS ({OPENS} of {OPENS} opens traced with the window; control traced {lost})")
