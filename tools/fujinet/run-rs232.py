#!/usr/bin/env python3
"""Run a FujiNet-PC RS232 distribution with the CPC desktop's user data."""

import argparse
import os
from pathlib import Path
import re
import shutil
import tempfile


def port(value):
    number = int(value)
    if not 1 <= number <= 65535:
        raise argparse.ArgumentTypeError("port must be between 1 and 65535")
    return number


def configure(data_dir, default_config, boip_port):
    data_dir.mkdir(parents=True, exist_ok=True)
    sd = data_dir / "SD"
    sd.mkdir(exist_ok=True)
    config = data_dir / "fnconfig.ini"
    if not config.exists() and default_config.is_file():
        shutil.copyfile(default_config, config)

    original = config.read_text() if config.exists() else ""
    boip = f"[BOIP]\nenabled=1\nhost=127.0.0.1\nport={boip_port}\n\n"
    section = r"(?ims)^\[BOIP\][^\n]*\n?(?:(?!^\[).)*"
    changed, count = re.subn(section, lambda _: boip, original, count=1)
    if count == 0:
        changed = original.rstrip() + "\n\n" + boip if original.strip() else boip
    if changed != original:
        with tempfile.NamedTemporaryFile("w", dir=data_dir, delete=False) as output:
            output.write(changed)
        os.replace(output.name, config)
    return config, sd


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dist", type=Path, help="FujiNet-PC RS232 build/dist directory")
    parser.add_argument("--boip-port", type=port, default=11510)
    parser.add_argument("--web-port", type=port, default=11511)
    parser.add_argument("--data-dir", type=Path)
    args = parser.parse_args()
    if args.boip_port == args.web_port:
        parser.error("BoIP and web ports must be different")

    dist = args.dist.resolve()
    binary = dist / "fujinet"
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error(f"no FujiNet-PC executable at {binary}")
    data_home = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share")
    data = (args.data_dir or data_home / "fujinet-go-amstrad").resolve()
    config, sd = configure(data, dist / "fnconfig.ini", args.boip_port)
    os.chdir(dist)
    os.execv(binary, [str(binary), "-u", f"http://127.0.0.1:{args.web_port}",
                      "-c", str(config), "-s", str(sd)])


if __name__ == "__main__":
    main()
