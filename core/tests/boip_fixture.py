"""Check the CPC directory browser and program loader against an RS232 BoIP peer."""

import argparse
import os
import socket
import struct
import subprocess
import threading


DIRECTORY = "fixture.test:16385/GAMES/ONE TWO"
PROGRAM_NAME = "BORDER.BIN"


def demo_program():
    # Jump into the second mailbox chunk. Check an unmodified binary marker
    # through IY before calling the CPC firmware to set both border inks to 6.
    program = bytearray.fromhex("18 46 0D 0A C0 DB")  # JR +70, then marker bytes
    program.extend(bytes(72 - len(program)))
    for offset, byte in enumerate(program[2:6], start=2):
        program.extend((0x3E, byte, 0xFD, 0xBE, offset, 0xC0))  # LD A,n / CP (IY+n) / RET NZ
    program.extend(bytes.fromhex("01 06 06 CD 38 BC C9"))  # LD BC,&0606 / CALL &BC38 / RET
    return bytes(program)


def checksum(packet):
    total = 0
    for byte in packet:
        total += byte
        total = total % 256 + total // 256
    return total


def read_request(stream):
    start = stream.read(1)
    assert start == b"\xc0", f"unexpected start of FujiBus frame {start!r}"
    packet = bytearray()
    while True:
        byte = stream.read(1)
        if byte == b"\xc0":
            break
        if not byte or len(packet) >= 256:
            raise ValueError("Invalid FujiBus frame")
        if byte == b"\xdb":
            byte = {b"\xdc": b"\xc0", b"\xdd": b"\xdb"}[stream.read(1)]
        packet.extend(byte)
    device, command, length, check, descriptor = struct.unpack_from("<BBHBB", packet)
    packet[4] = 0
    assert (device, length, check) == (0x71, len(packet), checksum(packet)), packet.hex()
    return chr(command), descriptor, bytes(packet[6:])


def send_ack(sock, payload=b""):
    packet = bytearray(struct.pack("<BBHBB", 0x71, 6, 6 + len(payload), 0, 0) + payload)
    packet[4] = checksum(packet)
    encoded = packet.replace(b"\xdb", b"\xdb\xdd").replace(b"\xc0", b"\xdb\xdc")
    # Deliberately split every frame across writes.
    for part in (b"\xc0" + encoded[:3], encoded[3:] + b"\xc0"):
        if os.write(sock.fileno(), part) != len(part):
            raise OSError("Short fixture write")


def expect(stream, expected):
    received = read_request(stream)
    assert received == expected, f"expected {expected!r}, received {received!r}"


def serve(sock, program):
    if program:
        data = demo_program().ljust(2048, b"\0")
        target = f"{DIRECTORY}/{PROGRAM_NAME}"
        access, limit = 4, 63
    else:
        data = b"FIRST.BAS\r\nDIRECTORY/\x9bSECOND-FILE.BIN\nTHIRD.TXT\r\n"
        target = DIRECTORY + "/"
        access, limit = 6, 31

    with sock, os.fdopen(sock.fileno(), "rb", closefd=False) as stream:
        expect(stream, ("C", 0, b""))
        send_ack(sock)
        expect(stream, ("O", 2, bytes((access, 0)) + f"N1:TNFS://{target}\0".encode("ascii")))
        send_ack(sock)
        while data:
            expect(stream, ("S", 0, b""))
            send_ack(sock, struct.pack("<HBB", len(data), 1, 1))
            length = min(limit, len(data))
            expect(stream, ("R", 5, struct.pack("<H", length)))
            send_ack(sock, data[:length])
            data = data[length:]
        expect(stream, ("S", 0, b""))
        send_ack(sock, struct.pack("<HBB", 0, 0, 0x88))
        expect(stream, ("C", 0, b""))
        send_ack(sock)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", action="store_true")
    parser.add_argument("headless")
    parser.add_argument("output")
    parser.add_argument("roms", nargs="+")
    args = parser.parse_args()

    host, guest = socket.socketpair()
    errors = []

    def fixture():
        try:
            serve(host, args.program)
        except Exception as exc:
            errors.append(exc)

    thread = threading.Thread(target=fixture)
    thread.start()
    try:
        with guest:
            mode = "--fuji-run-fixture" if args.program else "--fuji-fixture"
            target = f"{DIRECTORY}/{PROGRAM_NAME}" if args.program else DIRECTORY
            process = subprocess.run(
                [args.headless, mode, str(guest.fileno()), target,
                 args.output, *args.roms], pass_fds=(guest.fileno(),), timeout=35,
            )
    finally:
        thread.join()
    if errors:
        raise SystemExit(f"BoIP fixture failed: {errors[0]!r}")
    raise SystemExit(process.returncode)


if __name__ == "__main__":
    main()
