// Decompile functions to C: args = output file, then function addresses (hex). Read-only use:
//   analyzeHeadless <proj> <name> -process -noanalysis -readOnly -scriptPath tools\ghidra -postScript Decompile.java out.c 7014b0 701740
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class Decompile extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(a[0])) {
            for (int i = 1; i < a.length; i++) {
                Function f = getFunctionContaining(toAddr(Long.parseLong(a[i], 16)));
                if (f == null) { out.println("// no function at " + a[i]); continue; }
                DecompileResults r = d.decompileFunction(f, 120, monitor);
                out.println("// ===== " + f.getName() + " @ " + f.getEntryPoint());
                out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
            }
        }
    }
}
