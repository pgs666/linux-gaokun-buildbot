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

PKG_TOPDIR="$WORKDIR/archpkg"
BUILDROOT_DIR="$WORKDIR/package-buildroots"
ARCH_PKG_ARCH="aarch64"
FIRMWARE_PKGVER="${FIRMWARE_PKGVER:-$(date -u +%Y%m%d)}"
BUILD_TIME_UTC="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
BUILD_DATE="$(date -u +%s)"

mkdir -p "$ARTIFACT_DIR" "$PKG_TOPDIR/SOURCES" "$PKG_TOPDIR/PKGBUILDS"

for tool in bsdtar gzip; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "missing required host tool: $tool" >&2
    exit 1
  fi
done

arch_pkgver() {
  printf '%s\n' "$1" | sed -e 's/[+-]/./g' -e 's/_/./g' -e 's/\.\.+/./g' -e 's/\.$//'
}

prepare_tarball() {
  local tar_name="$1"
  local source_dir="$2"
  tar -C "$source_dir" --transform 's,^\.,payload,' -czf "$PKG_TOPDIR/SOURCES/$tar_name" .
}

render_pkgbuild() {
  local template_path="$1"
  local output_path="$2"
  shift 2

  local sed_args=()
  while [[ $# -gt 0 ]]; do
    sed_args+=(-e "s|$1|$2|g")
    shift 2
  done

  sed "${sed_args[@]}" "$template_path" >"$output_path"
}

build_pkg_archive() {
  local pkg_name="$1"
  local pkgver="$2"
  local arch="$3"
  local pkgdesc="$4"
  local stage_dir="$5"
  shift 5
  local depends=("$@")

  local archive_name="${pkg_name}-${pkgver}-1-${arch}.pkg.tar.zst"
  local archive_path="$ARTIFACT_DIR/$archive_name"
  local archive_root="$WORKDIR/archive-root-$pkg_name"
  local pkginfo="$archive_root/.PKGINFO"
  local buildinfo="$archive_root/.BUILDINFO"
  local installed_size

  rm -rf "$archive_root"
  mkdir -p "$archive_root"
  rsync -a "$stage_dir"/ "$archive_root"/
  installed_size="$(du -sb "$stage_dir" | awk '{print $1}')"

  {
    printf 'pkgname = %s\n' "$pkg_name"
    printf 'pkgbase = %s\n' "$pkg_name"
    printf 'pkgver = %s-1\n' "$pkgver"
    printf 'pkgdesc = %s\n' "$pkgdesc"
    printf 'url = https://github.com/KawaiiHachimi/linux-gaokun-buildbot\n'
    printf 'builddate = %s\n' "$BUILD_DATE"
    printf 'packager = cool <bilibili@att.net>\n'
    printf 'size = %s\n' "$installed_size"
    printf 'arch = %s\n' "$arch"
    for dep in "${depends[@]}"; do
      [[ -n "$dep" ]] && printf 'depend = %s\n' "$dep"
    done
  } > "$pkginfo"

  {
    printf 'format = 2\n'
    printf 'pkgname = %s\n' "$pkg_name"
    printf 'pkgver = %s-1\n' "$pkgver"
    printf 'pkgarch = %s\n' "$arch"
    printf 'builddate = %s\n' "$BUILD_DATE"
    printf 'packager = cool <bilibili@att.net>\n'
  } > "$buildinfo"

  # Match Arch package expectations by always shipping a gzip-compressed .MTREE.
  # Build it from the payload tree only so package metadata files are excluded.
  (cd "$stage_dir" && LANG=C bsdtar --format=mtree -cf - usr | gzip -n > "$archive_root/.MTREE")

  archive_entries=(.PKGINFO .BUILDINFO .MTREE usr)

  tar -C "$archive_root" \
    --sort=name \
    --mtime="@$BUILD_DATE" \
    --owner=0 --group=0 --numeric-owner \
    -I 'zstd -19 -T0' \
    -cf "$archive_path" \
    "${archive_entries[@]}"

  rm -rf "$archive_root"
  printf '%s\n' "$archive_name"
}

render_reference_pkgbuild() {
  local template_rel="$1"
  local pkg_name="$2"
  local pkgver="$3"
  local pkgdesc="$4"
  local source_name="$5"
  shift 5
  local depends=("$@")
  local depends_rendered=""

  if [[ ${#depends[@]} -gt 0 ]]; then
    depends_rendered="$(printf "'%s' " "${depends[@]}")"
  fi

  render_pkgbuild \
    "$GAOKUN_DIR/$template_rel" \
    "$PKG_TOPDIR/PKGBUILDS/${pkg_name}.PKGBUILD" \
    "@PKG_NAME@" "$pkg_name" \
    "@PKGVER@" "$pkgver" \
    "@PKG_DESC@" "$pkgdesc" \
    "@DEPENDS@" "$depends_rendered" \
    "@SOURCE_NAME@" "$source_name"
}

build_kernel_variant_pkgset() {
  local variant_key="$1"
  local pkg_suffix="$2"
  local src_dir="$3"
  local out_dir="$4"
  local krel="$5"
  local dtb_name="$6"

  local pkgver
  pkgver="$(arch_pkgver "$krel")"
  local kernel_pkg="linux-gaokun3${pkg_suffix}"
  local headers_pkg="linux-gaokun3${pkg_suffix}-headers"
  local kernel_stage="$BUILDROOT_DIR/${kernel_pkg}"
  local kernel_raw_stage="$BUILDROOT_DIR/${kernel_pkg}-raw"
  local headers_stage="$BUILDROOT_DIR/${headers_pkg}"
  local headers_tree="$headers_stage/usr/lib/modules/$krel/build"
  local kernel_tar="${kernel_pkg}.tar.gz"
  local headers_tar="${headers_pkg}.tar.gz"

  rm -rf "$kernel_stage" "$kernel_raw_stage" "$headers_stage"
  mkdir -p "$kernel_stage/usr/lib/modules" "$headers_tree" "$headers_stage/usr/src"

  make -C "$src_dir" O="$out_dir" ARCH=arm64 INSTALL_MOD_PATH="$kernel_raw_stage" modules_install
  mv "$kernel_raw_stage/lib/modules/$krel" "$kernel_stage/usr/lib/modules/"
  rm -rf "$kernel_raw_stage"

  install -Dm644 "$out_dir/arch/arm64/boot/Image" \
    "$kernel_stage/usr/lib/modules/$krel/vmlinuz"
  install -Dm644 "$out_dir/System.map" \
    "$kernel_stage/usr/lib/modules/$krel/System.map"
  install -Dm644 "$out_dir/.config" \
    "$kernel_stage/usr/lib/modules/$krel/config"
  install -Dm644 "$out_dir/arch/arm64/boot/dts/qcom/$dtb_name" \
    "$kernel_stage/usr/lib/modules/$krel/dtb/qcom/$dtb_name"
  printf '%s\n' "$kernel_pkg" > "$kernel_stage/usr/lib/modules/$krel/pkgbase"
  rm -f "$kernel_stage/usr/lib/modules/$krel/build" \
        "$kernel_stage/usr/lib/modules/$krel/source"
  depmod -b "$kernel_stage/usr" -a "$krel"

  rsync -a --delete --exclude '.git' "$src_dir/" "$headers_tree/"
  rsync -a "$out_dir/" "$headers_tree/"
  find "$headers_tree" -type f \
    \( -name '*.o' -o -name '*.ko' -o -name '*.a' -o -name '*.cmd' -o -name '*.mod' -o -name '*.mod.c' \) \
    -delete
  find "$headers_tree" -type l \( -name build -o -name source \) -delete
  ln -s "../lib/modules/$krel/build" "$headers_stage/usr/src/${headers_pkg}"
  ln -s "../../../../../usr/src/${headers_pkg}" "$headers_stage/usr/lib/modules/$krel/source"

  prepare_tarball "$kernel_tar" "$kernel_stage"
  prepare_tarball "$headers_tar" "$headers_stage"

  render_reference_pkgbuild \
    "packaging/archlinux/linux-gaokun3/PKGBUILD.in" \
    "$kernel_pkg" "$pkgver" \
    "Linux kernel for Huawei MateBook E Go 2023 (${krel})" \
    "$kernel_tar" \
    "mkinitcpio" "systemd"

  render_reference_pkgbuild \
    "packaging/archlinux/linux-gaokun3-headers/PKGBUILD.in" \
    "$headers_pkg" "$pkgver" \
    "Headers for Linux kernel ${krel} on Huawei MateBook E Go 2023" \
    "$headers_tar" \
    "$kernel_pkg=${pkgver}-1"

  local kernel_archive
  local headers_archive
  kernel_archive="$(build_pkg_archive "$kernel_pkg" "$pkgver" "$ARCH_PKG_ARCH" \
    "Linux kernel for Huawei MateBook E Go 2023 (${krel})" \
    "$kernel_stage" "mkinitcpio" "systemd")"
  headers_archive="$(build_pkg_archive "$headers_pkg" "$pkgver" "$ARCH_PKG_ARCH" \
    "Headers for Linux kernel ${krel} on Huawei MateBook E Go 2023" \
    "$headers_stage" "$kernel_pkg=${pkgver}-1")"

  printf -v "KREL_${variant_key^^}" '%s' "$krel"
  printf -v "KERNEL_PKG_${variant_key^^}" '%s' "$kernel_archive"
  printf -v "HEADERS_PKG_${variant_key^^}" '%s' "$headers_archive"
}

build_firmware_pkg() {
  local firmware_stage="$BUILDROOT_DIR/linux-firmware-gaokun3"
  local firmware_tar="linux-firmware-gaokun3.tar.gz"

  rm -rf "$firmware_stage"
  mkdir -p "$firmware_stage/usr/lib/firmware"
  cp -a "$GAOKUN_DIR/firmware/." "$firmware_stage/usr/lib/firmware/"
  rm -f "$firmware_stage/usr/lib/firmware/"*.spec.in

  prepare_tarball "$firmware_tar" "$firmware_stage"
  render_reference_pkgbuild \
    "packaging/archlinux/linux-firmware-gaokun3/PKGBUILD.in" \
    "linux-firmware-gaokun3" "$FIRMWARE_PKGVER" \
    "Firmware bundle for Huawei MateBook E Go 2023 (gaokun3)" \
    "$firmware_tar"

  FIRMWARE_PKG="$(build_pkg_archive "linux-firmware-gaokun3" "$FIRMWARE_PKGVER" "any" \
    "Firmware bundle for Huawei MateBook E Go 2023 (gaokun3)" \
    "$firmware_stage")"
}

build_kernel_variant_pkgset "standard" "" "$KERN_SRC_BASE" "$KERN_OUT" "$BASE_KREL" \
  "sc8280xp-huawei-gaokun3.dtb"

if [[ "$BUILD_EL2" == "true" ]]; then
  : "${KERN_OUT_EL2:?missing KERN_OUT_EL2}"
  if [[ -z "$EL2_KREL" ]]; then
    echo "BUILD_EL2=true but kernel-release-el2.txt is missing" >&2
    exit 1
  fi

  build_kernel_variant_pkgset "el2" "-el2" "$KERN_SRC_EL2" "$KERN_OUT_EL2" "$EL2_KREL" \
    "sc8280xp-huawei-gaokun3-el2.dtb"
fi

build_firmware_pkg

EL2_MANIFEST_BLOCK=""
EL2_RELEASE_BLOCK=""
if [[ "$BUILD_EL2" == "true" ]]; then
  EL2_MANIFEST_BLOCK="$(cat <<EOF
,
    "el2": {
      "release": "${KREL_EL2}",
      "packages": {
        "kernel": "${KERNEL_PKG_EL2}",
        "headers": "${HEADERS_PKG_EL2}"
      }
    }
EOF
)"
  EL2_RELEASE_BLOCK="$(cat <<EOF
- \`${KERNEL_PKG_EL2}\`
- \`${HEADERS_PKG_EL2}\`
EOF
)"
fi

cat >"$ARTIFACT_DIR/package-manifest.json" <<EOF
{
  "package_release_tag": "${PACKAGE_RELEASE_TAG}",
  "kernel_tag": "${KERNEL_TAG}",
  "build_el2": ${BUILD_EL2},
  "built_at_utc": "${BUILD_TIME_UTC}",
  "firmware_version": "${FIRMWARE_PKGVER}",
  "kernels": {
    "standard": {
      "release": "${KREL_STANDARD}",
      "packages": {
        "kernel": "${KERNEL_PKG_STANDARD}",
        "headers": "${HEADERS_PKG_STANDARD}"
      }
    }${EL2_MANIFEST_BLOCK}
  },
  "packages": {
    "firmware": "${FIRMWARE_PKG}"
  }
}
EOF

(
  cd "$ARTIFACT_DIR"
  sha256sum ./*.pkg.tar.zst package-manifest.json > package-sha256sums.txt
)

cat >"$ARTIFACT_DIR/package-release-body.md" <<EOF
## Package Bundle

- Package Tag: \`${PACKAGE_RELEASE_TAG}\`
- Kernel Tag: \`${KERNEL_TAG}\`
- EL2 Package Set Included: \`${BUILD_EL2}\`
- Firmware Version: \`${FIRMWARE_PKGVER}\`
- Architecture: \`aarch64\`
- Build Time (UTC): \`${BUILD_TIME_UTC}\`

## Integrity

- SHA256 checksum list: \`package-sha256sums.txt\`

## Included Arch Packages

- \`${KERNEL_PKG_STANDARD}\`
- \`${HEADERS_PKG_STANDARD}\`
${EL2_RELEASE_BLOCK}
- \`${FIRMWARE_PKG}\`
EOF
