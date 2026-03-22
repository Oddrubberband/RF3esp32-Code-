Import("env")

from pathlib import Path
import subprocess

from SCons.Script import ARGUMENTS

# This helper script teaches PlatformIO about the project's custom audio-prep
# step. The resulting target shows up in the user interface (UI) as
# "Prepare Demo Audio".
PROJECT_DIR = Path(env["PROJECT_DIR"])
SCRIPT_PATH = PROJECT_DIR / "tools" / "prepare_demo_audio.py"
PYTHON = env.subst("$PYTHONEXE")


def build_command():
    # Build the command line for the standalone audio conversion helper. The
    # command is assembled here so both the default UI flow and optional custom
    # arguments go through exactly the same path.
    command = [
        PYTHON,
        str(SCRIPT_PATH),
        "--data-dir",
        str(PROJECT_DIR / "data"),
        "--partitions",
        str(PROJECT_DIR / "partitions.csv"),
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
    # PlatformIO custom targets execute inside SCons. This function bridges that
    # world to the normal Python helper script and forwards its exit status.
    print("Preparing demo audio...")
    result = subprocess.run(build_command(), cwd=str(PROJECT_DIR))
    if result.returncode != 0:
        raise SystemExit(result.returncode)
    return None


env.AddCustomTarget(
    # The custom target itself does not depend on normal build outputs; it just
    # prepares files in data/ for the later SPIFFS image upload.
    name="prepare_demo_audio",
    dependencies=None,
    actions=[prepare_demo_audio],
    title="Prepare Demo Audio",
    description="Pick an MP3, convert it to SPIFFS-ready PCM, and verify that it fits",
)
