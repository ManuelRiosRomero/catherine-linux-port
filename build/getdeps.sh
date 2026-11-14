#!/bin/bash
# FreeType headers + cabextract + unzip into ~/catherine-deps without sudo.
set -e
mkdir -p ~/catherine-deps/debs && cd ~/catherine-deps/debs
apt-get download libfreetype-dev cabextract libmspack0t64 unzip
for f in *.deb; do dpkg -x "$f" ~/catherine-deps; done
