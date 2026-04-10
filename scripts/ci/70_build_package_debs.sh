#!/usr/bin/env bash
set -euo pipefail

: "${GAOKUN_DIR:?missing GAOKUN_DIR}"
: "${WORKDIR:?missing WORKDIR}"
: "${ARTIFACT_DIR:?missing ARTIFACT_DIR}"
: "${KERNEL_TAG:?missing KERNEL_TAG}"
: "${PACKAGE_RELEASE_TAG:?missing PACKAGE_RELEASE_TAG}"

BUILD_EL2="${BUILD_EL2:-false}"
KERN_SRC_BASE="${KERN_SRC_BASE:-${KERN_SRC:-}}"
KERN_OUT="${KERN_OUT:-}"
KERN_SRC_EL2="${KERN_SRC_EL2:-${KERN_SRC:-}}"
KERN_OUT_EL2="${KERN_OUT_EL2:-}"

: "${KERN_SRC_BASE:?missing KERN_SRC_BASE}"
: "${KERN_OUT:?missing KERN_OUT}"

BASE_KREL="$(cat "$WORKDIR/kernel-release.txt")"
EL2_KREL=""
if [[ -f "$WORKDIR/kernel-release-el2.txt" ]]; then
  EL2_KREL="$(cat "$WORKDIR/kernel-release-el2.txt")"
fi

DEB_TOPDIR="$WORKDIR/debbuild"
BUILDROOT_DIR="$WORKDIR/package-buildroots"
DEB_ARCH="arm64"
FIRMWARE_DEB_VERSION="${FIRMWARE_DEB_VERSION:-$(date -u +%Y%m%d)-1}"
BUILD_TIME_UTC="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

mkdir -p "$ARTIFACT_DIR" "$DEB_TOPDIR"

build_deb() {
  local pkg_name="$1"
  local stage_dir="$2"
  local version="$3"
  local description="$4"
  local depends="${5:-}"
  local arch="${6:-$DEB_ARCH}"
  local postinst="${7:-}"

  local deb_dir="$stage_dir/DEBIAN"
  mkdir -p "$deb_dir"

  cat > "$deb_dir/control" <<EOF
Package: ${pkg_name}
Version: ${version}
Architecture: ${arch}
Maintainer: cool <bilibili@att.net>
Description: ${description}
EOF

  if [[ -n "$depends" ]]; then
    echo "Depends: ${depends}" >> "$deb_dir/control"
  fi

  if [[ -n "$postinst" ]]; then
    cat > "$deb_dir/postinst" <<POSTEOF
#!/bin/bash
set -e
${postinst}
POSTEOF
    chmod 755 "$deb_dir/postinst"
  fi

  dpkg-deb --build --root-owner-group "$stage_dir" "$DEB_TOPDIR/${pkg_name}_${version}_${arch}.deb"
}

