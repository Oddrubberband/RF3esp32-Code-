from __future__ import annotations

import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch
import urllib.error
import zipfile

from tools import package_current_release as packaging
from tools import publish_current_release as publishing

COMMIT = "a" * 40
TAG = "v2026.10.24-r2"


class FakeGitHub:
    """In-memory API contract: no socket, credential or external mutation."""
    repository = "example/rf3"

    def __init__(self, *, tag_commit=None, release=None, assets=None, corrupt_upload=False):
        self.commit = tag_commit
        self.release = release
        self.assets = list(assets or [])
        self.corrupt_upload = corrupt_upload
        self.calls = []

    def tag_commit(self, tag):
        return self.commit

    def request(self, method, path, **kwargs):
        self.calls.append((method, path, kwargs))
        if method == "GET" and "/releases/tags/" in path:
            return self.release
        if method == "GET" and "/assets?" in path:
            return list(self.assets)
        if method == "POST" and path.endswith("/git/refs"):
            self.commit = kwargs["payload"]["sha"]
            return {"object": {"sha": self.commit, "type": "commit"}}
        if method == "POST" and path.endswith("/releases"):
            self.release = {**kwargs["payload"], "id": 123}
            return self.release
        if method == "POST" and "/assets?name=" in path:
            name = path.split("?name=", 1)[1]
            data = kwargs["data"]
            asset = {"name": name, "state": "uploaded", "size": len(data),
                     "digest": "sha256:" + ("0" * 64 if self.corrupt_upload else packaging.sha256(data))}
            self.assets.append(asset)
            return asset
        if method == "PATCH":
            self.release.update(kwargs["payload"])
            self.release["html_url"] = "https://github.com/example/rf3/releases/tag/" + TAG
            return self.release
        raise AssertionError((method, path, kwargs))


