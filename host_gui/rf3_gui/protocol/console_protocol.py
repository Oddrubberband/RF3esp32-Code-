from __future__ import annotations

import hashlib
import re
from pathlib import Path


def valid_storage_name(name: str) -> bool:
    if not isinstance(name, str):
        return False
    try:
        size = len(name.encode("utf-8"))
    except UnicodeError:
        return False
    return (0 < size <= 30 and not name.startswith(".") and ".." not in name
            and not name.lower().endswith(".part")
            and not any(ch in "/\\" or ord(ch) < 32 or ord(ch) == 127 for ch in name))


def storage_list_command(offset: int = 0) -> str:
    return f"STORAGE LIST {offset}"


def storage_delete_command(name: str) -> str:
    if not valid_storage_name(name):
        raise ValueError("Invalid SPIFFS file name")
    return "STORAGE DELETE " + name.encode("utf-8").hex().upper()


def download_begin_hex_command(name: str, chunk_bytes: int = 64) -> str:
    if not valid_storage_name(name):
        raise ValueError("Invalid SPIFFS file name")
    return "DOWNLOAD BEGINHEX " + name.encode("utf-8").hex().upper() + _chunk_argument(chunk_bytes)


def import_filename(name: str) -> str:
    sanitized = re.sub(r"[^A-Za-z0-9._-]", "_", name)
    sanitized = sanitized.strip(".") or "payload.bin"
    while ".." in sanitized:
        sanitized = sanitized.replace("..", "__")
    if sanitized.lower().endswith(".part"):
        sanitized += ".bin"
    if sanitized == name and len(sanitized.encode("ascii")) <= 30:
        return sanitized
    tag = hashlib.sha256(name.encode("utf-8")).hexdigest()[:8]
    suffix = Path(sanitized).suffix
    if len(suffix) > 20:
        raise ValueError("File extension is too long to preserve in a SPIFFS name")
    stem = Path(sanitized).stem[:30 - len(suffix) - len(tag) - 1] or "payload"
    return f"{stem}_{tag}{suffix}"[:30]


def upload_extension(name: str) -> str:
    """Host-safe suffix; reject a type change before normalizing the stem."""
    suffix = Path(name).suffix
    if not suffix:
        return ""
    if (not re.fullmatch(r"\.[A-Za-z0-9_-]+", suffix)
            or suffix.lower() == ".part"):
        raise ValueError("File type cannot be preserved: use an extension with letters, digits, '_' or '-'. The .part extension is reserved.")
    return suffix


def transfer_extension(name: str) -> str:
    """The optional RF START field holds ten bytes, including the dot."""
    suffix = upload_extension(name)
    if len(suffix) > 10:
        raise ValueError("RF file type metadata supports an extension of up to 10 characters including the dot. Host-only import/export remains available.")
    return suffix


def status_command() -> str:
    return "STATUS"


def stop_command() -> str:
    return "STOP"


def standby_command() -> str:
    return "STANDBY"


def rx_command() -> str:
    return "RX"


def sleep_command() -> str:
    return "SLEEP"


def wake_command() -> str:
    return "WAKE"


def power_down_command() -> str:
    return "POWERDOWN"


def channel_command(channel: int) -> str:
    return f"CHANNEL {channel}"


def power_command(level: int) -> str:
    return f"POWER {level}"


def tx_command(filename: str | None = None) -> str:
    if filename:
        return f"TX {filename}"
    return "TX"


def _chunk_argument(chunk_bytes: int) -> str:
    if type(chunk_bytes) is not int or not 64 <= chunk_bytes <= 256:
        raise ValueError("Host chunk size must be between 64 and 256 bytes")
    return f" {chunk_bytes}" if chunk_bytes != 64 else ""


def upload_begin_command(filename: str, size: int, crc32: int, chunk_bytes: int = 64) -> str:
    return f"UPLOAD BEGIN {filename} {size} {crc32:08X}" + _chunk_argument(chunk_bytes)


def upload_data_command(sequence: int, payload: bytes) -> str:
    return f"UPLOAD DATA {sequence} {payload.hex().upper()}"


def upload_end_command() -> str:
    return "UPLOAD END"


def upload_cancel_command() -> str:
    return "UPLOAD CANCEL"


def download_begin_command(filename: str, chunk_bytes: int = 64) -> str:
    return f"DOWNLOAD BEGIN {filename}" + _chunk_argument(chunk_bytes)


def download_data_command(sequence: int) -> str:
    return f"DOWNLOAD DATA {sequence}"


def download_end_command() -> str:
    return "DOWNLOAD END"


def download_cancel_command() -> str:
    return "DOWNLOAD CANCEL"


def morse_command(text: str) -> str:
    return f"MORSE {text}"


def remote_command(command: str) -> str:
    return f"REMOTE {command}"


def text_command(text: str) -> str:
    return f"TEXT {text}"


def echo_command(text: str) -> str:
    return f"ECHO {text}"


def cw_start_command(channel: int | None = None, power_level: int | None = None) -> str:
    parts = ["CW", "START"]
    if channel is not None:
        parts.append(str(channel))
    if power_level is not None:
        parts.append(str(power_level))
    return " ".join(parts)


def cw_loop_command(
    on_ms: int,
    off_ms: int,
    channel: int | None = None,
    power_level: int | None = None,
) -> str:
    parts = ["CW", "LOOP", str(on_ms), str(off_ms)]
    if channel is not None:
        parts.append(str(channel))
    if power_level is not None:
        parts.append(str(power_level))
    return " ".join(parts)


def test_mode_command(enabled: bool) -> str:
    return "TESTMODE ON" if enabled else "TESTMODE OFF"


def test_tx_command(size: int) -> str:
    return f"TESTTX {size}"


def select_stored_command(name: str) -> str:
    if not valid_storage_name(name):
        raise ValueError("Invalid SPIFFS file name")
    return "SELECTHEX " + name.encode("utf-8").hex().upper()


def stored_tx_command(name: str) -> str:
    if not valid_storage_name(name):
        raise ValueError("Invalid SPIFFS file name")
    return "TXHEX " + name.encode("utf-8").hex().upper()
