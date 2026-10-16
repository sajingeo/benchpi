# benchprobe protocol

benchprobe is Arduino firmware that turns a cheap board into a debug probe for
GPIO, ADC, I2C, SPI and UART. It speaks a line protocol over USB serial at
**115200 8N1**:

- Send one command per line, ending in `\n`.
- Every reply is exactly one line of JSON, with `"ok": true` or
  `"ok": false, "err": "..."`.
- Start a line with `#<tag>` and the reply includes `"id": "<tag>"`. The CLI
  uses this to match replies.
- On reset the firmware prints `{"ok":true,"event":"boot",...}`.

Numbers are decimal, or hex with `0x`. Pins are `7`, `D7` or `A0`.

## Commands

| Command | What it does | Reply fields |
|---|---|---|
| `info` | Board, firmware version, pin counts, buffer sizes, I2C/SPI pins | `fw ver board digital_pins analog_pins adc_bits vref_mv max_edges max_samples uart i2c spi` |
| `help` | List of commands | `cmds` |
| `pin <p> in\|out\|pullup` | Set pin mode (`pulldown` where the board has it) | `pin mode` |
| `read <p>` | Digital read | `pin v` |
| `write <p> 0\|1` | Drive a pin (sets it to output) | `pin v` |
| `pwm <p> <0-255>` | PWM duty on a PWM-capable pin | `pin duty` |
| `adc <p> [n]` | Average of `n` ADC reads (1–1024) | `raw min max mv n` |
| `adcs <p> <n> <us>` | `n` ADC samples every `us` microseconds, streamed | `raw[] interval_us elapsed_us vref_mv` |
| `vref <mV>` | Set the reference used for `mv` (default 5000 on AVR, 3300 otherwise) | `vref_mv` |
| `watch <p,p..> <ms>` | Log every level change on up to 8 pins for a window | `edges[[t_us,pin,level]] final count overflow poll_us` |
| `cap <p,p..> <n> <us>` | Logic capture: `n` samples of up to 8 pins every `us` µs (0 = as fast as possible) | `pins n interval_us elapsed_us data` (hex, one byte per sample, bit *i* = `pins[i]`) |
| `freq <p> <ms>` | Count rising edges for a window | `rising_edges hz duty_pct poll_us` |
| `pulse <p> 0\|1 [timeout_us]` | Width of one pulse at that level | `width_us timeout` |
| `i2c scan` | Probe addresses 0x08–0x77 | `found[]` |
| `i2c speed <hz>` | Bus clock, 10 kHz–1 MHz | `hz` |
| `i2c read <addr> <n> [reg]` | Read `n` bytes (1–32), optionally after writing `reg` with a repeated start | `addr n data[]` |
| `i2c write <addr> <b..>` | Write bytes | `addr written` |
| `spi cfg <hz> <mode> [msb\|lsb]` | Clock, SPI mode 0–3, bit order | `hz mode order` |
| `spi xfer <cs\|none> <b..>` | Full-duplex transfer with CS held low | `n rx[]` |
| `uart baud <b>` | Target UART speed (boards with a spare UART, e.g. Mega, Leonardo, RP2040) | `baud` |
| `uart send <b..>` | Send raw bytes to the target | `sent` |
| `uart text <words..>` | Send text plus `\r\n` | `sent` |
| `uart read [ms]` | Collect target output for a window (default 100 ms) | `rx[] n baud` |

Windows (`watch`, `freq`, `uart read`) are capped at 10 s. Long captures are
capped by `info.max_samples` and `info.max_edges`.

## Examples

```
> i2c scan
{"ok":true,"found":[60,104]}
> i2c read 0x68 1 0x75
{"ok":true,"addr":104,"n":1,"data":[104]}
> spi xfer 10 0x9f 0 0 0
{"ok":true,"n":4,"rx":[255,239,64,24]}
> watch 2,3 200
{"ok":true,"window_ms":200,"poll_us":9,"final":{"2":1,"3":0},"count":2,"overflow":0,"edges":[[1532,2,1],[80211,3,0]]}
> write 0 1
{"ok":false,"err":"pin is used by the USB serial link"}
```

## Limits worth knowing

- **Speed.** Pins are sampled in software with `digitalRead`. On a 16 MHz Uno,
  `poll_us` is several microseconds per pin, so `cap`, `watch` and `freq` suit
  signals up to roughly tens of kHz. Use a real logic analyzer with
  `sigrok-cli` for anything faster. Each reply reports the timing it actually
  achieved.
- **Voltage.** Uno, Nano and Mega drive 5 V. Don't connect them to 3.3 V parts
  without level shifting. A 3.3 V board (Pro Mini 3.3 V, Nano 33, any RP2040
  or SAMD board) is the safer default.
- **Reserved pins.** On classic AVR boards, pins 0 and 1 carry the USB serial
  link and are refused.
- **Auto-reset.** Classic Arduinos reset when the port opens, which clears pin
  state. The CLI turns off HUPCL so later opens don't reset, and resends a
  command if a boot event arrives first. To keep state across a whole
  sequence, use `benchprobe run` or `benchprobe shell`.
