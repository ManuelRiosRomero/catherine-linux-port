#!/bin/bash
# Disassemble Catherine.exe and the two rebased DLLs with Ghidra, every repair scripted: build/catherine.pcode.txt,
# d3dx9.pcode.txt, d3dcompiler.pcode.txt (~15 min on a desktop). Ghidra 12.1.4's analysis is deterministic.
# Future repairs go here. Each search step loops until it finds nothing.
set -e
. "$(dirname "$0")/paths.sh"; cd "$REPO"
. build/status.sh
export JAVA_HOME=~/catherine-deps/jdk
G=~/catherine-deps/ghidra/support/analyzeHeadless
T=build/tmp/rebuild; mkdir -p $T
cp "$EXE" $T/Catherine.exe      # no spaces in the path Ghidra sees

ghidra() {   # (project, name, log, dump, post-script args...): run them on the project, then export the dump
    local p=$1 n=$2 l=$3 d=$4; shift 4
    "$G" "$p" "$n" -process -noanalysis -scriptPath tools/ghidra "$@" -postScript ExportPcode.java "$d" > "$l" 2>&1
    grep -Eo "(Redecode|FixHoles|FixJumpTables|CreateFunctions|ExportPcode): (made|fixed|[0-9]+ tables|[0-9]+ functions|cleared).*" "$l" | sed 's/^/   /' || true
}
import() {   # (project, name, image, first dump)
    rm -rf "$1"; mkdir -p "$1"
    "$G" "$1" "$2" -import "$3" -scriptPath tools/ghidra -postScript ExportPcode.java "$4" > "$T/import_$2.log" 2>&1
    grep -Eo "ExportPcode: .*" "$T/import_$2.log" | sed 's/^/   /'
}
discover() {   # (image, project, name, dump base): create functions at uncovered code pointers until nothing is new
    local d=build/$4.pcode.txt l=$T/create_$4.log made
    for round in 1 2 3 4 5 6; do
        python3 tools/recomp/lift.py $d /tmp/idxonly_$$ 0 > /dev/null
        python3 tools/recomp/find_missing.py "$1" $d build/tmp/missing_$4.txt
        ghidra "$2" "$3" $l $d -postScript CreateFunctions.java build/tmp/missing_$4.txt
        made=$(grep -Eo "CreateFunctions: made [0-9]+" $l | grep -Eo "[0-9]+$")
        echo "   round $round: made $made"
        [ "$made" = 0 ] && break
    done
    rm -rf /tmp/idxonly_$$
}

P=build/ghidra_rebuild D=build/catherine.pcode.txt I=$T/Catherine.exe
fix() { ghidra $P Catherine "$T/$1.log" $D "${@:2}"; }
status "Ghidra: Catherine.exe, import + analysis"; import $P Catherine $I $D
status "Ghidra: Catherine.exe, function discovery"; discover $I $P Catherine catherine
status "Ghidra: Catherine.exe, switch tables"
for i in 1 2 3 4 5; do
    python3 tools/recomp/jumptables.py $I $D $T/jt$i.txt
    [ -s $T/jt$i.txt ] || break
    fix jt$i -postScript FixJumpTables.java $T/jt$i.txt
done
status "Ghidra: Catherine.exe, discovery after the new switch cases"; discover $I $P Catherine catherine
status "Ghidra: Catherine.exe, holes"
for i in 1 2 3 4 5; do
    python3 tools/recomp/find_holes.py $D > $T/holes$i.txt
    fix holes$i -postScript FixHoles.java $T/holes$i.txt
    grep -q "fixed 0," $T/holes$i.log && break
done
status "Ghidra: Catherine.exe, hand-found mis-decodes"
# 0xa37790: two byte-indexed switches whose tables (a37cbc..a37cf7) Ghidra decoded as code (session 10)
printf 'a37bce a37945 a37be0 a37bf6\na37c24 a37945 a37be0 a37c2b\n' > $T/jt_a37.txt
fix fix_a37 -postScript Redecode.java a37bc7-a37bd5 a37c24-a37c33 a37c6c-a37cf8 a37bc7 a37c24 -postScript FixJumpTables.java $T/jt_a37.txt
# 0x6a90fb: decoded one byte off (session 10)
fix fix_6a9 -postScript Redecode.java 6a90fb-6a910b 6a90fb
# 0x67ba50 (the bar's START menu): hidden in a switch table decoded as code; the junk function at 67ba20 runs over it (session 11e)
echo 67ba50 > $T/fn_67ba50.txt
echo "67ba91 67ba98 67bad3 67bb87 67bb9f 67bbbb 67bbdb 67bbfa 67bc14 67bc2e 67bc71" > $T/jt_67ba50.txt
fix fix_67ba50 -postScript Redecode.java 67ba20-67be58 -postScript CreateFunctions.java $T/fn_67ba50.txt -postScript FixJumpTables.java $T/jt_67ba50.txt
# real code that falls into bytes decoded one byte off, in functions reached only through pointers (tools/recomp/misdecodes.py,
# each reviewed by hand against objdump, 2026-10-26; the other candidates were data)
fix fix_misdecodes -postScript Redecode.java 669df7-669e15 69b528-69b53c 73c7da-73c7dd 747b4a-747b55 8c9e71-8c9e85 669df7 69b528 73c7da 747b4a 8c9e71
# no discovery after this: it would take the switch tables cleared above (pointers into a37c6c..) for code again

for pair in d3dx9:d3dx9_43.dll d3dcompiler:D3DCompiler_43.dll; do
    n=${pair%%:*}; I=build/msdll/rebased/${pair#*:}
    status "Ghidra: $n"
    import build/ghidra_$n $n $I build/$n.pcode.txt
    discover $I build/ghidra_$n $n $n
    python3 tools/recomp/jumptables.py $I build/$n.pcode.txt $T/jt_$n.txt
    ghidra build/ghidra_$n $n $T/jt_$n.log build/$n.pcode.txt -postScript FixJumpTables.java $T/jt_$n.txt
    discover $I build/ghidra_$n $n $n   # jump-table targets can reach more uncovered pointers
done
status "Ghidra done (idle)"
