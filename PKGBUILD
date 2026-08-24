# Maintainer: MEGAS <halil@namli.xyz>

pkgname=mergen
pkgver=1.0.0
pkgrel=1
pkgdesc='A minimal PDF viewer'
arch=('x86_64')
url='https://github.com/sudo-megas/MERGEN'
license=('GPL-3.0-only')
depends=('qt6-base' 'poppler-qt6' 'polkit' 'ttf-cascadia-code-nerd' 'hicolor-icon-theme')
makedepends=('cmake' 'ninja' 'gcc' 'git')
# Integrity comes from the signed tag rather than a checksum, which is the
# usual arrangement for a VCS source and is why SKIP is correct here.
source=("$pkgname::git+$url.git#tag=v$pkgver")
sha256sums=('SKIP')

build() {
    cmake -S "$pkgname" -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -Wno-dev
    cmake --build build
}

package() {
    DESTDIR="$pkgdir" cmake --install build
}
