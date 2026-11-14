#!/bin/bash
# pkg-config without sudo: unpack Ubuntu's pkgconf packages into ~/catherine-deps/pkgconf, add a pkg-config wrapper.
set -e
D=~/catherine-deps/pkgconf; mkdir -p $D/debs && cd $D/debs
apt-get download pkgconf pkgconf-bin libpkgconf3 > /dev/null
for f in *.deb; do dpkg -x "$f" $D; done
mkdir -p $D/bin
cat > $D/bin/pkg-config <<'W'
#!/bin/sh
D=$(dirname "$0")/..
LD_LIBRARY_PATH=$D/usr/lib/x86_64-linux-gnu exec $D/usr/bin/pkgconf "$@"
W
chmod +x $D/bin/pkg-config
PKG_CONFIG_PATH=~/catherine-deps/sdl3/lib/pkgconfig $D/bin/pkg-config --modversion sdl3
