#!/usr/bin/env python3
"""Emit the small CPC background ROM and its C header without a cross assembler."""

import argparse
from pathlib import Path
import re
import textwrap


ORIGIN = 0xC000
ROM_SIZE = 0x4000
TXT_OUTPUT = 0xBB5A
KM_WAIT_CHAR = 0xBB06
TARGET_CANCELLED = 0xFF


class Rom:
    def __init__(self):
        self.data = bytearray()
        self.labels = {}
        self.fixups = []

    def emit(self, *values):
        self.data.extend(values)

    def label(self, name):
        self.labels[name] = ORIGIN + len(self.data)

    def word(self, value):
        if isinstance(value, str):
            self.fixups.append((len(self.data), value))
            self.emit(0, 0)
        else:
            self.data.extend(value.to_bytes(2, "little"))

    def jump(self, target, opcode=0xC3):
        self.emit(opcode)
        self.word(target)

    def port(self, address):
        self.emit(0x01)  # LD BC,nn
        self.word(address)

    def read(self, address):
        self.port(address)
        self.emit(0xED, 0x78)  # IN A,(C)

    def write(self, address, value):
        self.port(address)
        self.emit(0x3E, value, 0xED, 0x79)  # LD A,n / OUT (C),A

    def print_string(self, label):
        self.emit(0x21)  # LD HL,nn
        self.word(label)
        self.jump("print_string", 0xCD)  # CALL

    def finish(self):
        for offset, name in self.fixups:
            self.data[offset:offset + 2] = self.labels[name].to_bytes(2, "little")
        if len(self.data) > ROM_SIZE:
            raise ValueError("FujiNet ROM exceeds its 16 KiB bank")
        return bytes(self.data)


