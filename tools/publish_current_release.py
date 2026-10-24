"""Verify/prepare release assets locally; publish only with --publish in trusted main CI.

GitHub REST references:
https://docs.github.com/en/rest/releases/releases
https://docs.github.com/en/rest/releases/assets
https://docs.github.com/en/rest/git/refs
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import urllib.error
import urllib.parse
import urllib.request
import zipfile

try:
    from tools.package_current_release import (ENVIRONMENTS, IMAGE_NAMES, GUI_ARCHIVE,
                                               FIRMWARE_ARCHIVE, ROOT, json_bytes,
                                               release_identity, sha256)
except ModuleNotFoundError:  # Direct execution from tools/ without changing PYTHONPATH.
    from package_current_release import (ENVIRONMENTS, IMAGE_NAMES, GUI_ARCHIVE,
                                         FIRMWARE_ARCHIVE, ROOT, json_bytes,
                                         release_identity, sha256)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file, code, message, headers, new_url):
        return None  # Never forward the CI authorization header to a redirect.


class GitHubReleaseClient:
    def __init__(self, repository: str, token: str, *, opener=None) -> None:
        if re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository) is None:
            raise ValueError("Expected an owner/repository GitHub name")
        if not token:
            raise ValueError("GITHUB_TOKEN is required for explicit publication")
        self.repository = repository
        self.token = token
        self.opener = opener or urllib.request.build_opener(NoRedirect())

    def request(self, method: str, path: str, *, payload=None, data: bytes | None = None,
                content_type: str = "application/json", upload: bool = False,
                missing_ok: bool = False):
        if not path.startswith("/") or "\r" in path or "\n" in path:
            raise ValueError("Invalid GitHub API path")
        if payload is not None:
            if data is not None:
                raise ValueError("Choose JSON or binary payload")
            data = json_bytes(payload)
        base = "https://uploads.github.com" if upload else "https://api.github.com"
        headers = {"Authorization": f"Bearer {self.token}",
                   "Accept": "application/vnd.github+json",
                   "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "RF3-current-release",
                   "Content-Type": content_type}
        request = urllib.request.Request(base + path, method=method, data=data, headers=headers)
        try:
            with self.opener.open(request, timeout=120) as response:
                body = response.read(4 * 1024 * 1024 + 1)
        except urllib.error.HTTPError as exc:
            if missing_ok and exc.code == 404:
                return None
            raise RuntimeError(f"GitHub {method} request failed with HTTP {exc.code}; no existing asset was replaced") from None
        if len(body) > 4 * 1024 * 1024:
            raise RuntimeError("GitHub returned an oversized JSON response")
        return json.loads(body.decode("utf-8"))

    def tag_commit(self, tag: str) -> str | None:
        ref = self.request("GET", f"/repos/{self.repository}/git/ref/tags/{urllib.parse.quote(tag, safe='')}",
                           missing_ok=True)
        if ref is None:
            return None
        obj = ref.get("object", {})
        for _ in range(8):
            if obj.get("type") == "commit" and re.fullmatch(r"[0-9a-f]{40}", str(obj.get("sha"))):
                return obj["sha"]
            if obj.get("type") != "tag" or re.fullmatch(r"[0-9a-f]{40}", str(obj.get("sha"))) is None:
                raise RuntimeError("Release tag does not resolve to an exact commit")
            annotated = self.request("GET", f"/repos/{self.repository}/git/tags/{obj['sha']}")
            obj = annotated.get("object", {})
        raise RuntimeError("Annotated release tag nesting is too deep")


def _regular_asset(path: Path) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"Missing regular release asset: {path.name}")
    return path.read_bytes()


def verify_build(directory: Path, kind: str, tag: str, commit: str) -> dict:
    manifest = json.loads(_regular_asset(directory / f"{kind.upper()}-BUILD.json").decode("utf-8"))
    filename = GUI_ARCHIVE if kind == "gui" else FIRMWARE_ARCHIVE
    if (manifest.get("schema") != 1 or manifest.get("kind") != kind
            or manifest.get("release_tag") != tag or manifest.get("source_commit") != commit
            or manifest.get("archive") != filename):
        raise ValueError("Build metadata does not match the selected release and exact source commit")
    data = _regular_asset(directory / filename)
    if manifest.get("sha256") != sha256(data) or manifest.get("size") != len(data):
        raise ValueError("Release archive checksum/size does not match its build metadata")
    expected = manifest.get("members")
    if not isinstance(expected, dict) or not expected:
        raise ValueError("Build metadata lacks archive member hashes")
    with zipfile.ZipFile(directory / filename) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)) or set(names) != set(expected) or archive.testzip() is not None:
            raise ValueError("Archive members or CRC are invalid")
        for info in archive.infolist():
            name = info.filename
            if (PurePosixPath(name).is_absolute() or ".." in PurePosixPath(name).parts
                    or "\\" in name or (info.external_attr >> 16) & 0o170000 == 0o120000
                    or sha256(archive.read(name)) != expected[name]):
                raise ValueError("Archive member path/hash is invalid")
        identity = json.loads(archive.read("BUILD_INFO.json"))
        if identity != {"schema": 1, "release_tag": tag, "source_commit": commit}:
            raise ValueError("Packaged build identity does not match the release")
        if kind == "gui":
            required = {"RF3Desktop/RF3Desktop.exe", "Launch_GUI.cmd", "README.txt", "BUILD_INFO.json"}
            if not required.issubset(names) or not any(name.startswith("RF3Desktop/_internal/") for name in names):
                raise ValueError("Portable GUI distribution is incomplete")
            if any(name not in required and not name.startswith("RF3Desktop/_internal/") for name in names):
                raise ValueError("Unexpected GUI package content")
        else:
            required = {f"firmware/{env}/{name}" for env in ENVIRONMENTS for name in IMAGE_NAMES}
            required |= {"Flash_Manual.ps1", "Firmware_Images.json", "README.txt", "BUILD_INFO.json"}
            if set(names) != required:
                raise ValueError("Firmware package must contain only the six verified images and portable helper")
            images = json.loads(archive.read("Firmware_Images.json"))
            if set(images.get("Current", {})) != set(ENVIRONMENTS):
                raise ValueError("Firmware image profile manifest is invalid")
            for environment in ENVIRONMENTS:
                if set(images["Current"][environment]) != set(IMAGE_NAMES):
                    raise ValueError("Firmware image filename manifest is invalid")
                for name in IMAGE_NAMES:
                    if images["Current"][environment][name] != sha256(archive.read(f"firmware/{environment}/{name}")):
                        raise ValueError("Firmware image hash does not match the packaged image")
    return {"name": filename, "kind": kind, "sha256": sha256(data), "size": len(data),
            "member_count": len(expected)}


def _write_preparation(path: Path, data: bytes) -> None:
    if path.exists():
        if path.read_bytes() != data:
            raise FileExistsError("Prepared release metadata already exists with different contents")
        return
    with path.open("xb") as stream:
        stream.write(data)


def prepare_release(root: Path, directory: Path, commit: str | None = None) -> dict:
    tag, commit = release_identity(root, commit)
    builds = [verify_build(directory, kind, tag, commit) for kind in ("gui", "firmware")]
    manifest = {"schema": 1, "release_tag": tag, "source_commit": commit, "artifacts": builds}
    _write_preparation(directory / "release-manifest.json", json_bytes(manifest))
    names = [GUI_ARCHIVE, FIRMWARE_ARCHIVE, "release-manifest.json"]
    sums = "".join(f"{sha256(_regular_asset(directory / name))}  {name}\n" for name in sorted(names))
    _write_preparation(directory / "SHA256SUMS", sums.encode("ascii"))
    return {**manifest, "assets": [*names, "SHA256SUMS"]}


def trusted_ci(commit: str) -> None:
    if (os.environ.get("GITHUB_ACTIONS") != "true" or os.environ.get("GITHUB_REF") != "refs/heads/main"
            or os.environ.get("GITHUB_EVENT_NAME") not in {"push", "workflow_dispatch"}
            or os.environ.get("GITHUB_SHA", "").lower() != commit):
        raise RuntimeError("--publish is restricted to the exact trusted main commit in GitHub Actions")
    if os.environ.get("RF3_RELEASE_AUTHORIZED") != "true":
        raise RuntimeError("Publication requires the workflow's explicit release-current marker or dispatch opt-in")


def publish_release(client: GitHubReleaseClient, directory: Path, plan: dict) -> str:
    """No replace/delete endpoints: failed attempts leave a draft for inspection."""
    tag, commit = plan["release_tag"], plan["source_commit"]
    existing_commit = client.tag_commit(tag)
    if existing_commit is not None and existing_commit != commit:
        raise RuntimeError("Existing release tag points to a different source commit; refusing publication")
    base = f"/repos/{client.repository}"
    release = client.request("GET", f"{base}/releases/tags/{urllib.parse.quote(tag, safe='')}", missing_ok=True)
    if release is not None:
        target = str(release.get("target_commitish", ""))
        if (release.get("tag_name") != tag or not release.get("draft")
                or (re.fullmatch(r"[0-9a-fA-F]{40}", target) is not None and target.lower() != commit)
                or (target != commit and existing_commit is None)):
            raise RuntimeError("Existing release is published or does not identify the exact source commit")
        assets = client.request("GET", f"{base}/releases/{release['id']}/assets?per_page=100")
        if assets:
            raise RuntimeError("Existing release assets will not be overwritten; inspect the retained draft")
    if existing_commit is None:
        client.request("POST", f"{base}/git/refs", payload={"ref": "refs/tags/" + tag, "sha": commit})
        if client.tag_commit(tag) != commit:
            raise RuntimeError("Created tag did not resolve to the selected source commit")
    if release is None:
        body = (f"RF3 current implementation, source commit `{commit}`.\n\n"
                f"[Complete change log](https://github.com/{client.repository}/blob/{commit}/CHANGELOG.md)\n\n"
                "Download both ZIP packages and SHA256SUMS. Extract the complete desktop ZIP and open "
                "Launch_GUI.cmd. The firmware ZIP includes separate board images and a manual BOOT/EN "
                "flashing helper. Read README.txt in each ZIP. Both boards require matching updated "
                "firmware for received extension metadata. One user-operated file round trip was compared "
                "byte-for-byte. Board/image identity, wider RF behavior and Bluetooth/driver acceptance "
                "remain unqualified.\n")
        release = client.request("POST", f"{base}/releases", payload={"tag_name": tag,
                                 "target_commitish": commit, "name": "RF3 " + tag,
                                 "body": body, "draft": True, "prerelease": False})
    release_id = release.get("id")
    if type(release_id) is not int or release_id <= 0:
        raise RuntimeError("GitHub returned an invalid draft release identifier")
    uploaded = {}
    for name in plan["assets"]:
        data = _regular_asset(directory / name)
        digest = sha256(data)
        content_type = "application/zip" if name.endswith(".zip") else "application/json" if name.endswith(".json") else "text/plain"
        asset = client.request("POST", f"{base}/releases/{release_id}/assets?name={urllib.parse.quote(name, safe='')}",
                               data=data, content_type=content_type, upload=True)
        if (asset.get("name") != name or asset.get("state") != "uploaded"
                or asset.get("size") != len(data) or asset.get("digest") != "sha256:" + digest):
            raise RuntimeError("Uploaded asset verification failed; release remains a draft")
        uploaded[name] = (len(data), "sha256:" + digest)
    actual = client.request("GET", f"{base}/releases/{release_id}/assets?per_page=100")
    if (not isinstance(actual, list) or len(actual) != len(uploaded)
            or {item.get("name"): (item.get("size"), item.get("digest")) for item in actual} != uploaded):
        raise RuntimeError("Final release asset verification failed; release remains a draft")
    if client.tag_commit(tag) != commit:
        raise RuntimeError("Release tag changed during upload; release remains a draft")
    published = client.request("PATCH", f"{base}/releases/{release_id}",
                               payload={"draft": False, "make_latest": "true"})
    if published.get("draft") is not False or published.get("tag_name") != tag:
        raise RuntimeError("GitHub did not confirm release publication")
    return str(published.get("html_url", f"https://github.com/{client.repository}/releases/tag/{tag}"))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--artifacts-dir", type=Path, default=ROOT / "release-dist")
    parser.add_argument("--commit")
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY", ""))
    parser.add_argument("--publish", action="store_true", help="Explicit trusted-CI publication; default only prepares/checks local files")
    args = parser.parse_args(argv)
    plan = prepare_release(args.root, args.artifacts_dir, args.commit)
    if not args.publish:
        print(json.dumps({"mode": "dry-run", "release_tag": plan["release_tag"],
                          "source_commit": plan["source_commit"], "assets": plan["assets"]}, indent=2))
        return 0
    trusted_ci(plan["source_commit"])
    client = GitHubReleaseClient(args.repository, os.environ.get("GITHUB_TOKEN", ""))
    print(publish_release(client, args.artifacts_dir, plan))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
