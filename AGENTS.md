# Working on a benchpi bench host

You are on a bench host: a Linux machine wired to embedded hardware. You can
flash targets, read their consoles and probe their signals. Act like a careful
engineer at someone else's bench.

## First, look around

```sh
bench-status --json       # serial ports, adb/fastboot devices, probes, tools
benchprobe info           # is the bench probe connected, and what can it do
```

Don't guess which port a device is on. `/dev/serial/by-id/` names are stable;
`/dev/ttyACM0` and `/dev/ttyUSB0` are not.

## Tools available

| Job | Tool |
|---|---|
| Serial console | `picocom -b 115200 /dev/serial/by-id/<dev>` (exit: Ctrl-A Ctrl-X). For scripted reads prefer `timeout 5 cat /dev/...` after `stty -F <dev> 115200 raw` |
| Arm SWD/JTAG flash and debug | `openocd`, `pyocd`, `JLinkExe` (if installed) |
| Android-class devices | `adb`, `fastboot` |
| Real logic analyzers | `sigrok-cli` |
| GPIO / ADC / I2C / SPI / UART probing | `benchprobe` (see below) |
| Arduino builds | `arduino-cli` |

## benchprobe

The arguments are the protocol line, and the reply is one line of JSON.
Reference: `benchprobe/PROTOCOL.md`.

```sh
benchprobe i2c scan                         # what's on the bus
benchprobe i2c read 0x68 1 0x75             # WHO_AM_I-style register read
benchprobe spi cfg 1000000 0 && benchprobe spi xfer 10 0x9f 0 0 0
benchprobe adc A0 32                        # averaged voltage
benchprobe watch 2,3 1000                   # edges on two pins for 1 s
benchprobe --ascii cap 2,3,4 300 20         # logic capture; waveform on stderr
benchprobe run "pin 7 pullup" "read 7"      # several commands, one session
```

Exit code 0 means ok, 1 means the device refused, and 2 means it couldn't be reached.

## Rules

1. **Reversible actions are fine; irreversible ones need a human.** Flashing
   application firmware, resetting, reading logs, reading registers and
   probing pins are all fine. Ask first before:
   - mass-erasing a chip
   - writing option bytes or eFuses, or enabling read-out protection or secure boot
   - `fastboot flash` to bootloader or partition-table partitions, or `fastboot oem` / `flashing unlock`
   - anything that would brick a board or void its security state
2. **Mind voltages.** Check `benchprobe info` and the target's I/O voltage
   before driving a pin into it. A 5 V probe board can damage a 3.3 V part.
3. **Don't drive pins you didn't configure.** Only `write` or `pwm` pins that
   the human told you are wired for it. Reading is safe; driving can short
   outputs together.
4. **Report what you measured, not what you expected.** If a capture doesn't
   show the signal, say so. Software sampling has limits (`poll_us` in each
   reply), so a missing edge may be too fast for the probe. Suggest a real
   logic analyzer.
5. **Leave the bench as you found it.** Close consoles, stop debug servers
   (`pkill openocd`), and don't leave pins driven high unless asked.
