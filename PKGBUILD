pkgname=sdrpp-sq5fox
_basever=1.0.4
pkgver=1.0.4.r1804.242bc442
pkgrel=1
pkgdesc="SQ5FOX build of the bloat-free SDR receiver"
arch=(x86_64)
url="https://www.sdrpp.org/"
license=(GPL-3.0-only)
depends=(fftw glew glfw libvolk)
_plugindeps=(rtl-sdr portaudio rtaudio)
makedepends=(cmake git "${_plugindeps[@]}")
optdepends=("${_plugindeps[@]}")
provides=(sdrpp sdrpp-git)
conflicts=(sdrpp sdrpp-git)
replaces=(sdrpp sdrpp-git)
source=()

pkgver() {
    printf "%s.r%s.%s" "$_basever" "$(git -C "$startdir" rev-list --count HEAD)" "$(git -C "$startdir" rev-parse --short HEAD)"
}

build() {
    cmake -B "$srcdir/build" -S "$startdir" \
        -DOPT_BUILD_RTL_SDR_SOURCE=ON \
        -DOPT_BUILD_AIRSPY_SOURCE=OFF \
        -DOPT_BUILD_AIRSPYHF_SOURCE=OFF \
        -DOPT_BUILD_BLADERF_SOURCE=OFF \
        -DOPT_BUILD_HACKRF_SOURCE=OFF \
        -DOPT_BUILD_LIMESDR_SOURCE=OFF \
        -DOPT_BUILD_PLUTOSDR_SOURCE=OFF \
        -DOPT_BUILD_SDRPLAY_SOURCE=OFF \
        -DOPT_BUILD_SOAPY_SOURCE=OFF \
        -DOPT_BUILD_NEW_PORTAUDIO_SINK=ON \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    cmake --build "$srcdir/build"
}

package() {
    DESTDIR="$pkgdir" cmake --install "$srcdir/build"
}