def build_rom(c):
    rom = Rom()
    rom.emit(1, 1, 0, 0)  # Background ROM, mark 1, version 0.0
    rom.word("names")
    rom.jump("init")
    rom.jump("fuji")
    rom.jump("fuji_run")

    rom.label("names")
    rom.emit(*b"FUJINET INI", ord("T") | 0x80, *b"FUJ", ord("I") | 0x80,
             *b"FUJIRU", ord("N") | 0x80, 0)

    rom.label("init")
    rom.read(c["FUJI_PORT_ID"])
    rom.emit(0xFE, c["FUJI_LIVE_ID"])  # CP live
    rom.jump("init_ok", 0xC2)
    rom.emit(0xE5, 0x01)  # PUSH HL / LD BC,nn
    rom.word(c["CPC_FUJI_PROGRAM_LIMIT"])
    rom.emit(0xB7, 0xED, 0x42)  # OR A / SBC HL,BC (reserve from the top of the pool)
    rom.jump("init_no_memory", 0xDA)
    rom.emit(0xE5, 0xED, 0x52, 0xE1)  # PUSH HL / SBC HL,DE / POP HL
    rom.jump("init_no_memory", 0xDA)
    rom.emit(0xC1)  # POP BC (discard the old upper bound)
    rom.label("init_ok")
    rom.emit(0x37, 0xC9)  # SCF / RET
    rom.label("init_no_memory")
    rom.emit(0xE1, 0xB7, 0xC9)  # POP HL / OR A / RET

    rom.label("fuji")
    rom.read(c["FUJI_PORT_ID"])
    rom.emit(0xFE, c["FUJI_LIVE_ID"])  # CP n
    rom.jump("config", 0xCA)  # JP Z
    rom.emit(0xFE, c["FUJI_ID"])  # CP n
    rom.jump("missing", 0xC2)  # JP NZ
    rom.write(c["FUJI_PORT_STATUS"], c["FUJI_CLEAR"])
    rom.write(c["FUJI_PORT_SIZE"], 2)
    rom.write(c["FUJI_PORT_OFFSET"], 0)
    rom.write(c["FUJI_PORT_DATA"], c["CPC_FUJI_PING"])
    rom.write(c["FUJI_PORT_OFFSET"], 1)
    rom.write(c["FUJI_PORT_DATA"], c["CPC_FUJI_PING_DATA"])
    rom.write(c["FUJI_PORT_STATUS"], c["FUJI_SUBMIT"])
    rom.jump("wait_ready", 0xCD)
    rom.jump("failed", 0xC2)  # JP NZ
    rom.print_string("prefix")
    rom.emit(0x16, 0)  # LD D,0 (start of payload)
    rom.jump("print_reply", 0xCD)
    rom.print_string("newline")
    rom.jump("clear")

    rom.label("wait_ready")
    rom.port(c["FUJI_PORT_STATUS"])
    rom.emit(0x26, 8, 0x11, 0, 0)  # LD H,8 / LD DE,0. Allow 8 * 65536 polls.
    rom.label("poll")
    rom.emit(0xED, 0x78, 0xFE, c["FUJI_BUSY"])  # IN A,(C) / CP busy
    rom.jump("finished", 0xC2)  # JP NZ
    rom.emit(0x1B, 0x7A, 0xB3)  # DEC DE / LD A,D / OR E
    rom.jump("poll", 0xC2)  # JP NZ
    rom.emit(0x25)  # DEC H
    rom.jump("poll", 0xC2)
    rom.emit(0x3E, c["FUJI_BUSY"])  # Return NZ from the READY comparison.

    rom.label("finished")
    rom.emit(0xFE, c["FUJI_READY"], 0xC9)  # CP ready / RET

    rom.label("print_reply")
    rom.read(c["FUJI_PORT_SIZE"])
    rom.emit(0x92, 0xC8, 0xD8, 0x5F)  # SUB D / RET Z / RET C / LD E,A (count)

    rom.label("reply")
    rom.port(c["FUJI_PORT_OFFSET"])
    rom.emit(0x7A, 0xED, 0x79)  # LD A,D / OUT (C),A
    rom.read(c["FUJI_PORT_DATA"])
    rom.emit(0xD5)  # PUSH DE
    rom.jump(TXT_OUTPUT, 0xCD)
    rom.emit(0xD1, 0x14, 0x1D)  # POP DE / INC D / DEC E
    rom.jump("reply", 0xC2)  # JP NZ
    rom.emit(0xC9)  # RET

    build_config(rom, c)
    build_loader(rom, c)

    rom.label("missing")
    rom.print_string("absent")
    rom.emit(0xC9)  # RET

    rom.label("failed")
    rom.print_string("error")
    rom.label("clear")
    rom.write(c["FUJI_PORT_STATUS"], c["FUJI_CLEAR"])
    rom.emit(0xC9)  # RET

    rom.label("print_string")
    rom.emit(0x7E, 0xB7, 0xC8, 0x23, 0xE5)  # LD A,(HL) / OR A / RET Z / INC HL / PUSH HL
    rom.jump(TXT_OUTPUT, 0xCD)
    rom.emit(0xE1)  # POP HL
    rom.jump("print_string")

    for name, value in (
        ("prefix", b"FujiNet: "),
        ("newline", b"\r\n"),
        ("absent", b"FujiNet card not found\r\n"),
        ("error", b"FujiNet mailbox error\r\n"),
        ("host_prompt", b"FujiNet CONFIG\r\nEnter opens the first saved TNFS host.\r\nType @2-@8 or a host/path. Esc exits.\r\n"),
        ("browsing", b"Browsing "),
        ("erase", b"\x08 \x08"),
        ("directory_end", b"\r\nEnd of directory\r\n"),
        ("directory_error", b"\r\nUnable to list TNFS directory\r\n"),
        ("file_prompt", b"FujiNet RUN\r\nTNFS host/path/file (Esc cancels):\r\n"),
        ("file_error", b"\r\nUnable to load TNFS program\r\n"),
        ("file_unavailable", b"FujiNet RUN requires a live connection\r\n"),
    ):
        rom.label(name)
        rom.emit(*value, 0)
    return rom.finish()


