#!/usr/bin/env bash
# benchpi setup: turn a Raspberry Pi (or any Debian/Ubuntu box) into an
# embedded-systems bench host that people and coding agents can drive.
#
#   ./setup.sh                      # default components
#   ./setup.sh --only base,adb      # pick components
#   ./setup.sh --with jlink --accept-jlink-license
#   ./setup.sh --with ssm           # needs SSM_ACTIVATION_CODE/ID and AWS_REGION
#   ./setup.sh --list               # show components
#
# Safe to re-run: every step checks before it installs.

set -euo pipefail

BENCHPI_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCHPI_PREFIX="${BENCHPI_PREFIX:-/opt/benchpi}"
VENV="$BENCHPI_PREFIX/venv"

DEFAULT_COMPONENTS="base python udev serial adb openocd pyocd sigrok arduino probe"
ALL_COMPONENTS="$DEFAULT_COMPONENTS jlink ssm"

ACCEPT_JLINK_LICENSE="${ACCEPT_JLINK_LICENSE:-0}"

log()  { printf '\033[1;36m[benchpi]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[benchpi] warning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[benchpi] error:\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
  sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
}

# ---------------------------------------------------------------- helpers

SUDO=""
if [[ $EUID -ne 0 ]]; then
  command -v sudo >/dev/null || die "run as root or install sudo"
  SUDO="sudo"
fi

# The non-root user who will use the bench (gets group memberships).
BENCH_USER="${SUDO_USER:-${USER:-$(id -un)}}"

APT_UPDATED=0
apt_install() {
  local missing=()
  for pkg in "$@"; do
    dpkg -s "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
  done
  [[ ${#missing[@]} -eq 0 ]] && return 0
  if [[ $APT_UPDATED -eq 0 ]]; then
    $SUDO apt-get update -y
    APT_UPDATED=1
  fi
  $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${missing[@]}"
}

# Install the first package name that exists in the apt index.
apt_install_any() {
  local pkg
  for pkg in "$@"; do
    if apt-cache show "$pkg" >/dev/null 2>&1; then
      apt_install "$pkg"
      return 0
    fi
  done
  warn "none of these packages are available: $*"
  return 1
}

add_groups() {
  local g
  for g in "$@"; do
    getent group "$g" >/dev/null || $SUDO groupadd -f "$g"
    if ! id -nG "$BENCH_USER" | tr ' ' '\n' | grep -qx "$g"; then
      $SUDO usermod -aG "$g" "$BENCH_USER"
      NEED_RELOGIN=1
    fi
  done
}

deb_arch() { dpkg --print-architecture; }

link_bin() { # link_bin <source> <name>
  $SUDO ln -sf "$1" "/usr/local/bin/$2"
}

# ---------------------------------------------------------------- components

install_base() {
  log "base tools"
  apt_install git curl wget ca-certificates build-essential cmake ninja-build \
    pkg-config jq unzip xz-utils usbutils pciutils i2c-tools gpiod \
    libusb-1.0-0 libusb-1.0-0-dev libhidapi-hidraw0 tmux
  add_groups plugdev dialout i2c gpio spi
}

install_python() {
  log "python venv at $VENV"
  apt_install python3 python3-venv python3-pip
  if [[ ! -x "$VENV/bin/python" ]]; then
    $SUDO mkdir -p "$BENCHPI_PREFIX"
    $SUDO python3 -m venv "$VENV"
  fi
  $SUDO "$VENV/bin/pip" install --quiet --upgrade pip
  $SUDO "$VENV/bin/pip" install --quiet -r "$BENCHPI_DIR/requirements.txt"
}

install_udev() {
  log "udev rules"
  $SUDO install -m 0644 "$BENCHPI_DIR/udev/99-benchpi.rules" /etc/udev/rules.d/99-benchpi.rules
  $SUDO udevadm control --reload-rules || warn "udev not running; rules apply after reboot"
  $SUDO udevadm trigger || true
}

install_serial() {
  log "serial consoles"
  apt_install picocom minicom screen socat lrzsz
}

install_adb() {
  log "adb and fastboot"
  apt_install_any adb android-tools-adb
  apt_install_any fastboot android-tools-fastboot
  # Ships the Android vendor udev rules on Debian/Raspberry Pi OS.
  apt_install_any android-sdk-platform-tools-common || true
}

install_openocd() {
  log "OpenOCD"
  apt_install openocd
}

install_pyocd() {
  log "pyOCD (CMSIS-DAP and other Arm debug probes)"
  [[ -x "$VENV/bin/python" ]] || install_python
  $SUDO "$VENV/bin/pip" install --quiet pyocd
  link_bin "$VENV/bin/pyocd" pyocd
}

install_sigrok() {
  log "sigrok-cli (for real USB logic analyzers)"
  apt_install sigrok-cli
}

install_arduino() {
  log "arduino-cli"
  if ! command -v arduino-cli >/dev/null; then
    local tmp
    tmp="$(mktemp -d)"
    curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh -o "$tmp/install.sh"
    $SUDO env BINDIR=/usr/local/bin sh "$tmp/install.sh"
    rm -rf "$tmp"
  fi
  # Core for the default probe board (Uno/Nano). Add others with
  #   arduino-cli core install <core>
  sudo -u "$BENCH_USER" -H arduino-cli core update-index
  sudo -u "$BENCH_USER" -H arduino-cli core install arduino:avr
}

install_probe() {
  log "benchprobe CLI and bench-status"
  [[ -x "$VENV/bin/python" ]] || install_python
  local wrapper=/usr/local/bin/benchprobe
  $SUDO tee "$wrapper" >/dev/null <<EOF
#!/bin/sh
exec "$VENV/bin/python" "$BENCHPI_DIR/benchprobe/host/benchprobe.py" "\$@"
EOF
  $SUDO chmod 0755 "$wrapper"
  link_bin "$BENCHPI_DIR/tools/bench-status" bench-status
  link_bin "$BENCHPI_DIR/tools/flash-probe" flash-probe
}

install_jlink() {
  log "SEGGER J-Link software"
  if command -v JLinkExe >/dev/null; then
    log "J-Link already installed"; return 0
  fi
  local deb="${JLINK_DEB:-}"
  if [[ -z "$deb" ]]; then
    if [[ "$ACCEPT_JLINK_LICENSE" != "1" ]]; then
      die "J-Link is distributed under SEGGER's license (https://www.segger.com/downloads/jlink/).
Re-run with --accept-jlink-license to download it, or download the .deb yourself
and run: JLINK_DEB=/path/to/JLink_Linux_*.deb ./setup.sh --only jlink"
    fi
    local pkg
    case "$(deb_arch)" in
      arm64) pkg=JLink_Linux_arm64.deb ;;
      armhf) pkg=JLink_Linux_arm.deb ;;
      amd64) pkg=JLink_Linux_x86_64.deb ;;
      *) die "no J-Link package known for $(deb_arch); download it manually and set JLINK_DEB" ;;
    esac
    deb="$(mktemp -d)/$pkg"
    curl -fSL -o "$deb" \
      --data "accept_license_agreement=accepted&submit=Download+software" \
      "https://www.segger.com/downloads/jlink/$pkg"
  fi
  $SUDO apt-get install -y "$deb"
}

