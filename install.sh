#!/bin/sh
# Hostmortem installer for Debian and Ubuntu (amd64, arm64).
#
#   curl -fsSL https://github.com/yeegz/hostmortem/releases/latest/download/install.sh | sudo sh
#
# What it does, and nothing else:
#   1. picks the .deb for this architecture from a GitHub release
#      (latest, or HM_VERSION=0.2.0 for a specific one);
#   2. checks it against the release's SHA256SUMS and stops on a mismatch;
#   3. installs it with apt (which creates the 'hostmortem' service user);
#   4. enables and starts the hmd service (skip with HM_NO_START=1);
#   5. adds the user who ran sudo to the 'hostmortem' group, so hm can read
#      the store without sudo after their next login.
# Re-running it upgrades in place; the evidence store is kept and migrated.
# Remove with 'sudo apt remove hostmortem' (keeps the store) or
# 'sudo apt purge hostmortem' (deletes the store and the service user).
#
# Environment:
#   HM_VERSION    release to install, e.g. 0.2.0 (default: latest)
#   HM_DEB_DIR    install from a local directory holding the .deb and SHA256SUMS
#                 instead of downloading (used by CI and for offline installs)
#   HM_NO_START   1 = install but do not enable or start hmd
set -eu

REPO="https://github.com/yeegz/hostmortem"
MIN_GLIBC="2.35"

say() { printf 'hostmortem: %s\n' "$*"; }
die() { printf 'hostmortem: error: %s\n' "$*" >&2; exit 1; }

# Version comparison for dotted numbers: true when $1 >= $2.
version_ge() {
  [ "$(printf '%s\n%s\n' "$2" "$1" | sort -t. -k1,1n -k2,2n -k3,3n | head -n1)" = "$2" ]
}

fetch() {  # url dest
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL --proto '=https' --tlsv1.2 -o "$2" "$1"
  elif command -v wget >/dev/null 2>&1; then
    wget -q --https-only -O "$2" "$1"
  else
    die "need curl or wget to download the package"
  fi
}

latest_version() {
  if command -v curl >/dev/null 2>&1; then
    url=$(curl -fsSLI --proto '=https' -o /dev/null -w '%{url_effective}' "$REPO/releases/latest")
  else
    url=$(wget -q --https-only --max-redirect=5 -S -O /dev/null "$REPO/releases/latest" 2>&1 |
          sed -n 's/^ *[Ll]ocation: *//p' | tail -n1 | tr -d '\r')
  fi
  v=${url##*/tag/v}
  case "$v" in
    [0-9]*.[0-9]*.[0-9]*) printf '%s\n' "$v" ;;
    *) die "could not work out the latest release from $REPO/releases/latest" ;;
  esac
}

# Everything runs inside main, called on the last line, so a download cut
# short by the network cannot run half a script.
main() {
  [ "$(uname -s)" = "Linux" ] || die "Hostmortem records Linux hosts; this is $(uname -s)."
  [ "$(id -u)" -eq 0 ] || die "run as root, e.g.: curl -fsSL $REPO/releases/latest/download/install.sh | sudo sh"
  if ! command -v dpkg >/dev/null 2>&1 || ! command -v apt-get >/dev/null 2>&1; then
    die "this installer needs dpkg and apt (Debian, Ubuntu, Raspberry Pi OS). Other distributions: build from source, see $REPO#build-from-source-any-os-with-docker"
  fi

  arch=$(dpkg --print-architecture)
  case "$arch" in
    amd64|arm64) ;;
    *) die "no package for '$arch' yet (amd64 and arm64 only). Build from source: $REPO" ;;
  esac

  glibc=$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}')
  [ -n "$glibc" ] || die "could not read the glibc version (getconf GNU_LIBC_VERSION)"
  version_ge "$glibc" "$MIN_GLIBC" ||
    die "glibc $glibc is older than $MIN_GLIBC (Ubuntu 22.04 / Debian 12 or newer needed)"

  work=$(mktemp -d)
  trap 'rm -rf "$work"' EXIT INT TERM

  if [ -n "${HM_DEB_DIR:-}" ]; then
    deb=$(find "$HM_DEB_DIR" -maxdepth 1 -name "hostmortem_*_${arch}.deb" | sort | tail -n1)
    [ -n "$deb" ] || die "no hostmortem_*_${arch}.deb in $HM_DEB_DIR"
    [ -f "$HM_DEB_DIR/SHA256SUMS" ] || die "no SHA256SUMS in $HM_DEB_DIR"
    cp "$deb" "$HM_DEB_DIR/SHA256SUMS" "$work/"
    name=$(basename "$deb")
    say "installing $name from $HM_DEB_DIR"
  else
    version=${HM_VERSION:-}
    [ -n "$version" ] || version=$(latest_version)
    version=${version#v}
    name="hostmortem_${version}_${arch}.deb"
    say "downloading $name (v$version) from $REPO/releases"
    fetch "$REPO/releases/download/v$version/$name" "$work/$name"
    fetch "$REPO/releases/download/v$version/SHA256SUMS" "$work/SHA256SUMS"
  fi

  (
    cd "$work"
    grep " $name\$" SHA256SUMS > expected || die "$name is not listed in SHA256SUMS"
    sha256sum --check --status expected || die "checksum mismatch for $name: not installing it"
  )
  say "checksum OK ($(cut -c1-16 "$work/expected")...)"

  was=$(dpkg-query -W -f='${Status} ${Version}' hostmortem 2>/dev/null | awk '$3=="installed"{print $4}') || true
  export DEBIAN_FRONTEND=noninteractive
  # apt needs a path with a slash to treat the argument as a file.
  if ! apt-get install -y -q "$work/$name" >"$work/apt.log" 2>&1; then
    say "apt could not resolve dependencies; refreshing package lists and retrying"
    apt-get update -q >>"$work/apt.log" 2>&1 || true
    apt-get install -y -q "$work/$name" >>"$work/apt.log" 2>&1 || { cat "$work/apt.log" >&2; die "apt install failed"; }
  fi
  now=$(dpkg-query -W -f='${Version}' hostmortem)
  if [ -n "$was" ] && [ "$was" != "$now" ]; then
    say "upgraded $was -> $now (evidence store kept)"
  else
    say "installed $now"
  fi

  if [ "${HM_NO_START:-0}" != "1" ]; then
    if [ -d /run/systemd/system ]; then
      systemctl enable --now hmd >/dev/null 2>&1 || die "could not start hmd: systemctl status hmd"
      say "hmd is running and starts at boot (systemctl status hmd)"
    else
      say "systemd is not running here; start the agent yourself: hmd --config /etc/hostmortem/hmd.conf"
    fi
  fi

  user=${SUDO_USER:-}
  if [ -n "$user" ] && [ "$user" != "root" ] && id "$user" >/dev/null 2>&1; then
    if id -nG "$user" | tr ' ' '\n' | grep -qx hostmortem; then
      :
    else
      usermod -aG hostmortem "$user"
      say "added $user to the 'hostmortem' group (takes effect at the next login; until then use sudo hm)"
    fi
  fi

  cat <<'EOF'

Next:
  sudo hm doctor          what this host lets Hostmortem see, and what each gap costs
  sudo hm drill           record a harmless test incident (about 10 seconds)
  sudo hm incidents       list incidents; sudo hm explain <id> for a diagnosis
  sudo hm report          one HTML file with every incident, to open or share
                          (add --redact to replace IP and MAC addresses)

Config: /etc/hostmortem/hmd.conf (watch your own services under [watch]).
Evidence: /var/lib/hostmortem, at most 14 days or 256 MB by default.
EOF
}

main "$@" </dev/null