def build_config(rom, c):
    rom.label("config")
    rom.print_string("host_prompt")
    rom.emit(0x3E, c["CPC_FUJI_DIR_OPEN"])  # LD A,command
    rom.jump("read_target", 0xCD)
    rom.emit(0xFE, TARGET_CANCELLED, 0xC8)  # CP cancelled / RET Z
    rom.emit(0xFE, c["CPC_FUJI_RESULT_OK"])
    rom.jump("directory_failed", 0xC2)
    rom.print_string("browsing")
    rom.emit(0x16, 1)  # LD D,1 (skip result byte)
    rom.jump("print_reply", 0xCD)
    rom.print_string("newline")

    rom.label("next_directory")
    rom.emit(0x3E, c["CPC_FUJI_DIR_NEXT"])  # LD A,command
    rom.jump("next_chunk", 0xCD)
    rom.emit(0xFE, c["CPC_FUJI_RESULT_END"])
    rom.jump("end_directory", 0xCA)
    rom.emit(0xFE, c["CPC_FUJI_RESULT_OK"])
    rom.jump("directory_failed", 0xC2)
    rom.emit(0x16, 1)  # LD D,1 (skip result byte)
    rom.jump("print_reply", 0xCD)
    rom.jump("next_directory")

    rom.label("end_directory")
    rom.print_string("directory_end")
    rom.jump("clear")

    rom.label("directory_failed")
    rom.print_string("directory_error")
    rom.jump("config")

    rom.label("next_chunk")
    rom.emit(0xF5)  # PUSH AF (command)
    rom.jump("clear", 0xCD)
    rom.write(c["FUJI_PORT_SIZE"], 1)
    rom.emit(0xF1)  # POP AF
    rom.port(c["FUJI_PORT_DATA"])
    rom.emit(0xED, 0x79)  # OUT (C),A (offset is zero after clear)
    rom.jump("submit_request")

    rom.label("read_target")
    rom.emit(0xF5)  # PUSH AF (command)
    rom.jump("clear", 0xCD)
    rom.emit(0xF1)  # POP AF
    rom.emit(0xF5)  # PUSH AF (retain the command until Enter or Escape)
    rom.port(c["FUJI_PORT_DATA"])
    rom.emit(0xED, 0x79)  # OUT (C),A
    rom.emit(0x16, 1)  # LD D,1 (next offset)

    rom.label("host_key")
    rom.emit(0xD5)  # PUSH DE
    rom.jump(KM_WAIT_CHAR, 0xCD)
    rom.emit(0xD1, 0xFE, 0x0D)  # POP DE / CP Enter
    rom.jump("target_enter", 0xCA)
    rom.emit(0xFE, 0xFC)  # CP the firmware's default Escape token
    rom.jump("target_cancel", 0xCA)
    rom.emit(0xFE, 0x7F)  # CP Delete
    rom.jump("erase_host_key", 0xCA)
    rom.emit(0xFE, 0x20)  # CP first printable
    rom.jump("host_key", 0xDA)
    rom.emit(0xFE, 0x7F)  # CP last printable + 1
    rom.jump("host_key", 0xD2)
    rom.emit(0x5F, 0x7A, 0xFE, c["CPC_FUJI_CAPACITY"])  # LD E,A / LD A,D / CP capacity
    rom.jump("host_key", 0xD2)
    rom.port(c["FUJI_PORT_OFFSET"])
    rom.emit(0xED, 0x79, 0x7B)  # OUT (C),A / LD A,E
    rom.port(c["FUJI_PORT_DATA"])
    rom.emit(0xED, 0x79, 0x14, 0xD5)  # OUT (C),A / INC D / PUSH DE
    rom.jump(TXT_OUTPUT, 0xCD)
    rom.emit(0xD1)  # POP DE
    rom.jump("host_key")

    rom.label("erase_host_key")
    rom.emit(0x7A, 0xFE, 1)  # LD A,D / CP 1
    rom.jump("host_key", 0xCA)
    rom.emit(0x15, 0xD5)  # DEC D / PUSH DE
    rom.print_string("erase")
    rom.emit(0xD1)  # POP DE
    rom.jump("host_key")

    rom.label("target_enter")
    rom.emit(0xF1, 0xFE, c["CPC_FUJI_DIR_OPEN"])  # POP AF / CP directory open
    rom.jump("target_submit", 0xCA)
    rom.emit(0x7A, 0xFE, 1)  # LD A,D / CP 1
    rom.jump("target_empty", 0xCA)
    rom.label("target_submit")
    rom.emit(0x7A)  # LD A,D
    rom.port(c["FUJI_PORT_SIZE"])
    rom.emit(0xED, 0x79)  # OUT (C),A
    rom.print_string("newline")
    rom.jump("submit_request")

    rom.label("target_cancel")
    rom.emit(0xF1)  # POP AF (command)
    rom.label("target_empty")
    rom.print_string("newline")
    rom.jump("clear", 0xCD)
    rom.emit(0x3E, TARGET_CANCELLED, 0xC9)  # LD A,cancelled / RET

    rom.label("submit_request")
    rom.write(c["FUJI_PORT_STATUS"], c["FUJI_SUBMIT"])
    rom.jump("wait_ready", 0xCD)
    rom.emit(0x3E, c["CPC_FUJI_RESULT_ERROR"], 0xC0)  # LD A,error / RET NZ
    rom.read(c["FUJI_PORT_DATA"])
    rom.emit(0xC9)  # RET


