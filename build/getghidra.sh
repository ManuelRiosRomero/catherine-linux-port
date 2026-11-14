#!/bin/bash
# Ghidra 12.1.4 (pinned: same analysis = same translation) + a Java 21 for it, into ~/catherine-deps, no sudo.
set -e
. "$(dirname "$0")/paths.sh"
D=~/catherine-deps; mkdir -p $D/src; cd $D/src
if [ ! -x $D/ghidra/support/analyzeHeadless ]; then
    Z=ghidra_12.1.4_PUBLIC_20260921.zip
    [ -f $Z ] || curl -sSfL --retry 5 --retry-all-errors -O https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/$Z
    rm -rf ghidra_12.1.4_PUBLIC && unzip -q $Z && mv ghidra_12.1.4_PUBLIC $D/ghidra && rm $Z
fi
if [ ! -x $D/jdk/bin/java ]; then
    mkdir -p $D/jdk
    curl -sSfL --retry 5 --retry-all-errors https://api.adoptium.net/v3/binary/latest/21/ga/linux/x64/jdk/hotspot/normal/eclipse | tar xz -C $D/jdk --strip-components=1
fi
$D/jdk/bin/java -version 2>&1 | head -1