build_kernel_variant() {
  local variant_key="$1"
  local pkg_suffix="$2"
  local src_dir="$3"
  local out_dir="$4"
  local krel="$5"
  local dtb_name="$6"

  local deb_version="${krel//-/\~}-1"
  local image_pkg="linux-image-gaokun3${pkg_suffix}"
  local modules_pkg="linux-modules-gaokun3${pkg_suffix}"
  local headers_pkg="linux-headers-gaokun3${pkg_suffix}"
  local image_stage="$BUILDROOT_DIR/${image_pkg}"
  local modules_stage="$BUILDROOT_DIR/${modules_pkg}"
  local modules_raw_stage="$BUILDROOT_DIR/${modules_pkg}-raw"
  local headers_stage="$BUILDROOT_DIR/${headers_pkg}"
  local headers_tree="$headers_stage/usr/src/linux-headers-$krel"
  local postinst_script
  local extra_cmdline=""
  local payload_root=""
  local image_depends="linux-firmware-gaokun3, initramfs-tools, systemd, util-linux"

  rm -rf "$image_stage" "$modules_stage" "$modules_raw_stage" "$headers_stage"
  mkdir -p "$image_stage/boot" "$image_stage/usr/lib/linux-image-$krel/qcom"
  mkdir -p "$modules_stage/lib" "$headers_tree" "$headers_stage/lib/modules/$krel"

  install -Dm644 "$out_dir/arch/arm64/boot/Image" \
    "$image_stage/boot/vmlinuz-$krel"
  install -Dm644 "$out_dir/System.map" \
    "$image_stage/boot/System.map-$krel"
  install -Dm644 "$out_dir/.config" \
    "$image_stage/boot/config-$krel"
  install -Dm644 "$out_dir/arch/arm64/boot/dts/qcom/$dtb_name" \
    "$image_stage/boot/dtb-$krel"
  install -Dm644 "$out_dir/arch/arm64/boot/dts/qcom/$dtb_name" \
    "$image_stage/usr/lib/linux-image-$krel/qcom/$dtb_name"
  if [[ "$variant_key" == "el2" ]]; then
    extra_cmdline="modprobe.blacklist=simpledrm"
    payload_root="/usr/lib/gaokun3/el2/$krel"
    install -Dm644 "$GAOKUN_DIR/tools/el2/slbounceaa64.efi" \
      "$image_stage${payload_root}/EFI/systemd/drivers/slbounceaa64.efi"
    install -Dm644 "$GAOKUN_DIR/tools/el2/qebspilaa64.efi" \
      "$image_stage${payload_root}/EFI/systemd/drivers/qebspilaa64.efi"
    install -Dm644 "$GAOKUN_DIR/tools/el2/tcblaunch.exe" \
      "$image_stage${payload_root}/tcblaunch.exe"
    install -Dm644 "$GAOKUN_DIR/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcadsp8280.mbn" \
      "$image_stage${payload_root}/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcadsp8280.mbn"
    install -Dm644 "$GAOKUN_DIR/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qccdsp8280.mbn" \
      "$image_stage${payload_root}/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qccdsp8280.mbn"
    install -Dm644 "$GAOKUN_DIR/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcslpi8280.mbn" \
      "$image_stage${payload_root}/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcslpi8280.mbn"
  fi

  make -C "$src_dir" O="$out_dir" ARCH=arm64 INSTALL_MOD_PATH="$modules_raw_stage" modules_install
  mv "$modules_raw_stage/lib/modules" "$modules_stage/lib/"
  rm -rf "$modules_raw_stage"
  rm -f "$modules_stage/lib/modules/$krel/build" \
        "$modules_stage/lib/modules/$krel/source"
  depmod -b "$modules_stage" -a "$krel"

  rsync -a --delete --exclude '.git' "$src_dir/" "$headers_tree/"
  rsync -a "$out_dir/" "$headers_tree/"
  find "$headers_tree" -type f \
    \( -name '*.o' -o -name '*.ko' -o -name '*.a' -o -name '*.cmd' -o -name '*.mod' -o -name '*.mod.c' \) \
    -delete
  find "$headers_tree" -type l \( -name build -o -name source \) -delete
  ln -s "../../../src/linux-headers-$krel" "$headers_stage/lib/modules/$krel/build"
  ln -s "../../../src/linux-headers-$krel" "$headers_stage/lib/modules/$krel/source"

  read -r -d '' postinst_script <<EOF || true
temp_kernel_dir=\$(mktemp -d)
cleanup() {
  for name in install.conf cmdline devicetree; do
    if [ -f "\$temp_kernel_dir/\$name.orig" ]; then
      cp "\$temp_kernel_dir/\$name.orig" "/etc/kernel/\$name"
    else
      rm -f "/etc/kernel/\$name"
    fi
  done
  rm -rf "\$temp_kernel_dir"
}
trap cleanup EXIT
mkdir -p /etc/kernel
for name in install.conf cmdline devicetree; do
  if [ -f "/etc/kernel/\$name" ]; then
    cp "/etc/kernel/\$name" "\$temp_kernel_dir/\$name.orig"
  fi
done
if [ -s /etc/kernel/cmdline ]; then
  cmdline="\$(tr -s '[:space:]' ' ' </etc/kernel/cmdline)"
else
  cmdline="\$(tr ' ' '\n' </proc/cmdline | grep -ve '^BOOT_IMAGE=' -e '^initrd=' | tr '\n' ' ')"
fi
cmdline="\${cmdline%" "}"
extra_cmdline="${extra_cmdline}"
case " \$cmdline " in
  *" \$extra_cmdline "*)
    ;;
  *)
    if [ -n "\$extra_cmdline" ]; then
      cmdline="\${cmdline} \$extra_cmdline"
      cmdline="\${cmdline#" "}"
    fi
    ;;
