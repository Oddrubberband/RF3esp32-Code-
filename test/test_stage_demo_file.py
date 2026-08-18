from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tools.stage_demo_file import (
    DEFAULT_MARGIN_BYTES,
    PROTOCOL_V2_MAX_FILE_BYTES,
    SPIFFS_MAX_FILENAME_BYTES,
    commit_staged_file,
    sanitize_output_name,
    stage_data_directory,
    validate_transfer_size,
    validate_spiffs_fit,
)


class StageDemoFileTests(unittest.TestCase):
    def test_replacement_size_counts_new_file_once(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            data = Path(root) / "data"
            data.mkdir()
            (data / "payload.bin").write_bytes(b"x" * 100)
            (data / "keep.bin").write_bytes(b"k" * 10)
            source = Path(root) / "new.bin"
            source.write_bytes(b"n" * 40)
            staged = stage_data_directory(data, "payload.bin", source, False)
            self.addCleanup(lambda: __import__("shutil").rmtree(staged.parent, ignore_errors=True))
            self.assertEqual(50, sum(p.stat().st_size for p in staged.iterdir()))

    def test_overcapacity_is_rejected_without_mkspiffs(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            staged = Path(root)
            (staged / "large.bin").write_bytes(b"x" * 65)
            fits, _ = validate_spiffs_fit(
                staged, DEFAULT_MARGIN_BYTES + 64, mkspiffs_path=None
            )
            self.assertFalse(fits)

    def test_near_full_conservative_budget_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            staged = Path(root)
            (staged / "near.bin").write_bytes(b"x" * 64)
            fits, _ = validate_spiffs_fit(
                staged, DEFAULT_MARGIN_BYTES + 64, mkspiffs_path=None
            )
            self.assertTrue(fits)

    def test_partial_files_are_excluded_from_image_staging(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            data = Path(root) / "data"
            data.mkdir()
            (data / "stale.part").write_bytes(b"partial")
            source = Path(root) / "payload.bin"
            source.write_bytes(b"ok")
            staged = stage_data_directory(data, "payload.bin", source, False)
            self.addCleanup(lambda: __import__("shutil").rmtree(staged.parent, ignore_errors=True))
            self.assertFalse((staged / "stale.part").exists())

    def test_binary_filename_is_sanitized_without_forcing_audio_extension(self) -> None:
        self.assertEqual("firmware_image.bin", sanitize_output_name("firmware image.bin"))
        self.assertEqual("payload.bin", sanitize_output_name("���.invalid-extension"))

    def test_long_filename_is_bounded_for_spiffs_and_preserves_extension(self) -> None:
        sanitized = sanitize_output_name("a" * 100 + ".u8")
        self.assertLessEqual(len(sanitized.encode("ascii")), SPIFFS_MAX_FILENAME_BYTES)
        self.assertTrue(sanitized.endswith(".u8"))

    def test_protocol_v2_transfer_size_boundary_is_enforced(self) -> None:
        self.assertTrue(validate_transfer_size(0)[0])
        self.assertTrue(validate_transfer_size(PROTOCOL_V2_MAX_FILE_BYTES)[0])
        self.assertFalse(validate_transfer_size(PROTOCOL_V2_MAX_FILE_BYTES + 1)[0])

    def test_replace_existing_commits_validated_directory_shape(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            data = Path(root) / "data"
            data.mkdir()
            (data / "old.bin").write_bytes(b"old")
            source = Path(root) / "source.bin"
            source.write_bytes(b"new")
            output = data / "payload.bin"
            commit_staged_file(data, output, source, True)
            self.assertEqual([output], list(data.iterdir()))
            self.assertEqual(b"new", output.read_bytes())


if __name__ == "__main__":
    unittest.main()
