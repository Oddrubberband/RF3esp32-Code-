"""PC-observed RF file telemetry. All times are monotonic seconds."""
from collections import deque
from dataclasses import dataclass
from statistics import median

RATE_WINDOW = 6.0
GAP_SECONDS = 9.0  # Three normal 3 s status polling periods.
PLOT_SECONDS = 60.0
TERMINAL = {"Completed", "Failed", "Cancelled", "SentUnconfirmed"}
MOVING_PAYLOAD = {"Receiving", "Transferring"}


@dataclass(frozen=True)
class Sample:
    t: float
    count: int
    rate: float | None  # bytes per second


class Trace:
    def __init__(self, identity, total, start, first_count, command_start=None, *, attempt=0):
        self.identity = identity
        self.attempt = attempt
        self.total = total
        self.start = start
        self.average_start = start if command_start is None else command_start
        self.average_offset = first_count if command_start is None else 0
        self.phase = ""
        self.local_end = False
        self.points = deque(maxlen=512)

    def add(self, phase, count, now):
        if self.phase in TERMINAL:
            return False
        points = self.points
        if points and (now < points[-1].t or count < points[-1].count):
            return False
        # Consolidate bursts inside fixed 250 ms bins. Retain the first observation
        # and both sides of an acquisition gap for the derivative and the plot.
        if len(points) >= 2 and (int((now-self.start)*4) == int((points[-1].t-self.start)*4)
                                and points[-1].t - points[-2].t <= GAP_SECONDS):
            points.pop()
        if points and points[-1].t == now:
            points.pop()
        boundary = now - RATE_WINDOW
        anchor_t, anchor_count = now, count
        for old in reversed(points):
            if anchor_t - old.t > GAP_SECONDS:
                break
            if old.t <= boundary:
                fraction = (boundary-old.t) / (anchor_t-old.t)
                anchor_count = old.count + fraction * (anchor_count-old.count)
                anchor_t = boundary
                break
            anchor_t, anchor_count = old.t, old.count
        span = now-anchor_t
        rate = (count-anchor_count)/span if span >= 1.0 else None
        points.append(Sample(now, count, rate))
        while len(points) > 2 and points[1].t < now-PLOT_SECONDS:
            points.popleft()
        self.phase = phase
        return True

    def stale(self, now):
        return self.phase not in TERMINAL and now-self.points[-1].t > GAP_SECONDS

    def average_rate(self, now):
        last = self.points[-1]
        if self.local_end or self.stale(now):
            return None
        end = last.t if self.phase in TERMINAL else now
        elapsed = end - self.average_start
        return (last.count - self.average_offset) / elapsed if elapsed > 0 else None

    def eta(self, now):
        last = self.points[-1]
        if self.phase not in MOVING_PAYLOAD or last.count >= self.total:
            return None
        average = self.average_rate(now)
        if average is None or average <= 0:
            return None
        return (self.total - last.count) / average


class SessionTraces:
    def __init__(self):
        self.traces = {}
        self.latest = None
        self.tx_command_start = None

    def tx_command(self, active, now):
        self.tx_command_start = now if active else None
        if active:
            self.traces.pop("TX", None)

    def observe(self, direction, identity, phase, count, total, now, *, attempt=0):
        if direction not in {"TX", "RX"} or not identity or not 0 <= count <= total:
            return None
        trace = self.traces.get(direction)
        restarted_rx = direction == "RX" and trace is not None and (
            (attempt and attempt != trace.attempt)
            or (not attempt and trace.phase == "Failed"
                and phase in {"Receiving", "WaitingForEnd", "Verifying", "Publishing", "Completed"})
        )
        if trace is None or trace.identity != identity or restarted_rx:
            start = self.tx_command_start if direction == "TX" else None
            trace = self.traces[direction] = Trace(identity, total, now, count, start, attempt=attempt)
        elif trace.total != total:
            # An inconsistent size is not evidence of new progress.
            return None
        if not trace.add(phase, count, now):
            return None
        self.latest = direction
        return trace

    def end_observation(self, direction, identity, phase):
        trace = self.traces.get(direction)
        if (trace and trace.identity == identity and trace.phase not in TERMINAL
                and phase in {"Failed", "Cancelled", "SentUnconfirmed"}):
            trace.phase = phase
            trace.local_end = True


class StatusCadence:
    """Intervals between accepted STATUS arrivals in the current connection."""
    def __init__(self):
        self.last = None
        self.intervals = deque(maxlen=20)

    def observe(self, now):
        if self.last is not None:
            if now <= self.last:
                return
            self.intervals.append(now - self.last)
        self.last = now

    def typical(self):
        return median(self.intervals) if self.intervals else None
