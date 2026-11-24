import unittest

from rf3_gui.protocol.status_parser import parse_fraction, parse_status_line


def test_parse_status_line_coerces_common_firmware_fields():
    parsed = parse_status_line(
        "profile=custom-pcb filesystem=ready State=Standby channel=76 "
        "power=3 tx_ok=false rx_packets=4 rpd=true tx_bytes=20/100"
    )

    assert parsed["profile"] == "custom-pcb"
    assert parsed["State"] == "Standby"
    assert parsed["channel"] == 76
    assert parsed["power"] == 3
    assert parsed["tx_ok"] is False
    assert parsed["rx_packets"] == 4
    assert parsed["rpd"] is True
    assert parse_fraction(parsed["tx_bytes"]) == (20, 100)


class TestStatusParser(unittest.TestCase):
    def test_status_parser(self):
        test_parse_status_line_coerces_common_firmware_fields()
