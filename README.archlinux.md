# Arch Linux package

This repository contains a local Arch Linux `PKGBUILD` for building the custom `sdrpp-sq5fox` package from the current checkout.

## Build

```bash
makepkg -s
```

This creates a package file like:

```text
sdrpp-sq5fox-<version>-x86_64.pkg.tar.zst
```

## Install locally

```bash
sudo pacman -U sdrpp-sq5fox-*.pkg.tar.zst
```

## Install on another machine

Copy the generated `.pkg.tar.zst` file to the target machine and install it with:

```bash
sudo pacman -U sdrpp-sq5fox-*.pkg.tar.zst
```

## Notes

- The package is built from the current repository state.
- Local uncommitted changes are included in the build.
- The package provides and replaces `sdrpp` and `sdrpp-git`.