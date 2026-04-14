#!/usr/bin/env bash
set -euo pipefail

install_common_image_assets() {
  local rootfs_dir="$1"
  local gaokun_dir="$2"
  local touchscreen_share_dir="/usr/local/share/gaokun-touchscreen-tuner"
  local directory_assets=(
    "tools/image-assets/etc/modules-load.d:/etc/modules-load.d"
    "tools/image-assets/etc/modprobe.d:/etc/modprobe.d"
    "tools/image-assets/etc/sddm.conf.d:/etc/sddm.conf.d"
  )
  local executable_assets=(
    "tools/bluetooth/patch-nvm-bdaddr.py:/usr/local/bin/patch-nvm-bdaddr.py"
    "tools/monitors/gdm-monitor-sync:/usr/local/bin/gdm-monitor-sync"
    "tools/touchscreen-tuner/gaokun-touchscreen-tuner:/usr/local/bin/gaokun-touchscreen-tuner"
    "tools/touchpad/huawei-tp-activate.py:/usr/local/bin/huawei-tp-activate.py"
  )
  local service_assets=(
    "tools/bluetooth/patch-nvm-bdaddr.service:/etc/systemd/system/patch-nvm-bdaddr.service"
    "tools/monitors/gdm-monitor-sync.service:/etc/systemd/system/gdm-monitor-sync.service"
    "tools/touchpad/huawei-touchpad.service:/etc/systemd/system/huawei-touchpad.service"
  )
  local data_assets=(
    "tools/touchscreen-tuner/tune.py:${touchscreen_share_dir}/tune.py"
    "tools/touchscreen-tuner/tune-icon.svg:${touchscreen_share_dir}/tune-icon.svg"
    "tools/touchscreen-tuner/gaokun-touchscreen-tuner.desktop:/usr/share/applications/gaokun-touchscreen-tuner.desktop"
  )
  local optional_data_assets=(
    "tools/image-assets/usr/local/share/gaokun/monitors.xml:/usr/local/share/gaokun/monitors.xml"
  )
  local asset src dest

  sudo mkdir -p \
    "$rootfs_dir/etc/modules-load.d" \
    "$rootfs_dir/etc/modprobe.d" \
    "$rootfs_dir/etc/sddm.conf.d" \
    "$rootfs_dir/etc/systemd/system" \
    "$rootfs_dir/usr/local/bin" \
    "$rootfs_dir${touchscreen_share_dir}" \
    "$rootfs_dir/usr/local/lib" \
    "$rootfs_dir/usr/share/applications" \
    "$rootfs_dir/usr/local/share/gaokun"

  for asset in "${directory_assets[@]}"; do
    src="${asset%%:*}"
    dest="${asset#*:}"
    if [[ -d "$gaokun_dir/$src" ]]; then
      sudo mkdir -p "$rootfs_dir$dest"
      sudo cp -a "$gaokun_dir/$src/." "$rootfs_dir$dest/"
    fi
  done

  for asset in "${executable_assets[@]}"; do
    src="${asset%%:*}"
    dest="${asset#*:}"
    sudo install -Dm755 "$gaokun_dir/$src" "$rootfs_dir$dest"
  done

  for asset in "${service_assets[@]}"; do
    src="${asset%%:*}"
    dest="${asset#*:}"
    sudo install -Dm644 "$gaokun_dir/$src" "$rootfs_dir$dest"
  done

  for asset in "${data_assets[@]}"; do
    src="${asset%%:*}"
    dest="${asset#*:}"
    sudo install -Dm644 "$gaokun_dir/$src" "$rootfs_dir$dest"
  done

  for asset in "${optional_data_assets[@]}"; do
    src="${asset%%:*}"
    dest="${asset#*:}"
    if [[ -f "$gaokun_dir/$src" ]]; then
      sudo install -Dm644 "$gaokun_dir/$src" "$rootfs_dir$dest"
    fi
  done

  sudo ln -sfn gaokun-touchscreen-tuner "$rootfs_dir/usr/local/bin/touchscreen-tune"
  sudo ln -sfn ../share/gaokun-touchscreen-tuner "$rootfs_dir/usr/local/lib/gaokun-touchscreen-tuner"
}

install_el2_efi_payloads() {
  local rootfs_dir="$1"
  local gaokun_dir="$2"

  sudo install -d \
    "$rootfs_dir/boot/efi/EFI/systemd/drivers" \
    "$rootfs_dir/boot/efi/firmware"

  sudo install -Dm644 "$gaokun_dir/tools/el2/slbounceaa64.efi" \
    "$rootfs_dir/boot/efi/EFI/systemd/drivers/slbounceaa64.efi"
  sudo install -Dm644 "$gaokun_dir/tools/el2/qebspilaa64.efi" \
    "$rootfs_dir/boot/efi/EFI/systemd/drivers/qebspilaa64.efi"
  sudo install -Dm644 "$gaokun_dir/tools/el2/tcblaunch.exe" \
    "$rootfs_dir/boot/efi/tcblaunch.exe"
  sudo install -Dm644 "$rootfs_dir/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcadsp8280.mbn" \
    "$rootfs_dir/boot/efi/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcadsp8280.mbn"
  sudo install -Dm644 "$rootfs_dir/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qccdsp8280.mbn" \
    "$rootfs_dir/boot/efi/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qccdsp8280.mbn"
  sudo install -Dm644 "$rootfs_dir/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcslpi8280.mbn" \
    "$rootfs_dir/boot/efi/firmware/qcom/sc8280xp/HUAWEI/gaokun3/qcslpi8280.mbn"
}
