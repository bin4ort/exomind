#!/usr/bin/env bash
# release.sh — build the Early Alpha release artifacts:
#   1. source tarball (make dist) + sha256
#   2. Arch package (PKGBUILD, needs makepkg)
#   3. Debian/Ubuntu package (needs dpkg-deb)
# Upload the tarball + packages + sha256 to a GitHub release tagged
# v<VERSION> labeled "Early Alpha".
set -u
cd "$(dirname "$0")/.."

VERSION="${VERSION:-0.4.0-alpha.1}"
PKGVER_PACMAN="$(printf '%s' "$VERSION" | sed 's/-alpha\./alpha/')" # 0.4.0alpha1
PKGVER_DEB="$(printf '%s' "$VERSION" | tr '-' '~')"                    # 0.4.0~alpha1
DIST=dist/exomind-$VERSION
mkdir -p dist

echo "==> building source tarball"
git archive --format=tar --prefix="exomind-$VERSION/" HEAD -o "$DIST.tar" || exit 1
gzip -f "$DIST.tar"
SHA=$(sha256sum "$DIST.tar.gz" | awk '{print $1}')
SHA_PACMAN="$SHA"
echo "==> tarball: dist/exomind-$VERSION.tar.gz ($SHA)"
printf '%s  exomind-%s.tar.gz\n' "$SHA" "$VERSION" > dist/SHA256SUMS
# keep the tarball also inside dist/ for the checksum file relative names
[ -f "dist/exomind-$VERSION.tar.gz" ] && true

echo "==> Arch package"
if command -v makepkg >/dev/null 2>&1; then
    WORK=$(mktemp -d /tmp/opencode/pkgbuild-XXXXXX)
    cp packaging/PKGBUILD "$WORK/"
    # pacman has no dashes in pkgver: the Arch tarball must ALSO use the
    # pkgver as its internal directory prefix, or makepkg cannot cd into it
    sed -i "s/^pkgver=.*/pkgver=$PKGVER_PACMAN/" "$WORK/PKGBUILD"
    mkdir -p "$WORK/src"
    tar -xzf "$DIST.tar.gz" -C "$WORK/src"
    mv "$WORK/src/exomind-$VERSION" "$WORK/src/exomind-$PKGVER_PACMAN"
    ( cd "$WORK/src" && tar -czf "$WORK/exomind-$PKGVER_PACMAN.tar.gz" \
        "exomind-$PKGVER_PACMAN" )
    SHA_PACMAN=$(sha256sum "$WORK/exomind-$PKGVER_PACMAN.tar.gz" | \
        awk '{print $1}')
    sed -i "s/^sha256sums=.*/sha256sums=('$SHA_PACMAN')/" "$WORK/PKGBUILD"
    cp "$WORK/exomind-$PKGVER_PACMAN.tar.gz" dist/
    # also next to the PKGBUILD, so `cd packaging && makepkg` works directly
    cp "$WORK/exomind-$PKGVER_PACMAN.tar.gz" packaging/
    ( cd "$WORK" && makepkg -f 2>&1 | tail -3 )
    cp "$WORK"/*.pkg.tar.zst dist/ 2>/dev/null
    # .SRCINFO: machine-readable metadata so pacman/AUR tools can load it
    ( cd "$WORK" && makepkg --printsrcinfo > .SRCINFO 2>/dev/null )
    if [ -s "$WORK/.SRCINFO" ]; then
        cp "$WORK/.SRCINFO" packaging/exomind.SRCINFO
        echo "==> .SRCINFO written to packaging/exomind.SRCINFO"
    else
        echo "==> WARNING: .SRCINFO could not be generated"
    fi
    echo "==> arch pkg: $(ls dist/*.pkg.tar.zst 2>/dev/null)"
    rm -rf "$WORK"
else
    echo "==> makepkg not found, skipping Arch package (install pacman)"
fi

echo "==> Debian package"
if command -v dpkg-deb >/dev/null 2>&1; then
    WORK=$(mktemp -d /tmp/opencode/deb-XXXXXX)
    mkdir -p "$WORK/exomind_$PKGVER_DEB-1/usr/bin"
    make install DESTDIR="$WORK/exomind_$PKGVER_DEB-1" PREFIX=/usr > /dev/null
    mkdir -p "$WORK/exomind_$PKGVER_DEB-1/DEBIAN"
    cat > "$WORK/exomind_$PKGVER_DEB-1/DEBIAN/control" <<EOF
Package: exomind
Version: $PKGVER_DEB-1
Section: utils
Priority: optional
Architecture: amd64
Maintainer: exomind stack <maintainers@exomind.dev>
Homepage: https://github.com/bin4ort/exomind
Depends: curl
Description: AINSS - AI-Native Software Stack (Early Alpha)
 Memory, scheduling, orchestration, docs auditing, QMS, research,
 context continuity and the behavioral development kit for AI agents.
 Plain-text zero-dependency daemons; console and MCP server modes.
EOF
    dpkg-deb --build "$WORK/exomind_$PKGVER_DEB-1" dist/ 2>&1 | tail -1
    echo "==> deb: $(ls dist/*.deb 2>/dev/null)"
    rm -rf "$WORK"
else
    echo "==> dpkg-deb not found, skipping Debian package"
fi

echo ""
echo "==> upload these to a GitHub release tagged v$VERSION (label: Early Alpha):"
ls -la dist/
cat dist/SHA256SUMS