install_ssm() {
  log "AWS Systems Manager agent (remote access without inbound ports)"
  if ! command -v amazon-ssm-agent >/dev/null; then
    local arch
    case "$(deb_arch)" in
      arm64) arch=debian_arm64 ;;
      armhf) arch=debian_arm ;;
      amd64) arch=debian_amd64 ;;
      *) die "no SSM agent package known for $(deb_arch)" ;;
    esac
    local deb
    deb="$(mktemp -d)/amazon-ssm-agent.deb"
    curl -fsSL -o "$deb" "https://s3.amazonaws.com/ec2-downloads-windows/SSMAgent/latest/$arch/amazon-ssm-agent.deb"
    $SUDO apt-get install -y "$deb"
  fi
  if [[ -n "${SSM_ACTIVATION_CODE:-}" && -n "${SSM_ACTIVATION_ID:-}" && -n "${AWS_REGION:-}" ]]; then
    $SUDO systemctl stop amazon-ssm-agent || true
    $SUDO amazon-ssm-agent -register -y \
      -code "$SSM_ACTIVATION_CODE" -id "$SSM_ACTIVATION_ID" -region "$AWS_REGION"
    $SUDO systemctl enable --now amazon-ssm-agent
  else
    warn "SSM agent installed but not registered. Create a hybrid activation in AWS,
then re-run with SSM_ACTIVATION_CODE, SSM_ACTIVATION_ID and AWS_REGION set."
  fi
}

# ---------------------------------------------------------------- main

components="$DEFAULT_COMPONENTS"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --only) components="${2//,/ }"; shift 2 ;;
    --with) components="$components ${2//,/ }"; shift 2 ;;
    --all) components="$ALL_COMPONENTS"; shift ;;
    --accept-jlink-license) ACCEPT_JLINK_LICENSE=1; shift ;;
    --list) echo "default: $DEFAULT_COMPONENTS"; echo "optional: jlink ssm"; exit 0 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
done

command -v apt-get >/dev/null || die "benchpi needs a Debian-based OS (Raspberry Pi OS, Debian, Ubuntu)"

NEED_RELOGIN=0
for c in $components; do
  case " $ALL_COMPONENTS " in
    *" $c "*) "install_$c" ;;
    *) die "unknown component: $c (try --list)" ;;
  esac
done

log "done. Run 'bench-status' to see what's attached."
if [[ $NEED_RELOGIN -eq 1 ]]; then
  warn "$BENCH_USER was added to new groups; log out and back in (or reboot) before using USB devices."
fi
