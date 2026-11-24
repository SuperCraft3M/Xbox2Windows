"""Step-over window of the startup debugger's one-byte hooks (docs/startup-runtime.md).

A hook is one patched byte per function. To run the original instruction the debugger restores the
byte, single-steps the thread that hit it and patches the byte again. A second thread that enters
the function inside that window would run the original instruction untraced, so the window holds
every other thread for the step: the owner steps alone and the others resume when it is done.

This module is the bookkeeping only (no Win32 calls); debug_launch.py passes the suspend and resume
functions. Every window has a deadline, a thread that cannot be held is counted, and the counters
are always reported so a run says how many calls it may have missed.
"""

HELD, GONE, FAILED = "held", "gone", "failed"


class StepWindow:
    def __init__(self, hold, release, protect=True, limit=2.0):
        self._hold, self._release = hold, release  # hold(tid) -> HELD | GONE | FAILED; release(tid)
        self.protect, self.limit = protect, limit
        self.owner = None
        self._held = []
        self._deadline = 0.0
        self._exposed = False
        self.windows = self.unprotected = self.deferred = self.timeouts = 0

    def open(self, owner, others, now):
        """The owner is about to step over its hook: hold every other thread first."""
        if self.owner is not None:
            raise RuntimeError("a step window is already open")
        self.windows += 1
        if not self.protect:  # measuring mode: nothing is held, every window with company is exposed
            self.unprotected += 1 if others else 0
            return
        self.owner, self._deadline, self._exposed = owner, now + self.limit, False
        for tid in others:
            self.hold(tid)

    def hold(self, tid):
        """Holds one more thread while a window is open (a thread created during the step)."""
        if self.owner is None or tid == self.owner or tid in self._held:
            return
        result = self._hold(tid)
        if result == HELD:
            self._held.append(tid)
        elif result == FAILED:
            self._expose()

    def defer(self, tid):
        """True when a hook hit comes from a held thread: it trapped before it was held, so it
        must go back to the hook address and trap again once the window is closed."""
        if self.owner is None or tid == self.owner:
            return False
        self.deferred += 1
        return True

    def finish(self, tid):
        """The owner's step is done (or the owner is gone): the held threads resume."""
        if self.owner is None or tid != self.owner:
            return False
        self._close()
        return True

    def expire(self, now):
        """A window that is still open at its deadline is closed and counted as exposed."""
        if self.owner is None or now < self._deadline:
            return False
        self.timeouts += 1
        self._expose()
        self._close()
        return True

    def abandon(self):
        """The run is over: resume whatever is held without counting a timeout."""
        self._close()

    def summary(self, entries):
        return (f"hook windows: entries={entries} windows={self.windows} unprotected={self.unprotected} "
                f"deferred={self.deferred} timeouts={self.timeouts} protection={'on' if self.protect else 'off'}")

    def _expose(self):
        if not self._exposed:
            self._exposed = True
            self.unprotected += 1

    def _close(self):
        held, self._held, self.owner = self._held, [], None
        for tid in held:
            self._release(tid)
