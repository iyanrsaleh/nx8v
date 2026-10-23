#!/bin/sh
# Install nx8v onto PATH for the current user.
#   ./install.sh            copy the binary to ~/.local/bin
#   ./install.sh --remove   delete it
#   sudo ./install.sh --service /path/to/project
#       install a systemd unit that runs `nx8v start` in that directory

set -eu

PREFIX="${HOME}/.local"
BIN_DIR="${PREFIX}/bin"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
SRC=""

for candidate in \
  "${SCRIPT_DIR}/build/nx8v" \
  "${SCRIPT_DIR}/build/Release/nx8v" \
  "${SCRIPT_DIR}/nx8v"
do
  if [ -x "${candidate}" ]; then
    SRC="${candidate}"
    break
  fi
done

remove=0
service_dir=""
while [ $# -gt 0 ]; do
  case "$1" in
    --remove) remove=1 ;;
    --service)
      shift
      service_dir="${1:-}"
      ;;
    *)
      echo "usage: install.sh [--remove] [--service PROJECT_DIR]" >&2
      exit 1
      ;;
  esac
  shift
done

if [ "${remove}" -eq 1 ]; then
  rm -f "${BIN_DIR}/nx8v"
  echo "removed ${BIN_DIR}/nx8v"
  exit 0
fi

if [ -n "${service_dir}" ]; then
  if [ ! -d "${service_dir}" ]; then
    echo "project directory not found: ${service_dir}" >&2
    exit 1
  fi
  unit_src="${SCRIPT_DIR}/packaging/nx8v.service"
  if [ ! -f "${unit_src}" ]; then
    echo "missing ${unit_src}" >&2
    exit 1
  fi
  tmp=$(mktemp)
  sed "s|@PROJECT@|${service_dir}|g" "${unit_src}" > "${tmp}"
  if [ "$(id -u)" -eq 0 ]; then
    install -m 644 "${tmp}" /etc/systemd/system/nx8v.service
    systemctl daemon-reload
    echo "installed /etc/systemd/system/nx8v.service"
    echo "enable with: systemctl enable --now nx8v"
  else
    mkdir -p "${HOME}/.config/systemd/user"
    install -m 644 "${tmp}" "${HOME}/.config/systemd/user/nx8v.service"
    systemctl --user daemon-reload
    echo "installed ${HOME}/.config/systemd/user/nx8v.service"
    echo "enable with: systemctl --user enable --now nx8v"
  fi
  rm -f "${tmp}"
  exit 0
fi

tmp=""
if [ -z "${SRC}" ]; then
  tmp=$(mktemp -d)
  curl -fsSL "https://github.com/iyanrsaleh/nx8v/releases/latest/download/SHA256SUMS" -o "${tmp}/SHA256SUMS"
  curl -fsSL "https://github.com/iyanrsaleh/nx8v/releases/latest/download/nx8v-linux-x64.tar.gz" -o "${tmp}/nx8v.tar.gz"
  want=$(awk '$2 == "nx8v-linux-x64.tar.gz" || $2 == "*nx8v-linux-x64.tar.gz" { print $1 }' "${tmp}/SHA256SUMS")
  got=$(sha256sum "${tmp}/nx8v.tar.gz" | awk '{ print $1 }')
  if [ -z "${want}" ] || [ "${got}" != "${want}" ]; then
    echo "nx8v-linux-x64.tar.gz hash does not match the release." >&2
    exit 1
  fi
  tar -xzf "${tmp}/nx8v.tar.gz" -C "${tmp}"
  SRC=$(find "${tmp}" -type f -name nx8v | head -n 1)
  if [ -z "${SRC}" ]; then
    echo "nx8v binary not found in the GitHub release." >&2
    exit 1
  fi
fi

mkdir -p "${BIN_DIR}"
install -m 755 "${SRC}" "${BIN_DIR}/nx8v"
if [ -n "${tmp}" ]; then
  so=$(find "${tmp}" -type f -name libnx8v-window.so | head -n 1)
  if [ -n "${so}" ]; then
    install -m 755 "${so}" "${BIN_DIR}/libnx8v-window.so"
  fi
fi
echo "installed ${BIN_DIR}/nx8v"
if [ -n "${tmp}" ]; then
  rm -rf "${tmp}"
fi

case ":${PATH}:" in
  *":${BIN_DIR}:"*) ;;
  *)
    echo "add this to your shell profile if it is not already there:"
    echo "  export PATH=\"${BIN_DIR}:\$PATH\""
    ;;
esac
