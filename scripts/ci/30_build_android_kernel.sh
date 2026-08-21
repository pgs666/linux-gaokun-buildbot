#!/usr/bin/env bash
set -euo pipefail

: "${GAOKUN_DIR:?missing GAOKUN_DIR}"
: "${GAOKUN_ANDROID_DIR:?missing GAOKUN_ANDROID_DIR}"
: "${KERN_SRC:?missing KERN_SRC}"
: "${KERN_OUT:?missing KERN_OUT}"
: "${ARTIFACT_DIR:?missing ARTIFACT_DIR}"

if [[ "$(uname -m)" == "aarch64" ]]; then
    CROSS_COMPILE="${CROSS_COMPILE:-}"
else
    CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
fi

export ARCH=arm64
export CROSS_COMPILE
export CCACHE_DIR="${CCACHE_DIR:-$HOME/.ccache}"
export CCACHE_BASEDIR="${CCACHE_BASEDIR:-$GAOKUN_DIR}"
export CCACHE_NOHASHDIR=true
export CCACHE_COMPILERCHECK=content
export PATH="/usr/lib/ccache:$PATH"

git -C "$KERN_SRC" config user.name "github-actions[bot]"
git -C "$KERN_SRC" config user.email "github-actions[bot]@users.noreply.github.com"

git -C "$KERN_SRC" am "$GAOKUN_DIR"/patches/upstream/*.patch
git -C "$KERN_SRC" am "$GAOKUN_DIR"/patches/others/*.patch
git -C "$KERN_SRC" am "$GAOKUN_DIR"/patches/media/*.patch
git -C "$KERN_SRC" am "$GAOKUN_DIR"/patches/0099-arm64-gaokun3-import-local-dts-and-defconfig.patch

KERN_SRC="$KERN_SRC" GAOKUN_ANDROID_DIR="$GAOKUN_ANDROID_DIR" \
    "$GAOKUN_DIR/scripts/ci/25_prepare_android_kernel.sh"

mkdir -p "$KERN_OUT" "$ARTIFACT_DIR"
cp "$GAOKUN_DIR/defconfig/gaokun3_android.config" "$KERN_OUT/.config"

(
    cd "$KERN_SRC"
    CROSS_COMPILE="$CROSS_COMPILE" \
        bash "$GAOKUN_ANDROID_DIR/scripts/kernel-config-android.sh" "$KERN_OUT"
)

ccache -z || true
make -C "$KERN_SRC" O="$KERN_OUT" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" \
    -j"$(nproc)" vmlinuz.efi dtbs
ccache -s || true

VMLINUX="$KERN_OUT/arch/arm64/boot/vmlinuz.efi"
IMAGE="$KERN_OUT/arch/arm64/boot/Image"
DTB="$KERN_OUT/arch/arm64/boot/dts/qcom/sc8280xp-huawei-gaokun3.dtb"

test -s "$VMLINUX"
test -s "$IMAGE"
test -s "$DTB"
test -s "$KERN_OUT/.config"

cp "$VMLINUX" "$ARTIFACT_DIR/vmlinuz.efi"
cp "$IMAGE" "$ARTIFACT_DIR/Image"
cp "$DTB" "$ARTIFACT_DIR/gaokun3.dtb"
cp "$KERN_OUT/.config" "$ARTIFACT_DIR/config"
(
    cd "$ARTIFACT_DIR"
    sha256sum vmlinuz.efi Image gaokun3.dtb config > SHA256SUMS
)
