# packaging — Early Alpha build instructions

The stack ships as one package containing all modules: console binaries
(`exomind`, `exosched`, ...), MCP server symlinks (`exomind-server`, ...),
batch tools (`exodoc`, `exokit`).

Run `bash packaging/release.sh` to produce everything (it needs `git`,
`make`, and, when present, `makepkg` and `dpkg-deb`):

```
dist/exomind-<version>.tar.gz      # source tarball for GitHub
dist/SHA256SUMS                    # checksums
dist/exomind-0.4.0alpha1.tar.gz    # Arch source (pkgver-named)
packaging/exomind-0.4.0alpha1.tar.gz
packaging/exomind.SRCINFO          # pacman/AUR metadata (makepkg-generated)
dist/exomind_0.4.0~alpha.1-1_amd64.deb
```

## Arch / pacman

```sh
# option A: from the release artifacts
cd packaging
makepkg -i            # requires exomind-0.4.0alpha1.tar.gz next to PKGBUILD

# option B: everything from source
bash release.sh
makepkg -i            # now inside packaging/
# or: pacman -U exomind-0.4.0alpha1-1-x86_64.pkg.tar.zst
```

If you run `makepkg` directly, the source tarball must be named
`exomind-$pkgver.tar.gz` (the PKGBUILD globs the extracted directory, so
the internal prefix does not matter).

## Debian / Ubuntu (apt)

```sh
sudo apt install ./exomind_0.4.0~alpha.1-1_amd64.deb
# or build it: dpkg-deb --build <extracted layout> dist/
```

## From source (no package manager)

```sh
make
make install PREFIX=/usr/local      # or PREFIX=~/.local
```

Verify integrity before installing: `sha256sum -c dist/SHA256SUMS`.
