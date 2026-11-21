"""Run the CPC guest against FujiNet-PC and a local read-only TNFS fixture."""

import argparse
import configparser
from contextlib import contextmanager
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

from boip_fixture import PROGRAM_NAME, demo_program


# The pinned TNFS protocol mounts on the default port even when the URL has a port.
TNFS_PORT = 16384


def ports():
    with socket.socket() as boip, socket.socket() as web:
        for listener in (boip, web):
            listener.bind(("127.0.0.1", 0))
        return tuple(listener.getsockname()[1] for listener in (boip, web))


@contextmanager
def running(*command):
    process = subprocess.Popen(command)
    try:
        yield process
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def wait_ready(process, port):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"The server on port {port} exited early")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise TimeoutError(f"The server did not listen on port {port}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", action="store_true")
    parser.add_argument("headless")
    parser.add_argument("launcher")
    parser.add_argument("runtime")
    parser.add_argument("tnfsd")
    parser.add_argument("output")
    parser.add_argument("roms", nargs="+")
    args = parser.parse_args()
    boip_port, web_port = map(str, ports())

    with tempfile.TemporaryDirectory(prefix="cpc-tnfs-") as temp:
        root = Path(temp) / "tnfs"
        folder = root / "GAMES"
        folder.mkdir(parents=True)
        (folder / "FIRST.BAS").write_text('10 PRINT "TNFS FIXTURE"\n')
        (folder / "SECOND.TXT").write_text("CPC FujiNet TNFS fixture\n")
        if args.program:
            (folder / PROGRAM_NAME).write_bytes(demo_program())
        state = Path(temp) / "runtime"
        state.mkdir()
        settings = configparser.ConfigParser(interpolation=None)
        settings.optionxform = str
        settings.read(Path(args.runtime) / "fnconfig.ini")
        settings["Host2"] = {"type": "TNFS", "name": "127.0.0.1/GAMES"}
        with (state / "fnconfig.ini").open("w") as config:
            settings.write(config, space_around_delimiters=False)
        with running(args.tnfsd, "-r", "-p", str(TNFS_PORT), str(root)) as tnfs:
            wait_ready(tnfs, TNFS_PORT)
            with running(sys.executable, args.launcher, args.runtime, "--data-dir",
                         str(state), "--boip-port", boip_port,
                         "--web-port", web_port) as runtime:
                # Probing the web service leaves the one-client BoIP listener available.
                wait_ready(runtime, int(web_port))
                mode = "--fuji-run" if args.program else "--fuji-live"
                target = f"127.0.0.1/GAMES/{PROGRAM_NAME}" if args.program else ""
                subprocess.run(
                    [args.headless, mode, boip_port, target, args.output, *args.roms],
                    check=True, timeout=35,
                )


if __name__ == "__main__":
    main()
