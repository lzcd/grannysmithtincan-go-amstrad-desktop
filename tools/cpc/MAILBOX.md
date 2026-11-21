# CPC 464 virtual FujiNet expansion

The prototype uses background ROM slot 5. CPC 464 firmware scans slots 1–7, with BASIC in the built-in ROM and AMSDOS conventionally in slot 7. Slot 5 selects the FujiNet ROM when the card is attached. All other selections keep the stock core’s mapping. Disabling the upper ROM exposes RAM at `&C000–&FFFF`, and writes go to RAM even when a ROM is visible. The firmware registers `|FUJI` and `|FUJIRUN` from the background ROM command table. In live mode, the ROM reserves 2048 bytes from the top of the firmware memory pool. The firmware supplies the base of this reserved area in `IY` when calling a ROM command. [CPC 464 firmware guide](https://cpctech.cpcwiki.de/docs/manual/s158se09.pdf) · [Amstrad firmware I/O appendix](https://cpctech.cpcwiki.de/docs/manual/s968ap12.pdf)

The mailbox decodes all 16 address bits at `&FD60–&FD65` on ordinary Z80 I/O reads and writes. The `&FD` high byte leaves A10, A11, A13, A14, and A15 high, avoiding the partially decoded FDC, PPI, ROM select, CRTC, and gate array in the core. The DDI-1 uses `&FA7E`, `&FB7E`, and `&FB7F`. M4 command I/O uses `&FC00` and `&FE00`. Net4CPC uses `&FD20–&FD23`. The proposed range sits between the listed SYMBiFACE III and CPC-CPLink ranges. Port and slot assignments apply to the virtual card and should be checked on combined hardware before wiring a physical device. [CPC I/O allocations](https://cpcwiki.eu/index.php/I/O_Port_Summary) · [M4 example source](https://github.com/M4Duke/M4examples/blob/master/telnet.s) · [Net4CPC port map](https://github.com/salafek/Net4CPC#notes)

| Port | Read | Write |
| --- | --- | --- |
| `FD60` | `F1` for the local ping demo, `F2` for live FujiNet | Ignored |
| `FD61` | `00` idle, `01` busy, `02` ready, `03` error | `00` clear or cancel, `01` submit |
| `FD62` | Reply length when ready, zero otherwise | Request length while idle |
| `FD63` | Byte offset | Byte offset while idle or ready |
| `FD64` | Reply byte at offset when ready | Request byte at offset while idle |
| `FD65` | `00` none, `01` invalid request length, `02` timeout, `03` invalid reply length | Ignored |

Valid lengths are 1–64 bytes. Offsets are zero-based and never advance implicitly. An out-of-range data read returns `FF`, and an out-of-range write is ignored. The request buffer starts zeroed. Clear to idle, set the length, write the request bytes at their explicit offsets, and submit. Submit while busy, ready, or in error has no effect. After ready, read the response length and bytes, then clear. After an error, read the error code and clear before retrying.

The emulated card enters error after 500 ms in demo mode or 5 seconds in live mode, measured in emulated time at the session’s 20 ms service interval. The host takes each submitted request once and returns its ticket with the response. A cancellation, timeout, or cold reset invalidates the old ticket. The worker performs BoIP exchanges and hands completed replies back to the session’s owning thread.

The prototype command sends `01 2A`. The local demo replies with the four ASCII bytes `PONG`, which `|FUJI` prints using the firmware. The mailbox request and reply are local to milestone 2.

## CPC CONFIG directory requests

- `10` opens the first usable saved TNFS host, or the official FujiNet host if no TNFS host is saved. Append `@1` through `@8` for a specific saved slot, or send an ASCII host and optional path of up to 63 characters without a terminator.
- `11` alone reads the next chunk of the listing. The guest prints the chunk and repeats until the reply reports end or error.
- A live reply starts with `00` for success, `01` for end of directory, or `02` for failure. A successful open includes the resolved host and path. A successful read includes listing text.

The link reads saved hosts with command `F4` on RS232 FujiNet device `0x70`. It skips SD and other protocol schemes when choosing a TNFS host. [Pinned FujiNet host-slot implementation](https://github.com/FujiNetWIFI/fujinet-firmware/blob/8f7231a78/lib/device/fujiDevice/fujiDevice.cpp)

The host uses FujiBus over SLIP/TCP on loopback and translates these requests into directory-mode `OPEN`, `STATUS`, `READ` and `CLOSE` on RS232 network device `0x71`. The devicespec is `N1:TNFS://host[:port]/directory/`. FujiNet-PC resolves and opens the TNFS path. The host bounds each read to 31 bytes, converts line endings for the CPC firmware, and hands at most 63 text bytes to the guest in a reply. Reset discards the TCP connection and outstanding mailbox reply. [FujiBus codec](https://github.com/FujiNetWIFI/fujinet-firmware/blob/master/lib/bus/rs232/FujiBusPacket.cpp) · [RS232 network requests](https://github.com/trgeuy/fujinet-cpm22/blob/main/docs/rs232-protocol.md)

## CPC program requests

- `12` followed by the ASCII `host[:port]/path/file` opens a program for reading. The guest sends up to 63 characters without a terminator.
- `13` alone reads the next chunk. A reply begins with `00` plus 1–63 unmodified file bytes, `01` for end of file, or `02` for failure.

The host uses the same FujiBus network device with read-only `OPEN` (access 4) and translation disabled. The pinned FujiNet-PC runtime uses TNFS port 16384 for both directory and file requests, even when the guest supplies a port. The host reads at most 63 bytes per reply and caps the transfer at 2048 bytes. It closes the stream before reporting end of file. The guest checks its own buffer bounds, rejects empty or failed transfers, and calls the program only after end of file. The first instruction runs at the reserved base address in `IY`. A normal Z80 `RET` returns to BASIC. [Pinned FujiNet network device source](https://github.com/FujiNetWIFI/fujinet-firmware/blob/8f7231a78/lib/device/NDevice/NDevice.cpp) · [Pinned TNFS implementation](https://github.com/FujiNetWIFI/fujinet-firmware/blob/8f7231a78/lib/network-protocol/TNFS.cpp)
