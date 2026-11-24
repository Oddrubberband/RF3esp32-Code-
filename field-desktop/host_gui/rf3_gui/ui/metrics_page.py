"""Read-only RF transfer instruments for the Field Edition desktop."""
import csv
from math import ceil, floor, log10
from datetime import datetime
from time import monotonic

from PySide6.QtCore import QPointF, QRectF, Qt, QTimer
from PySide6.QtGui import QColor, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import (QFileDialog, QFrame, QGridLayout, QHBoxLayout, QLabel,
                               QMessageBox, QPushButton, QVBoxLayout, QWidget)

from rf3_gui.app.live_metrics import GAP_SECONDS, PLOT_SECONDS, TERMINAL, SessionTraces, StatusCadence
from rf3_gui.app.state_model import ConnectionState
from rf3_gui.app.report_csv import ReportCapture
from rf3_gui.ui.field_art import FieldComboBox


def scale_top(value):
    value = max(0.25, value)
    unit = 10 ** floor(log10(value))
    return next(tick * unit for tick in (1, 2, 5, 10) if tick * unit >= value)


def duration(seconds):
    if seconds is None:
        return "—"
    seconds = max(1, ceil(seconds))
    if seconds < 60:
        return f"{seconds} s"
    if seconds < 3600:
        return f"{seconds//60}m {seconds%60:02d}s"
    return f"{seconds//3600}h {seconds%3600//60:02d}m"


def speed(value):
    if value is None:
        return "—"
    return f"{value:.0f} B/s" if value < 1024 else f"{value/1024:.2f} KiB/s"


class HistoryPlot(QWidget):
    def __init__(self):
        super().__init__()
        self.setMinimumHeight(228)
        self.trace = None
        self.clock_origin = monotonic()
        self.frames = QTimer(self)
        self.frames.setTimerType(Qt.TimerType.PreciseTimer)
        self.frames.setInterval(16)
        self.frames.timeout.connect(self.update)
        self.setAccessibleName("RF payload rate and cumulative byte history")

    def set_trace(self, trace):
        self.trace = trace
        self.update()

    def showEvent(self, event):
        super().showEvent(event)
        self.frames.start()

    def hideEvent(self, event):
        self.frames.stop()
        super().hideEvent(event)

    def time_bounds(self, now):
        end = now - self.clock_origin
        return end - PLOT_SECONDS, end

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor("#141315"))
        painter.drawRoundedRect(QRectF(self.rect()).adjusted(0, 0, -1, -1), 9, 9)
        trace = self.trace
        begin, end = self.time_bounds(monotonic())
        samples = list(trace.points) if trace else []
        left, right = 50, self.width()-20
        pane_height = (self.height()-49)/2
        tick_step = 10
        ticks = [n*tick_step for n in range(ceil(begin/tick_step), floor(end/tick_step)+1)]
        for i, (title, color) in enumerate((("Payload rate  ·  KiB/s", "#f3a3b8"),
                                           ("Reported payload  ·  KiB", "#f8f6f6"))):
            y = 7 + i*pane_height
            top, bottom = y+23, y+pane_height-8
            painter.setPen(QColor(color))
            painter.drawText(QRectF(left, y, right-left, 20), Qt.AlignmentFlag.AlignLeft, title)
            readings = [(s.t - self.clock_origin, (s.rate if i == 0 else s.count), s.t) for s in samples]
            visible = [v/1024 for x, v, _ in readings if begin <= x <= end and v is not None]
            ceiling = scale_top(max(visible, default=1))
            for tick in ticks:
                xx = left + (tick-begin)/(end-begin)*(right-left)
                painter.setPen(QPen(QColor("#302d31"), 1))
                painter.drawLine(QPointF(xx, top), QPointF(xx, bottom))
            for j in range(3):
                yy = top+(bottom-top)*j/2
                painter.setPen(QPen(QColor("#464147"), 1))
                painter.drawLine(QPointF(left, yy), QPointF(right, yy))
                painter.setPen(QColor("#c6c0c5"))
                painter.drawText(QRectF(0, yy-9, left-7, 18),
                                 Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter,
                                 f"{ceiling*(1-j/2):g}")
            painter.save()
            painter.setClipRect(QRectF(left-3, top-3, right-left+6, bottom-top+6))
            path = QPainterPath()
            dots = []
            prev = None
            for x, value, timestamp in readings:
                if value is None or x > end:
                    prev = None
                    continue
                point = QPointF(left+(x-begin)/(end-begin)*(right-left),
                                bottom-(value/1024)/ceiling*(bottom-top))
                if prev is None or timestamp-prev > GAP_SECONDS:
                    path.moveTo(point)
                else:
                    path.lineTo(point)
                if x >= begin:
                    dots.append(point)
                prev = timestamp
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.setPen(QPen(QColor(color), 2))
            painter.drawPath(path)
            painter.setBrush(QColor(color))
            painter.setPen(Qt.PenStyle.NoPen)
            for point in dots:
                painter.drawEllipse(point, 2, 2)
            painter.restore()
            if i == 1:
                painter.setPen(QColor("#c6c0c5"))
                for tick in ticks:
                    if tick < 0:
                        continue
                    xx = left + (tick-begin)/(end-begin)*(right-left)
                    painter.drawText(QRectF(xx-20, bottom+2, 40, 18), Qt.AlignmentFlag.AlignCenter, f"{tick:g}")
                painter.drawText(QRectF(left, bottom+18, right-left, 20), Qt.AlignmentFlag.AlignCenter,
                                 "Seconds since opening RF3")
        painter.end()


