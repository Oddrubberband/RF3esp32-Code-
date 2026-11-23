"""Read-only RF transfer instruments for the Field Edition desktop."""
from math import ceil, floor, log10
from time import monotonic

from PySide6.QtCore import QPointF, QRectF, Qt, QTimer
from PySide6.QtGui import QColor, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import QFrame, QGridLayout, QHBoxLayout, QLabel, QVBoxLayout, QWidget

from rf3_gui.app.live_metrics import GAP_SECONDS, PLOT_SECONDS, TERMINAL, SessionTraces, StatusCadence
from rf3_gui.app.state_model import ConnectionState
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
        painter.setBrush(QColor("#fffdf7"))
        painter.drawRoundedRect(QRectF(self.rect()).adjusted(0, 0, -1, -1), 9, 9)
        trace = self.trace
        begin, end = self.time_bounds(monotonic())
        samples = list(trace.points) if trace else []
        left, right = 50, self.width()-20
        pane_height = (self.height()-49)/2
        tick_step = 10
        ticks = [n*tick_step for n in range(ceil(begin/tick_step), floor(end/tick_step)+1)]
        for i, (title, color) in enumerate((("Payload rate  ·  KiB/s", "#236454"),
                                           ("Reported payload  ·  KiB", "#a14a22"))):
            y = 7 + i*pane_height
            top, bottom = y+23, y+pane_height-8
            painter.setPen(QColor(color))
            painter.drawText(QRectF(left, y, right-left, 20), Qt.AlignmentFlag.AlignLeft, title)
            readings = [(s.t - self.clock_origin, (s.rate if i == 0 else s.count), s.t) for s in samples]
            visible = [v/1024 for x, v, _ in readings if begin <= x <= end and v is not None]
            ceiling = scale_top(max(visible, default=1))
            for tick in ticks:
                xx = left + (tick-begin)/(end-begin)*(right-left)
                painter.setPen(QPen(QColor("#edf0e6"), 1))
                painter.drawLine(QPointF(xx, top), QPointF(xx, bottom))
            for j in range(3):
                yy = top+(bottom-top)*j/2
                painter.setPen(QPen(QColor("#ddded3"), 1))
                painter.drawLine(QPointF(left, yy), QPointF(right, yy))
                painter.setPen(QColor("#565e54"))
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
                painter.setPen(QColor("#565e54"))
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
        layout.addLayout(row)
        metrics = QGridLayout()
        self.rate = number(metrics, 0, "Recent payload rate")
        self.average = number(metrics, 1, "Transfer average")
        self.payload = number(metrics, 2, "Reported payload")
        self.eta = number(metrics, 3, "Payload ETA")
        self.rate.setToolTip("Byte growth per second over up to six seconds of PC arrival times. 1 KiB = 1024 bytes.")
        self.eta.setToolTip("Estimated time to move the remaining payload at the recent rate. Verification and saving take additional time.")
        layout.addLayout(metrics)
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
        self.setStyleSheet("QFrame#statPanel {background:#e7ecdf; border-radius:7px;} "
                           "QLabel#statValue {font-size:17pt; font-weight:650; color:#294337;} "
                           "QLabel#sourceLabel {font-weight:700;}")
        controller.rf_sample.connect(self.observe)
        controller.rf_tx_command_active.connect(self.tx_command)
        controller.rf_ended.connect(self.end_observation)
        controller.fresh_status_report.connect(self.observe_status)
        controller.status_changed.connect(self.on_status)
        self.track.currentIndexChanged.connect(self.refresh)
        self.timer = QTimer(self)
        self.timer.setInterval(250)
        self.timer.timeout.connect(self.refresh)
        self.refresh()

    def observe(self, direction, identity, phase, count, total):
        self.live.observe(direction, identity, phase, count, total, monotonic())

    def tx_command(self, active):
        self.live.tx_command(active, monotonic())

    def observe_status(self):
        self.cadence.observe(monotonic())

    def end_observation(self, direction, identity, phase):
        self.live.end_observation(direction, identity, phase)

    def on_status(self, snapshot):
        connected = snapshot.connection_state in {ConnectionState.CONNECTED, ConnectionState.BUSY}
        if not connected and (self.live.traces or self.live.pending_tx_start is not None or self.cadence.last is not None):
            self.live = SessionTraces()
            self.cadence = StatusCadence()
        self.online = connected

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
