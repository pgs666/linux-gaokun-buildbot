# linux-gaokun-build

Build scripts, patches, tools, and firmware for Linux images targeting the Huawei MateBook E Go 2023 (`gaokun3` / `SC8280XP`).

## What is included

- `gaokun-patches/`: kernel patches and device support changes
- `firmware-huawei-gaokun3_minimal/`: minimal firmware bundle used by the image build
- `tools/`: device-specific helper scripts and service files
- `scripts/ci/`: workflow build, image creation, and packaging scripts

## Getting started

- Build guide (Chinese): [matebook_ego_build_guide_fedora44.md](matebook_ego_build_guide_fedora44.md)
- GitHub Actions workflow: [.github/workflows/fedora-gaokun3-release.yml](.github/workflows/fedora-gaokun3-release.yml)

## References

- [right-0903/linux-gaokun](https://github.com/right-0903/linux-gaokun)
- [whitelewi1-ctrl/matebook-e-go-linux](https://github.com/whitelewi1-ctrl/matebook-e-go-linux)
- [gaokun on AUR](https://aur.archlinux.org/packages?O=0&K=gaokun)
