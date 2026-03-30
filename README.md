# linux-gaokun-buildbot

Build scripts, patches, kernel config, DTS files, tools, and firmware for Linux images targeting the Huawei MateBook E Go 2023 (`gaokun3` / `SC8280XP`).

## What is included

- `patches/`: kernel patches and device support changes
- `defconfig/`: local kernel configuration used by CI/manual builds
- `drivers/`: local mirrors of the patched driver sources kept in the patch series
- `dts/`: local mirrors of the patched device tree sources kept in the patch series
- `firmware/`: minimal firmware bundle used by the image build
- `packaging/`: RPM spec templates for kernel and firmware packages
- `tools/`: device-specific helper scripts and service files
- `scripts/ci/`: workflow build, image creation, and packaging scripts

The image pipeline now builds and installs a dedicated package set:

- **Fedora (RPM)**: `kernel-gaokun3`, `kernel-modules-gaokun3`, `kernel-devel-gaokun3`, `linux-firmware-gaokun3`
- **Ubuntu (DEB)**: `linux-image-gaokun3`, `linux-modules-gaokun3`, `linux-headers-gaokun3`, `linux-firmware-gaokun3`

## Getting started

- Dual-boot guide (Chinese): [dual_boot_guide.md](dual_boot_guide.md)
- Build guide – Fedora 44 (Chinese): [matebook_ego_build_guide_fedora44.md](matebook_ego_build_guide_fedora44.md)
- Build guide – Ubuntu 26.04 (Chinese): [matebook_ego_build_guide_ubuntu26.04.md](matebook_ego_build_guide_ubuntu26.04.md)
- GitHub Actions – Fedora: [.github/workflows/fedora-gaokun3-release.yml](.github/workflows/fedora-gaokun3-release.yml)
- GitHub Actions – Ubuntu: [.github/workflows/ubuntu-gaokun3-release.yml](.github/workflows/ubuntu-gaokun3-release.yml)

## References

- [right-0903/linux-gaokun](https://github.com/right-0903/linux-gaokun)
- [whitelewi1-ctrl/matebook-e-go-linux](https://github.com/whitelewi1-ctrl/matebook-e-go-linux)
- [gaokun on AUR](https://aur.archlinux.org/packages?O=0&K=gaokun)
- [TheUnknownThing/linux-gaokun](https://github.com/TheUnknownThing/linux-gaokun)
- [chenxuecong2/firmware-huawei-gaokun3](https://github.com/chenxuecong2/firmware-huawei-gaokun3)
