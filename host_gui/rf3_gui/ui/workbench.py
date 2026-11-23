"""Task navigation and visual layout for the RF3 Workbench desktop."""
from PySide6.QtCore import QEvent, QSize, Qt
from PySide6.QtGui import QIcon
from rf3_gui.ui.field_art import FieldHeader, mark, nav_icon
from rf3_gui.ui.metrics_page import MetricsPage
from PySide6.QtWidgets import (
    QAbstractItemView, QBoxLayout, QFrame, QHBoxLayout, QLabel, QListWidget, QListWidgetItem,
    QScrollArea, QSizePolicy, QStackedWidget, QVBoxLayout, QWidget,
)

PAGE_NAMES = ("File transfer", "Live metrics", "Board files", "Text and CW", "Radio settings",
              "Connection", "Device status", "Diagnostics")

STYLE = """
QWidget { color: #292c29; font: 10pt 'Segoe UI'; }
QWidget#surface, QStackedWidget, QScrollArea, QWidget#page { background: #f5f3ed; }
QFrame#rail, QListWidget#navigation { background: #252a27; border: none; }
QLabel#brand { color: #fff5e3; font-size: 22pt; font-weight: 750; }
QLabel#product, QLabel#edition { color: #b9c2b9; font-size: 9pt; }
QLabel#session { color: #ffe1bf; font-weight: 650; }
QListWidget#navigation { color: #e6e5dd; outline: none; padding: 0; }
QListWidget#navigation::item { padding: 8px; margin: 2px 0; border: 2px solid transparent; border-radius: 7px; }
QListWidget#navigation::item:hover { background: #3b403a; }
QListWidget#navigation::item:selected { background: #ffb47f; color: #212421; font-weight: 700; }
QListWidget#navigation:focus::item:selected { border-color: #ffeacf; }
QFrame#top { background: #f5f3ed; }
QLabel#station { color: #323932; font-weight: 650; }
QLabel#metricTitle { color: #586057; font-size: 9pt; }
QLabel[visualTone] { background: #e8e9e1; color: #485046; border-radius: 4px; padding: 3px 8px; font-weight: 650; }
QLabel[visualTone="active"] { background: #dce9d7; color: #294b2c; }
QLabel[visualTone="outbound"] { background: #fbe0bc; color: #6b3b09; }
QLabel[visualTone="attention"] { background: #ffecbf; color: #60430c; }
QLabel[visualTone="fault"] { background: #fbdad1; color: #782e22; }
QLabel[visualTone="stage"] { background: #2e503c; color: white; }
QLabel#heading { color: #242824; font-family: Georgia; font-size: 31pt; background: transparent; }
QGroupBox { background: #fffdf8; border: 1px solid #d9d8ce; border-radius: 10px;
            margin: 0; padding: 32px 13px 11px; font-size: 12pt; font-weight: 700; }
QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left;
                   left: 17px; top: 10px; color: #313831; }
QGroupBox#sendCard { background: #f0f3e9; border-color: #cdd6c3; }
QPushButton, QComboBox, QSpinBox, QLineEdit { min-height: 25px; padding: 3px 8px;
    background: #fffdf8; color: #282d28; border: 1px solid #abaea2; border-radius: 6px; }
QPushButton { font-weight: 600; }
QComboBox { padding-right: 20px; }
QComboBox QAbstractItemView { background-color: #fffdf8; color: #292c29;
    alternate-background-color: #f5f3ed; selection-background-color: #2c4437;
    selection-color: #ffffff; border: 1px solid #abaea2; }
QComboBox QAbstractItemView::item { padding: 4px 8px; }
QComboBox QAbstractItemView::item:selected, QComboBox QAbstractItemView::item:hover {
    background-color: #2c4437; color: #ffffff; }
QMessageBox { background-color: #f5f3ed; color: #292c29; }
QMessageBox QLabel, QMessageBox QDialogButtonBox { background-color: transparent; color: #292c29; }
QMessageBox QTextEdit { background-color: #fffdf8; color: #292c29;
    selection-background-color: #2c4437; selection-color: #ffffff; }
QComboBox::drop-down { border: 0; width: 20px; subcontrol-position: center right; }
QPushButton:hover { background: #eeeade; border-color: #697063; }
QPushButton:pressed { background: #dddacc; }
QPushButton#primary { background: #2c4437; color: white; border-color: #2c4437; }
QPushButton#primary:hover { background: #1e3627; }
QPushButton#stop { background: #ffebe3; color: #77372a; border-color: #c79684; }
QPushButton#stop:hover { background: #f8d6c9; }
QPushButton:disabled, QPushButton#primary:disabled, QPushButton#stop:disabled,
QComboBox:disabled, QSpinBox:disabled, QLineEdit:disabled { background: #e7e8df; color: #65695f; border-color: #d0d2c7; }
QPushButton:focus, QPushButton#primary:focus, QPushButton#stop:focus, QLineEdit:focus,
QComboBox:focus, QSpinBox:focus, QPlainTextEdit:focus, QTableWidget:focus { border: 2px solid #945b28; }
QPlainTextEdit, QTableWidget { background: #fffdf8; border: 1px solid #bcbeb2; border-radius: 5px; }
QHeaderView::section { background: #e9e9df; padding: 6px; border: 0; border-right: 1px solid #cccec2; }
QProgressBar { background: #e4e7dd; color: #233322; border: 0; border-radius: 4px; text-align: center; min-height: 13px; }
QProgressBar::chunk { background: #a5c19c; border-radius: 4px; }
QStatusBar { background: #ebece4; color: #434d41; }
"""

