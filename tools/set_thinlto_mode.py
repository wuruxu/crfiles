#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license in LICENSE.

"""Set the crFiles link optimization argument in an existing GN output."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--optimize-max", choices=("true", "false"), required=True)
    parser.add_argument("--gn", type=Path,
                        help="GN executable; regenerate only on mode changes or missing build.ninja")
    args = parser.parse_args()
    args_file = args.out_dir / "args.gn"
    if not args_file.is_file():
        parser.error(f"Missing GN arguments: {args_file}")

    with args_file.open(encoding="utf-8", newline="") as source:
        original = source.read()
    name = "crfiles_thinlto_optimize_max"
    assignment = re.compile(
        rf"^([ \t]*{name}[ \t]*=[ \t]*)(true|false)"
        r"([ \t]*(?:#[^\r\n]*)?)(?=\r?$)", re.MULTILINE)
    updated, count = assignment.subn(
        lambda match: match[1] + args.optimize_max + match[3], original)
    if count > 1:
        parser.error(f"Multiple {name} assignments in {args_file}")
    if count == 0:
        if re.search(rf"^[ \t]*{name}[ \t]*=", original, re.MULTILINE):
            parser.error(f"Expected a boolean {name} assignment in {args_file}")
        newline = "\r\n" if "\r\n" in original else "\n"
        if updated and not updated.endswith("\n"):
            updated += newline
        updated += f"{name} = {args.optimize_max}{newline}"

    changed = updated != original
    if changed:
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(
                    mode="w", encoding="utf-8", newline="",
                    dir=args_file.parent, prefix=".crfiles-args-", delete=False) as output:
                temporary = Path(output.name)
                output.write(updated)
            temporary.chmod(args_file.stat().st_mode & 0o777)
            os.replace(temporary, args_file)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
        print(f"{name} = {args.optimize_max} ({args_file})", flush=True)
    if args.gn and (changed or not (args.out_dir / "build.ninja").is_file()):
        subprocess.run([str(args.gn.resolve()), "gen", str(args.out_dir)], check=True)


if __name__ == "__main__":
    main()
