"""Time-to-byte replay of two recorded, active RF3 sender histories."""
from bisect import bisect_left, bisect_right
from csv import DictReader
from dataclasses import dataclass
from functools import lru_cache
from math import floor
from pathlib import Path

DATA_BYTES = 20  # Protocol v2 fixed DATA payload capacity.


@dataclass(frozen=True)
class TimingProfile:
    no_ack: bool
    times: tuple[int, ...]  # milliseconds since the first active STATUS
    counts: tuple[int, ...]  # payload bytes above the first active count

    @property
    def frame_seconds(self):
        return self.times[-1] / 1000 * DATA_BYTES / self.counts[-1]

    @property
    def data_start(self):
        # Control exchanges aren't individually timestamped by the 3 s polls.
        # Approximate one ACK exchange by an average DATA cycle. For No ACK,
        # START has its own 250 ms guard; the DATA cycle contains a 10 ms gap.
        if self.no_ack:
            return 0.250 + max(0.0, self.frame_seconds - 0.010)
        return self.frame_seconds

    def data_end(self, size: int):
        if size <= 0:
            return self.data_start
        target = ((size + DATA_BYTES - 1) // DATA_BYTES) * DATA_BYTES
        cycles, remainder = divmod(target, self.counts[-1])
        ms = cycles * self.times[-1]
        if remainder:
            hi = bisect_left(self.counts, remainder)
            lo = hi - 1
            fraction = (remainder-self.counts[lo]) / (self.counts[hi]-self.counts[lo])
            ms += self.times[lo] + fraction * (self.times[hi]-self.times[lo])
        return self.data_start + ms/1000

    def elapsed_for_size(self, size: int):
        # END has no preceding DATA gap for an empty one-way file.
        tail = self.frame_seconds
        if self.no_ack and size == 0:
            tail = max(0.0, tail - 0.010)
        return self.data_end(size) + tail

    def bytes_at(self, size: int, elapsed: float):
        if size <= 0 or elapsed <= self.data_start:
            return 0
        if elapsed >= self.data_end(size):
            return size
        ms = (elapsed-self.data_start)*1000
        cycles, remainder = divmod(ms, self.times[-1])
        hi = bisect_right(self.times, remainder)
        lo = hi - 1
        fraction = (remainder-self.times[lo]) / (self.times[hi]-self.times[lo])
        count = self.counts[lo] + fraction * (self.counts[hi]-self.counts[lo])
        # Intermediate progress advances only in complete DATA frames.
        frames = floor((cycles*self.counts[-1] + count)/DATA_BYTES + 1e-9)
        return min(size, frames*DATA_BYTES)


@lru_cache(maxsize=2)
def timing_profile(no_ack: bool) -> TimingProfile:
    mode = "no_ack" if no_ack else "reliable"
    with Path(__file__).with_name("tx_profiles.csv").open(encoding="ascii", newline="") as stream:
        points = [(int(row["elapsed_ms"]), int(row["payload_bytes"]))
                  for row in DictReader(stream) if row["mode"] == mode]
    if (len(points) < 2 or points[0] != (0, 0) or points[-1][1] <= 0
            or any(t1 <= t0 or b1 < b0 or b1 % DATA_BYTES
                   for (t0,b0), (t1,b1) in zip(points, points[1:]))):
        raise ValueError(f"Invalid RF3 timing capture for {mode}")
    times, counts = zip(*points)
    return TimingProfile(no_ack, times, counts)
