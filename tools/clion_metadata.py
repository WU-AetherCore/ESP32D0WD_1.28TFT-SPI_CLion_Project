"""Write PlatformIO's real include paths and flags for CLion code indexing."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
env = dict(os.environ, PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
result = subprocess.run(
    [sys.executable, "-m", "platformio", "project", "metadata", "-e",
     "esp32d0wd", "--json-output"],
    cwd=root,
    env=env,
    capture_output=True,
    text=True,
    encoding="utf-8",
)
if result.returncode:
    print(result.stdout, result.stderr, file=sys.stderr)
    raise SystemExit(result.returncode)

data = json.loads(result.stdout)["esp32d0wd"]
includes = list(data.get("includes", {}).get("build", []))
raw_flags = data.get("cxx_flags", [])
flags = shlex.split(raw_flags) if isinstance(raw_flags, str) else list(raw_flags)
for index, flag in enumerate(flags[:-1]):
    if flag == "-include" and not Path(flags[index + 1]).is_absolute():
        flags[index + 1] = (root / flags[index + 1]).as_posix()

def cmake_list(name, values):
    lines = [f"set({name}"]
    lines.extend(
        '  "' + str(value).replace("\\", "/").replace('"', '\\"').replace(";", "\\;") + '"'
        for value in dict.fromkeys(values)
    )
    lines.append(")")
    return "\n".join(lines) + "\n"

Path(sys.argv[1]).write_text(
    cmake_list("PIO_INCLUDES", includes)
    + cmake_list("PIO_DEFINES", data.get("defines", []))
    + cmake_list("PIO_CXX_FLAGS", flags),
    encoding="utf-8",
)
