# benchpi

Turn a Raspberry Pi (or any Debian/Ubuntu machine) into an embedded-systems
bench host that you and your coding agents can drive.

Clone it, run one script, and the machine has the tools to flash, debug and
probe hardware:

- **Debug probes:** OpenOCD, pyOCD (CMSIS-DAP), SEGGER J-Link (optional)
- **Android-class devices:** adb, fastboot
- **Consoles:** picocom, minicom, screen, socat
- **Logic analyzers:** sigrok-cli
- **benchprobe:** Arduino firmware plus a CLI that turns a cheap board into a
  GPIO / ADC / I2C / SPI / UART probe with JSON output for agents
- **Remote access:** AWS Systems Manager agent (optional), so you can reach
  the bench without opening inbound ports
- udev rules for common probes, and the right group memberships

It isn't tied to any chip family: point it at whatever is on your bench.

## Quick start

```sh
git clone https://github.com/sajingeo/benchpi.git
cd benchpi
./setup.sh                 # default components; safe to re-run
# log out and back in so the new group memberships apply
bench-status               # what's attached
```

Written for Raspberry Pi OS (64-bit, Bookworm). The installer has been
exercised on Ubuntu 24.04; please open an issue if a component fails on your Pi.

### Pick components

```sh
./setup.sh --list                              # see them all
./setup.sh --only base,serial,adb              # just these
./setup.sh --with jlink --accept-jlink-license # add J-Link (SEGGER license)
JLINK_DEB=~/JLink_Linux_arm64.deb ./setup.sh --only jlink   # or bring your own .deb
```

| Component | Installs |
|---|---|
| `base` | build tools, git, jq, usbutils, i2c-tools, gpiod, libusb; adds you to `plugdev dialout i2c gpio spi` |
| `python` | a venv at `/opt/benchpi/venv` with pyserial |
| `udev` | `udev/99-benchpi.rules`: ST-Link, J-Link, CMSIS-DAP, FTDI JTAG, fx2 logic analyzers, `/dev/benchprobe` symlink |
| `serial` | picocom, minicom, screen, socat, lrzsz |
| `adb` | adb, fastboot and Android udev rules |
| `openocd` | OpenOCD |
| `pyocd` | pyOCD in the venv, linked to `/usr/local/bin/pyocd` |
| `sigrok` | sigrok-cli |
| `arduino` | arduino-cli and the AVR core (for flashing benchprobe) |
| `probe` | `benchprobe`, `bench-status` and `flash-probe` commands |
| `jlink` *(optional)* | SEGGER J-Link software (you must accept SEGGER's license) |
| `ssm` *(optional)* | AWS SSM agent; registers it when `SSM_ACTIVATION_CODE`, `SSM_ACTIVATION_ID` and `AWS_REGION` are set |

## benchprobe: a probe your agent can use

Flash `benchprobe/firmware/benchprobe` onto an Arduino, plug it into the bench
host, and it answers commands with one line of JSON:

```sh
flash-probe                                  # Uno on /dev/benchprobe
flash-probe arduino:avr:nano /dev/ttyUSB0    # or any board + port

benchprobe info
benchprobe i2c scan
# {"ok": true, "found": [60, 104], "found_hex": ["0x3c", "0x68"]}
benchprobe i2c read 0x68 6 0x3b
benchprobe spi xfer 10 0x9f 0 0 0
benchprobe adc A0 32
benchprobe watch 2,3 1000                    # edges with µs timestamps
benchprobe --ascii --vcd cap.vcd cap 2,3,4 400 20   # logic capture
benchprobe run "pin 7 pullup" "read 7"       # several commands, one session
```

| Capability | Commands |
|---|---|
| GPIO | `pin`, `read`, `write`, `pwm` |
| Edge monitoring | `watch`, `freq`, `pulse` |
| Logic capture (≤ 8 channels) | `cap`, exported as ASCII or VCD for PulseView/GTKWave |
| ADC | `adc` (averaged), `adcs` (sampled stream), `vref` |
| I2C | `i2c scan`, `speed`, `read`, `write` |
| SPI | `spi cfg`, `spi xfer` |
| Target UART | `uart baud`, `send`, `text`, `read` (boards with a spare UART) |

Full reference: [benchprobe/PROTOCOL.md](benchprobe/PROTOCOL.md).

**Before you wire it up:**
- Uno, Nano and Mega are **5 V** boards. Use a 3.3 V board or level shifters
  for 3.3 V targets.
- It samples in software, so it handles signals up to tens of kHz. For
  anything faster, use a USB logic analyzer with `sigrok-cli`.

Firmware sizes on an Uno: about 16 KB flash and 1.1 KB RAM.

No hardware yet? `python3 benchprobe/host/fake_probe.py` prints a
pseudo-terminal path you can pass to `benchprobe --port` for testing.

## Using it with coding agents

`AGENTS.md` (also loaded through `CLAUDE.md`) tells an agent how to discover
the bench, which tools to use, and which actions need a human, such as eFuses,
read-out protection and bootloader partitions. Point your agent at this repo,
or copy that file into your project.

To reach a bench from elsewhere, run commands through SSH or AWS SSM:

```sh
aws ssm start-session --target mi-0123456789abcdef0
```

## Layout

```
setup.sh                  component installer
udev/99-benchpi.rules     probe permissions and /dev/benchprobe
tools/bench-status        inventory of attached hardware (--json)
tools/flash-probe         build + upload the benchprobe firmware
benchprobe/firmware/      Arduino sketch
benchprobe/host/          benchprobe CLI and a fake device for testing
benchprobe/PROTOCOL.md    command reference
AGENTS.md                 instructions for coding agents
```
