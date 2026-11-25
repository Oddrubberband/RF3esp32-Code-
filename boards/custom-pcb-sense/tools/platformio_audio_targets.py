Import("env")

from pathlib import Path
import subprocess

from SCons.Script import ARGUMENTS

PROJECT_DIR = Path(env["PROJECT_DIR"])
SCRIPT_PATH = PROJECT_DIR / "tools" / "stage_demo_file.py"
PYTHON = env.subst("$PYTHONEXE")


def resolve_partitions_path() -> Path:
    # Keep the SPIFFS fit check aligned with the selected PlatformIO environment.
    try:
        configured = env.GetProjectOption("board_build.partitions")
    except Exception:
        configured = "partitions.csv"

    if not configured:
        configured = "partitions.csv"

    return PROJECT_DIR / configured


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
    if source_arg:
        command.append(source_arg)
    else:
        command.append("--pick")

    if ARGUMENTS.get("output_name"):
        command.extend(["--output-name", ARGUMENTS["output_name"]])

    replace_existing = ARGUMENTS.get("replace_existing_files")
    if replace_existing is None:
        replace_existing = ARGUMENTS.get("replace_existing_audio")
    if str(replace_existing or "").lower() in ("1", "true", "yes", "on"):
        command.append("--replace-existing-files")

    return command


def run_stage_demo_file(source, target, env_):
    print("Staging demo file...")
    result = subprocess.run(build_command(), cwd=str(PROJECT_DIR))
    if result.returncode != 0:
        raise SystemExit(result.returncode)
    return None


env.AddCustomTarget(
    name="stage_demo_file",
    dependencies=None,
    actions=[run_stage_demo_file],
    title="Stage Demo File",
    description="Pick a file, stage it into data/, and verify that the SPIFFS image still fits",
)

env.AddCustomTarget(
    name="prepare_demo_audio",
    dependencies=None,
    actions=[run_stage_demo_file],
    title="Prepare Demo Audio (Legacy Alias)",
    description="Legacy alias for Stage Demo File",
)
