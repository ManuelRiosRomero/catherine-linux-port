#!/bin/bash
# The game needs Microsoft's d3dx9_43.dll and D3DCompiler_43.dll (32-bit): taken from the DirectX redistributable that
# Steam installs (Steamworks Shared), else from Microsoft's own download. Checked by hash, then rebased (rebase.py).
set -e
. "$(dirname "$0")/paths.sh"
M="$REPO/build/msdll"; mkdir -p "$M/rebased"; cd "$M"
S="$GAME/../Steamworks Shared/_CommonRedist/DirectX/Jun2010"
if [ ! -f "$S/Jun2010_d3dx9_43_x86.cab" ]; then
    S=.
    if [ ! -f Jun2010_d3dx9_43_x86.cab ]; then
        curl -sSfL --retry 5 --retry-all-errors -o redist.exe https://download.microsoft.com/download/8/4/A/84A35BF1-DAFE-4AE8-82AF-AD2AE20B6B14/directx_Jun2010_redist.exe
        LD_LIBRARY_PATH=$HOME/catherine-deps/usr/lib/x86_64-linux-gnu cabextract -q -F Jun2010_d3dx9_43_x86.cab -F Jun2010_D3DCompiler_43_x86.cab redist.exe
        rm redist.exe
    fi
fi
export LD_LIBRARY_PATH=$HOME/catherine-deps/usr/lib/x86_64-linux-gnu   # cabextract unpacked by getdeps.sh
cabextract -q -F d3dx9_43.dll "$S/Jun2010_d3dx9_43_x86.cab"
cabextract -q -F D3DCompiler_43.dll "$S/Jun2010_D3DCompiler_43_x86.cab"
python3 "$REPO/tools/recomp/rebase.py" d3dx9_43.dll rebased/d3dx9_43.dll 5000000
python3 "$REPO/tools/recomp/rebase.py" D3DCompiler_43.dll rebased/D3DCompiler_43.dll 6000000
sha256sum -c --quiet <<'SUMS'
0b28546be22c71834501f7d7185ede5d79742457331c7ee09efc14490dd64f5f  d3dx9_43.dll
2f23182ec6f4889397ac4bf03d62536136c5bdba825c7d2c4ef08c827f3a8a1c  D3DCompiler_43.dll
91ab05c2fe622d69b509d7dc316aaa891943db1184192cf4ad0f62360cd04b3a  rebased/d3dx9_43.dll
8b44fbc0faeca016ea195c70ba82491980c2214556c76341c12ad3e33cd0ba0c  rebased/D3DCompiler_43.dll
SUMS
