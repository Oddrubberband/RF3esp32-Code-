from __future__ import annotations

import sys
from argparse import ArgumentParser

from PySide6.QtWidgets import QApplication

from rf3_gui.app.controller import AppController
from rf3_gui.device.mock_device import MockDeviceBackend
from rf3_gui.device.serial_device import SerialDeviceBackend
from rf3_gui.logging_config import configure_logging
from rf3_gui.ui.main_window import MainWindow


def _parse_args(argv: list[str]) -> tuple[str, str | None, list[str]]:
    parser = ArgumentParser(description="RF3 desktop radio control GUI")
    parser.add_argument(
        "--backend",
        choices=("mock", "serial"),
        default="serial",
        help="Device backend to use. The serial backend is the default.",
    )
    parser.add_argument(
        "--port",
        help="Preferred serial port, for example COM5. Used with --backend serial.",
    )
    args, qt_args = parser.parse_known_args(argv[1:])
    return args.backend, args.port, [argv[0], *qt_args]


def main() -> int:
    backend_name, port, qt_argv = _parse_args(sys.argv)
    log_path = configure_logging()
    app = QApplication(qt_argv)
    app.setApplicationName("RF3 Desktop")

    backend = (
        SerialDeviceBackend(preferred_port=port)
        if backend_name == "serial"
        else MockDeviceBackend()
    )
    controller = AppController(backend)
    window = MainWindow(controller)
    window.log_location_label.setText(f"Diagnostic log: {log_path or 'unavailable'}")
    window.resize(1080, 720)
    window.show()

    return app.exec()