esac
printf 'layout=bls\\n' > /etc/kernel/install.conf
printf '%s\\n' "\$cmdline" > /etc/kernel/cmdline
printf 'qcom/%s\\n' "$dtb_name" > /etc/kernel/devicetree
if command -v update-initramfs >/dev/null 2>&1; then
  update-initramfs -c -k $krel 2>/dev/null || true
fi
if command -v kernel-install >/dev/null 2>&1; then
  kernel-install --entry-token=machine-id remove $krel >/dev/null 2>&1 || true
  kernel-install --make-entry-directory=yes --entry-token=machine-id add \
    $krel /boot/vmlinuz-$krel /boot/initrd.img-$krel >/dev/null 2>&1 || true
fi
payload_root="${payload_root}"
if [ -n "\$payload_root" ] && [ -d "\$payload_root" ]; then
  esp_path=""
  if command -v bootctl >/dev/null 2>&1; then
    esp_path="\$(bootctl --print-esp-path 2>/dev/null || true)"
  fi
  if [ -z "\$esp_path" ] && findmnt -rn --target /boot/efi >/dev/null 2>&1; then
    esp_path="/boot/efi"
  fi
  if [ -n "\$esp_path" ] && findmnt -rn --target "\$esp_path" >/dev/null 2>&1; then
    install -d "\$esp_path/EFI/systemd/drivers" \
               "\$esp_path/firmware/qcom/sc8280xp/HUAWEI/gaokun3"
    install -m 0644 "\$payload_root/EFI/systemd/drivers/slbounceaa64.efi" \
      "\$esp_path/EFI/systemd/drivers/slbounceaa64.efi"
    install -m 0644 "\$payload_root/EFI/systemd/drivers/qebspilaa64.efi" \
      "\$esp_path/EFI/systemd/drivers/qebspilaa64.efi"
    install -m 0644 "\$payload_root/tcblaunch.exe" \
      "\$esp_path/tcblaunch.exe"
    install -m 0644 "\$payload_root/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcadsp8280.mbn" \
      "\$esp_path/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcadsp8280.mbn"
    install -m 0644 "\$payload_root/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qccdsp8280.mbn" \
      "\$esp_path/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qccdsp8280.mbn"
    install -m 0644 "\$payload_root/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcslpi8280.mbn" \
      "\$esp_path/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcslpi8280.mbn"
  else
    echo "warning: EL2 payload install skipped because the ESP is not mounted" >&2
  fi
fi
EOF

  build_deb "$image_pkg" "$image_stage" "$deb_version" \
    "Linux kernel image for gaokun3 (${krel})" \
    "$image_depends" "$DEB_ARCH" \
    "$postinst_script"

  build_deb "$modules_pkg" "$modules_stage" "$deb_version" \
    "Linux kernel modules for gaokun3 (${krel})" \
    "${image_pkg} (= $deb_version)" "$DEB_ARCH" \
    "depmod -a $krel 2>/dev/null || true"

  build_deb "$headers_pkg" "$headers_stage" "$deb_version" \
    "Linux kernel headers for gaokun3 (${krel})" \
    "${modules_pkg} (= $deb_version)" "$DEB_ARCH"

  local image_deb="${image_pkg}_${deb_version}_${DEB_ARCH}.deb"
  local modules_deb="${modules_pkg}_${deb_version}_${DEB_ARCH}.deb"
  local headers_deb="${headers_pkg}_${deb_version}_${DEB_ARCH}.deb"

  cp "$DEB_TOPDIR/$image_deb" "$ARTIFACT_DIR/"
  cp "$DEB_TOPDIR/$modules_deb" "$ARTIFACT_DIR/"
  cp "$DEB_TOPDIR/$headers_deb" "$ARTIFACT_DIR/"

  printf -v "KREL_${variant_key^^}" '%s' "$krel"
  printf -v "IMAGE_DEB_${variant_key^^}" '%s' "$image_deb"
  printf -v "MODULES_DEB_${variant_key^^}" '%s' "$modules_deb"
  printf -v "HEADERS_DEB_${variant_key^^}" '%s' "$headers_deb"
}

