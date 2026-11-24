"""Keep complete directional report histories independently of the scrolling plot."""
import csv
import os
import shutil
import tempfile
from datetime import datetime, timezone
from pathlib import Path


from rf3_gui.app.delivery import DeliveryBook, FIELDS, COLUMNS as SUMMARY_COLUMNS

COLUMNS = (
    "timestamp_utc", "source", "direction", "transfer_id", "phase", "packet_number",
    "total_packets", "file_crc32_hex", "payload_bytes", "total_bytes", "tx_retries",
    "elapsed_s", "recent_Bps", "average_Bps", "payload_eta_s",
) + FIELDS


class ReportCapture:
    def __init__(self):
        self.streams = {}
        self.delivery = DeliveryBook()

    def record(self, direction, trace, details):
        sample = trace.points[-1]
        row = dict(details, timestamp_utc=datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
                   direction=direction, transfer_id=trace.identity, phase=trace.phase,
                   payload_bytes=sample.count, total_bytes=trace.total)
        metrics = (sample.t - trace.average_start, sample.rate,
                   trace.average_rate(sample.t), trace.eta(sample.t))
        for name, value in zip(("elapsed_s", "recent_Bps", "average_Bps", "payload_eta_s"), metrics):
            row[name] = round(value, 3) if value is not None else None
        if direction not in self.streams:
            stream = tempfile.SpooledTemporaryFile(max_size=1024 * 1024, mode="w+",
                                                  encoding="utf-8", newline="")
            csv.writer(stream).writerow(COLUMNS)
            self.streams[direction] = stream
        csv.DictWriter(self.streams[direction], COLUMNS).writerow(row)

    def save(self, direction, path):
        stream = self.streams.get(direction)
        end = stream.tell() if stream else None
        path = Path(path)
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="",
                                             dir=path.parent, delete=False) as output:
                temporary = Path(output.name)
                columns = COLUMNS if direction else SUMMARY_COLUMNS
                writer = csv.DictWriter(output, columns)
                if stream:
                    stream.seek(0)
                    shutil.copyfileobj(stream, output)
                else:
                    writer.writeheader()
                writer.writerows(self.delivery.rows(direction))
            os.replace(temporary, path)
        finally:
            if stream:
                stream.seek(end)
            if temporary is not None:
                temporary.unlink(missing_ok=True)

    def close(self, *_):
        for stream in self.streams.values():
            stream.close()
        self.streams.clear()
