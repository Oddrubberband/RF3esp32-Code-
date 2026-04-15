Import("env")

from pathlib import Path
import subprocess

from SCons.Script import ARGUMENTS

PROJECT_DIR = Path(env["PROJECT_DIR"])
SCRIPT_PATH = PROJECT_DIR / "tools" / "prepare_demo_audio.py"
PYTHON = env.subst("$PYTHONEXE")


def resolve_partitions_path() -> Path:
    # Keep the audio-fit check aligned with the selected PlatformIO environment.
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

    if ARGUMENTS.get("audio"):
        command.append(ARGUMENTS["audio"])
    else:
        command.append("--pick")

    if ARGUMENTS.get("output_name"):
        command.extend(["--output-name", ARGUMENTS["output_name"]])

    if str(ARGUMENTS.get("replace_existing_audio", "")).lower() in ("1", "true", "yes", "on"):
        command.append("--replace-existing-audio")

    return command


def prepare_demo_audio(source, target, env_):
    print("Preparing demo audio...")
    result = subprocess.run(build_command(), cwd=str(PROJECT_DIR))
    if result.returncode != 0:
        raise SystemExit(result.returncode)
    return None


env.AddCustomTarget(
    name="prepare_demo_audio",
    dependencies=None,
    actions=[prepare_demo_audio],
    title="Prepare Demo Audio",
    description="Pick an MP3, convert it to SPIFFS-ready PCM, and verify that it fits",
)