build_firmware_package() {
  local firmware_stage="$BUILDROOT_DIR/linux-firmware-gaokun3"
  local firmware_deb="linux-firmware-gaokun3_${FIRMWARE_DEB_VERSION}_all.deb"

  rm -rf "$firmware_stage"
  mkdir -p "$firmware_stage/lib/firmware" "$firmware_stage/etc/initramfs-tools/hooks"
  cp -a "$GAOKUN_DIR/firmware/." "$firmware_stage/lib/firmware/"
  rm -f "$firmware_stage/lib/firmware/"*.spec.in
  cat > "$firmware_stage/etc/initramfs-tools/hooks/gaokun3-firmware" <<'EOF'
#!/bin/sh
set -e

. /usr/share/initramfs-tools/hook-functions

copy_fw() {
    copy_file firmware "$1" || [ "$?" -eq 1 ]
}

copy_fw /lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcadsp8280.mbn
copy_fw /lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qccdsp8280.mbn
copy_fw /lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcslpi8280.mbn
copy_fw /lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/audioreach-tplg.bin
EOF
  chmod 0755 "$firmware_stage/etc/initramfs-tools/hooks/gaokun3-firmware"

  build_deb "linux-firmware-gaokun3" "$firmware_stage" "$FIRMWARE_DEB_VERSION" \
    "Firmware bundle for Huawei MateBook E Go 2023 (gaokun3)" "" "all"

  cp "$DEB_TOPDIR/$firmware_deb" "$ARTIFACT_DIR/"
  FIRMWARE_DEB="$firmware_deb"
}

build_kernel_variant "standard" "" "$KERN_SRC_BASE" "$KERN_OUT" "$BASE_KREL" \
  "sc8280xp-huawei-gaokun3.dtb"

if [[ "$BUILD_EL2" == "true" ]]; then
  : "${KERN_OUT_EL2:?missing KERN_OUT_EL2}"
  if [[ -z "$EL2_KREL" ]]; then
    echo "BUILD_EL2=true but kernel-release-el2.txt is missing" >&2
    exit 1
  fi

  build_kernel_variant "el2" "-el2" "$KERN_SRC_EL2" "$KERN_OUT_EL2" "$EL2_KREL" \
    "sc8280xp-huawei-gaokun3-el2.dtb"
fi

build_firmware_package

EL2_MANIFEST_BLOCK=""
EL2_RELEASE_BLOCK=""
if [[ "$BUILD_EL2" == "true" ]]; then
  EL2_MANIFEST_BLOCK="$(cat <<EOF
,
    "el2": {
      "release": "${EL2_KREL}",
      "packages": {
        "image": "${IMAGE_DEB_EL2}",
        "modules": "${MODULES_DEB_EL2}",
        "headers": "${HEADERS_DEB_EL2}"
      }
    }
EOF
)"
  EL2_RELEASE_BLOCK="$(cat <<EOF
- \`${IMAGE_DEB_EL2}\`
- \`${MODULES_DEB_EL2}\`
- \`${HEADERS_DEB_EL2}\`
EOF
)"
fi

cat >"$ARTIFACT_DIR/package-manifest.json" <<EOF
{
  "package_release_tag": "${PACKAGE_RELEASE_TAG}",
  "kernel_tag": "${KERNEL_TAG}",
  "build_el2": ${BUILD_EL2},
  "built_at_utc": "${BUILD_TIME_UTC}",
  "firmware_version": "${FIRMWARE_DEB_VERSION}",
  "kernels": {
    "standard": {
      "release": "${KREL_STANDARD}",
      "packages": {
        "image": "${IMAGE_DEB_STANDARD}",
        "modules": "${MODULES_DEB_STANDARD}",
        "headers": "${HEADERS_DEB_STANDARD}"
      }
    }${EL2_MANIFEST_BLOCK}
  },
  "packages": {
    "firmware": "${FIRMWARE_DEB}"
  }
}
EOF

cat >"$ARTIFACT_DIR/package-release-body.md" <<EOF
## Package Bundle

- Package Tag: \`${PACKAGE_RELEASE_TAG}\`
- Kernel Tag: \`${KERNEL_TAG}\`
- EL2 Package Set Included: \`${BUILD_EL2}\`
- Firmware Version: \`${FIRMWARE_DEB_VERSION}\`
- Architecture: \`${DEB_ARCH}\`
- Build Time (UTC): \`${BUILD_TIME_UTC}\`

## Included DEBs

- \`${IMAGE_DEB_STANDARD}\`
- \`${MODULES_DEB_STANDARD}\`
- \`${HEADERS_DEB_STANDARD}\`
${EL2_RELEASE_BLOCK}
- \`${FIRMWARE_DEB}\`
EOF
