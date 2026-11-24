"""Observed payload throughput from status samples, independent of radio bitrate."""
from collections import deque
from dataclasses import dataclass
import time


ACTIVE_STATES = {"Receiving", "WaitingForEnd", "Verifying", "Publishing"}


@dataclass(frozen=True)
class ReceiveRate:
    bytes_per_second: float | None
    elapsed_seconds: float


class ReceiveRateTracker:
    def __init__(self, window_seconds: float = 10.0):
        self.window_seconds = window_seconds
        self.reset()

    def reset(self):
        self.transfer_id = ""
        self.samples = deque(maxlen=64)
        self.started = None
        self.finished = False
        self.estimate = None

    def observe(self, transfer_id: str, state: str, received: int, now=None):
        now = time.monotonic() if now is None else now
        if not transfer_id:
            self.reset()
            return None
        if state in {"", "Idle"}:
            return self.estimate
        if transfer_id != self.transfer_id:
            self.reset()
            self.transfer_id = transfer_id
            if state not in ACTIVE_STATES:
                return None  # A historical terminal snapshot has no timing samples.
            self.started = now
        if self.finished or self.started is None:
            return self.estimate
        if self.samples and now <= self.samples[-1][0]:
            return self.estimate
        if self.samples and received < self.samples[-1][1]:
            self.samples.clear()  # Never convert a counter reset into negative throughput.
            self.started = now
        self.samples.append((now, received))
        while len(self.samples) > 2 and self.samples[1][0] <= now - self.window_seconds:
            self.samples.popleft()
        first_time, first_bytes = self.samples[0]
        span = now - first_time
        rate = (received - first_bytes) / span if span > 0 else None
        self.estimate = ReceiveRate(rate, max(0.0, now - self.started))
        if state not in ACTIVE_STATES:
            self.finished = True  # Preserve rate and duration after completion/cancellation.
        return self.estimate
