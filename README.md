# FujiNet Go for Amstrad CPC 464

The GNOME window and headless runner share a 64 KiB CPC 464 session. With imported firmware and the FujiNet-PC runtime installed, the desktop boots into CPC CONFIG for browsing TNFS. Offline sessions boot into Locomotive BASIC.

The [user guide](docs/USER_GUIDE.md) covers first launch, CPC CONFIG, TNFS program loading, and all desktop controls.

## Build

Use CMake 3.20 or newer, a C11 compiler, and Python 3 to generate the FujiNet expansion ROM. The GNOME frontend also needs pkg-config and the development packages for GTK 4.8 or newer, libadwaita 1.2 or newer, and SDL2.

```sh
cmake -S . -B build
cmake --build build
```

CMake fetches the pinned Floooh chips revision. For an offline build, pass `-DCHIPS_SRC=/path/to/chips` with a checkout of the revision listed in `cmake/Dependencies.cmake`. Use `-DFRONTEND=none` to build only the emulator session and headless runner.

## Desktop setup and controls

After building the native desktop, run the ROM utility from the repository root. It downloads and verifies the combined 32 KiB CPC 464 OS and BASIC image, installs it at `${XDG_DATA_HOME:-~/.local/share}/fujinet-go-amstrad/cpc464.rom`, and prints that path. [Pinned Caprice32 ROM](https://raw.githubusercontent.com/ColinPitrat/caprice32/66a4a7af9125d2f20111493d13537db9f9cb6ff5/rom/cpc464.rom)

```sh
python3 tools/cpc/install-roms.py
```

To copy your own ROMs, pass one combined 32 KiB file or two 16 KiB files in OS, BASIC order. The utility preserves any different installed ROM unless you pass `--replace`. You can also choose **Import Firmware** in the desktop window to import ROMs through the file chooser.

```sh
python3 tools/cpc/install-roms.py /path/to/os.rom /path/to/basic.rom
```

For CONFIG startup, build the pinned FujiNet-PC runtime once and launch the native app. The app picks up the ROM from the location printed by the utility and starts FujiNet-PC itself.

```sh
bash tools/fujinet/build-rs232.sh
build/frontends/gnome/fujinet-go-amstrad --runtime-dir build/fujinet-firmware/build/dist
```

At the CPC CONFIG prompt, press **Enter** to browse the first TNFS host saved in FujiNet. A fresh profile uses the standard FujiNet host slots, starting with the official server `fujinet.online`. Type `@3` and press Enter to browse slot 3, or type any TNFS host and optional path. The **Configuration** menu opens the FujiNet web UI, where you can view and edit the saved hosts. [FujiNet TNFS server documentation](https://fujinetwifi.github.io/fujinet-docs/features/tnfs/)

To boot offline into BASIC, use `--offline` with the installed ROM, or pass its path directly.

```sh
build/frontends/gnome/fujinet-go-amstrad --offline
build/frontends/gnome/fujinet-go-amstrad "${XDG_DATA_HOME:-$HOME/.local/share}/fujinet-go-amstrad/cpc464.rom"
```

- **Reset to BASIC** and **Reset to CONFIG** select the guest’s reset destination. Press Ctrl+B or Ctrl+R.
- **Configuration**, **Open SD Folder**, **FujiNet Log**, **FujiNet Traffic**, and **Restart FujiNet** are in the main menu. Traffic shows mailbox requests, replies, and timeouts.
- **Preferences** saves the boot mode, controller setting, and loopback BoIP and web ports. A connected SDL controller uses its D-pad or left stick and south face button for CPC joystick input.
- **Open Snapshot** loads uncompressed 64 KiB CPC 464 snapshots in SNA v1 or v2 format. **Z80 Debugger**, also available with F12, shows registers, disassembly, and memory and supports instruction stepping and address breakpoints.

The desktop stores its settings, imported ROM, FujiNet configuration, log, and SD folder in `${XDG_DATA_HOME:-~/.local/share}/fujinet-go-amstrad`. Flatpak uses its own per-app data directory. Set `--runtime-dir /path/to/build/dist` to use a native FujiNet-PC build. Packagers can use `CPC_DESKTOP_RUNTIME` and `CPC_DESKTOP_ROM` to supply the runtime and a combined firmware image at install time.

## Build the Flatpak

The GNOME Flatpak bundles the pinned RS232 runtime. The first launch imports user-supplied CPC 464 firmware. Its application ID matches the scheme used by the FujiNet Go remote. The build uses Flatpak Builder and `appstreamcli`.

```sh
flatpak-builder --user --install-deps-from=flathub --repo=build/flatpak-repo \
  --force-clean build/flatpak-app build-aux/flatpak/online.fujinet.go.amstrad.gnome.yml
flatpak build-bundle build/flatpak-repo build/fujinet-go-amstrad.flatpak \
  online.fujinet.go.amstrad.gnome master
flatpak install --user build/fujinet-go-amstrad.flatpak
```

For public distribution, export the `master` ref into the project’s signed Flatpak repository. The Nix launcher can register a GNOME-only Amstrad entry when the signed remote advertises `online.fujinet.go.amstrad.gnome`. [FujiNet Go remote](https://fujinetwifi.github.io/fujinet-go-desktop-flatpaks/) · [Flatpak repository hosting](https://docs.flatpak.org/en/latest/hosting-a-repository.html)

## Boot and test with local ROMs

Supply either a 32 KiB CPC 464 image containing OS followed by BASIC or two 16 KiB files in OS, BASIC order. The build does not include ROM images.

```sh
build/frontends/gnome/fujinet-go-amstrad /path/to/cpc464.rom
```

To check the window, confirm that the display shows the BASIC `Ready` prompt, then type `BORDER 6:SOUND 1,100,200,7` and press Enter. The border should change and a tone should play for about two seconds. Click **Reset** to return to the boot screen.

```sh
mkdir -p build/boot-smoke
build/core/cpc-headless build/boot-smoke /path/to/cpc464.rom
```

The headless runner executes the same command via emulated key presses, checks the border change and audio samples, then compares the reset image with the initial boot image. It executes 80,000 emulated CPU ticks per 20 ms step and writes `boot.ppm`, `command.ppm`, and `reset.ppm`. Open the captures to inspect the BASIC prompt and the command output.

To register the smoke check with CTest, configure with `-DCPC464_ROM=/path/to/cpc464.rom` and run `ctest --test-dir build --output-on-failure`. With separate ROM files, set `CPC464_ROM` to the OS image and add `-DCPC464_BASIC_ROM=/path/to/basic.rom`.

See the [project conventions](IMPLEMENTATION_PLAN.md#project-conventions) for the repository layout and the FujiNet Go ports to use as references.

## Test the FujiNet expansion

```sh
build/frontends/gnome/fujinet-go-amstrad --fuji-demo /path/to/cpc464.rom
```

At the BASIC prompt, type `|FUJI` and press Enter. The slot 5 ROM sends a ping through the mailbox and displays `FujiNet: PONG` from the local responder. Reset returns to BASIC with the expansion attached.

```sh
mkdir -p build/fuji-smoke
build/core/cpc-headless --fuji build/fuji-smoke /path/to/cpc464.rom
```

The headless check sends `|FUJI` through the emulated keyboard. It exercises a successful reply, a timeout followed by a retry, and a reset with a reply still in flight. It writes `fuji-reply.ppm`, `fuji-timeout.ppm`, and `fuji-reset.ppm`. CTest runs this check when `CPC464_ROM` is set. The standalone card test also runs without system ROM images and covers port decoding, ROM selection, and oversized requests.

See the [mailbox specification](tools/cpc/MAILBOX.md) for the port map, ROM selection, and request lifecycle.

## Browse TNFS from CPC CONFIG

Build the RS232 FujiNet-PC runtime at firmware revision `8f7231a78`. The script uses the upstream PC build and its dependencies. [FujiNet-PC build prerequisites](https://github.com/FujiNetWIFI/fujinet-firmware/blob/master/CONTRIBUTING.md#build-and-test-on-the-host-fujinet-pc)

```sh
bash tools/fujinet/build-rs232.sh
python3 tools/fujinet/run-rs232.py build/fujinet-firmware/build/dist
```

The launcher stores `fnconfig.ini` and `SD/` in `${XDG_DATA_HOME:-~/.local/share}/fujinet-go-amstrad`. It uses loopback ports 11510 for BoIP and 11511 for the web UI. Change them with `--boip-port` and `--web-port`.

Once FujiNet-PC is running, start the desktop and enter `|FUJI` at the BASIC prompt. At the CONFIG prompt enter a host such as `tnfs.fujinet.online` or a host and path such as `tnfs.fujinet.online/adam/`. Enter opens the directory and displays the entries. Escape at the host prompt returns to BASIC. [Public FujiNet TNFS hosts](https://fujinetwifi.github.io/fujinet-docs/features/tnfs/)

```sh
build/frontends/gnome/fujinet-go-amstrad --fuji-live 11510 /path/to/cpc464.rom
```

The headless runner performs the same guest flow and saves the resulting screen as `fuji-directory.ppm`. Pass an empty host argument (`''`) to press Enter on the saved default.

```sh
mkdir -p build/tnfs-smoke
build/core/cpc-headless --fuji-live 11510 tnfs.fujinet.online/adam/ \
  build/tnfs-smoke /path/to/cpc464.rom
```

With `CPC464_ROM` configured, CTest runs the CPC CONFIG browser against a local BoIP protocol fixture. To test the full connection, also set `CPC_FUJINET_RUNTIME_DIST` to the pinned runtime’s `build/dist` and `CPC_TNFSD` to the FujiNet `tnfsd` executable. CTest launches both processes with temporary state and serves a read-only fixture on port 16384, the port used by the pinned TNFS implementation. [FujiNet tnfsd](https://github.com/FujiNetWIFI/tnfsd) · [Pinned TNFS implementation](https://github.com/FujiNetWIFI/fujinet-firmware/blob/8f7231a78/lib/network-protocol/TNFS.cpp)

## Run a program from TNFS

In the live desktop session, type `|FUJIRUN` at the BASIC prompt. Enter the full TNFS host and file path, for example `127.0.0.1/GAMES/BORDER.BIN`. Escape returns to BASIC. The loader accepts raw, position-independent Z80 code from 1 to 2048 bytes and calls the first byte after the complete file has arrived. The program can use `IY` as its load address and `RET` to return to BASIC.

To make a sample program, create `BORDER.BIN` in the directory served by TNFS. This program calls the CPC firmware to set the border to ink 6 and returns. [Amstrad firmware guide](https://www.cpcwiki.eu/imgs/0/02/CPC_firmware.pdf)

```sh
python3 -c 'from pathlib import Path; Path("BORDER.BIN").write_bytes(bytes.fromhex("01 06 06 CD 38 BC C9"))'
```

With FujiNet-PC and the TNFS host running, the headless runner checks the border change and verifies that BASIC accepts a command after the program returns. It saves the screen as `fuji-program.ppm`.

```sh
mkdir -p build/program-smoke
build/core/cpc-headless --fuji-run 11510 127.0.0.1/GAMES/BORDER.BIN \
  build/program-smoke /path/to/cpc464.rom
```
