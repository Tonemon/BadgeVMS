#!/usr/bin/env python3
"""Rename ROM files in a directory to be VMS-path-safe.

VMS paths allow only: A-Z  a-z  0-9  -  _  $  .
Everything else is stripped; spaces become underscores.

Usage:
    python3 sanitize_rom_names.py <directory> [--dry-run]

Options:
    --dry-run   Print what would be renamed without doing it.
"""

import argparse
import os
import re
import sys


VALID = re.compile(r"[^A-Za-z0-9\-_$.]")


def sanitize(name: str) -> str:
    stem, _, ext = name.rpartition(".")
    if not stem:
        # No extension — treat whole name as stem
        stem, ext = name, ""

    stem = stem.replace(" ", "_")
    stem = VALID.sub("", stem)

    if ext:
        ext = ext.replace(" ", "_")
        ext = VALID.sub("", ext)
        return f"{stem}.{ext}"
    return stem


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Rename ROM files to be VMS-path-safe."
    )
    parser.add_argument("directory", help="Directory containing ROM files")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print renames without applying them",
    )
    args = parser.parse_args()

    directory = args.directory
    if not os.path.isdir(directory):
        print(f"Error: '{directory}' is not a directory", file=sys.stderr)
        sys.exit(1)

    renames = []
    for name in sorted(os.listdir(directory)):
        src = os.path.join(directory, name)
        if not os.path.isfile(src):
            continue
        new_name = sanitize(name)
        if new_name != name:
            renames.append((src, os.path.join(directory, new_name), name, new_name))

    if not renames:
        print("Nothing to rename.")
        return

    for src, dst, old, new in renames:
        print(f"  {old!r}  →  {new!r}")
        if not args.dry_run:
            if os.path.exists(dst):
                print(f"    SKIP: '{new}' already exists")
                continue
            os.rename(src, dst)

    if args.dry_run:
        print(f"\n{len(renames)} file(s) would be renamed (dry run — nothing changed)")
    else:
        print(f"\n{len(renames)} file(s) renamed")


if __name__ == "__main__":
    main()
