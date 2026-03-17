#!/usr/bin/env bash
set -euo pipefail

: "${GAOKUN_DIR:?missing GAOKUN_DIR}"
: "${WORKDIR:?missing WORKDIR}"
: "${KERN_SRC:?missing KERN_SRC}"
: "${KERN_OUT:?missing KERN_OUT}"
: "${ARTIFACT_DIR:?missing ARTIFACT_DIR}"

mkdir -p "$WORKDIR" "$KERN_OUT" "$ARTIFACT_DIR"

if [[ "$(uname -m)" == "aarch64" ]]; then
  CROSS_COMPILE=""
else
  CROSS_COMPILE="aarch64-linux-gnu-"
fi

export ARCH=arm64
export CCACHE_DIR="${CCACHE_DIR:-$HOME/.ccache}"
export PATH="/usr/lib/ccache:$PATH"

git -C "$KERN_SRC" config user.name "github-actions[bot]"
git -C "$KERN_SRC" config user.email "github-actions[bot]@users.noreply.github.com"

git -C "$KERN_SRC" am "$GAOKUN_DIR"/gaokun-patches/*.patch

unset KCONFIG_CONFIG
make -C "$KERN_SRC" O="$KERN_OUT" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" defconfig
make -C "$KERN_SRC" O="$KERN_OUT" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" olddefconfig
make -C "$KERN_SRC" O="$KERN_OUT" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" -j"$(nproc)"

KREL="$(cat "$KERN_OUT/include/config/kernel.release")"
echo "$KREL" > "$WORKDIR/kernel-release.txt"

make -C "$KERN_SRC" O="$KERN_OUT" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" savedefconfig
cp "$KERN_OUT/defconfig" "$ARTIFACT_DIR/gaokun3_defconfig"
