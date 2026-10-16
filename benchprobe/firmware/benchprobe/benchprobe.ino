/*
 * benchprobe — a tiny hardware debug probe for coding agents.
 *
 * Turns an Arduino into a GPIO / ADC / I2C / SPI / UART probe driven over
 * USB serial. Every command is one text line; every reply is one JSON line,
 * so an agent can read it without guessing.
 *
 *   115200 baud, 8N1, commands end in \n.
 *   Optional request tag: start a line with #<tag> and the reply carries "id".
 *
 * See ../../PROTOCOL.md for every command. Quick taste:
 *   info
 *   pin 7 in            read 7          write 8 1
 *   adc A0 16           watch 2,3 500   cap 2,3,4 256 50
 *   i2c scan            i2c read 0x68 6 0x3b
 *   spi xfer 10 0x9f 0 0 0
 *
 * VOLTAGE WARNING: a 5 V board (Uno, Nano, Mega) will damage 3.3 V parts if
 * you drive its pins into them. Use a 3.3 V board or level shifters.
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <string.h>
#include <stdlib.h>

#define FW_NAME    "benchprobe"
#define FW_VERSION "0.1.0"
#define BAUD       115200

// ---------------------------------------------------------------- sizing
// Buffers are sized for an ATmega328P (2 KB RAM). Bigger boards get more.
#if defined(__AVR_ATmega328P__) || defined(__AVR_ATmega168__) || defined(__AVR_ATmega32U4__)
  #define MAX_LINE     96
  #define MAX_EDGES    64
  #define MAX_SAMPLES  400
#elif defined(__AVR__)
  #define MAX_LINE     128
  #define MAX_EDGES    256
  #define MAX_SAMPLES  2048
#else
  #define MAX_LINE     192
  #define MAX_EDGES    1024
  #define MAX_SAMPLES  8192
#endif

#define MAX_WATCH_PINS 8
#define MAX_TOKENS     24
#define MAX_WINDOW_MS  10000UL

#ifndef ADC_BITS
  #define ADC_BITS 10
#endif

#ifndef NUM_DIGITAL_PINS
  #define NUM_DIGITAL_PINS 20
#endif

// On classic AVR boards the USB serial bridge sits on pins 0/1.
#if defined(__AVR__) && !defined(USBCON)
  #define RESERVED_SERIAL_PINS 1
#endif

// Second hardware UART for talking to a target's console, where one exists.
#if defined(HAVE_HWSERIAL1) || defined(SERIAL_PORT_HARDWARE1) || defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_SAMD)
  #define HAS_TARGET_UART 1
  #define TARGET_UART Serial1
#endif

// ---------------------------------------------------------------- state

static char     line[MAX_LINE];
static uint8_t  lineLen = 0;
static bool     lineOverflow = false;
static char    *tok[MAX_TOKENS];
static uint8_t  ntok = 0;
static const char *reqId = NULL;

static uint16_t vrefMv =
#if defined(__AVR__)
  5000;
#else
  3300;
#endif

static SPISettings spiSettings(1000000, MSBFIRST, SPI_MODE0);
static bool spiStarted = false;
static bool wireStarted = false;

// Shared scratch buffer: edges for `watch`, samples for `cap`.
struct Edge { uint32_t t; uint8_t pin; uint8_t level; };
static union {
  Edge    edges[MAX_EDGES];
  uint8_t samples[MAX_SAMPLES];
} scratch;

// Analog pin table so "A3" works on cores where A0..An aren't contiguous.
static const uint8_t kAnalogPins[] = {
#ifdef PIN_A0
  PIN_A0,
#endif
#ifdef PIN_A1
  PIN_A1,
#endif
#ifdef PIN_A2
  PIN_A2,
#endif
#ifdef PIN_A3
  PIN_A3,
#endif
#ifdef PIN_A4
  PIN_A4,
#endif
#ifdef PIN_A5
  PIN_A5,
#endif
#ifdef PIN_A6
  PIN_A6,
#endif
#ifdef PIN_A7
  PIN_A7,
#endif
  255
};

// ---------------------------------------------------------------- JSON out

static void idField() {
  if (reqId) { Serial.print(F(",\"id\":\"")); Serial.print(reqId); Serial.print('"'); }
}
static void beginOk()  { Serial.print(F("{\"ok\":true")); idField(); }
static void endReply() { Serial.println('}'); }

static void key(const __FlashStringHelper *k) {
  Serial.print(F(",\"")); Serial.print(k); Serial.print(F("\":"));
}
static void kv(const __FlashStringHelper *k, long v) { key(k); Serial.print(v); }
static void kvs(const __FlashStringHelper *k, const char *v) {
  key(k); Serial.print('"'); Serial.print(v); Serial.print('"');
}
static void kvf(const __FlashStringHelper *k, const __FlashStringHelper *v) {
  key(k); Serial.print('"'); Serial.print(v); Serial.print('"');
}

static void fail(const __FlashStringHelper *msg) {
  Serial.print(F("{\"ok\":false")); idField();
  Serial.print(F(",\"err\":\"")); Serial.print(msg); Serial.println(F("\"}"));
}

// ---------------------------------------------------------------- parsing

// Decimal, or hex with 0x. (No octal surprises: "010" is ten.)
static bool parseNum(const char *s, long &out) {
  if (!s || !*s) return false;
  char *end;
  bool neg = (*s == '-');
  const char *p = neg ? s + 1 : s;
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) out = strtol(p + 2, &end, 16);
  else                                              out = strtol(p, &end, 10);
  if (neg) out = -out;
  return *end == '\0' && end != p;
}

static bool parseByte(const char *s, uint8_t &out) {
  long v;
  if (!parseNum(s, v) || v < 0 || v > 255) return false;
  out = (uint8_t)v;
  return true;
}

// "7", "D7", "A0" -> pin number. 255 on failure.
static uint8_t parsePin(const char *s) {
  if (!s) return 255;
  long v;
  if (s[0] == 'A' || s[0] == 'a') {
    if (!parseNum(s + 1, v) || v < 0) return 255;
    uint8_t n = sizeof(kAnalogPins) - 1;
    if (n == 0) return (uint8_t)(A0 + v);
    return v < n ? kAnalogPins[v] : 255;
  }
  if (s[0] == 'D' || s[0] == 'd') s++;
  if (!parseNum(s, v) || v < 0 || v >= NUM_DIGITAL_PINS) return 255;
  return (uint8_t)v;
}

static bool reservedPin(uint8_t p) {
#ifdef RESERVED_SERIAL_PINS
  return p == 0 || p == 1;
#else
  (void)p; return false;
#endif
}

// Validate a pin token and complain if it's bad. Returns 255 on failure.
static uint8_t needPin(const char *s) {
  uint8_t p = parsePin(s);
  if (p == 255) { fail(F("bad pin")); return 255; }
  if (reservedPin(p)) { fail(F("pin is used by the USB serial link")); return 255; }
  return p;
}

// "2,3,A0" -> pins[]; returns count, 0 on error (and replies).
static uint8_t parsePinList(char *s, uint8_t *pins, uint8_t maxPins) {
  uint8_t n = 0;
  char *save = NULL;
  for (char *t = strtok_r(s, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
    if (n >= maxPins) { fail(F("too many pins")); return 0; }
    uint8_t p = needPin(t);
    if (p == 255) return 0;
    pins[n++] = p;
  }
  if (n == 0) fail(F("no pins"));
  return n;
}

static bool needArgs(uint8_t n) {
  if (ntok < n) { fail(F("missing arguments")); return false; }
  return true;
}

// ---------------------------------------------------------------- commands

static void cmdInfo() {
  beginOk();
  kvf(F("fw"), F(FW_NAME));
  kvf(F("ver"), F(FW_VERSION));
#if defined(ARDUINO_BOARD)
  kvs(F("board"), ARDUINO_BOARD);
#endif
  kv(F("digital_pins"), NUM_DIGITAL_PINS);
  kv(F("analog_pins"), sizeof(kAnalogPins) - 1);
  kv(F("adc_bits"), ADC_BITS);
  kv(F("vref_mv"), vrefMv);
  kv(F("max_edges"), MAX_EDGES);
  kv(F("max_samples"), MAX_SAMPLES);
#ifdef HAS_TARGET_UART
  kv(F("uart"), 1);
#else
  kv(F("uart"), 0);
#endif
#ifdef RESERVED_SERIAL_PINS
  key(F("reserved")); Serial.print(F("[0,1]"));
#endif
  key(F("i2c")); Serial.print(F("{\"sda\":")); Serial.print(SDA);
  Serial.print(F(",\"scl\":")); Serial.print(SCL); Serial.print('}');
  key(F("spi")); Serial.print(F("{\"mosi\":")); Serial.print(MOSI);
  Serial.print(F(",\"miso\":")); Serial.print(MISO);
  Serial.print(F(",\"sck\":")); Serial.print(SCK); Serial.print('}');
  endReply();
}

static void cmdHelp() {
  beginOk();
  key(F("cmds"));
  Serial.print(F("[\"info\",\"help\",\"pin <p> in|out|pullup\",\"read <p>\",\"write <p> 0|1\","
                 "\"pwm <p> <0-255>\",\"adc <p> [n]\",\"adcs <p> <n> <us>\",\"vref <mV>\","
                 "\"watch <p,p..> <ms>\",\"cap <p,p..> <n> <us>\",\"freq <p> <ms>\","
                 "\"pulse <p> 0|1 [timeout_us]\",\"i2c scan|speed|read|write\","
                 "\"spi cfg|xfer\",\"uart baud|send|text|read\"]"));
  endReply();
}

static void cmdPin() {
  if (!needArgs(3)) return;
  uint8_t p = needPin(tok[1]); if (p == 255) return;
  const char *m = tok[2];
  if      (!strcmp(m, "in"))     pinMode(p, INPUT);
  else if (!strcmp(m, "out"))    pinMode(p, OUTPUT);
  else if (!strcmp(m, "pullup")) pinMode(p, INPUT_PULLUP);
#ifdef INPUT_PULLDOWN
  else if (!strcmp(m, "pulldown")) pinMode(p, INPUT_PULLDOWN);
#endif
  else { fail(F("mode must be in, out or pullup")); return; }
  beginOk(); kv(F("pin"), p); kvs(F("mode"), m); endReply();
}

static void cmdRead() {
  if (!needArgs(2)) return;
  uint8_t p = needPin(tok[1]); if (p == 255) return;
  beginOk(); kv(F("pin"), p); kv(F("v"), digitalRead(p)); endReply();
}

static void cmdWrite() {
  if (!needArgs(3)) return;
  uint8_t p = needPin(tok[1]); if (p == 255) return;
  long v;
  if (!parseNum(tok[2], v) || (v != 0 && v != 1)) { fail(F("value must be 0 or 1")); return; }
  pinMode(p, OUTPUT);
  digitalWrite(p, v ? HIGH : LOW);
  beginOk(); kv(F("pin"), p); kv(F("v"), v); endReply();
}

static void cmdPwm() {
  if (!needArgs(3)) return;
  uint8_t p = needPin(tok[1]); if (p == 255) return;
  long v;
  if (!parseNum(tok[2], v) || v < 0 || v > 255) { fail(F("duty must be 0-255")); return; }
  pinMode(p, OUTPUT);
  analogWrite(p, (int)v);
  beginOk(); kv(F("pin"), p); kv(F("duty"), v); endReply();
}

static long toMv(long raw) { return (raw * (long)vrefMv) / ((1L << ADC_BITS) - 1); }

static void cmdAdc() {
  if (!needArgs(2)) return;
  uint8_t p = needPin(tok[1]); if (p == 255) return;
  long n = 1;
  if (ntok > 2 && (!parseNum(tok[2], n) || n < 1 || n > 1024)) { fail(F("n must be 1-1024")); return; }
  long sum = 0, mn = 0x7fffffff, mx = 0;
  for (long i = 0; i < n; i++) {
    long r = analogRead(p);
    sum += r; if (r < mn) mn = r; if (r > mx) mx = r;
  }
  long avg = (sum + n / 2) / n;
  beginOk(); kv(F("pin"), p); kv(F("n"), n);
  kv(F("raw"), avg); kv(F("min"), mn); kv(F("max"), mx); kv(F("mv"), toMv(avg));
  endReply();
}

// Streams samples straight to serial, so no buffer limit beyond n.
static void cmdAdcSample() {
  if (!needArgs(4)) return;
  uint8_t p = needPin(tok[1]); if (p == 255) return;
  long n, us;
  if (!parseNum(tok[2], n) || n < 1 || n > 5000) { fail(F("n must be 1-5000")); return; }
  if (!parseNum(tok[3], us) || us < 0 || us > 1000000L) { fail(F("interval must be 0-1000000 us")); return; }
  beginOk(); kv(F("pin"), p); kv(F("n"), n); kv(F("interval_us"), us); kv(F("vref_mv"), vrefMv);
  key(F("raw")); Serial.print('[');
  uint32_t start = micros(), next = start;
  for (long i = 0; i < n; i++) {
    while ((int32_t)(micros() - next) < 0) {}
    next += us;
    if (i) Serial.print(',');
    Serial.print(analogRead(p));
  }
  Serial.print(']');
  kv(F("elapsed_us"), micros() - start);
  endReply();
}

static void cmdVref() {
  if (!needArgs(2)) return;
  long v;
  if (!parseNum(tok[1], v) || v < 500 || v > 6000) { fail(F("vref must be 500-6000 mV")); return; }
  vrefMv = (uint16_t)v;
  beginOk(); kv(F("vref_mv"), vrefMv); endReply();
}

// Record level changes on up to 8 pins for a time window.
static void cmdWatch() {
  if (!needArgs(3)) return;
  uint8_t pins[MAX_WATCH_PINS];
  uint8_t np = parsePinList(tok[1], pins, MAX_WATCH_PINS); if (!np) return;
  long ms;
  if (!parseNum(tok[2], ms) || ms < 1 || (unsigned long)ms > MAX_WINDOW_MS) { fail(F("window must be 1-10000 ms")); return; }

  uint8_t last[MAX_WATCH_PINS];
  for (uint8_t i = 0; i < np; i++) { pinMode(pins[i], INPUT); last[i] = digitalRead(pins[i]); }

  uint16_t ne = 0; bool overflow = false;
  uint32_t start = micros(), window = (uint32_t)ms * 1000UL, loops = 0;
  while (micros() - start < window) {
    loops++;
    for (uint8_t i = 0; i < np; i++) {
      uint8_t v = digitalRead(pins[i]);
      if (v != last[i]) {
        last[i] = v;
        if (ne < MAX_EDGES) { scratch.edges[ne].t = micros() - start; scratch.edges[ne].pin = pins[i]; scratch.edges[ne].level = v; ne++; }
        else overflow = true;
      }
    }
  }
  uint32_t elapsed = micros() - start;

  beginOk(); kv(F("window_ms"), ms);
  kv(F("poll_us"), loops ? (long)(elapsed / loops) : 0);  // effective sampling period
  key(F("final")); Serial.print('{');
  for (uint8_t i = 0; i < np; i++) { if (i) Serial.print(','); Serial.print('"'); Serial.print(pins[i]); Serial.print(F("\":")); Serial.print(last[i]); }
  Serial.print('}');
  kv(F("count"), ne); kv(F("overflow"), overflow ? 1 : 0);
  key(F("edges")); Serial.print('[');  // [t_us, pin, level]
  for (uint16_t i = 0; i < ne; i++) {
    if (i) Serial.print(',');
    Serial.print('['); Serial.print(scratch.edges[i].t); Serial.print(',');
    Serial.print(scratch.edges[i].pin); Serial.print(','); Serial.print(scratch.edges[i].level); Serial.print(']');
  }
  Serial.print(']');
  endReply();
}

// Logic-analyzer style capture: n samples of up to 8 pins at a fixed interval.
// Each sample is a bitmask (bit i = pins[i]), returned as a hex string.
static void cmdCapture() {
  if (!needArgs(4)) return;
  uint8_t pins[8];
  uint8_t np = parsePinList(tok[1], pins, 8); if (!np) return;
  long n, us;
  if (!parseNum(tok[2], n) || n < 1 || n > MAX_SAMPLES) { fail(F("sample count out of range (see info.max_samples)")); return; }
  if (!parseNum(tok[3], us) || us < 0 || us > 1000000L) { fail(F("interval must be 0-1000000 us")); return; }
  for (uint8_t i = 0; i < np; i++) pinMode(pins[i], INPUT);

  uint32_t start = micros(), next = start;
  for (long s = 0; s < n; s++) {
    if (us) { while ((int32_t)(micros() - next) < 0) {} next += us; }
    uint8_t m = 0;
    for (uint8_t i = 0; i < np; i++) if (digitalRead(pins[i])) m |= (1 << i);
    scratch.samples[s] = m;
  }
  uint32_t elapsed = micros() - start;

  beginOk();
  key(F("pins")); Serial.print('[');
  for (uint8_t i = 0; i < np; i++) { if (i) Serial.print(','); Serial.print(pins[i]); }
  Serial.print(']');
  kv(F("n"), n); kv(F("interval_us"), us); kv(F("elapsed_us"), elapsed);
  key(F("data")); Serial.print('"');
  for (long s = 0; s < n; s++) {
    uint8_t b = scratch.samples[s];
    Serial.print("0123456789abcdef"[b >> 4]); Serial.print("0123456789abcdef"[b & 15]);
  }
  Serial.print('"');
  endReply();
}

// Frequency and duty cycle by polling. Good to tens of kHz on an Uno.
static void cmdFreq() {
  if (!needArgs(3)) return;
  uint8_t p = needPin(tok[1]); if (p == 255) return;
  long ms;
  if (!parseNum(tok[2], ms) || ms < 1 || (unsigned long)ms > MAX_WINDOW_MS) { fail(F("window must be 1-10000 ms")); return; }
  pinMode(p, INPUT);
  uint8_t last = digitalRead(p);
  uint32_t rising = 0, highLoops = 0, loops = 0;
  uint32_t start = micros(), window = (uint32_t)ms * 1000UL;
  while (micros() - start < window) {
    uint8_t v = digitalRead(p);
    loops++; if (v) highLoops++;
    if (v && !last) rising++;
    last = v;
  }
  beginOk(); kv(F("pin"), p); kv(F("window_ms"), ms);
  kv(F("rising_edges"), rising);
  kv(F("hz"), (long)((rising * 1000UL) / (uint32_t)ms));
  kv(F("duty_pct"), loops ? (long)((highLoops * 100UL) / loops) : 0);
  kv(F("poll_us"), loops ? (long)(window / loops) : 0);
  endReply();
}

static void cmdPulse() {
  if (!needArgs(3)) return;
  uint8_t p = needPin(tok[1]); if (p == 255) return;
  long level, timeout = 1000000L;
  if (!parseNum(tok[2], level) || (level != 0 && level != 1)) { fail(F("level must be 0 or 1")); return; }
  if (ntok > 3 && (!parseNum(tok[3], timeout) || timeout < 1 || timeout > 10000000L)) { fail(F("bad timeout")); return; }
  pinMode(p, INPUT);
  unsigned long w = pulseIn(p, level ? HIGH : LOW, (unsigned long)timeout);
  beginOk(); kv(F("pin"), p); kv(F("level"), level); kv(F("width_us"), (long)w); kv(F("timeout"), w == 0 ? 1 : 0); endReply();
}

// ---------------------------------------------------------------- I2C

static void ensureWire() { if (!wireStarted) { Wire.begin(); wireStarted = true; } }

static void printBytes(const __FlashStringHelper *k, const uint8_t *b, uint16_t n) {
  key(k); Serial.print('[');
  for (uint16_t i = 0; i < n; i++) { if (i) Serial.print(','); Serial.print(b[i]); }
  Serial.print(']');
}

static void cmdI2c() {
  if (!needArgs(2)) return;
  ensureWire();
  const char *sub = tok[1];

  if (!strcmp(sub, "scan")) {
    beginOk(); key(F("found")); Serial.print('[');
    bool first = true;
    for (uint8_t a = 0x08; a < 0x78; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) { if (!first) Serial.print(','); Serial.print(a); first = false; }
    }
    Serial.print(']'); endReply();
    return;
  }

  if (!strcmp(sub, "speed")) {
    long hz;
    if (!needArgs(3) || !parseNum(tok[2], hz) || hz < 10000 || hz > 1000000L) { if (ntok >= 3) fail(F("speed must be 10000-1000000 Hz")); return; }
    Wire.setClock((uint32_t)hz);
    beginOk(); kv(F("hz"), hz); endReply();
    return;
  }

  if (!strcmp(sub, "read")) {          // i2c read <addr> <n> [reg]
    if (!needArgs(4)) return;
    uint8_t addr, n, reg;
    if (!parseByte(tok[2], addr) || addr > 0x7f) { fail(F("bad address")); return; }
    if (!parseByte(tok[3], n) || n < 1 || n > 32) { fail(F("n must be 1-32")); return; }
    if (ntok > 4) {
      if (!parseByte(tok[4], reg)) { fail(F("bad register")); return; }
      Wire.beginTransmission(addr); Wire.write(reg);
      uint8_t e = Wire.endTransmission(false);  // repeated start
      if (e) { Serial.print(F("{\"ok\":false")); idField(); Serial.print(F(",\"err\":\"nack\",\"code\":")); Serial.print(e); Serial.println('}'); return; }
    }
    uint8_t buf[32];
    uint8_t got = Wire.requestFrom(addr, n);
    for (uint8_t i = 0; i < got && i < sizeof(buf); i++) buf[i] = Wire.read();
    beginOk(); kv(F("addr"), addr); kv(F("n"), got); printBytes(F("data"), buf, got); endReply();
    return;
  }

  if (!strcmp(sub, "write")) {         // i2c write <addr> <b0> [b1...]
    if (!needArgs(4)) return;
    uint8_t addr, b;
    if (!parseByte(tok[2], addr) || addr > 0x7f) { fail(F("bad address")); return; }
    Wire.beginTransmission(addr);
    for (uint8_t i = 3; i < ntok; i++) {
      if (!parseByte(tok[i], b)) { Wire.endTransmission(); fail(F("bad byte")); return; }
      Wire.write(b);
    }
    uint8_t e = Wire.endTransmission();
    if (e) { Serial.print(F("{\"ok\":false")); idField(); Serial.print(F(",\"err\":\"nack\",\"code\":")); Serial.print(e); Serial.println('}'); return; }
    beginOk(); kv(F("addr"), addr); kv(F("written"), ntok - 3); endReply();
    return;
  }

  fail(F("i2c: scan, speed, read or write"));
}

// ---------------------------------------------------------------- SPI

static void cmdSpi() {
  if (!needArgs(2)) return;
  const char *sub = tok[1];

  if (!strcmp(sub, "cfg")) {           // spi cfg <hz> <mode 0-3> [msb|lsb]
    if (!needArgs(4)) return;
    long hz, mode;
    if (!parseNum(tok[2], hz) || hz < 1000 || hz > 50000000L) { fail(F("bad clock")); return; }
    if (!parseNum(tok[3], mode) || mode < 0 || mode > 3) { fail(F("mode must be 0-3")); return; }
    bool lsb = ntok > 4 && !strcmp(tok[4], "lsb");
    switch (mode) {  // SPI_MODEx types differ between cores, so no table
      case 0:  spiSettings = SPISettings((uint32_t)hz, lsb ? LSBFIRST : MSBFIRST, SPI_MODE0); break;
      case 1:  spiSettings = SPISettings((uint32_t)hz, lsb ? LSBFIRST : MSBFIRST, SPI_MODE1); break;
      case 2:  spiSettings = SPISettings((uint32_t)hz, lsb ? LSBFIRST : MSBFIRST, SPI_MODE2); break;
      default: spiSettings = SPISettings((uint32_t)hz, lsb ? LSBFIRST : MSBFIRST, SPI_MODE3); break;
    }
    beginOk(); kv(F("hz"), hz); kv(F("mode"), mode); kvf(F("order"), lsb ? F("lsb") : F("msb")); endReply();
    return;
  }

  if (!strcmp(sub, "xfer")) {          // spi xfer <cs|none> <b0> [b1...]
    if (!needArgs(4)) return;
    uint8_t cs = 255;
    if (strcmp(tok[2], "none")) { cs = needPin(tok[2]); if (cs == 255) return; }
    uint8_t tx[MAX_TOKENS], n = 0;
    for (uint8_t i = 3; i < ntok; i++) {
      if (!parseByte(tok[i], tx[n])) { fail(F("bad byte")); return; }
      n++;
    }
    if (!spiStarted) { SPI.begin(); spiStarted = true; }
    if (cs != 255) { pinMode(cs, OUTPUT); digitalWrite(cs, HIGH); }
    SPI.beginTransaction(spiSettings);
    if (cs != 255) digitalWrite(cs, LOW);
    for (uint8_t i = 0; i < n; i++) tx[i] = SPI.transfer(tx[i]);   // rx replaces tx
    if (cs != 255) digitalWrite(cs, HIGH);
    SPI.endTransaction();
    beginOk(); kv(F("n"), n); printBytes(F("rx"), tx, n); endReply();
    return;
  }

  fail(F("spi: cfg or xfer"));
}

// ---------------------------------------------------------------- target UART

static void cmdUart() {
#ifndef HAS_TARGET_UART
  fail(F("this board has no spare hardware UART"));
#else
  if (!needArgs(2)) return;
  const char *sub = tok[1];
  static bool started = false;
  static long baud = 115200;

  if (!strcmp(sub, "baud")) {
    long b;
    if (!needArgs(3) || !parseNum(tok[2], b) || b < 300 || b > 2000000L) { if (ntok >= 3) fail(F("bad baud")); return; }
    baud = b; TARGET_UART.end(); TARGET_UART.begin(baud); started = true;
    beginOk(); kv(F("baud"), baud); endReply();
    return;
  }
  if (!started) { TARGET_UART.begin(baud); started = true; }

  if (!strcmp(sub, "send")) {          // uart send <b0> [b1...]
    if (!needArgs(3)) return;
    uint8_t b;
    for (uint8_t i = 2; i < ntok; i++) {
      if (!parseByte(tok[i], b)) { fail(F("bad byte")); return; }
      TARGET_UART.write(b);
    }
    beginOk(); kv(F("sent"), ntok - 2); endReply();
    return;
  }

  if (!strcmp(sub, "text")) {          // uart text <words...>  (sends words + \r\n)
    if (!needArgs(3)) return;
    long sent = 0;
    for (uint8_t i = 2; i < ntok; i++) {
      if (i > 2) { TARGET_UART.write(' '); sent++; }
      sent += TARGET_UART.print(tok[i]);
    }
    sent += TARGET_UART.print("\r\n");
    beginOk(); kv(F("sent"), sent); endReply();
    return;
  }

  if (!strcmp(sub, "read")) {          // uart read <ms>
    long ms = 100;
    if (ntok > 2 && (!parseNum(tok[2], ms) || ms < 0 || (unsigned long)ms > MAX_WINDOW_MS)) { fail(F("window must be 0-10000 ms")); return; }
    beginOk(); kv(F("baud"), baud); key(F("rx")); Serial.print('[');
    uint32_t start = millis(); uint16_t n = 0;
    while (millis() - start < (uint32_t)ms || TARGET_UART.available()) {
      if (TARGET_UART.available()) {
        if (n++) Serial.print(',');
        Serial.print(TARGET_UART.read());
        if (n >= 4096) break;
      }
    }
    Serial.print(']'); kv(F("n"), n); endReply();
    return;
  }

  fail(F("uart: baud, send, text or read"));
#endif
}

// ---------------------------------------------------------------- dispatch

static void dispatch() {
  // Tokenize in place.
  ntok = 0; reqId = NULL;
  char *save = NULL;
  for (char *t = strtok_r(line, " \t", &save); t && ntok < MAX_TOKENS; t = strtok_r(NULL, " \t", &save)) {
    if (ntok == 0 && t[0] == '#' && !reqId) { reqId = t + 1; continue; }
    tok[ntok++] = t;
  }
  if (ntok == 0) return;

  const char *c = tok[0];
  if      (!strcmp(c, "info"))  cmdInfo();
  else if (!strcmp(c, "help"))  cmdHelp();
  else if (!strcmp(c, "pin"))   cmdPin();
  else if (!strcmp(c, "read"))  cmdRead();
  else if (!strcmp(c, "write")) cmdWrite();
  else if (!strcmp(c, "pwm"))   cmdPwm();
  else if (!strcmp(c, "adc"))   cmdAdc();
  else if (!strcmp(c, "adcs"))  cmdAdcSample();
  else if (!strcmp(c, "vref"))  cmdVref();
  else if (!strcmp(c, "watch")) cmdWatch();
  else if (!strcmp(c, "cap"))   cmdCapture();
  else if (!strcmp(c, "freq"))  cmdFreq();
  else if (!strcmp(c, "pulse")) cmdPulse();
  else if (!strcmp(c, "i2c"))   cmdI2c();
  else if (!strcmp(c, "spi"))   cmdSpi();
  else if (!strcmp(c, "uart"))  cmdUart();
  else fail(F("unknown command, try help"));
}

void setup() {
  Serial.begin(BAUD);
#if defined(USBCON) || defined(ARDUINO_ARCH_SAMD) || defined(ARDUINO_ARCH_RP2040)
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) {}
#endif
#if !defined(__AVR__)
  analogReadResolution(ADC_BITS);
#endif
  Serial.print(F("{\"ok\":true,\"event\":\"boot\",\"fw\":\"" FW_NAME "\",\"ver\":\"" FW_VERSION "\"}\n"));
}

void loop() {
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      if (lineOverflow) { reqId = NULL; fail(F("line too long")); }
      else { line[lineLen] = '\0'; dispatch(); }
      lineLen = 0; lineOverflow = false;
      continue;
    }
    if (lineLen < MAX_LINE - 1) line[lineLen++] = ch;
    else lineOverflow = true;
  }
}
