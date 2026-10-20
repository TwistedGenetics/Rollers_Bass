#!/usr/bin/env bash
# Build Rollers for the Korg NTS-1 mkII and microKORG 2 without Docker.
#
#   ./build.sh            build both units into dist/
#   ./build.sh nts1       NTS-1 mkII only   -> dist/Rollers.nts1mkiiunit
#   ./build.sh mk2        microKORG 2 only  -> dist/Rollers.mk2unit
#
# Toolchains and the logue SDK are downloaded into .deps/ on first run.
#   NTS-1 mkII : Arm GCC 10.3-2021.10 (Korg's toolchain), xPack GCC 10.3 as fallback
#   microKORG 2: Korg's arm-unknown-linux-gnueabihf toolchain (same download as Korg's Docker image)
# Set ROLLERS_STRICT=1 (as CI does) to fail instead of falling back or skipping.
set -euo pipefail

TARGET="${1:-all}"
case "$TARGET" in all|nts1|mk2) ;; *) echo "Usage: $0 [all|nts1|mk2]"; exit 1 ;; esac

ROOT="$(cd "$(dirname "$0")" && pwd)"
DEPS="$ROOT/.deps"
SDK_DIR="$DEPS/logue-sdk"
SDK_REPO="https://github.com/korginc/logue-sdk"
SDK_COMMIT="e78b16c6068b1c30fc072f2aebb8da5eef6c005c"
STRICT="${ROLLERS_STRICT:-0}"

mkdir -p "$DEPS" "$ROOT/dist"

if [ ! -d "$SDK_DIR/platform/nts-1_mkii" ]; then
  echo "==> Fetching logue SDK"
  git clone -q "$SDK_REPO" "$SDK_DIR"
  git -C "$SDK_DIR" checkout -q "$SDK_COMMIT"
  git -C "$SDK_DIR" submodule update -q --init --depth 1 platform/ext/CMSIS
fi

HOST="$(uname -s)-$(uname -m)"

# ---------------------------------------------------------------------------
# NTS-1 mkII
# ---------------------------------------------------------------------------
build_nts1() {
  local ARM_VER="10.3-2021.10"
  local ARM_BASE="https://developer.arm.com/-/media/Files/downloads/gnu-rm/$ARM_VER"
  local XPACK_VER="10.3.1-2.3"
  local XPACK_BASE="https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/v$XPACK_VER"
  local ARM_PKG XPACK_PLAT
  case "$HOST" in
    Linux-x86_64)  ARM_PKG="gcc-arm-none-eabi-$ARM_VER-x86_64-linux.tar.bz2";  XPACK_PLAT="linux-x64" ;;
    Linux-aarch64) ARM_PKG="gcc-arm-none-eabi-$ARM_VER-aarch64-linux.tar.bz2"; XPACK_PLAT="linux-arm64" ;;
    Darwin-arm64)  ARM_PKG="gcc-arm-none-eabi-$ARM_VER-mac.tar.bz2";            XPACK_PLAT="darwin-arm64" ;;
    Darwin-x86_64) ARM_PKG="gcc-arm-none-eabi-$ARM_VER-mac.tar.bz2";            XPACK_PLAT="darwin-x64" ;;
    *) echo "Unsupported platform: $HOST"; exit 1 ;;
  esac
  local ARM_DIR="$DEPS/gcc-arm-none-eabi-$ARM_VER"
  local XPACK_DIR="$DEPS/xpack-arm-none-eabi-gcc-$XPACK_VER"
  local GCC_DIR

  if [ ! -x "$ARM_DIR/bin/arm-none-eabi-gcc" ]; then
    echo "==> Downloading Arm GNU toolchain $ARM_VER"
    curl -fsSL "$ARM_BASE/$ARM_PKG" | tar xj -C "$DEPS" || echo "    (Arm download unavailable)"
  fi
  if [ -x "$ARM_DIR/bin/arm-none-eabi-gcc" ]; then
    GCC_DIR="$ARM_DIR"
  elif [ "$STRICT" = "1" ]; then
    echo "Arm toolchain required (ROLLERS_STRICT=1) but not available"; exit 1
  else
    if [ ! -x "$XPACK_DIR/bin/arm-none-eabi-gcc" ]; then
      echo "==> Downloading xPack ARM GCC $XPACK_VER (fallback)"
      curl -fsSL "$XPACK_BASE/xpack-arm-none-eabi-gcc-$XPACK_VER-$XPACK_PLAT.tar.gz" | tar xz -C "$DEPS"
    fi
    GCC_DIR="$XPACK_DIR"
  fi
  echo "==> NTS-1 mkII toolchain: $("$GCC_DIR/bin/arm-none-eabi-gcc" --version | head -1)"

  local PLAT="$SDK_DIR/platform/nts-1_mkii"
  echo "==> Building Rollers for NTS-1 mkII"
  # clean first: the Makefile doesn't track the shared core/ header
  make -s -C "$ROOT/nts1mkii" clean >/dev/null
  make -C "$ROOT/nts1mkii" install \
    GCC_BIN_PATH="$GCC_DIR/bin" \
    COMMON_INC_PATH="$PLAT/common" \
    COMMON_SRC_PATH="$PLAT/common" \
    EXTDIR="$SDK_DIR/platform/ext" \
    LDDIR="$PLAT/ld" \
    INSTALLDIR="$ROOT/dist"
  echo "==> Done: dist/Rollers.nts1mkiiunit"
}

