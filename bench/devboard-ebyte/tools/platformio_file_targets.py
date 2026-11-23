Import("env")

from pathlib import Path
import subprocess

from SCons.Script import ARGUMENTS

PROJECT_DIR = Path(env["PROJECT_DIR"])
SCRIPT_PATH = PROJECT_DIR / "tools" / "stage_demo_file.py"
PYTHON = env.subst("$PYTHONEXE")


def resolve_partitions_path() -> Path:
    try:
        configured = env.GetProjectOption("board_build.partitions")
    except Exception:
        configured = "partitions.csv"
    return PROJECT_DIR / (configured or "partitions.csv")


def build_command():
    command = [
        PYTHON,
        str(SCRIPT_PATH),
        "--data-dir",
        str(PROJECT_DIR / "data"),
        "--partitions",
        str(resolve_partitions_path()),
    ]

    source_arg = ARGUMENTS.get("file") or ARGUMENTS.get("audio")
    command.append(source_arg if source_arg else "--pick")

    if ARGUMENTS.get("output_name"):
        command.extend(["--output-name", ARGUMENTS["output_name"]])

    replace_existing = ARGUMENTS.get("replace_existing_files")
    if replace_existing is None:
        replace_existing = ARGUMENTS.get("replace_existing_audio")
    if str(replace_existing or "").lower() in ("1", "true", "yes", "on"):
        command.append("--replace-existing-files")
    return command


def run_stage_demo_file(source, target, env_):
    print("Staging transfer file...")
    result = subprocess.run(build_command(), cwd=str(PROJECT_DIR))
    if result.returncode != 0:
        raise SystemExit(result.returncode)
    return None


env.AddCustomTarget(
    name="stage_demo_file",
    dependencies=None,
    actions=[run_stage_demo_file],
    title="Stage Transfer File",
    description="Stage a file into data/ and verify that the selected SPIFFS image fits",
)

env.AddCustomTarget(
    name="prepare_demo_audio",
    dependencies=None,
    actions=[run_stage_demo_file],
    title="Prepare Demo Audio (Legacy Alias)",
    description="Legacy alias for Stage Transfer File",
)
