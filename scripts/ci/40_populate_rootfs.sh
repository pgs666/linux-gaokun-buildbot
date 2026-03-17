#!/usr/bin/env bash
set -euo pipefail

: "${GAOKUN_DIR:?missing GAOKUN_DIR}"
: "${WORKDIR:?missing WORKDIR}"
: "${KERN_SRC:?missing KERN_SRC}"
: "${KERN_OUT:?missing KERN_OUT}"
: "${ROOTFS_DIR:?missing ROOTFS_DIR}"
: "${FW_CACHE_DIR:?missing FW_CACHE_DIR}"
: "${FW_BUILD_DIR:?missing FW_BUILD_DIR}"

KREL="$(cat "$WORKDIR/kernel-release.txt")"

if [[ "$(uname -m)" == "aarch64" ]]; then
  CROSS_COMPILE=""
else
  CROSS_COMPILE="aarch64-linux-gnu-"
fi

sudo make -C "$KERN_SRC" O="$KERN_OUT" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" \
  INSTALL_MOD_PATH="$ROOTFS_DIR" modules_install
sudo rm -f "$ROOTFS_DIR/lib/modules/$KREL/build" "$ROOTFS_DIR/lib/modules/$KREL/source"

sudo cp "$KERN_OUT/arch/arm64/boot/Image" "$ROOTFS_DIR/boot/vmlinuz-$KREL"
sudo mkdir -p "$ROOTFS_DIR/boot/dtb-$KREL/qcom"
sudo cp "$KERN_OUT/arch/arm64/boot/dts/qcom/sc8280xp-huawei-gaokun3.dtb" \
  "$ROOTFS_DIR/boot/dtb-$KREL/qcom/"

mkdir -p "$FW_CACHE_DIR"
if [ ! -d "$FW_CACHE_DIR/linux-firmware" ]; then
  git clone --depth 1 https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git \
    "$FW_CACHE_DIR/linux-firmware"
fi

if [ ! -f "$FW_CACHE_DIR/uup-drivers.zip" ]; then
  wget https://github.com/matebook-e-go/uup-drivers-sc8280xp/releases/download/200.0.10.0/200.0.10.0.zip \
    -O "$FW_CACHE_DIR/uup-drivers.zip"
fi

if [ ! -d "$FW_CACHE_DIR/uup-drivers" ]; then
  mkdir -p "$FW_CACHE_DIR/uup-drivers"
  unzip -o "$FW_CACHE_DIR/uup-drivers.zip" -d "$FW_CACHE_DIR/uup-drivers"
  pushd "$FW_CACHE_DIR/uup-drivers" >/dev/null
  for cab in qcdx8280.cab qcsubsys_ext_adsp8280.cab qcsubsys_ext_cdsp8280.cab qcsubsys_ext_scss8280.cab; do
    bsdtar -xf "$cab"
  done
  popd >/dev/null
fi

rm -rf "$FW_BUILD_DIR"
mkdir -p "$FW_BUILD_DIR"
ln -s "$FW_CACHE_DIR/linux-firmware" "$FW_BUILD_DIR/linux-firmware"
ln -s "$FW_CACHE_DIR/uup-drivers" "$FW_BUILD_DIR/uup-drivers"

GK3_DIR="$ROOTFS_DIR/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3"
sudo mkdir -p "$GK3_DIR"

for fw in adspr.jsn adspua.jsn battmgr.jsn cdspr.jsn qcadsp8280.mbn qccdsp8280.mbn qcdxkmsuc8280.mbn qcslpi8280.mbn qcvss8280.mbn; do
  sudo cp "$FW_BUILD_DIR/uup-drivers/$fw" "$GK3_DIR/"
done

sudo cp "$FW_BUILD_DIR/linux-firmware/qcom/sc8280xp/LENOVO/21BX/audioreach-tplg.bin" "$GK3_DIR/"
sudo ln -sf HUAWEI/gaokun3/audioreach-tplg.bin \
  "$ROOTFS_DIR/lib/firmware/qcom/sc8280xp/SC8280XP-HUAWEI-GAOKUN3-tplg.bin"

sudo mkdir -p "$ROOTFS_DIR/lib/firmware/ath11k/WCN6855/hw2.0" \
              "$ROOTFS_DIR/lib/firmware/qca" \
              "$ROOTFS_DIR/lib/firmware/qcom"
sudo cp -r "$FW_BUILD_DIR/linux-firmware/ath11k/WCN6855/hw2.0/"* \
  "$ROOTFS_DIR/lib/firmware/ath11k/WCN6855/hw2.0/"
sudo ln -sf hw2.0 "$ROOTFS_DIR/lib/firmware/ath11k/WCN6855/hw2.1"
sudo cp "$FW_BUILD_DIR/linux-firmware/qca/hp"* "$ROOTFS_DIR/lib/firmware/qca/" || true
sudo cp "$FW_BUILD_DIR/linux-firmware/qcom/a660_gmu.bin" \
        "$FW_BUILD_DIR/linux-firmware/qcom/a660_sqe.fw" \
        "$ROOTFS_DIR/lib/firmware/qcom/"

sudo mkdir -p "$ROOTFS_DIR/usr/local/bin" "$ROOTFS_DIR/etc/systemd/system"
sudo cp "$GAOKUN_DIR/matebook-e-go-linux/tools/touchpad/huawei-tp-activate.py" "$ROOTFS_DIR/usr/local/bin/"
sudo cp "$GAOKUN_DIR/matebook-e-go-linux/tools/touchpad/huawei-touchpad.service" "$ROOTFS_DIR/etc/systemd/system/"
sudo chmod +x "$ROOTFS_DIR/usr/local/bin/huawei-tp-activate.py"
sudo cp "$GAOKUN_DIR/matebook-e-go-linux/tools/bluetooth/patch-nvm-bdaddr.py" "$ROOTFS_DIR/usr/local/bin/"
sudo chmod +x "$ROOTFS_DIR/usr/local/bin/patch-nvm-bdaddr.py"
