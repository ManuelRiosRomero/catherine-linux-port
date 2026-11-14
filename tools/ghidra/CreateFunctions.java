// Creates functions at the addresses listed in a file (one hex address per line), skipping any already
// inside a function or that don't disassemble. Usage (headless): -postScript CreateFunctions.java <list.txt>
// @category CatherineRecomp
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.FunctionManager;
import java.nio.file.*;

public class CreateFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        int made = 0, skipped = 0, failed = 0;
        for (String line : Files.readAllLines(Paths.get(getScriptArgs()[0]))) {
            if (line.isBlank() || monitor.isCancelled()) continue;
            Address a = toAddr(Long.parseLong(line.trim(), 16));
            // Skip addresses inside a known function or in the middle of an instruction (a "pointer" that is really
            // data). Code Ghidra decoded but never put in a function is fine: the lifter derives each function's
            // code by following flow, so Ghidra re-assigning shared instructions no longer matters.
            if (fm.getFunctionContaining(a) != null) { skipped++; continue; }
            if (getInstructionAt(a) == null) {
                if (currentProgram.getListing().getInstructionContaining(a) != null) { skipped++; continue; }
                DisassembleCommand dis = new DisassembleCommand(a, null, true);
                if (!dis.applyTo(currentProgram, monitor) || getInstructionAt(a) == null) { failed++; continue; }
            }
            if (new CreateFunctionCmd(a).applyTo(currentProgram, monitor)) made++; else failed++;
        }
        println("CreateFunctions: made " + made + ", already covered " + skipped + ", not code " + failed);
    }
}
