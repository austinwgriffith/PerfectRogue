#!/usr/bin/env python3
"""Copy and verify all non-system DLL imports for a Windows package."""

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


package = Path(sys.argv[1]).resolve()
mingw_bin = Path(os.environ["PD_MINGW_BIN"])
system_bin = Path(os.environ["SYSTEMROOT"]) / "System32"
imports = re.compile(r"^\s*DLL Name:\s*(\S+)", re.MULTILINE)


def files_by_name(directory):
    return {p.name.lower(): p for p in directory.iterdir() if p.is_file()}


mingw_files = files_by_name(mingw_bin)
system_files = files_by_name(system_bin)
pending = [package / "pd.x86_64.exe"]
checked = set()
while pending:
    binary = pending.pop()
    if binary.name.lower() in checked:
        continue
    checked.add(binary.name.lower())
    output = subprocess.check_output(["objdump", "-p", str(binary)], text=True)
    for name in imports.findall(output):
        key = name.lower()
        if key in system_files or key.startswith(("api-ms-win-", "ext-ms-win-")):
            continue
        packaged = files_by_name(package).get(key)
        if packaged is None:
            source = mingw_files.get(key)
            if source is None:
                raise SystemExit(f"Missing dependency {name}, required by {binary.name}")
            packaged = package / source.name
            shutil.copy2(source, packaged)
            print(f"Bundled {source.name} (required by {binary.name})")
        pending.append(packaged)

print(f"Verified imports for {len(checked)} Windows binaries")