def number(parent, column, title):
    panel = QFrame()
    panel.setObjectName("statPanel")
    box = QVBoxLayout(panel)
    box.setContentsMargins(12, 6, 12, 6)
    box.setSpacing(1)
    box.addWidget(QLabel(title))
    value = QLabel("—")
    value.setObjectName("statValue")
    box.addWidget(value)
    parent.addWidget(panel, 0, column)
    parent.setColumnStretch(column, 1)
    return value


class MetricsPage(QWidget):
    def __init__(self, controller):
        super().__init__()
        self.live = SessionTraces()
        self.cadence = StatusCadence()
        self.capture = ReportCapture()
        self.destroyed.connect(self.capture.close)
        self.online = False
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(7)
        row = QHBoxLayout()
        self.source = QLabel("Board reports")
        self.source.setObjectName("sourceLabel")
        row.addWidget(self.source)
        row.addStretch()
        self.track = FieldComboBox()
        self.track.setAccessibleName("RF transfer direction")
        self.track.addItem("Latest activity", None)
        self.track.addItem("Transmit (TX)", "TX")
        self.track.addItem("Receive (RX)", "RX")
        row.addWidget(self.track)
        self.export_buttons = {}
        for direction in ("TX", "RX"):
            button = QPushButton(f"Export {direction} CSV")
            button.setToolTip("Save all observed " + direction + " transfer reports since opening the app. "
                              "Packet number is the firmware progress counter. CRC32 is the file-level reading.")
            button.setEnabled(False)
            button.clicked.connect(lambda checked=False, d=direction: self.export_csv(d))
            row.addWidget(button)
            self.export_buttons[direction] = button
        layout.addLayout(row)
        metrics = QGridLayout()
        self.rate = number(metrics, 0, "Recent payload rate")
        self.average = number(metrics, 1, "Transfer average")
        self.payload = number(metrics, 2, "Reported payload")
        self.eta = number(metrics, 3, "Payload ETA")
        self.rate.setToolTip("Byte growth per second over up to six seconds of PC arrival times. 1 KiB = 1024 bytes.")
        self.eta.setToolTip("Remaining file bytes divided by the Transfer average. Repair passes, verification and saving can continue after all payload bytes are reported.")
        layout.addLayout(metrics)
        delivery_row = QHBoxLayout()
        self.delivery_history = FieldComboBox()
        self.delivery_history.setAccessibleName("Transmission history")
        self.delivery_history.addItem("Latest transmission", None)
        self.delivery_history.setMaximumWidth(230)
        self.delivery_history.currentIndexChanged.connect(self.refresh_delivery)
        delivery_row.addWidget(self.delivery_history)
        self.delivery_label = QLabel("Packet delivery: waiting for a transmission")
        self.delivery_label.setToolTip("Requires delivery-reporting firmware. DATA packets are counted once per sequence number. Parity repair can verify a file even when direct packets were missed.")
        self.delivery_label.setWordWrap(True)
        self.delivery_label.setTextFormat(Qt.TextFormat.PlainText)
        delivery_row.addWidget(self.delivery_label, 1)
        peer_button = QPushButton("Import report CSV")
        peer_button.setToolTip("With no observations, open a saved local report first. Import the opposite board’s report to match transmissions.")
        peer_button.clicked.connect(self.import_peer)
        delivery_row.addWidget(peer_button)
        self.summary_button = QPushButton("Export summary")
        self.summary_button.setEnabled(False)
        self.summary_button.clicked.connect(lambda: self.export_csv(None))
        delivery_row.addWidget(self.summary_button)
        layout.addLayout(delivery_row)
        self.plot = HistoryPlot()
        layout.addWidget(self.plot, 1)
        self.detail = QLabel()
        self.detail.setWordWrap(True)
        self.detail.setTextFormat(Qt.TextFormat.PlainText)
        layout.addWidget(self.detail)
        self.status_timing = QLabel()
        self.status_timing.setWordWrap(True)
        self.status_timing.setToolTip("Intervals between up to 21 accepted board STATUS reports, measured on the PC. Some host commands suspend polling.")
        layout.addWidget(self.status_timing)
        self.setStyleSheet("QFrame#statPanel {background:#272529; border-radius:7px;} "
                           "QLabel#statValue {font-size:17pt; font-weight:650; color:#ffffff;} "
                           "QLabel#sourceLabel {font-weight:700;}")
        controller.delivery_observed.connect(self.delivery_update)
        controller.rf_sample.connect(self.observe)
        controller.rf_report_details.connect(self.record_report)
        controller.rf_tx_command_active.connect(self.tx_command)
        controller.rf_ended.connect(self.end_observation)
        controller.fresh_status_report.connect(self.observe_status)
        controller.status_changed.connect(self.on_status)
        self.track.currentIndexChanged.connect(self.refresh)
        self.track.currentIndexChanged.connect(lambda: self.delivery_history.setCurrentIndex(0))
        self.track.currentIndexChanged.connect(self.refresh_delivery)
        self.timer = QTimer(self)
        self.timer.setInterval(250)
        self.timer.timeout.connect(self.refresh)
        self.refresh()

    def observe(self, direction, identity, phase, count, total):
        self.live.observe(direction, identity, phase, count, total, monotonic())

    def tx_command(self, active):
        self.live.tx_command(active, monotonic())

    def record_report(self, direction, identity, details):
        trace = self.live.traces.get(direction)
        if trace is None or trace.identity != identity:
            return
        self.capture.record(direction, trace, details)
        self.export_buttons[direction].setEnabled(True)

    def delivery_update(self, row):
        book = self.capture.delivery
        new = row.key not in book.local
        book.observe(row)
        if new:
            self.delivery_history.addItem(f"{row.board} {row.direction} · {row.mode} · {row.id}", "|".join(row.key))
        self.last_delivery = row.direction
        self.export_buttons[row.direction].setEnabled(True)
        self.summary_button.setEnabled(True)
        self.refresh_delivery()

    def import_peer(self):
        path, _ = QFileDialog.getOpenFileName(self, "Import RF3 delivery CSV", "", "CSV files (*.csv)")
        if not path:
            return
        restore = not self.capture.delivery.local
        try:
            n = self.capture.delivery.import_csv(path)
        except (ValueError, KeyError, OSError, csv.Error) as exc:
            QMessageBox.warning(self, "Import failed", str(exc))
            return
        if restore:
            for row in self.capture.delivery.local.values():
                self.delivery_history.addItem(f"{row.board} {row.direction} · {row.mode} · {row.id}", "|".join(row.key))
                self.export_buttons[row.direction].setEnabled(True)
                self.last_delivery = row.direction
            self.summary_button.setEnabled(True)
        self.refresh_delivery()
        matches = sum(self.capture.delivery.partner(r) is not None for r in self.capture.delivery.local.values())
        QMessageBox.information(self, "Peer observations", f"Read {n} endpoint summaries. {matches} local transmissions have a matching peer.")

    def refresh_delivery(self):
        direction = self.track.currentData() or getattr(self, "last_delivery", None)
        book = self.capture.delivery
        selected = self.delivery_history.currentData()
        key = tuple(selected.split("|")) if selected else book.latest.get(direction)
        if not key:
            self.delivery_label.setText("Packet delivery: waiting for a transmission")
            return
        row = book.summary(book.local[key])
        parts = [row["link_direction"], row["mode"], row["transfer_id"]]
        if row["delivery_pct"] is not None:
            parts.append(f'{row["unique_data_received"]}/{row["distinct_data_sent"]} delivered ({row["delivery_pct"]:g}%)')
            parts.append(f'Loss {row["loss_pct"]:g}%')
        else:
            if row["observation_closed"] and row["local_sent"] is not None: parts.append(f'{row["local_sent"]} sent')
            if row["unique_data_received"] is not None: parts.append(f'{row["unique_data_received"]} received')
            parts.append("Delivery rate unknown")
        if row["first_pass_missing"] is not None:
            parts.append(f'{row["first_pass_missing"]} missing on first pass; {row["recovered_later"]} received later')
        parts.append(row["result"])
        if row["crc_result"] == "passed": parts.append("CRC passed")
        if row["receiver_result"] and row["receiver_result"] != row["result"]:
            parts.append(f'RX {row["receiver_result"]}')
        self.delivery_label.setText("  ·  ".join(parts))

    def export_csv(self, direction):
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        name = direction or "Transmission-Summary"
        path, _ = QFileDialog.getSaveFileName(self, f"Export {name} reports",
                                              f"RF3-{name}-{stamp}.csv", "CSV files (*.csv)")
        if path:
            try:
                self.capture.save(direction, path)
            except OSError as error:
                QMessageBox.warning(self, "CSV export failed", str(error))

    def observe_status(self):
        self.cadence.observe(monotonic())

    def end_observation(self, direction, identity, phase):
        self.live.end_observation(direction, identity, phase)

    def on_status(self, snapshot):
        connected = snapshot.connection_state in {ConnectionState.CONNECTED, ConnectionState.BUSY}
        if not connected and (self.live.traces or self.live.pending_tx_start is not None or self.cadence.last is not None):
            self.live = SessionTraces()
            self.cadence = StatusCadence()
        if self.online and not connected:
            self.capture.delivery.disconnect()
        self.online = connected
        self.refresh_delivery()

    def showEvent(self, event):
        super().showEvent(event)
        self.timer.start()
        self.refresh()

    def hideEvent(self, event):
        self.timer.stop()
        super().hideEvent(event)

    def refresh(self, *_):
        now = monotonic()
        direction = self.track.currentData() or self.live.latest
        trace = self.live.traces.get(direction)
        self.source.setText(f"Board reports  ·  {direction}" if direction else "Board reports")
        self.plot.set_trace(trace)
        self.average.setText(speed(trace.average_rate(now) if trace else None))
        self.average.setToolTip("Average since the first observed report. Earlier bytes are excluded."
                                if trace and trace.average_offset else
                                "Payload bytes divided by elapsed transfer time, including waits. For RX, timing starts with the first transfer report. The final report freezes the average.")
        cadence = self.cadence
        timing = "Awaiting board STATUS"
        if cadence.last is not None:
            timing = f"Last STATUS {max(0, now-cadence.last):.1f} s ago"
            if cadence.intervals:
                timing += (f"   ·   Median interval {cadence.typical():.2f} s"
                           f"   ·   Longest recent gap {max(cadence.intervals):.2f} s")
        self.status_timing.setText(timing)
        if trace is None:
            self.rate.setText("—")
            self.payload.setText("—")
            self.eta.setText("—")
            self.detail.setText("Waiting for RF file reports." if self.online else
                                "Connect a board and start an RF file transfer to collect live reports.")
            return
        last = trace.points[-1]
        stale = trace.stale(now)
        self.rate.setText(speed(last.rate if not stale and not trace.local_end else None))
        self.payload.setText(f"{last.count/1024:,.1f} KiB")
        self.eta.setText(duration(trace.eta(now)))
        phase = {"SentUnconfirmed": "Sent · delivery unconfirmed", "WaitingForEnd": "Waiting for end"}.get(trace.phase, trace.phase)
        age = max(0, now-last.t)
        timing = ("PC ended observation · outcome unconfirmed" if trace.local_end else
                  "Final report" if trace.phase in TERMINAL else
                  f"Reports paused · last report {age:.0f} s ago" if stale else f"Last report {age:.1f} s ago")
        self.detail.setText(f"{phase}  ·  {last.count:,} / {trace.total:,} bytes  ·  {timing}")
