#!/bin/bash
# Image base, size and imported DLLs of the Microsoft D3DX/compiler DLLs (PC copies, for planning).
for f in /mnt/c/Windows/SysWOW64/d3dx9_43.dll /mnt/c/Windows/SysWOW64/D3DCompiler_43.dll; do
  echo "== $(basename "$f")"
  objdump -p "$f" | grep -E "^ImageBase|^SizeOfImage|DLL Name" | tr '\n' ' '; echo
done
which cabextract bsdtar 7z 2>&1 | grep -v "no " | head -3
