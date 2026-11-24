"""Summarize observed storage and presentation activity from the startup log."""

import re


QUIET_SECONDS = 30
STORAGE = re.compile(
    r"^x2w-dstorage heartbeat \d+ s: requests done=(\d+) inflated=(\d+) failed=(\d+)(?: bcpack-blank=(\d+))?"
)
PRESENT = re.compile(r"^x2w-d3d12 heartbeat: (\d+) frames presented in [\d.]+ s \(([\d.]+) fps")
SURFACE = re.compile(r"^x2w-render presented surface .* mean level ([\d.]+)/255")


class LiveProgress:
    def __init__(self, started):
        self.started = started
        self.storage = None
        self.present = None
        self.surface = None
        self.storage_progress = None
        self.present_progress = None

    def observe(self, line, now, historical=False):
        if match := STORAGE.match(line):
            counts = tuple(int(value) if value is not None else None for value in match.groups())
            if not historical and self.storage is not None and sum(counts[:2]) > sum(self.storage[:2]):
                self.storage_progress = now
            self.storage = counts
        elif match := PRESENT.match(line):
            frame, fps = int(match[1]), match[2]
            if not historical and self.present is not None and frame > self.present[0]:
                self.present_progress = now
            self.present = frame, fps
        elif match := SURFACE.match(line):
            self.surface = match[1]

    def activity(self, now, last_progress):
        if last_progress is not None:
            elapsed = int(now - last_progress)
            return f"active, last increase {elapsed} s ago" if elapsed < QUIET_SECONDS else f"no increase for {elapsed} s"
        elapsed = int(now - self.started)
        return "observing for an increase" if elapsed < QUIET_SECONDS else f"no increase observed in {elapsed} s"

    def describe(self, now):
        if self.storage is None:
            storage = "waiting for a storage sample"
        else:
            done, inflated, failed, blank = self.storage
            detail = f"{done} plain, {inflated} compressed, {failed} failed"
            if blank:
                detail += f", {blank} blank BCPack"
            storage = f"{self.activity(now, self.storage_progress)} ({detail})"
        if self.present is None:
            present = "waiting for a presentation sample"
        else:
            frames, fps = self.present
            present = f"{self.activity(now, self.present_progress)} ({frames} frames, {fps} fps overall)"
        if self.surface is not None:
            present += f", last surface mean {self.surface}/255"
        return storage, present