class TestReleaseTools(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.output = self.root / "packages"
        (self.root / "CURRENT_RELEASE.json").write_text(json.dumps({"release_tag": TAG}), encoding="utf-8")
        gui = self.root / "dist/RF3Desktop"
        (gui / "_internal/PySide6").mkdir(parents=True)
        (gui / "RF3Desktop.exe").write_bytes(b"MZ-portable-gui-test")
        (gui / "_internal/python311.dll").write_bytes(b"python-test-runtime")
        (gui / "_internal/PySide6/QtCore.pyd").write_bytes(b"qt-test-runtime")
        (gui / "unrelated.txt").write_text("not part of the executable distribution")
        (self.root / "Flash_Manual.ps1").write_text("# manual BOOT/EN helper test\n", encoding="utf-8")
        for environment in packaging.ENVIRONMENTS:
            folder = self.root / ".pio/build" / environment
            folder.mkdir(parents=True)
            for name in packaging.IMAGE_NAMES:
                (folder / name).write_bytes((environment + ":" + name).encode())
            (folder / "firmware.elf").write_bytes(b"exclude-debug-build")
            (folder / "spiffs.bin").write_bytes(b"exclude-filesystem")

    def tearDown(self):
        self.temporary.cleanup()

    def packages(self):
        return packaging.package(self.root, self.output, "all", commit=COMMIT)

    def plan(self):
        self.packages()
        return publishing.prepare_release(self.root, self.output, COMMIT)

    def rewrite_member(self, kind, member, data):
        filename = packaging.GUI_ARCHIVE if kind == "gui" else packaging.FIRMWARE_ARCHIVE
        path = self.output / filename
        with zipfile.ZipFile(path) as archive:
            members = {name: archive.read(name) for name in archive.namelist()}
        members[member] = data
        path.unlink()
        hashes = packaging.write_archive(path, members)
        metadata_path = self.output / f"{kind.upper()}-BUILD.json"
        metadata = json.loads(metadata_path.read_text())
        metadata.update(members=hashes, sha256=packaging.sha256(path.read_bytes()), size=path.stat().st_size)
        metadata_path.write_bytes(packaging.json_bytes(metadata))

    def test_portable_payload_contents_and_firmware_hashes(self):
        paths = self.packages()
        self.assertEqual(len(paths), 4)
        with zipfile.ZipFile(self.output / packaging.GUI_ARCHIVE) as archive:
            names = set(archive.namelist())
            self.assertIn("RF3Desktop/_internal/PySide6/QtCore.pyd", names)
            self.assertNotIn("RF3Desktop/unrelated.txt", names)
            launcher = archive.read("Launch_GUI.cmd").decode()
            self.assertIn('"RF3Desktop\\RF3Desktop.exe"', launcher)
            self.assertNotIn("COM5", launcher)
            self.assertEqual(json.loads(archive.read("BUILD_INFO.json"))["source_commit"], COMMIT)
        with zipfile.ZipFile(self.output / packaging.FIRMWARE_ARCHIVE) as archive:
            names = set(archive.namelist())
            expected_images = {f"firmware/{env}/{name}" for env in packaging.ENVIRONMENTS for name in packaging.IMAGE_NAMES}
            self.assertTrue(expected_images.issubset(names))
            self.assertFalse(any(name.endswith((".elf", "spiffs.bin")) for name in names))
            image_hashes = json.loads(archive.read("Firmware_Images.json"))["Current"]
            for environment in packaging.ENVIRONMENTS:
                for name in packaging.IMAGE_NAMES:
                    self.assertEqual(image_hashes[environment][name],
                                     packaging.sha256(archive.read(f"firmware/{environment}/{name}")))
            self.assertIn(b"-Board custom", archive.read("README.txt"))
            self.assertIn(b"manual BOOT/EN", archive.read("Flash_Manual.ps1"))

    def test_packaging_is_byte_stable_and_does_not_overwrite(self):
        self.packages()
        other = self.root / "other-packages"
        packaging.package(self.root, other, "all", commit=COMMIT)
        for name in (packaging.GUI_ARCHIVE, packaging.FIRMWARE_ARCHIVE):
            self.assertEqual((self.output / name).read_bytes(), (other / name).read_bytes())
        original = (self.output / packaging.GUI_ARCHIVE).read_bytes()
        with self.assertRaises(FileExistsError):
            self.packages()
        self.assertEqual((self.output / packaging.GUI_ARCHIVE).read_bytes(), original)

    def test_package_kinds_work_independently_for_separate_ci_jobs(self):
        packaging.package(self.root, self.output, "gui", commit=COMMIT)
        self.assertFalse((self.output / packaging.FIRMWARE_ARCHIVE).exists())
        packaging.package(self.root, self.output, "firmware", commit=COMMIT)
        publishing.prepare_release(self.root, self.output, COMMIT)

    def test_missing_firmware_or_internal_runtime_refuses_packaging(self):
        missing = self.root / ".pio/build/rf3_custom_pcb/partitions.bin"
        missing.unlink()
        with self.assertRaises(ValueError):
            self.packages()
        self.assertFalse((self.output / packaging.GUI_ARCHIVE).exists())
        import shutil
        shutil.rmtree(self.root / "dist/RF3Desktop/_internal")
        with self.assertRaises(ValueError):
            packaging.package(self.root, self.output, "gui", commit=COMMIT)

    def test_invalid_release_identity_refused(self):
        with self.assertRaises(ValueError):
            packaging.package(self.root, self.output, "all", commit="main")
        (self.root / "CURRENT_RELEASE.json").write_text('{"release_tag":"../bad"}')
        with self.assertRaises(ValueError):
            self.packages()

    def test_commit_fallback_refuses_an_ancestor_git_checkout(self):
        result = Mock(stdout=str(self.root.parent) + "\n")
        with patch.dict(os.environ, {}, clear=True), \
             patch.object(packaging.subprocess, "run", return_value=result) as run:
            with self.assertRaisesRegex(ValueError, "checkout root"):
                packaging.release_identity(self.root)
        self.assertEqual(run.call_count, 1)
        self.assertIn("--show-toplevel", run.call_args.args[0])

    def test_symlinked_image_is_not_packaged(self):
        image = self.root / ".pio/build/rf3_custom_pcb/firmware.bin"
        other = self.root / "other.bin"
        other.write_bytes(image.read_bytes())
        image.unlink()
        try:
            image.symlink_to(other)
        except OSError:
            self.skipTest("Symlink creation is unavailable for this Windows account")
        with self.assertRaises(ValueError):
            self.packages()

    def test_prepare_writes_exact_commit_manifest_and_all_asset_checksums(self):
        plan = self.plan()
        self.assertEqual(plan["source_commit"], COMMIT)
        self.assertEqual(plan["release_tag"], TAG)
        manifest = json.loads((self.output / "release-manifest.json").read_text())
        self.assertEqual(manifest["source_commit"], COMMIT)
        sums = (self.output / "SHA256SUMS").read_text().splitlines()
        self.assertEqual(len(sums), 3)
        for line in sums:
            digest, name = line.split("  ")
            self.assertEqual(digest, packaging.sha256((self.output / name).read_bytes()))
        # Repeating preparation is safe only when bytes are exactly the same.
        publishing.prepare_release(self.root, self.output, COMMIT)

    def test_mismatching_build_commit_and_archive_tamper_refused(self):
        self.packages()
        metadata_path = self.output / "GUI-BUILD.json"
        metadata = json.loads(metadata_path.read_text())
        metadata["source_commit"] = "b" * 40
        metadata_path.write_bytes(packaging.json_bytes(metadata))
        with self.assertRaises(ValueError):
            publishing.prepare_release(self.root, self.output, COMMIT)
        metadata["source_commit"] = COMMIT
        metadata_path.write_bytes(packaging.json_bytes(metadata))
        with (self.output / packaging.GUI_ARCHIVE).open("ab") as stream:
            stream.write(b"tamper")
        with self.assertRaises(ValueError):
            publishing.prepare_release(self.root, self.output, COMMIT)

    def test_internal_firmware_manifest_tamper_is_detected_even_if_zip_hash_updated(self):
        self.packages()
        self.rewrite_member("firmware", "Firmware_Images.json", packaging.json_bytes({"Current": {
            environment: {name: "0" * 64 for name in packaging.IMAGE_NAMES}
            for environment in packaging.ENVIRONMENTS}}))
        with self.assertRaisesRegex(ValueError, "Firmware image hash"):
            publishing.prepare_release(self.root, self.output, COMMIT)

    def test_extra_firmware_file_or_parent_path_refused_even_with_matching_metadata(self):
        self.packages()
        self.rewrite_member("firmware", "firmware/private.txt", b"unexpected")
        with self.assertRaises(ValueError):
            publishing.prepare_release(self.root, self.output, COMMIT)
        with self.assertRaises(ValueError):
            packaging.write_archive(self.root / "unsafe.zip", {"../escape": b"invalid"})

    def test_default_dry_run_never_constructs_network_client(self):
        self.packages()
        with patch.object(publishing, "GitHubReleaseClient", side_effect=AssertionError("network access")), \
             contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(publishing.main(["--root", str(self.root), "--artifacts-dir", str(self.output),
                                              "--commit", COMMIT]), 0)
        self.assertIn('"mode": "dry-run"', output.getvalue())

    def test_publish_requires_explicit_trusted_main_opt_in(self):
        good = {"GITHUB_ACTIONS": "true", "GITHUB_REF": "refs/heads/main", "GITHUB_EVENT_NAME": "push",
                "GITHUB_SHA": COMMIT, "RF3_RELEASE_AUTHORIZED": "true"}
        with patch.dict(os.environ, good, clear=True):
            publishing.trusted_ci(COMMIT)
        for key, value in (("GITHUB_ACTIONS", "false"), ("GITHUB_REF", "refs/pull/1/merge"),
                           ("GITHUB_EVENT_NAME", "pull_request"), ("GITHUB_SHA", "b" * 40),
                           ("RF3_RELEASE_AUTHORIZED", "false")):
            with patch.dict(os.environ, {**good, key: value}, clear=True), self.assertRaises(RuntimeError):
                publishing.trusted_ci(COMMIT)

    def test_mock_api_draft_upload_hash_verification_then_publication(self):
        plan = self.plan()
        client = FakeGitHub()
        url = publishing.publish_release(client, self.output, plan)
        self.assertIn(TAG, url)
        self.assertEqual(client.commit, COMMIT)
        create = next(kwargs["payload"] for method, path, kwargs in client.calls if method == "POST" and path.endswith("/releases"))
        self.assertTrue(create["draft"])
        self.assertEqual(create["target_commitish"], COMMIT)
        self.assertIn(f"/blob/{COMMIT}/CHANGELOG.md", create["body"])
        self.assertIn("byte-for-byte", create["body"])
        self.assertIn("remain unqualified", create["body"])
        uploads = [(method, path) for method, path, _ in client.calls if "/assets?name=" in path]
        self.assertEqual(len(uploads), 4)
        self.assertEqual(client.calls[-1][0], "PATCH")
        self.assertEqual(set(item["name"] for item in client.assets), set(plan["assets"]))
        self.assertFalse(client.release["draft"])

    def test_tag_commit_mismatch_refuses_before_any_api_mutation(self):
        client = FakeGitHub(tag_commit="b" * 40)
        with self.assertRaises(RuntimeError):
            publishing.publish_release(client, self.output, self.plan())
        self.assertEqual(client.calls, [])

    def test_existing_assets_or_published_release_never_overwritten(self):
        plan = self.plan()
        for draft, assets in ((True, [{"name": packaging.GUI_ARCHIVE}]), (False, [])):
            client = FakeGitHub(tag_commit=COMMIT, release={"tag_name": TAG, "target_commitish": COMMIT,
                                                          "draft": draft, "id": 123}, assets=assets)
            with self.assertRaises(RuntimeError):
                publishing.publish_release(client, self.output, plan)
            self.assertFalse(any(method in {"POST", "PATCH", "DELETE"} for method, _, _ in client.calls))

    def test_existing_release_target_mismatch_refused_even_if_tag_matches(self):
        client = FakeGitHub(tag_commit=COMMIT, release={"tag_name": TAG, "target_commitish": "b" * 40,
                                                      "draft": True, "id": 123})
        with self.assertRaises(RuntimeError):
            publishing.publish_release(client, self.output, self.plan())
        self.assertFalse(any(method in {"POST", "PATCH"} for method, _, _ in client.calls))

    def test_upload_digest_failure_retains_draft_and_never_publishes(self):
        client = FakeGitHub(corrupt_upload=True)
        with self.assertRaisesRegex(RuntimeError, "remains a draft"):
            publishing.publish_release(client, self.output, self.plan())
        self.assertTrue(client.release["draft"])
        self.assertFalse(any(method == "PATCH" for method, _, _ in client.calls))

    def test_annotated_tag_is_resolved_to_commit_without_network(self):
        client = publishing.GitHubReleaseClient("example/rf3", "unit-test-token", opener=Mock())
        client.request = Mock(side_effect=[{"object": {"type": "tag", "sha": "c" * 40}},
                                          {"object": {"type": "commit", "sha": COMMIT}}])
        self.assertEqual(client.tag_commit(TAG), COMMIT)
        self.assertIn("/git/tags/", client.request.call_args_list[-1].args[1])

    def test_api_error_does_not_expose_token_and_disallows_redirects(self):
        token = "unit-test-token"
        opener = Mock()
        opener.open.side_effect = urllib.error.HTTPError("https://api.github.com/test", 500, "failure", {}, None)
        client = publishing.GitHubReleaseClient("example/rf3", token, opener=opener)
        with self.assertRaises(RuntimeError) as failure:
            client.request("GET", "/repos/example/rf3/releases")
        self.assertNotIn(token, str(failure.exception))
        self.assertIsNone(publishing.NoRedirect().redirect_request(None, None, 302, "", {}, "https://elsewhere.invalid"))
        request = opener.open.call_args.args[0]
        self.assertEqual(request.get_header("Authorization"), "Bearer " + token)
        self.assertTrue(request.full_url.startswith("https://api.github.com/"))

    def test_workflow_is_pinned_and_only_gated_release_has_write_permission(self):
        workflow = (Path(__file__).resolve().parents[1] / ".github/workflows/validate.yml").read_text()
        self.assertEqual(workflow.count("contents: write"), 1)
        self.assertIn("needs: [linux, windows]", workflow)
        self.assertIn("github.ref == 'refs/heads/main'", workflow)
        self.assertIn("[release-current]", workflow)
        self.assertIn("inputs.release", workflow)
        for line in workflow.splitlines():
            if "uses: actions/" in line:
                self.assertRegex(line, r"@[0-9a-f]{40}$")
        self.assertNotIn("pull_request_target", workflow)
        self.assertNotIn("-t upload", workflow)


if __name__ == "__main__":
    unittest.main()
