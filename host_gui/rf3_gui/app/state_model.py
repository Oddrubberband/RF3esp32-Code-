from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path


PROTOCOL_V2_MAX_FILE_SIZE = 1_310_720
CONSOLE_COMMAND_MAX_CHARS = 159
REMOTE_COMMAND_MAX_BYTES = 26
TEXT_MESSAGE_MAX_BYTES = 154
DEFAULT_CHANNEL = 76
MIN_CHANNEL = 0
MAX_CHANNEL = 125
MIN_POWER_LEVEL = 0
MAX_POWER_LEVEL = 3


class ConnectionState(str, Enum):
    DISCONNECTED = "Disconnected"
    SEARCHING = "Searching"
    CONNECTING = "Connecting"
    CONNECTED = "Connected"
    BUSY = "Busy"
    ERROR = "Error"


class RadioState(str, Enum):
    BOOT = "Boot"
    STANDBY = "Standby"
    RX_LISTENING = "RxListening"
    TX_BUSY = "TxBusy"
    SLEEP = "Sleep"
    POWER_DOWN = "PowerDown"
    CW_TEST = "CwTest"
    FAULT = "Fault"
    UNKNOWN = "Unknown"


class TransferState(str, Enum):
    IDLE = "Idle"
    PREPARING = "Preparing"
    HANDSHAKING = "Handshaking"
    TRANSFERRING = "Transferring"
    VERIFYING = "Verifying"
    COMPLETED = "Completed"
    CANCELLING = "Cancelling"
    CANCELLED = "Cancelled"
    FAILED = "Failed"


@dataclass(frozen=True)
class DeviceInfo:
    name: str
    address: str
    transport: str
    firmware_version: str = "unknown"
    protocol_version: str = "unknown"
    bluetooth_address: str = ""
    paired: bool = False
    seen_in_scan: bool = False


@dataclass(frozen=True)
class RadioCapabilities:
    min_channel: int = MIN_CHANNEL
    max_channel: int = MAX_CHANNEL
    power_levels: tuple[int, ...] = (0, 1, 2, 3)
    operating_modes: tuple[str, ...] = (
        "Standby",
        "RxListening",
        "Sleep",
        "PowerDown",
    )
    modulation_modes: tuple[str, ...] = ("nRF24 packet mode (fixed)",)
    supports_cw_test: bool = True
    supports_battery_percent: bool = False
    supports_host_file_upload: bool = False
    supports_host_file_download: bool = False
    supports_host_chunk_retry: bool = False
    supports_remote_commands: bool = False
    supports_text_messages: bool = False
    supports_host_echo: bool = False


@dataclass(frozen=True)
class Telemetry:
    battery_percent: int | None = None
    last_fault: int = 0
    last_tx_ok: bool = False
    tx_timeout: bool = False
    rx_packets: int = 0
    rx_saved: int = 0
    rx_saved_bytes: int = 0
    last_rx_file: str = ""
    last_rx_path: str = ""
    rpd: bool = False


@dataclass(frozen=True)
class DeviceSnapshot:
    connection_state: ConnectionState = ConnectionState.DISCONNECTED
    test_mode: bool = False
    test_tx_count: int = 0
    test_tx_bytes: int = 0
    requested_radio_state: RadioState = RadioState.UNKNOWN
    confirmed_radio_state: RadioState = RadioState.UNKNOWN
    channel: int = DEFAULT_CHANNEL
    power_level: int | None = None
    selected_file: str = ""
    telemetry: Telemetry = field(default_factory=Telemetry)
    capabilities: RadioCapabilities = field(default_factory=RadioCapabilities)


@dataclass(frozen=True)
class TransferProgress:
    state: TransferState
    sent_bytes: int = 0
    total_bytes: int = 0
    message: str = ""
    source_path: Path | None = None

    @property
    def ratio(self) -> float:
        if self.total_bytes <= 0:
            return 0.0 if self.state != TransferState.COMPLETED else 1.0
        return max(0.0, min(1.0, self.sent_bytes / self.total_bytes))
