from __future__ import annotations

import logging
import os
import uuid
from pathlib import Path
from logging.handlers import RotatingFileHandler


_log_path: Path | None = None


def configure_logging() -> Path | None:
    global _log_path
    root = logging.getLogger()
    # Repeated setup in one process must not duplicate records or file handles.
    if any(getattr(handler, "_rf3_handler", False) for handler in root.handlers):
        return _log_path
    root.setLevel(logging.DEBUG)
    formatter = logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s")
    console = logging.StreamHandler()
    console.setLevel(logging.INFO)
    console.setFormatter(formatter)
    console._rf3_handler = True
    root.addHandler(console)
    try:
        directory = Path(os.environ.get("RF3_LOG_DIR") or
                         Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "RF3" / "logs")
        directory.mkdir(parents=True, exist_ok=True)
        # Each process owns its rotation files, including when two windows use
        # the same explicitly configured directory. UUID also avoids PID reuse.
        filename = f"connection-{os.getpid()}-{uuid.uuid4().hex[:8]}.log"
        handler = RotatingFileHandler(directory / filename, maxBytes=1_000_000,
                                      backupCount=2, encoding="utf-8")
        handler.setLevel(logging.DEBUG)
        handler.setFormatter(formatter)
        handler._rf3_handler = True
        root.addHandler(handler)
        _log_path = Path(handler.baseFilename)
        logging.getLogger(__name__).info("Diagnostic log: %s", handler.baseFilename)
    except OSError as exc:
        logging.getLogger(__name__).warning("Could not create diagnostic log: %s", exc)
        _log_path = None
    return _log_path
