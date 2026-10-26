# PlatformIO pre-script: REBIKO_VERSION from the repository's VERSION file
Import("env")
import os

path = os.path.join(env.subst("$PROJECT_DIR"), "..", "VERSION")
try:
    with open(path) as f:
        version = f.read().strip()
except OSError:
    version = "dev"
env.Append(CPPDEFINES=[("REBIKO_VERSION", env.StringifyMacro(version))])
