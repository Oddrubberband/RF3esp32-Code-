from __future__ import annotations

import shlex
from typing import Any


def parse_status_line(line: str) -> dict[str, Any]:
    """Parse the current firmware STATUS key=value console line.

    The ESP firmware prints a human-oriented line today, not a stable JSON
    schema. This parser is deliberately small and should be replaced once the
    firmware exposes a machine-readable Bluetooth/serial status response.
    """
    values: dict[str, Any] = {}
    for token in shlex.split(line):
        if "=" not in token:
            continue
        key, raw_value = token.split("=", 1)
        values[key] = _coerce_value(raw_value)
    return values


def parse_fraction(raw_value: Any) -> tuple[int, int] | None:
    if not isinstance(raw_value, str) or "/" not in raw_value:
        return None
    left, right = raw_value.split("/", 1)
    try:
        return int(left, 0), int(right, 0)
    except ValueError:
        return None


def _coerce_value(raw_value: str) -> Any:
    lowered = raw_value.lower()
    if lowered == "true":
        return True
    if lowered == "false":
        return False
    if lowered in {"unknown", "unavailable"}:
        return lowered
    try:
        return int(raw_value, 0)
    except ValueError:
        return raw_value
