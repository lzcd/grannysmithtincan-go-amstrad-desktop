# FujiNet Go for Amstrad CPC 464 User Guide

FujiNet Go runs an Amstrad CPC 464 in a desktop window. Start in Locomotive BASIC to use the CPC on its own, or start in FujiNet CONFIG to browse files on a TNFS server. Once FujiNet and the CPC firmware are ready, press **Enter** in CONFIG to browse the official FujiNet server on a fresh profile.

## First launch and firmware

1. Open **FujiNet Go — Amstrad CPC 464** from your desktop launcher, or run `build/frontends/gnome/fujinet-go-amstrad` from the repository root after building the native app. [Native build instructions](../README.md#build)
2. If the **CPC 464 Firmware** screen appears, choose **Import Firmware**. Select the combined 32 KiB CPC 464 ROM, or select the 16 KiB OS ROM and then the 16 KiB BASIC ROM when prompted. The app keeps a copy for later launches. To install the firmware from a terminal on a native installation, run `python3 tools/cpc/install-roms.py`. [ROM utility and native setup](../README.md#desktop-setup-and-controls)
3. With FujiNet running and **Boot to CONFIG** enabled, wait for **FujiNet CONFIG** to appear. If you see the BASIC `Ready` prompt in an online session, type `|FUJI` and press **Enter**.

The Flatpak includes FujiNet-PC. The native app can use the installed runtime or one built in the repository. Use `--runtime-dir /path/to/build/dist` to point a native launch at a specific runtime. [Native FujiNet-PC setup](../README.md#desktop-setup-and-controls)

To replace the firmware later, open the main menu at the top right and choose **Import CPC Firmware…** or press **Ctrl+I**. Importing firmware starts a new CPC session.

## Browse TNFS in CPC CONFIG

TNFS is the file-sharing protocol FujiNet uses for its public servers and local libraries. In **FujiNet CONFIG**, choose how to open a directory.

| At the CONFIG prompt | What opens |
| --- | --- |
| Press **Enter** on an empty line | The first saved TNFS host. On a fresh profile this is the official server, `fujinet.online`. |
| Type `@3` and press **Enter** | The TNFS host in slot 3. Use `@2` through `@8` for the standard network host slots. |
| Type `fujinet.online` and press **Enter** | The root directory of that host. |
| Type a host and directory, such as `fujinet.online/adam/`, and press **Enter** | That directory on the named host. |

Use **Backspace** to edit the input or **Escape** to return to BASIC. CONFIG prints the directory entries as they arrive and returns to BASIC when it reaches the end. To browse another directory, type `|FUJI` again and enter its path. If the directory cannot be listed, CONFIG returns to the prompt so you can try again.

## Run a CPC program from TNFS

1. Start at the BASIC `Ready` prompt in an online session. Press **Escape** at the CONFIG host prompt if you are still there.
2. Type `|FUJIRUN` and press **Enter**.
3. Enter the full TNFS hostname and file path, such as `127.0.0.1/GAMES/BORDER.BIN` for a local server with that example file. Press **Enter** to load and run it, or **Escape** to cancel.

The loader accepts raw, position-independent Z80 programs from 1 to 2048 bytes. It starts at the first byte of the file, passes the load address in `IY`, and returns to BASIC when the program executes `RET`. Use the sample to confirm that a program loaded and ran. It turns the CPC border red and returns to `Ready`. [Create the sample program](../README.md#run-a-program-from-tnfs)

## BASIC keyboard and gamepad

Give the CPC display keyboard focus and type at the BASIC `Ready` prompt. The app passes printable ASCII, the arrow keys, **Backspace**, **Delete**, **Enter**, and **Escape** to the CPC. Try `BORDER 6:SOUND 1,100,200,7` and press **Enter** to change the border and play a tone. Resize the desktop window to change the size of the CPC display.

To use a gamepad, connect an SDL-compatible controller and turn on **Enable gamepad** in **Preferences**. The app uses the first available controller it recognizes. With the CPC display focused, use the D-pad or left stick to move and the south face button to fire.

## Machine controls and snapshots

| Action | Use |
| --- | --- |
| **Reset** in the title bar | Reboots to the preferred startup screen. |
| **Reset to BASIC** or **Ctrl+B** | Reboots to BASIC. In an online session, `|FUJI` can reopen CONFIG later. |
| **Reset to CONFIG** or **Ctrl+R** | Reboots to CONFIG when live FujiNet is available. |
| **Open Snapshot…** | Opens an uncompressed 64 KiB CPC 464 snapshot in SNA v1 or v2 format. Select the file after importing the CPC firmware. |
| **Quit** or **Ctrl+Q** | Closes the emulator. A managed session also stops its FujiNet-PC process. |

Reset clears unsaved CPC memory. Opening a snapshot replaces the current CPC machine state with the state from the file.

For a single offline run of the native app, use `build/frontends/gnome/fujinet-go-amstrad --offline`. For offline startup on future launches, turn off **Start FujiNet automatically** in **Preferences** and choose **Save**.

## FujiNet Configuration and SD files

Open **Configuration** from the main menu to open FujiNet’s web interface in your browser. The default address is `http://127.0.0.1:11511/`.

In **Hosts List**, click **[EDIT]** next to a slot, enter the server hostname, and confirm. For example, set **Host 3** to a TNFS server you use, then enter `@3` at the CPC CONFIG prompt. CONFIG reads the saved slots when you open a directory.

Choose **Open SD Folder** to open FujiNet’s SD folder in your desktop file manager. Copy files into this folder or organize the files already there. The standard host list reserves slot 1 for SD. Use the desktop file manager or the corresponding Host 1 link in the web interface to browse SD files.

## Preferences

Open **Preferences** from the main menu or press **Ctrl+,**. Choose **Save** to apply your changes.

| Setting | What it controls |
| --- | --- |
| **Start FujiNet automatically** | Starts FujiNet-PC in a managed session. Turn it off for offline BASIC. |
| **Boot to CONFIG** | Opens CONFIG at power-on and when using the title bar **Reset** button while online. |
| **Enable gamepad** | Sends controller input to the emulated CPC joystick. |
| **BoIP port** | Local port for communication between the emulator and FujiNet-PC. The default is `11510`. |
| **Web port** | Local port used by the Configuration browser window. The default is `11511`. |

The two ports must be different. Change them if another local app already uses one of them. Changing the ports or switching between online and offline modes can reset the CPC, so finish any work in the guest before saving those changes.

## Restart FujiNet and inspect traffic

- **FujiNet Log** shows recent messages from the managed FujiNet-PC process. Open it again to see messages written since you opened the window.
- **FujiNet Traffic** shows the current session’s mailbox requests, replies, and timeouts. Matching request and reply rows have the same ticket number, and payload bytes appear in hexadecimal.
- **Restart FujiNet** or **Ctrl+Shift+R** restarts the managed online service. Then press **Ctrl+R** if you want to return the CPC to CONFIG and try the request again.

If CONFIG cannot list a directory, check that the **Configuration** page can open, then check **FujiNet Log** and **FujiNet Traffic** for the failing request. The pinned runtime uses standard TNFS port `16384`. Use the hostname and path of a reachable server on that port.

## Z80 Debugger

Open **Z80 Debugger** from the main menu or press **F12**.

1. Click **Pause** to stop at an instruction boundary. The debugger shows registers, disassembly around the program counter, and a memory dump. The line marked `>` shows the current instruction address.
2. Click **Step** to execute one instruction while paused. Click **Continue** to resume normal emulation.
3. To stop at a specific address, enter it in the hexadecimal field beside **Toggle Breakpoint** and click the button. The enabled addresses appear under the controls. Enter the same address and click the button again to remove it.
4. To inspect memory, enter an address such as `4000` or `&4000` in **Memory address**. The debugger displays the bytes currently mapped there.

## Where the app keeps your files

Native installations use `${XDG_DATA_HOME:-$HOME/.local/share}/fujinet-go-amstrad/`. The Flatpak uses its own data directory, usually `~/.var/app/online.fujinet.go.amstrad.gnome/data/fujinet-go-amstrad/`.

| File or folder | Contents |
| --- | --- |
| `cpc464.rom` | Imported OS and BASIC firmware. |
| `desktop.ini` | The desktop preferences. |
| `fnconfig.ini` | FujiNet settings and saved hosts. |
| `SD/` | Files stored on FujiNet’s SD folder. |
| `fujinet.log` | Messages from the managed FujiNet-PC process. |
