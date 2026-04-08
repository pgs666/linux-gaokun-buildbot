#!/usr/bin/env bash
set -euo pipefail

: "${WORKDIR:?missing WORKDIR}"
: "${ARTIFACT_DIR:?missing ARTIFACT_DIR}"
: "${IMAGE_FILE:?missing IMAGE_FILE}"
: "${FEDORA_RELEASE:?missing FEDORA_RELEASE}"
: "${KERNEL_TAG:?missing KERNEL_TAG}"
: "${DESKTOP_ENVIRONMENT:?missing DESKTOP_ENVIRONMENT}"
: "${EXCLUDED_PACKAGES:?missing EXCLUDED_PACKAGES}"
: "${EXTRA_PACKAGES:?missing EXTRA_PACKAGES}"

KREL="$(cat "$WORKDIR/kernel-release.txt")"
BUILD_EL2="${BUILD_EL2:-false}"
EL2_KREL=""
GITHUB_RELEASE_ASSET_MAX_BYTES=$((2 * 1024 * 1024 * 1024 - 1))
if [[ "$BUILD_EL2" == "true" && -f "$WORKDIR/kernel-release-el2.txt" ]]; then
  EL2_KREL="$(cat "$WORKDIR/kernel-release-el2.txt")"
fi
IMAGE_BASENAME="$(basename "$IMAGE_FILE")"
ZST_FILE="$ARTIFACT_DIR/${IMAGE_BASENAME}.zst"
XZ_FILE="$ARTIFACT_DIR/${IMAGE_BASENAME}.xz"
RELEASE_BODY_FILE="$ARTIFACT_DIR/release-body.md"
EL2_RELEASE_BLOCK=""
EL2_PAYLOAD_BLOCK=""
if [[ "$BUILD_EL2" == "true" && -n "$EL2_KREL" ]]; then
  EL2_RELEASE_BLOCK="$(cat <<EOF
- Optional EL2 Kernel Release: \`${EL2_KREL}\`
EOF
)"
  EL2_PAYLOAD_BLOCK="## EL2 Payload

- Includes systemd-boot EL2 menu entry
- Includes \`slbounceaa64.efi\`, \`qebspilaa64.efi\`, \`tcblaunch.exe\`, and the three DSP firmware blobs in ESP \`/firmware/qcom/sc8280xp/HUAWEI/gaokun3/\`

"
fi

cp "$IMAGE_FILE" "$ARTIFACT_DIR/"
zstd -T0 -19 "$ARTIFACT_DIR/$IMAGE_BASENAME" -o "$ZST_FILE"

PACKAGE_FILE="$ZST_FILE"
if [ "$(stat -c '%s' "$ZST_FILE")" -gt "$GITHUB_RELEASE_ASSET_MAX_BYTES" ]; then
  rm -f "$ZST_FILE"
  xz -T0 -9e -c "$ARTIFACT_DIR/$IMAGE_BASENAME" > "$XZ_FILE"
  PACKAGE_FILE="$XZ_FILE"
fi

if [ "$(stat -c '%s' "$PACKAGE_FILE")" -gt "$GITHUB_RELEASE_ASSET_MAX_BYTES" ]; then
  echo "compressed image exceeds GitHub release asset limit: $(basename "$PACKAGE_FILE")" >&2
  exit 1
fi

rm -f "$ARTIFACT_DIR/$IMAGE_BASENAME"

COMPRESSED_BASENAME="$(basename "$PACKAGE_FILE")"
PACKAGE_GLOB="$COMPRESSED_BASENAME"
cat > "$RELEASE_BODY_FILE" <<EOF
## Build Information

- Distribution: \`Fedora Linux ${FEDORA_RELEASE}\`
- Kernel Tag: \`${KERNEL_TAG}\`
- Kernel Release: \`${KREL}\`
- Architecture: \`arm64\`
${EL2_RELEASE_BLOCK}
- Root Filesystem: \`Btrfs (@, @home, @var)\`
- Bootloader: \`systemd-boot\`
- Image File: \`${IMAGE_BASENAME}\`
- Compressed File: \`${COMPRESSED_BASENAME}\`
- Build Time (UTC): \`$(date -u +"%Y-%m-%dT%H:%M:%SZ")\`

## Rootfs Selection

- Desktop Environment: \`${DESKTOP_ENVIRONMENT}\`
- Excluded Packages: \`${EXCLUDED_PACKAGES}\`
- Extra Packages: \`${EXTRA_PACKAGES}\`

## Default Login

- Username: \`user\`
- Password: \`user\`
${EL2_PAYLOAD_BLOCK}
EOF

sudo chown "$(id -u):$(id -g)" "$RELEASE_BODY_FILE"

TAG_NAME="fedora${FEDORA_RELEASE}-${KREL}$(if [[ "$BUILD_EL2" == "true" ]]; then printf -- '-el2'; fi)-$(date -u +%Y%m%d%H%M%S)"

echo "$TAG_NAME" > "$WORKDIR/tag-name.txt"
echo "$KREL" > "$WORKDIR/kernel-release-export.txt"
echo "$PACKAGE_GLOB" > "$WORKDIR/package-glob.txt"
echo "$(basename "$RELEASE_BODY_FILE")" > "$WORKDIR/release-body-file.txt"
