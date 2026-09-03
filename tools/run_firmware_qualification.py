"""Build/run the native software qualification and retain measured evidence."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import time
import zlib
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXPECTED_RUNS = {
    "large": 1,
    "targeted": 6,
    "reliability": 100,
    "corruption": 28,
    "corruption_recovery": 28,
    "maximum": 1,
    "stress": 100,
}


def file_metrics(path: Path) -> tuple[int, int, str]:
    size, crc = 0, 0
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            size += len(chunk)
            crc = zlib.crc32(chunk, crc)
            sha.update(chunk)
    return size, crc, sha.hexdigest()


def independent_checks(records: list[dict], output: Path, fixture: Path) -> list[str]:
    """Cross-check production CRC with zlib, including every final disk file."""
    failures = []
    for category, expected in EXPECTED_RUNS.items():
        actual = sum(r["category"] == category for r in records)
        if actual != expected:
            failures.append(f"{category}: executed {actual}/{expected} transfers")
    if len(records) != sum(EXPECTED_RUNS.values()):
        failures.append("unexpected total record count")
    fixture_size, fixture_crc, _ = file_metrics(fixture)
    for record in records:
        if record["category"] == "large":
            if (fixture_size, fixture_crc) != (record["expected_bytes"], record["source_crc32"]):
                record["failures"].append("independent source zlib CRC/size")
        final = output / record["output"]
        if record["expected_rejection"]:
            if final.exists() or Path(str(final) + ".part").exists():
                record["failures"].append("rejected transfer left final or partial file")
        elif final.is_file():
            size, crc, _ = file_metrics(final)
            record["independent_crc32"] = crc
            if (size, crc) != (record["expected_bytes"], record["source_crc32"]):
                record["failures"].append("independent published file zlib CRC/size")
            if record["category"] == "large":
                with fixture.open("rb") as source, final.open("rb") as destination:
                    while True:
                        a, b = source.read(65536), destination.read(65536)
                        if a != b:
                            record["failures"].append("independent song byte comparison")
                            break
                        if not a:
                            break
        else:
            record["failures"].append("missing final output")
        record["passed"] = record["passed"] and not record["failures"]
    if list(output.rglob("*.part")):
        failures.append("partial files remain after campaign")
    return failures


def render_report(records: list[dict], failures: list[str], elapsed: float) -> tuple[str, bool]:
    lines = ["RF3 FIRMWARE QUALIFICATION", "==========================",
             "SOFTWARE ONLY - simulated packets + host files; NO RF/hardware validation.", ""]

    def group(category: str) -> list[dict]:
        return [r for r in records if r["category"] == category]

    def ok(category: str) -> bool:
        rows = group(category)
        return len(rows) == EXPECTED_RUNS[category] and all(r["passed"] for r in rows)

    def total(rows: list[dict], key: str) -> int:
        return sum(r.get(key, 0) for r in rows)

    def result(passed: bool) -> str:
        return "PASS" if passed else "FAIL"

    for category, title in [("large", "Large file integrity (song.u8)"), ("maximum", "Maximum transfer")]:
        rows = group(category)
        lines.append(f"{title}: {result(ok(category))}")
        if rows:
            r = rows[0]
            lines.extend([
                f"  Bytes TX / ACK / RX:      {r['tx_data_bytes']:,} / {r['acked_bytes']:,} / {r['accepted_bytes']:,}",
                f"  DATA TX / ACK / accepted: {r['tx_data_packets']:,} / {r['acked_packets']:,} / {r['accepted_packets']:,}",
                f"  CRC32 source / disk:     {r['source_crc32']:08X} / {r['persisted_crc32']:08X}",
                f"  Byte errors / publishes: {r['byte_errors']} / {r['publishes']}",
                f"  Sender / receiver:       {r['sender_state']} / {r['receiver_state']}",
            ])
            if category == "maximum":
                lines.append(f"  Last DATA / next seq:    {r['last_accepted_sequence']:,} / {r['receiver_next_sequence']:,}")
        lines.append("")

    rows = group("targeted")
    lines.append(f"Targeted loss recovery: {result(ok('targeted'))}")
    for r in rows:
        lines.append(f"  {r['name']:<9} drops={r['drops']} retries={r['retries']} {result(r['passed'])}")
    rows = group("reliability")
    completed = sum(r["sender_state"] == "Completed" and r["receiver_state"] == "Completed" for r in rows)
    integrity_failures = sum(r["byte_errors"] != 0 or r["persisted_crc32"] != r["source_crc32"] for r in rows)
    lines.extend(["", f"Deterministic loss campaign: {result(ok('reliability'))}",
                  f"  Attempted / completed / failed: {len(rows)} / {completed} / {len(rows) - completed}",
                  f"  Drops / retries:         {total(rows, 'drops'):,} / {total(rows, 'retries'):,}",
                  f"  Verified payload bytes:  {sum(r['accepted_bytes'] for r in rows if r['passed']):,}",
                  f"  Final integrity errors:  {integrity_failures}",
                  f"  Worst retries/transfer:  {max((r['retries'] for r in rows), default=0)}",
                  f"  Worst burst/exchange:    {max((r['max_retry_burst'] for r in rows), default=0)} / 5 allowed", ""])
    rows = group("corruption")
    detected = sum(r["sender_error"] == 16 and r["receiver_error"] == 16 and
                   r["sender_state"] == "Failed" and r["receiver_state"] == "Failed" for r in rows)
    lines.extend([f"Corruption rejection: {result(ok('corruption') and ok('corruption_recovery'))}",
                  f"  Injected / CRC detected: {total(rows, 'corruptions')} / {detected}",
                  f"  Invalid final publishes: {total(rows, 'publishes')}",
                  f"  Partials removed:        {total(rows, 'cleanup_calls')} / {len(rows)}",
                  f"  Subsequent clean probes: {sum(r['passed'] for r in group('corruption_recovery'))} / 28", ""])
    rows = group("stress")
    lines.extend([f"Back-to-back stress (same sessions): {result(ok('stress'))}",
                  f"  Successful transfers:    {sum(r['passed'] for r in rows)} / 100",
                  f"  Verified payload bytes:  {sum(r['accepted_bytes'] for r in rows if r['passed']):,}",
                  f"  Stale frames ignored:    {total(rows, 'stale_frames_ignored')}",
                  f"  Prior output byte errors: {total(rows, 'preserved_output_errors')}", ""])
    all_ok = not failures and all(ok(category) for category in EXPECTED_RUNS)
    lines.extend(["Seed base: 0x52463332; exact fault rules/seeds in transfers.jsonl.",
                  f"Host campaign + independent verification: {elapsed:.2f} s (NOT RF throughput).",
                  f"OVERALL RESULT: {result(all_ok)}"])
    for r in records:
        for failure in r["failures"]:
            lines.append(f"  FAIL {r['category']}/{r['name']}: {failure}")
    lines.extend(f"  FAIL {failure}" for failure in failures)
    return "\n".join(lines) + "\n", all_ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"), help="GCC/Clang C++17 compiler executable")
    parser.add_argument("--fixture", type=Path, default=ROOT / "data/song.u8")
    args = parser.parse_args()
    root = ROOT / ".pio/qualification"
    root.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="run-", dir=root))
    records: list[dict] = []
    failures: list[str] = []
    evidence = {"scope": "software-only; no RF or hardware validation",
                "created_utc": datetime.now(timezone.utc).isoformat(),
                "host": platform.platform(), "python": sys.version, "seed_base": "0x52463332"}
    began = time.monotonic()
    try:
        compiler = shutil.which(args.cxx)
        if compiler is None:
            raise RuntimeError(f"C++ compiler unavailable: {args.cxx}; install GCC/Clang or supply --cxx")
        executable = output / ("qualification.exe" if os.name == "nt" else "qualification")
        command = [compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-I", str(ROOT / "include"), "-I", str(ROOT / "test/include"),
                   str(ROOT / "test/qualification/main.cpp"), "-o", str(executable)]
        evidence["compiler"] = subprocess.check_output([compiler, "--version"], text=True).splitlines()[0]
        evidence["compile_command"] = command
        evidence["git_head"] = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
        evidence["git_status"] = subprocess.check_output(["git", "status", "--short"], cwd=ROOT, text=True)
        inputs = [ROOT / "test/qualification/main.cpp", ROOT / "test/include/protocol_v2_fake_transport.hpp",
                  ROOT / "include/protocol_v2.hpp", ROOT / "include/reliable_transfer_v2.hpp",
                  ROOT / "include/file_transfer_service.hpp", Path(__file__).resolve(), args.fixture.resolve()]
        evidence["input_sha256"] = {os.path.relpath(p, ROOT): file_metrics(p)[2] for p in inputs}
        build = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=120)
        (output / "build.log").write_text(build.stdout + build.stderr, encoding="utf-8")
        if build.returncode:
            raise RuntimeError(f"qualification compile failed ({build.returncode}); see build.log\n{build.stderr}")
        began = time.monotonic()
        run = subprocess.run([str(executable), str(args.fixture.resolve()), str(output)],
                             cwd=ROOT, capture_output=True, text=True, timeout=180)
        (output / "execution.log").write_text(run.stdout + run.stderr, encoding="utf-8")
        evidence["executable_exit_code"] = run.returncode
        raw = output / "transfers.jsonl"
        if raw.exists():
            records = [json.loads(line) for line in raw.read_text(encoding="utf-8").splitlines()]
        if run.returncode:
            failures.append(f"qualification executable exited {run.returncode}; see execution.log")
        failures.extend(independent_checks(records, output, args.fixture))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        failures.append(str(error))
    report, passed = render_report(records, failures, time.monotonic() - began)
    evidence.update(passed=passed, failures=failures, transfers=records)
    (output / "report.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    (output / "report.txt").write_text(report, encoding="utf-8")
    print(report, end="")
    print(f"Evidence: {output.relative_to(ROOT)}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
