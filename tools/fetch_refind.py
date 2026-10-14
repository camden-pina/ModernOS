#!/usr/bin/env python3
"""Download a pinned rEFInd release into the local build cache."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import zipfile

RELEASES = {"0.14.2": "410c7828c4fec2f2179bd956073522415831d27c00416381b8f71153c190a311"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", default="0.14.2", choices=RELEASES)
    parser.add_argument("--cache", type=Path, required=True)
    args = parser.parse_args()

    args.cache.mkdir(parents=True, exist_ok=True)
    archive = args.cache / f"refind-bin-{args.version}.zip"
    temporary = archive.with_suffix(".zip.tmp")
    try:
        if not archive.is_file():
            url = f"https://downloads.sourceforge.net/project/refind/{args.version}/{archive.name}"
            subprocess.run(["curl", "-fL", "--connect-timeout", "20",
                           "-o", str(temporary), url], check=True)
            temporary.replace(archive)

        if hashlib.sha256(archive.read_bytes()).hexdigest() != RELEASES[args.version]:
            raise ValueError(f"SHA-256 mismatch for {archive}; remove it and retry")

        with zipfile.ZipFile(archive) as zipped:
            for name in zipped.namelist():
                destination = (args.cache / name).resolve()
                if not destination.is_relative_to(args.cache.resolve()):
                    raise ValueError("Unsafe path in rEFInd archive")
            zipped.extractall(args.cache)
    except (OSError, ValueError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        print(f"rEFInd preparation failed: {error}", file=sys.stderr)
        return 1
    finally:
        temporary.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
