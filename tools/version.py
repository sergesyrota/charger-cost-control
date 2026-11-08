# PlatformIO pre-build hook: injects -DFW_VERSION="<git describe>" (fallback "0.0.0-dev").
# Owner: integrator.
import subprocess

Import("env")  # noqa: F821


def _version():
    try:
        v = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=env.subst("$PROJECT_DIR"), stderr=subprocess.DEVNULL,  # noqa: F821
        ).decode().strip()
        return v[1:] if v.startswith("v") else v
    except Exception:
        return "0.0.0-dev"


VERSION = _version() or "0.0.0-dev"
env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(VERSION))])  # noqa: F821
print("FW_VERSION =", VERSION)