# ---------------------------------------------------------------------------
# microKORG 2
# ---------------------------------------------------------------------------
build_mk2() {
  # Same archives and checksums as logue-sdk/docker/docker-app/builder/get_microkorg2_toolchain.sh
  local URL SHA1
  case "$HOST" in
    Linux-x86_64)
      URL="https://www.dropbox.com/s/3curhjx9yip29os/drumlogue-toolchain-amd64.tar.bz2?dl=1"
      SHA1="6b2a403195fbac0f7ee8d53bea4ed17b8c14e541" ;;
    Linux-aarch64)
      URL="https://www.dropbox.com/s/ioysa018ruopp72/drumlogue-toolchain-aarch64.tar.bz2?dl=1"
      SHA1="d907b640967e8ea40baa82ff55d8813716028aa9" ;;
    *)
      URL="" ;;
  esac
  local TC_DIR="$DEPS/mk2-toolchain"
  local GCC

  GCC="$(find "$TC_DIR" -type f -name 'arm-unknown-linux-gnueabihf-gcc' -path '*/bin/*' 2>/dev/null | head -1 || true)"
  if [ -z "$GCC" ] && [ -n "$URL" ]; then
    echo "==> Downloading Korg microKORG 2 toolchain"
    local ARCHIVE="$DEPS/mk2-toolchain.tar.bz2"
    if curl -fsSL -o "$ARCHIVE" "$URL"; then
      local GOT
      GOT="$( (sha1sum "$ARCHIVE" 2>/dev/null || shasum -a 1 "$ARCHIVE") | cut -d' ' -f1)"
      if [ "$GOT" = "$SHA1" ]; then
        mkdir -p "$TC_DIR" && tar xjf "$ARCHIVE" -C "$TC_DIR"
      else
        echo "    (checksum mismatch: $GOT)"
      fi
      rm -f "$ARCHIVE"
    else
      echo "    (download unavailable)"
    fi
    GCC="$(find "$TC_DIR" -type f -name 'arm-unknown-linux-gnueabihf-gcc' -path '*/bin/*' 2>/dev/null | head -1 || true)"
  fi
  if [ -z "$GCC" ]; then
    if [ "$STRICT" = "1" ]; then
      echo "microKORG 2 toolchain required (ROLLERS_STRICT=1) but not available"; exit 1
    fi
    echo "==> Skipping microKORG 2: Korg's toolchain isn't available on this machine (GitHub Actions builds it)"
    return 0
  fi
  echo "==> microKORG 2 toolchain: $("$GCC" --version | head -1)"

  local PLAT="$SDK_DIR/platform/microkorg2"
  echo "==> Building Rollers for microKORG 2"
  make -s -C "$ROOT/microkorg2" clean >/dev/null
  make -C "$ROOT/microkorg2" install \
    CROSS_COMPILE="${GCC%gcc}" \
    COMMON_INC_PATH="$PLAT/common" \
    COMMON_SRC_PATH="$PLAT/common" \
    PLATFORM_COMMON_PATH="$SDK_DIR/platform/common" \
    INSTALLDIR="$ROOT/dist"
  echo "==> Done: dist/Rollers.mk2unit"
}

case "$TARGET" in
  nts1) build_nts1 ;;
  mk2)  build_mk2 ;;
  all)  build_nts1; build_mk2 ;;
esac
