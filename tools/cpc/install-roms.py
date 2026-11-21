#!/usr/bin/env python3
"""Install CPC 464 OS and BASIC firmware for the native FujiNet Go app."""

import argparse
import hashlib
from http.client import HTTPException
import os
from pathlib import Path
import tempfile
from urllib.request import urlopen


ROM_SIZE = 16 * 1024
IMAGE_SIZE = 2 * ROM_SIZE
CAPRICE32_ROM = (
    "https://raw.githubusercontent.com/ColinPitrat/caprice32/"
    "66a4a7af9125d2f20111493d13537db9f9cb6ff5/rom/cpc464.rom"
)
ROM_SHA256 = "00960d9bf75b2b90856c970f1aa078e1e2aa028b2c104f1dded0262f5d37b15e"


def known_rom(image):
    return len(image) == IMAGE_SIZE and hashlib.sha256(image).hexdigest() == ROM_SHA256


def download_rom():
    with urlopen(CAPRICE32_ROM, timeout=30) as response:
        image = response.read(IMAGE_SIZE + 1)
    if not known_rom(image):
        raise ValueError("The downloaded ROM did not match the expected size and SHA-256")
    return image


def read_rom(path, size):
    with path.open("rb") as source:
        image = source.read(size + 1)
    if len(image) != size:
        raise ValueError(f"Expected a {size // 1024} KiB ROM at {path}")
    return image


def install_rom(destination, image, replace):
    destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as output:
        temporary = Path(output.name)
        try:
            output.write(image)
            output.close()
            if replace:
                temporary.replace(destination)
            else:
                # Publish the completed file without replacing a concurrent import.
                os.link(temporary, destination)
        finally:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "roms", metavar="ROM", nargs="*", type=Path,
        help="a combined ROM or two files in OS, BASIC order (default: download from Caprice32)",
    )
    parser.add_argument("--replace", action="store_true", help="replace an existing different ROM")
    args = parser.parse_args()
    if len(args.roms) > 2:
        parser.error("provide one combined ROM or two ROMs in OS, BASIC order")

    data_home = Path(os.environ.get("XDG_DATA_HOME") or (Path.home() / ".local" / "share"))
    destination = data_home / "fujinet-go-amstrad" / "cpc464.rom"
    conflict = f"A different ROM already exists at {destination}. Use --replace to overwrite it."
    try:
        try:
            with destination.open("rb") as installed:
                current = installed.read(IMAGE_SIZE + 1)
        except FileNotFoundError:
            current = None

        if args.roms:
            size = IMAGE_SIZE if len(args.roms) == 1 else ROM_SIZE
            image = b"".join(read_rom(path, size) for path in args.roms)
        elif current is not None and known_rom(current):
            image = current
        elif current is not None and not args.replace:
            raise ValueError(conflict)
        else:
            image = download_rom()

        if current != image:
            if current is not None and not args.replace:
                raise ValueError(conflict)
            install_rom(destination, image, args.replace)
    except FileExistsError:
        parser.exit(1, f"{conflict}\n")
    except (OSError, HTTPException, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(destination)


if __name__ == "__main__":
    main()