def label(text, name):
    widget = QLabel(text)
    widget.setObjectName(name)
    widget.setTextFormat(Qt.TextFormat.PlainText)
    return widget


def page(*widgets, expanding=False):
    body = QWidget()
    body.setObjectName("page")
    layout = QVBoxLayout(body)
    layout.setContentsMargins(22, 0, 22, 18)
    layout.setSpacing(10)
    for widget in widgets:
        layout.addWidget(widget)
    if not expanding:
        layout.addStretch()
    scroll = QScrollArea()
    scroll.setFrameShape(QFrame.Shape.NoFrame)
    scroll.setWidgetResizable(True)
    scroll.setWidget(body)
    return scroll


def tone(widget, name):
    if widget.property("visualTone") != name:
        widget.setProperty("visualTone", name)
        widget.style().unpolish(widget)
        widget.style().polish(widget)
        widget.update()


class TransferPanels(QWidget):
    """Use the actual page viewport so a wide layout cannot trap a narrowing window."""
    def __init__(self, outgoing, incoming):
        super().__init__()
        self.row = QBoxLayout(QBoxLayout.Direction.TopToBottom, self)
        self.row.setContentsMargins(0, 0, 0, 0)
        self.row.setSpacing(10)
        self.row.addWidget(outgoing, 1)
        self.row.addWidget(incoming, 1)

    def eventFilter(self, watched, event):
        if event.type() == QEvent.Type.Resize:
            width_needed = sum(self.row.itemAt(i).widget().minimumSizeHint().width() for i in range(2))
            side_by_side = event.size().width() >= max(806, width_needed + 58)
            self.row.setDirection(QBoxLayout.Direction.LeftToRight if side_by_side else QBoxLayout.Direction.TopToBottom)
        return super().eventFilter(watched, event)


def header_metric(layout, title):
    column = QVBoxLayout()
    column.setSpacing(2)
    column.addWidget(label(title, "metricTitle"))
    value = label("", "metric")
    column.addWidget(value)
    layout.addLayout(column)
    return value


class ReceiveStages(QWidget):
    """Emphasize only the stage the board reported for this file."""
    phases = {"Receiving": 0, "WaitingForEnd": 0, "Verifying": 1, "Publishing": 2, "Completed": 2}

    def __init__(self):
        super().__init__()
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(4)
        self.names = ("Receive bytes", "Verify file", "Save file")
        self.labels = []
        for name in self.names:
            item = label(name, "phase")
            item.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.labels.append(item)
            layout.addWidget(item, 1)

    def show_report(self, phase):
        current = self.phases.get(phase)
        self.setVisible(current is not None)
        for index, (item, name) in enumerate(zip(self.labels, self.names)):
            active = current == index
            text = name
            if active:
                text = {"Receiving": "Receiving bytes", "WaitingForEnd": "Waiting for end",
                        "Verifying": "Verifying file", "Publishing": "Saving file",
                        "Completed": "File saved"}[phase]
            item.setText(text)
            tone(item, "stage" if active else "neutral")


