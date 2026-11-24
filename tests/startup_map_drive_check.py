"""Check the launcher's --map-drive option (#1158).

usage: startup_map_drive_check.py <consumer.exe> <debug_launch.py> <scratch dir>

A console executable opens its package at a fixed drive letter. The option rewrites an open on a
mapped letter to a real folder, in the caller's own path structure, so the package can live on any
volume.

1. Mapped letter: the consumer opens a file through a letter that is not a real volume and reads it,
   and the trace shows the rewrite.
2. Negative control: without the option the same open fails.
3. Unmapped letter: with the option, an open on a letter that is not mapped is left alone.
"""
import os
import shutil
import subprocess
import sys

consumer, launcher, scratch = sys.argv[1:4]
python = sys.executable
failures = []


def free_letter():
    """A letter that is not a mounted volume, so only the rewrite can make the open succeed."""
    for letter in "ZYXWVUTSRQPONMLKJIHG":
        if not os.path.exists(letter + ":\\"):
            return letter
    return None


def run(letter, map_spec, log_name, mode=""):
    log = os.path.join(scratch, log_name)
    shutil.rmtree(log, ignore_errors=True)
    env = dict(os.environ, X2W_PROBE_DRIVE=letter)
    if mode:
        env["X2W_PROBE_MODE"] = mode
    command = [python, "-B", launcher, consumer, scratch, log, "--timeout", "15", "--memory-gib", "2",
               "--trace-files", "1", "--continue-asserts", "0"]
    if map_spec:
        command += ["--map-drive", map_spec]
    subprocess.run(command, capture_output=True, text=True, env=env, timeout=120)
    stdout_path = os.path.join(log, "stdout.txt")
    files_path = os.path.join(log, "files.txt")
    stdout = open(stdout_path, encoding="utf-8", errors="replace").read() if os.path.exists(stdout_path) else ""
    trace = open(files_path, encoding="utf-8", errors="replace").read() if os.path.exists(files_path) else ""
    return stdout, trace


letter = free_letter()
if not letter:
    print("no free drive letter to map; skipping")
    sys.exit(77)

package = os.path.join(scratch, "map_drive_pkg")
shutil.rmtree(package, ignore_errors=True)
os.makedirs(package)
with open(os.path.join(package, "map_drive_probe.txt"), "w", encoding="ascii", newline="") as handle:
    handle.write("mapped ok")

# The launcher exits 0 whether or not the child succeeded, so the child's own output is the signal.
stdout, trace = run(letter, f"{letter}={package}", "map_on")
ok = "mapped ok" in stdout and "MAP " in trace and "map_drive_pkg" in trace
print(f"mapped letter {letter}: {'ok' if ok else 'MISMATCH'} ({stdout.strip()!r})")
if not ok:
    failures.append(f"mapped letter: stdout {stdout.strip()!r}, trace has MAP: {'MAP ' in trace}")

stdout, trace = run(letter, "", "map_off")
ok = "open failed" in stdout and "mapped ok" not in stdout
print(f"negative control without the option: {'ok' if ok else 'MISMATCH'} ({stdout.strip()!r})")
if not ok:
    failures.append(f"negative control did not fail as expected: stdout {stdout.strip()!r}")

stdout, trace = run("C", f"{letter}={package}", "map_unmapped")
ok = "MAP " not in trace
print(f"unmapped letter C: {'ok' if ok else 'MISMATCH'}")
if not ok:
    failures.append("an open on an unmapped letter was rewritten")

# One caller-owned UNICODE_STRING through many opens: the borrowed name has to be given back after
# every return, and the backing allocation must be reused rather than grown per open.
stdout, trace = run(letter, f"{letter}={package}", "map_reuse", mode="reuse")
name_buffers = trace.count("NAMEBUF alloc")
ok = "reuse ok" in stdout and name_buffers <= 1 and trace.count("MAP ") >= 32
print(f"reused caller name over 32 opens: {'ok' if ok else 'MISMATCH'} "
      f"(name allocations={name_buffers}, rewrites={trace.count('MAP ')})")
if not ok:
    failures.append(f"reuse: stdout {stdout.strip()!r}, name allocations {name_buffers}, "
                    f"rewrites {trace.count('MAP ')}")

shutil.rmtree(package, ignore_errors=True)
if failures:
    print("FAIL")
    for text in failures:
        print("  " + text)
    sys.exit(1)
print("PASS")