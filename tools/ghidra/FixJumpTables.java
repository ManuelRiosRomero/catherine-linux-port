// Completes switch jump tables Ghidra only partly resolved. Input lines: "<jmp addr> <target> <target> ..." (hex),
// from tools/recomp/jumptables.py. For each target: disassemble it, add a COMPUTED_JUMP reference from the JMP, and
// finally recompute the owning function's body so the exported flows list every case.
// Usage (headless): -postScript FixJumpTables.java <tables.txt>
// @category CatherineRecomp
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.program.model.symbol.SourceType;
import java.nio.file.*;
import java.util.*;

public class FixJumpTables extends GhidraScript {
    @Override
    public void run() throws Exception {
        ReferenceManager refs = currentProgram.getReferenceManager();
        Set<Function> touched = new HashSet<>();
        int tables = 0, added = 0, disasm = 0;
        for (String line : Files.readAllLines(Paths.get(getScriptArgs()[0]))) {
            String[] p = line.trim().split("\\s+");
            if (p.length < 2 || monitor.isCancelled()) continue;
            Address jmp = toAddr(Long.parseLong(p[0], 16));
            if (getInstructionAt(jmp) == null) continue;
            tables++;
            for (int i = 1; i < p.length; i++) {
                Address t = toAddr(Long.parseLong(p[i], 16));
                if (getInstructionAt(t) == null) {
                    new DisassembleCommand(t, null, true).applyTo(currentProgram, monitor);
                    if (getInstructionAt(t) != null) disasm++;
                }
                if (refs.getReference(jmp, t, 0) == null) {
                    refs.addMemoryReference(jmp, t, RefType.COMPUTED_JUMP, SourceType.USER_DEFINED, 0);
                    added++;
                }
            }
            Function f = getFunctionContaining(jmp);
            if (f != null) touched.add(f);
        }
        for (Function f : touched) CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
        println("FixJumpTables: " + tables + " tables, " + added + " references added, " + disasm +
                " targets disassembled, " + touched.size() + " function bodies recomputed");
    }
}
