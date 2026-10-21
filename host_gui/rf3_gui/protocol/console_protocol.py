from __future__ import annotations


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


def upload_begin_command(filename: str, size: int, crc32: int) -> str:
    return f"UPLOAD BEGIN {filename} {size} {crc32:08X}"


def upload_data_command(sequence: int, payload: bytes) -> str:
    return f"UPLOAD DATA {sequence} {payload.hex().upper()}"


def upload_end_command() -> str:
    return "UPLOAD END"


def upload_cancel_command() -> str:
    return "UPLOAD CANCEL"


def download_begin_command(filename: str) -> str:
    return f"DOWNLOAD BEGIN {filename}"


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
