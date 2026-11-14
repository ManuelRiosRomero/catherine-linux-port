// Fixes a spot Ghidra decoded from the wrong byte (e.g. switch tables inside .text taken for code, so the real JMP
// before them is swallowed). Arguments: "lo-hi" ranges (hex, hi exclusive) to clear (instructions overlapping them and
// functions starting in them), then plain addresses to disassemble from. Follow with FixJumpTables.java for the
// switch cases and ExportPcode.java.
// Usage (headless): -postScript Redecode.java a37c6c-a37cf8 a37bc7 a37c24
// @category CatherineRecomp
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;

public class Redecode extends GhidraScript {
    @Override
    public void run() throws Exception {
        Listing listing = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();
        for (String a : getScriptArgs()) {
            if (!a.contains("-")) continue;
            Address lo = toAddr(Long.parseLong(a.split("-")[0], 16)), hi = toAddr(Long.parseLong(a.split("-")[1], 16) - 1);
            Instruction first = listing.getInstructionContaining(lo), last = listing.getInstructionContaining(hi);
            Address from = first != null ? first.getMinAddress() : lo, to = last != null ? last.getMaxAddress() : hi;
            for (Function f : fm.getFunctions(currentProgram.getAddressFactory().getAddressSet(lo, hi), true)) {
                println("Redecode: remove function " + f.getName());
                fm.removeFunction(f.getEntryPoint());
            }
            listing.clearCodeUnits(from, to, false);
            println("Redecode: cleared " + from + "-" + to);
        }
        for (String a : getScriptArgs()) {
            if (a.contains("-")) continue;
            Address at = toAddr(Long.parseLong(a, 16));
            new DisassembleCommand(at, null, true).applyTo(currentProgram, monitor);
            Instruction i = getInstructionAt(at);
            println("Redecode: " + at + " -> " + (i == null ? "nothing" : i.toString()));
            // the function the preceding instruction belongs to flows in here: grow its body (ExportPcode exports bodies)
            Instruction prev = listing.getInstructionBefore(at);
            Function f = prev == null ? null : fm.getFunctionContaining(prev.getMinAddress());
            if (f != null && CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor))
                println("Redecode: body of " + f.getName() + " now " + f.getBody().getNumAddresses() + " bytes");
        }
    }
}
