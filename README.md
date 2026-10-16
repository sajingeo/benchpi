# benchpi

Turn a Raspberry Pi (or any Debian/Ubuntu machine) into an embedded-systems
bench host that you and your coding agents can drive.

Clone it, run one script, and the machine has the tools to flash, debug and
probe hardware:

- **Debug probes:** OpenOCD, pyOCD (CMSIS-DAP), SEGGER J-Link (optional)
- **Android-class devices:** adb, fastboot
- **Consoles:** picocom, minicom, screen, socat
- **Logic analyzer:** sigrok-cli with FX2 firmware, plus `la`, a helper that
  captures and decodes I2C, SPI and UART in one command
- **Remote access:** AWS Systems Manager agent (optional), so you can reach
  the bench without opening inbound ports
- udev rules for common probes and analyzers, and the right group memberships

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
| `udev` | `udev/99-benchpi.rules`: ST-Link, J-Link, CMSIS-DAP, FTDI JTAG, FX2 logic analyzers |
| `serial` | picocom, minicom, screen, socat, lrzsz |
| `adb` | adb, fastboot and Android udev rules |
| `openocd` | OpenOCD |
| `pyocd` | pyOCD in the venv, linked to `/usr/local/bin/pyocd` |
| `sigrok` | sigrok-cli and the fx2lafw firmware |
| `tools` | the `bench-status` and `la` commands |
| `jlink` *(optional)* | SEGGER J-Link software (you must accept SEGGER's license) |
| `ssm` *(optional)* | AWS SSM agent; registers it when `SSM_ACTIVATION_CODE`, `SSM_ACTIVATION_ID` and `AWS_REGION` are set |

## Logic analyzer: `la`

benchpi is set up for the
[SparkFun USB Logic Analyzer, 24 MHz / 8-channel](https://www.sparkfun.com/usb-logic-analyzer-24mhz-8-channel.html)
(TOL-18627, about $27), which sigrok drives with its `fx2lafw` driver. Any
other FX2-based analyzer works too, and so does any analyzer sigrok supports:
set `LA_DRIVER`.

`la` wraps sigrok-cli so one command captures and decodes. Every capture is
saved to a `.sr` file first, so you can re-decode it with another protocol, or
open it in PulseView, without capturing again.

```sh
la scan                                   # is the analyzer there?
la i2c D0 D1 -r 2m -t 200ms               # SCL on D0, SDA on D1
#   i2c-1: Start
#   i2c-1: Address write: 68
#   i2c-1: ACK
#   i2c-1: Data write: 75 ...
la spi D0 D1 D2 D3 -r 8m -m 0             # CLK MOSI MISO CS, SPI mode 0
la uart D4 115200 -r 1m -t 1s             # RX on D4
la capture -c D0,D1,D2 -r 1m -t 10ms      # raw capture, printed as bits
la i2c D0 D1 -T D1=f                      # wait for SDA to fall, then capture
la decode /tmp/la-20261016-1345.sr uart:rx=D0:baudrate=9600
```

Rules of thumb:
- Sample at least 4× the bus clock: 400 kHz I2C needs `-r 2m`, a 2 MHz SPI
  clock needs `-r 8m`. All 8 channels at 24 MHz can overrun USB on a Pi; drop
  the rate or the channel count if captures fail.
- **Input levels:** the SparkFun analyzer reads ≥ 2.0 V as high and ≤ 0.8 V as
  low, and tolerates up to 5.25 V. That covers 3.3 V and 5 V logic, but
  **1.8 V signals won't register reliably**.
- Connect the analyzer's ground to the target's ground. SparkFun notes that on
  some units the GND pin next to channel 6 may carry a clock, so use a
  different GND pin.

## Using it with coding agents

`AGENTS.md` (also loaded through `CLAUDE.md`) tells an agent how to discover
the bench, which tools to use, and which actions need a human, such as eFuses,
read-out protection and bootloader partitions. Point your agent at this repo,
or copy that file into your project. `bench-status --json` is the agent's first
command: it lists serial ports, adb and fastboot devices, logic analyzers and
installed tools.

To reach a bench from elsewhere, run commands through SSH or AWS SSM:

```sh
aws ssm start-session --target mi-0123456789abcdef0
```

## Layout

```
setup.sh                  component installer
udev/99-benchpi.rules     probe and analyzer permissions
tools/bench-status        inventory of attached hardware (--json)
tools/la                  logic analyzer capture + decode (sigrok-cli)
AGENTS.md                 instructions for coding agents
```
