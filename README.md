# DesktopWidget

A standalone Windows desktop widget that shows live hardware readings in
NZXT-CAM-style tiles: big values with a small label, colored per category
(CPU, GPU, memory, disk, network, motherboard fans). Drag it anywhere, pin it
to a corner or edge, or let it follow a chosen monitor. A tray icon and a
settings window give full control.

It is a single `.exe` with no installer and no background service of its own.
Settings are stored in `%APPDATA%\DesktopWidget\settings.ini`.

## Features

- **Tiles** for: CPU load, CPU clock, CPU temperature, CPU fan; GPU load, GPU
  temperature, GPU clock, GPU fan, VRAM used, VRAM load; RAM used, RAM load;
  disk activity; network download and upload; two system (motherboard) fans.
- **Layout**: horizontal, vertical or two rows; or group tiles by category
  (one column, or one row, per category).
- **Placement**: free (drag anywhere, across monitors), or anchored to a
  corner / edge / center with an adjustable margin; optional edge snapping;
  stays fully on screen; can be pinned to a specific monitor.
- **Appearance**: dark or light theme, per-category value colors, optional
  custom colors, a chosen value font (bold optional), scale (50–150 %),
  opacity, always-on-top, and warning colors for hot temperatures.
- **Tray icon** (a small bar chart in the CPU / GPU / memory colors) with a
  menu: *Settings…*, *Start with Windows*, *Exit*. Right-clicking the widget
  itself opens the full quick-options menu (every setting is also in the
  settings window).
- **Single instance**: launching a second copy just opens the running
  widget's settings and exits.
- **Start with Windows**: a checkbox in the tray menu. It writes the quoted
  path of this exe to `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, so
  the widget starts at logon without elevation. See *Admin requirements* for
  the catch with the PawnIO sensors.

## Sensors and their requirements

Most readings need no special privileges and work on any machine:

| Reading | Source | Needs admin? |
| --- | --- | --- |
| CPU load | system performance counters | No |
| CPU clock | timed instruction burst on the current core | No |
| RAM used / load | `GlobalMemoryStatusEx` | No |
| GPU load / temp / clock / fan / VRAM | D3DKMT (the same API Task Manager uses) | No |
| Disk activity | `IOCTL_DISK_PERFORMANCE` per physical drive | No |
| Network down / up | `GetIfTable2` over the up physical interfaces | No |
| **CPU temperature** | **PawnIO** ring-0 driver (AMD only, see below) | **Yes** |
| **Motherboard fans** (CPU / SYS) | **PawnIO** + the Super I/O chip | **Yes** |

The two PawnIO-backed readings (CPU package temperature and motherboard fan
speeds) are the only ones that need administrator rights. When PawnIO or admin
rights are unavailable, those tiles simply show `—` and a short reason in the
right-click menu; everything else keeps working. Nothing is written to any
fan-control or voltage register — the chip is only read.

### CPU temperature

Implemented for AMD family 17h–1Ah (Zen 1–5). The reading uses the
`AMDFamily17` PawnIO module. Intel CPU temperature is not implemented.

### Motherboard fans

Read from the Super I/O chip via the PawnIO `LpcIO` module. Supported chips:

- ITE IT86xx / IT87xx with 16-bit tachometers at port `0x2E` (Gigabyte and
  many others; developed against an **IT8688E**).
- Nuvoton NCT6771F … NCT6799D / NCT610X hardware monitors at `0x2E` or `0x4E`.
  The EC-type NCT668x (some MSI boards) are not supported.

Which tachometer input maps to "CPU fan" / "SYS fan 1 / 2" follows the usual
board wiring; it may differ on unusual boards.

### About PawnIO

[PawnIO](https://pawnio.eu) is the signed kernel driver that
[LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor)
and FanControl use. The two small signed modules (`AMDFamily17.bin`,
`LpcIO.bin`) are downloaded once from LibreHardwareMonitor's resources, pinned
to a specific commit and verified by their git blob SHA-1 before use, then
cached in `%APPDATA%\DesktopWidget\`.

For the PawnIO readings you need either:

- the **PawnIO driver already installed** on the machine (install it from
  <https://pawnio.eu>) — then running the widget as administrator is enough; or
- a `PawnIODriver\pawnio.inf` folder placed next to the exe, in which case the
  widget can install the (WHQL-signed) driver itself on first use — this still
  needs an elevated run.

## Admin requirements

- The app runs fine **without** administrator rights. Only the CPU-temperature
  and motherboard-fan tiles are blank without admin.
- To get those two readings, run the exe **as administrator** (and have PawnIO
  installed, as above).
- *Start with Windows* uses the per-user Run key and therefore launches the
  widget **non-elevated** at logon, so the PawnIO sensors will be blank in that
  case. If you want them at logon, create a Scheduled Task that runs with
  highest privileges instead, e.g. from an elevated prompt:

  ```
  schtasks /Create /TN DesktopWidget /SC ONLOGON /RL HIGHEST ^
      /TR "\"C:\Path\To\DesktopWidget.exe\""
  ```

  (and leave *Start with Windows* unchecked so the two do not both launch).

## Updates

Right-click the tray icon and choose **Check for updates**. Nothing is checked automatically: no check at startup, no timer. The app asks GitHub for the latest release (`version.txt`) and, if a newer build exists, offers to update. Accepting downloads `DesktopWidget.exe` next to the running exe as `DesktopWidget.exe.new`, verifies it against the published `DesktopWidget.exe.sha256`, then a hidden PowerShell helper waits for the app to exit, swaps the file in and starts it again.

Versions are `1.0.N`, where N is the CI build number; local builds report `1.0.0`. The exe must sit in a folder you can write to (not Program Files) for the update to work.

## Build

Requirements: Visual Studio 2022 with the "Desktop development with C++"
workload (v143 toolset) and a Windows 10/11 SDK.

- Open `DesktopWidget.sln` and build the **Release | x64** configuration, or
- from a command prompt:

  ```
  msbuild DesktopWidget.sln /p:Configuration=Release /p:Platform=x64
  ```

The output is `build\x64\Release\DesktopWidget.exe`. The Release build links
the static CRT (`/MT`), so the exe has no extra runtime dependencies.

## Credits

- [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor)
  (MPL-2.0) — the register layouts for the Super I/O fan chips and the AMD
  temperature access, and the source of the signed PawnIO modules.
- [PawnIO](https://github.com/namazso/PawnIO) — the signed kernel driver used
  for ring-0 hardware access.

## License

Licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE): free for any non-commercial use.
