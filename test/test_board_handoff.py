"""Safety checks for the offline image handoff, without a device or toolchain."""
from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location("board_handoff", TOOLS / "prepare_board_handoff.py")
handoff = importlib.util.module_from_spec(spec)
spec.loader.exec_module(handoff)

LAYOUT = """nvs,data,nvs,0x9000,20K,
phy_init,data,phy,0xe000,4K,
factory,app,factory,0x10000,1536K,
spiffs,data,spiffs,0x190000,0x270000,
"""


class BoardHandoffTests(unittest.TestCase):
    def test_real_partition_profiles_fit_selected_flash(self):
        root = TOOLS.parent
        for filename, flash_mb in (("partitions.csv", 16), ("partitions_4mb.csv", 4)):
            parsed = handoff.parse_layout((root / filename).read_text(), flash_mb * 1024 * 1024)
            self.assertEqual(0x190000, parsed["spiffs"]["offset"])
            self.assertEqual(flash_mb * 1024 * 1024, parsed["spiffs"]["offset"] + parsed["spiffs"]["size"])

    def test_16mb_table_is_rejected_for_a_4mb_board(self):
        with self.assertRaisesRegex(ValueError, "exceeds selected flash"):
            handoff.parse_layout((TOOLS.parent / "partitions.csv").read_text(), 4 * 1024 * 1024)

    def test_overlapping_partitions_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "overlap"):
            handoff.parse_layout(LAYOUT.replace("0x190000", "0x180000"), 4 * 1024 * 1024)

    def test_missing_spiffs_or_duplicate_entries_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "Missing"):
            handoff.parse_layout(LAYOUT.split("spiffs")[0], 4 * 1024 * 1024)
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            handoff.parse_layout(LAYOUT + "nvs,data,nvs,0x9000,20K,\n", 4 * 1024 * 1024)

    def test_arduino_mkspiffs_metadata_mismatch_is_rejected(self):
        sdk = {"CONFIG_SPIFFS_OBJ_NAME_LEN": "32", "CONFIG_SPIFFS_META_LENGTH": "4",
               "CONFIG_SPIFFS_USE_MAGIC": "y", "CONFIG_SPIFFS_USE_MAGIC_LENGTH": "y",
               "CONFIG_SPIFFS_PAGE_SIZE": "256"}
        version = "\n".join(["SPIFFS_OBJ_NAME_LEN: 32", "SPIFFS_OBJ_META_LEN: 4", "SPIFFS_USE_MAGIC: 1",
                              "SPIFFS_USE_MAGIC_LENGTH: 1", "SPIFFS_ALIGNED_OBJECT_INDEX_TABLES: 0"])
        handoff.check_spiffs_config(sdk, version)
        with self.assertRaisesRegex(ValueError, "META_LEN"):
            handoff.check_spiffs_config(sdk, version.replace("META_LEN: 4", "META_LEN: 0"))

    def test_mixed_profile_and_duplicate_pin_flags_are_rejected(self):
        from platformio.project.config import ProjectConfig
        config = ProjectConfig.get_instance(str(TOOLS.parent / "platformio.ini"))
        flags = config.get("env:rf3_custom_pcb", "build_flags")
        handoff.check_flags(flags, handoff.PROFILES["rf3_custom_pcb"])
        with self.assertRaisesRegex(ValueError, "PROFILE_ID"):
            handoff.check_flags(flags, handoff.PROFILES["rf3_esp32_devboard"])
        with self.assertRaisesRegex(ValueError, "CE_PIN"):
            handoff.check_flags(flags + ["-DNRF24_CE_PIN=27"], handoff.PROFILES["rf3_custom_pcb"])

    def test_same_length_file_corruption_changes_inventory(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "one.bin"
            path.write_bytes(b"Z")
            before = handoff.inventory(Path(root))
            self.assertEqual("59BC5767", before["one.bin"]["crc32"])
            path.write_bytes(b"Y")
            after = handoff.inventory(Path(root))
            self.assertEqual(before["one.bin"]["bytes"], after["one.bin"]["bytes"])
            self.assertNotEqual(before["one.bin"]["sha256"], after["one.bin"]["sha256"])

    def test_loaded_devboard_cannot_hold_another_song(self):
        staged = sum(path.stat().st_size for path in (TOOLS.parent / "data").iterdir() if path.is_file())
        song = (TOOLS.parent / "data/song.u8").stat().st_size
        self.assertGreater(staged + song, 0x270000)
        self.assertGreater(int(0x270000 * 0.75) - 1, 1310720 + 65536)


if __name__ == "__main__":
    unittest.main()