def build_loader(rom, c):
    rom.label("fuji_run")
    rom.read(c["FUJI_PORT_ID"])
    rom.emit(0xFE, c["FUJI_LIVE_ID"])  # CP live
    rom.jump("run_unavailable", 0xC2)
    rom.print_string("file_prompt")
    rom.emit(0x3E, c["CPC_FUJI_FILE_OPEN"])  # LD A,command
    rom.jump("read_target", 0xCD)
    rom.emit(0xFE, TARGET_CANCELLED, 0xC8)  # CP cancelled / RET Z
    rom.emit(0xFE, c["CPC_FUJI_RESULT_OK"])
    rom.jump("load_failed", 0xC2)
    rom.emit(0xFD, 0xE5, 0xE1, 0x11)  # PUSH IY / POP HL / LD DE,nn
    rom.word(c["CPC_FUJI_PROGRAM_LIMIT"])

    # HL is the write cursor. DE counts bytes remaining in the reserved area.
    rom.label("load_chunk")
    rom.emit(0xE5, 0xD5, 0x3E, c["CPC_FUJI_FILE_NEXT"])  # PUSH HL / PUSH DE / LD A,command
    rom.jump("next_chunk", 0xCD)
    rom.emit(0xD1, 0xE1, 0xFE, c["CPC_FUJI_RESULT_END"])  # POP DE / POP HL / CP end
    rom.jump("run_loaded", 0xCA)
    rom.emit(0xFE, c["CPC_FUJI_RESULT_OK"])
    rom.jump("load_failed", 0xC2)
    rom.read(c["FUJI_PORT_SIZE"])
    rom.emit(0xFE, 2)  # CP minimum size (status plus at least one byte)
    rom.jump("load_failed", 0xDA)
    rom.emit(0x3D, 0x4F, 0x06, 0, 0xEB, 0xB7, 0xED, 0x42, 0xEB)
    # DEC A / LD C,A / LD B,0 / EX DE,HL / OR A / SBC HL,BC / EX DE,HL
    rom.jump("load_failed", 0xDA)
    rom.emit(0xD5, 0x59, 0x16, 1)  # PUSH DE / LD E,C / LD D,1 (skip status byte)

    rom.label("copy_byte")
    rom.port(c["FUJI_PORT_OFFSET"])
    rom.emit(0x7A, 0xED, 0x79)  # LD A,D / OUT (C),A
    rom.read(c["FUJI_PORT_DATA"])
    rom.emit(0x77, 0x23, 0x14, 0x1D)  # LD (HL),A / INC HL / INC D / DEC E
    rom.jump("copy_byte", 0xC2)
    rom.emit(0xD1)  # POP DE (remaining capacity)
    rom.jump("load_chunk")

    rom.label("run_loaded")
    rom.emit(0x21)  # LD HL,nn
    rom.word(c["CPC_FUJI_PROGRAM_LIMIT"])
    rom.emit(0xB7, 0xED, 0x52)  # OR A / SBC HL,DE (reject an empty file)
    rom.jump("load_failed", 0xCA)
    rom.jump("clear", 0xCD)
    rom.emit(0xDD, 0xE5, 0xFD, 0xE5)  # PUSH IX / PUSH IY
    rom.jump("call_program", 0xCD)
    rom.emit(0xFD, 0xE1, 0xDD, 0xE1, 0xC9)  # POP IY / POP IX / RET to BASIC

    rom.label("call_program")
    rom.emit(0xFD, 0xE5, 0xC9)  # PUSH IY / RET to the first program byte

    rom.label("load_failed")
    rom.print_string("file_error")
    rom.jump("clear")

    rom.label("run_unavailable")
    rom.print_string("file_unavailable")
    rom.emit(0xC9)  # RET


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("headers", nargs=2, type=Path)
    args = parser.parse_args()

    constants = {}
    constant = re.compile(r"^[ \t]*(\w+)[ \t]*=[ \t]*(0[xX][\da-fA-F]+|\d+)[ \t]*,?[ \t]*$", re.M)
    for path in args.headers:
        constants.update(
            (name, int(value, 0))
            for name, value in constant.findall(path.read_text())
        )
    program = build_rom(constants)
    rows = (", ".join(f"0x{x:02x}" for x in program[i:i + 12])
            for i in range(0, len(program), 12))
    initializer = textwrap.indent(",\n".join(rows), "    ")
    args.output.write_text(
        "/* Generated by build_fuji_rom.py */\n"
        f"static const uint8_t cpc_fuji_rom[{ROM_SIZE}] = {{\n{initializer}\n}};\n"
    )


if __name__ == "__main__":
    main()
