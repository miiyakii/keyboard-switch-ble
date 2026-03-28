# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a Zephyr RTOS firmware project for a Bluetooth Low Energy (BLE) keyboard switch. It targets the **nRF52840 Dongle** (`nrf52840dongle/nrf52840`) and uses the Zephyr ecosystem toolchain (`west`). Firmware is flashed via **USB DFU** (no J-Link required).

- **nRF Connect SDK**: v3.2.4 (`~/ncs/v3.2.4`)
- **Zephyr**: 4.2.99 (bundled with NCS v3.2.4)
- **Toolchain**: zephyr-sdk 0.17.0 (`~/ncs/toolchains/2ac5840438`)

## Build Commands

Requires Zephyr to be installed and `ZEPHYR_BASE` set in the environment.

```bash
west build -b nrf52840dongle/nrf52840    # Build for nRF52840 Dongle
west build -t menuconfig                 # Interactive Kconfig menu
```

To clean and rebuild:
```bash
rm -rf build && west build -b nrf52840dongle/nrf52840
```

## Flashing via DFU

The nRF52840 Dongle ships with the **Nordic nRF5 SDK Open DFU Bootloader** (`USB VID:PID 1915:521f`) and cannot be flashed with `west flash`. Use the following toolchain-bundled utilities instead.

All tools are in the NCS toolchain at:
```
NRFUTIL=~/ncs/toolchains/2ac5840438/nrfutil/bin/nrfutil
LEGACY=~/ncs/toolchains/2ac5840438/nrfutil/home/lib/nrfutil-nrf5sdk-tools/pc_nrfutil_legacy_v6.1.7
```

### Full flash procedure

1. **Enter bootloader mode** — press the RESET button; the red LED pulses and device appears as `1915:521f Open DFU Bootloader`.

2. **Generate the DFU zip** (using legacy v6 tool, required for this bootloader version):
   ```bash
   $LEGACY pkg generate \
     --hw-version 52 \
     --sd-req 0x00 \
     --application build/keyboard-switch-ble/zephyr/zephyr.hex \
     --application-version 1 \
     dfu_package.zip
   ```

3. **Flash via nrfutil 8.x** (detects device automatically via `nordicDfu` trait):
   ```bash
   $NRFUTIL device program --firmware dfu_package.zip
   ```

   After success the device re-enumerates as `2fe3:0004 NordicSemiconductor CDC ACM serial backend`.

### Verify device serial number
```bash
$NRFUTIL device list
```

## Linux Relay (`linux/`)

The `linux/` directory contains the host-side relay daemon that runs on a Raspberry Pi.

### Device paths (stable, by-id)

| 设备 | 路径 |
|------|------|
| 键盘 (Varmilo) | `/dev/input/by-id/usb-MosArt_Varmilo_Keyboard-if01-event-kbd` |
| Dongle 0 (active, FA154A3B) | `/dev/serial/by-id/usb-KBRelay_BLE_KB_Relay_FA154A3BDD26EF2C-if00` |
| Dongle 1 (64A7591E) | `/dev/serial/by-id/usb-KBRelay_BLE_KB_Relay_64A7591E51DEA2EE-if00` |

### Build

```bash
cd linux
sudo apt install libevdev-dev
make
```

### Manual start

```bash
sudo ./relay \
  /dev/input/by-id/usb-MosArt_Varmilo_Keyboard-if01-event-kbd \
  /dev/serial/by-id/usb-KBRelay_BLE_KB_Relay_FA154A3BDD26EF2C-if00 \
  /dev/serial/by-id/usb-KBRelay_BLE_KB_Relay_64A7591E51DEA2EE-if00
```

Switch hotkey: **double-tap ScrollLock** (within 400ms).

### systemd service (Raspberry Pi)

Service file: `/etc/systemd/system/kvm-relay.service`
udev rules: `/etc/udev/rules.d/99-kvm-relay.rules`

- 开机自启，`Restart=on-failure`（3s 重试）
- 键盘或 dongle 热插拔后自动重启服务

```bash
sudo systemctl status kvm-relay    # 查看状态
sudo systemctl restart kvm-relay   # 手动重启
sudo journalctl -u kvm-relay -f    # 实时日志
```

## Architecture

- **`CMakeLists.txt`** — Finds the Zephyr package and compiles `src/main.c` as the application.
- **`prj.conf`** — Zephyr Kconfig settings (currently empty). Enable subsystems here, e.g., `CONFIG_BT=y` for BLE, `CONFIG_GPIO=y` for GPIO.
- **`src/main.c`** — Application entry point.

The intended data flow is: hardware key switches → GPIO input → application logic → BLE HID stack → host device.

Zephyr's BLE HID profile (`CONFIG_BT_HID`) is the standard way to expose a keyboard over BLE. GPIO is used to scan switch matrix or individual switch inputs.

## Zephyr-Specific Conventions

- Board-specific device tree overlays go in `boards/nrf52840dongle_nrf52840.overlay`.
- Additional Kconfig fragments can be passed via `west build -- -DEXTRA_CONF_FILE=<file>`.
- Zephyr modules and dependencies are managed via `west.yml` (not yet present).
