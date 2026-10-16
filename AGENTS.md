# Working on a benchpi bench host

You are on a bench host: a Linux machine wired to embedded hardware. You can
flash targets, read their consoles and probe their signals. Act like a careful
engineer at someone else's bench.

## First, look around

```sh
bench-status --json       # serial ports, adb/fastboot devices, logic analyzers, tools
```

Don't guess which port a device is on. `/dev/serial/by-id/` names are stable;
`/dev/ttyACM0` and `/dev/ttyUSB0` are not. Ask the human how the target is
wired (which analyzer channel goes to which signal) if it isn't written down.

## Tools available

| Job | Tool |
|---|---|
| Serial console | `picocom -b 115200 /dev/serial/by-id/<dev>` (exit: Ctrl-A Ctrl-X). For scripted reads prefer `stty -F <dev> 115200 raw` then `timeout 5 cat <dev>` |
| Arm SWD/JTAG flash and debug | `openocd`, `pyocd`, `JLinkExe` (if installed) |
| Android-class devices | `adb`, `fastboot` |
| Logic analyzer: capture and protocol decode | `la` (wraps `sigrok-cli`) |

## Logic analyzer (`la`)

The analyzer is an 8-channel, 24 MHz FX2 device (sigrok driver `fx2lafw`),
with channels `D0`–`D7`.

```sh
la scan                                   # confirm it's connected
la i2c D0 D1 -r 2m -t 200ms               # SCL, SDA
la spi D0 D1 D2 D3 -r 8m -m 0             # CLK, MOSI, MISO, CS; SPI mode 0-3
la uart D4 115200 -r 1m -t 1s             # RX, baud
la capture -c D0,D1 -r 1m -t 10ms         # raw bits
la i2c D0 D1 -T D1=f                      # trigger: SDA falling edge
la decode <file.sr> i2c:scl=D0:sda=D1     # re-decode a saved capture
LA_VERBOSE=1 la i2c D0 D1                 # every annotation, bit level
```

Every capture is saved (path printed on stderr as `# capture: ...`), so re-decode
it instead of capturing again. `la` exits 2 when it can't capture.

How to read results:
- **Sample rate:** at least 4× the bus clock. Too slow, and decodes come out
  garbled or empty.
- **Empty decode:** if `la` prints `no ... traffic decoded`, check in this
  order: the right channels, the rate, a trigger that never fired, and whether
  the bus was actually active during the window.
- **1.8 V signals:** the analyzer's logic-high threshold is 2.0 V, so 1.8 V
  signals will look stuck low or noisy. Report that rather than a "dead" bus.

## Rules

1. **Reversible actions are fine; irreversible ones need a human.** Flashing
   application firmware, resetting, reading logs, reading registers and
   capturing signals are all fine. Ask first before:
   - mass-erasing a chip
   - writing option bytes or eFuses, or enabling read-out protection or secure boot
   - `fastboot flash` to bootloader or partition-table partitions, or `fastboot oem` / `flashing unlock`
   - anything that would brick a board or void its security state
2. **Mind voltages.** The analyzer tolerates up to 5.25 V on its inputs. Don't
   ask a human to clip it onto anything higher.
3. **Report what you measured, not what you expected.** If a capture doesn't
   show the signal, say so, and give the rate, window and channels you used.
4. **Leave the bench as you found it.** Close consoles, stop debug servers
   (`pkill openocd`), and clean up large capture files you no longer need.
