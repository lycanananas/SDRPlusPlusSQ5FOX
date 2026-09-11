pkgname=sdrpp-sq5fox
pkgver=1.0.4.r845.4658a1ad
pkgrel=1
pkgdesc="SQ5FOX build of the bloat-free SDR receiver"
arch=(x86_64)
url="https://www.sdrpp.org/"
license=(GPL-3.0-only)
depends=(fftw glew glfw libvolk)
_plugindeps=(airspy airspyhf-git bladerf codec2 hackrf libad9361 libiio libsdrplay limesuite rtaudio rtl-sdr portaudio soapysdr)
makedepends=(cmake git "${_plugindeps[@]}")
optdepends=("${_plugindeps[@]}")
provides=(sdrpp sdrpp-git)
conflicts=(sdrpp sdrpp-git)
replaces=(sdrpp sdrpp-git)
source=()

pkgver() {
    git -C "$startdir" describe --long --tags --exclude nightly | sed 's/\([^-/]*-\)g/r\1/;s/-/./g'
}

build() {
    cmake -B "$srcdir/build" -S "$startdir" \
        -DOPT_BUILD_BLADERF_SOURCE=ON \
        -DOPT_BUILD_LIMESDR_SOURCE=ON \
        -DOPT_BUILD_SDRPLAY_SOURCE=ON \
        -DOPT_BUILD_NEW_PORTAUDIO_SINK=ON \
        -DOPT_BUILD_M17_DECODER=ON \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    cmake --build "$srcdir/build"
}

package() {
    DESTDIR="$pkgdir" cmake --install "$srcdir/build"
}