def build(window):
    controls = window._build_controls_group()
    incoming = window._build_received_group()
    options = window._build_transfer_options()
    outgoing = window._build_transfer_group()
    connection = window._build_connection_group()
    telemetry = window._build_status_group()
    messages = window._build_over_air_group()
    storage = window._build_storage_page()
    diagnostics = window._build_log_group()

    window.transmit_button.setObjectName("primary")
    window.connect_button.setObjectName("primary")
    window.stop_button.setObjectName("stop")
    window.stop_button.setMinimumWidth(112)
    window.storage_table.setMinimumHeight(125)
    window.log_output.setMinimumHeight(150)

    surface = QWidget()
    surface.setObjectName("surface")
    horizontal = QHBoxLayout(surface)
    horizontal.setContentsMargins(0, 0, 0, 0)
    horizontal.setSpacing(0)

    rail = QFrame()
    rail.setObjectName("rail")
    rail.setFixedWidth(180)
    sidebar = QVBoxLayout(rail)
    sidebar.setContentsMargins(12, 18, 12, 18)
    sidebar.setSpacing(2)
    brand = QHBoxLayout()
    brand.setSpacing(9)
    emblem = QLabel()
    emblem.setPixmap(mark(36))
    brand.addWidget(emblem)
    wordmark = QVBoxLayout()
    wordmark.setSpacing(0)
    wordmark.addWidget(label("RF3", "brand"))
    wordmark.addWidget(label("Radio workbench", "product"))
    brand.addLayout(wordmark)
    brand.addStretch()
    sidebar.addLayout(brand)
    window.setWindowIcon(QIcon(mark()))
    sidebar.addSpacing(22)
    window.navigation = QListWidget()
    window.navigation.setObjectName("navigation")
    window.navigation.setAccessibleName("Workspaces")
    window.navigation.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
    window.navigation.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
    window.navigation.setIconSize(QSize(20, 20))
    for name in PAGE_NAMES:
        window.navigation.addItem(QListWidgetItem(nav_icon(name), name))
    sidebar.addWidget(window.navigation, 1)
    window.session_label = label("", "session")
    sidebar.addWidget(window.session_label)
    sidebar.addWidget(label("Field Edition", "edition"))
    horizontal.addWidget(rail)

    main = QVBoxLayout()
    main.setContentsMargins(0, 0, 0, 0)
    main.setSpacing(0)
    top = QFrame()
    top.setObjectName("top")
    bar = QHBoxLayout(top)
    bar.setContentsMargins(22, 10, 22, 10)
    bar.setSpacing(14)
    window.station = label("No board connected", "station")
    window.station.setWordWrap(True)
    window.station.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
    bar.addWidget(window.station, 1)
    window.host_state = header_metric(bar, "PC link")
    window.link_state = header_metric(bar, "Last radio report")
    bar.addWidget(window.stop_button)
    main.addWidget(top)
    banner = QHBoxLayout()
    banner.setContentsMargins(22, 0, 22, 12)
    window.page_title = FieldHeader()
    banner.addWidget(window.page_title)
    main.addLayout(banner)
    window.pages = QStackedWidget()
    transfer_panels = TransferPanels(outgoing, incoming)
    transfer_page = page(options, transfer_panels)
    transfer_page.viewport().installEventFilter(transfer_panels)
    window.metrics = MetricsPage(window._controller)
    for panel in (
        transfer_page, page(window.metrics, expanding=True), page(storage, expanding=True), page(messages, expanding=True),
        page(controls), page(connection), page(telemetry), page(diagnostics, expanding=True),
    ):
        window.pages.addWidget(panel)
    main.addWidget(window.pages, 1)
    horizontal.addLayout(main, 1)

    def select(index):
        if index >= 0:
            window.page_title.setText(PAGE_NAMES[index])
            window.pages.setCurrentIndex(index)

    window.navigation.currentRowChanged.connect(select)
    window.navigation.setCurrentRow(PAGE_NAMES.index("Connection"))
    for widget, name in (
        (window.device_combo, "Available boards"), (window.channel_spin, "Radio channel"),
        (window.power_combo, "Transmit power"), (window.cw_channel_spin, "CW channel"),
        (window.cw_power_combo, "CW power"), (window.cw_on_spin, "CW loop on time"),
        (window.cw_off_spin, "CW loop off time"), (window.file_path_edit, "Selected PC file"),
        (window.progress_bar, "File transfer progress"), (window.rx_progress, "Incoming file progress"),
        (window.storage_progress, "Board storage progress"), (window.live_text_output, "Message history"),
        (window.log_output, "Diagnostic log"), (window.storage_table, "Files on the board"),
        (window.morse_text_edit, "Morse text"), (window.remote_command_edit, "Remote command"),
        (window.live_text_edit, "Message to send"), (window.modulation_combo, "Radio modulation"),
    ):
        widget.setAccessibleName(name)
    window.setCentralWidget(surface)
    window.setStyleSheet(STYLE)
    window.statusBar().showMessage("Ready")

