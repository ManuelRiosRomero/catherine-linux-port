// Repairs "holes": real code flows into an address Ghidra never decoded because a junk function was created a few bytes
// further on (a constant like 0x5c2000 taken for a code pointer) and its bogus instructions claim the bytes.
// For each hole address in the list file: if the instruction that decodes at the hole covers the entry of a function
// starting within 15 bytes after it (and nothing CALLs that function), drop the function, clear its body, then
// disassemble from the hole and from every cleared address that remaining code still branches to. A hole that is just
// alignment padding before a real function ends at the entry instead of covering it, and is left alone.
// Usage (headless): -postScript FixHoles.java <holes.txt> [dry]   (dry: only print the decisions)
// @category CatherineRecomp
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.app.util.PseudoDisassembler;
import ghidra.app.util.PseudoInstruction;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.nio.file.*;
import java.util.*;

public class FixHoles extends GhidraScript {
    @Override
    public void run() throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        Listing listing = currentProgram.getListing();
        ReferenceManager rm = currentProgram.getReferenceManager();
        boolean dry = getScriptArgs().length > 1 && getScriptArgs()[1].equals("dry");
        PseudoDisassembler pd = new PseudoDisassembler(currentProgram);
        int fixed = 0, skipped = 0;
        for (String line : Files.readAllLines(Paths.get(getScriptArgs()[0]))) {
            if (line.isBlank() || monitor.isCancelled()) continue;
            Address hole = toAddr(Long.parseLong(line.trim(), 16));
            if (getInstructionAt(hole) != null) { skipped++; continue; }   // already fixed by an earlier entry
            Function junk = null;
            for (int d = 1; d <= 15 && junk == null; d++) junk = fm.getFunctionAt(hole.add(d));
            if (junk == null) { println("FixHoles: " + hole + " no junk function"); skipped++; continue; }
            boolean called = false;
            for (Reference r : rm.getReferencesTo(junk.getEntryPoint())) called |= r.getReferenceType().isCall();
            if (called) { println("FixHoles: " + hole + " " + junk.getName() + " is called, left alone"); skipped++; continue; }
            int b0 = getByte(hole) & 0xFF;
            if (b0 == 0x00 || b0 == 0xCC) {   // alignment padding (00 decodes as ADD byte ptr) before a real function
                println("FixHoles: " + hole + " is padding before " + junk.getName() + ", left alone");
                skipped++;
                continue;
            }
            PseudoInstruction real = null;
            try { real = pd.disassemble(hole); } catch (Exception e) { }
            if (real == null || hole.add(real.getLength()).compareTo(junk.getEntryPoint()) <= 0) {
                println("FixHoles: " + hole + " " + (real == null ? "does not decode" : "'" + real + "' ends before " +
                        junk.getName()) + ", left alone");
                skipped++;
                continue;
            }
            AddressSetView body = junk.getBody();
            if (dry) {
                println("FixHoles: " + hole + " '" + real + "' covers " + junk.getName() + " (" + body.getNumAddresses() + " bytes): would fix");
                fixed++;
                continue;
            }
            // addresses in the junk body that anything outside it points to (also mid-junk-instruction: the junk can
            // swallow the next real function, reached only through a vtable): real instruction starts to re-decode.
            // Call or data references there mark a function entry to recreate.
            List<Address> again = new ArrayList<>();
            Set<Address> entries = new HashSet<>();
            again.add(hole);
            AddressIterator dests = rm.getReferenceDestinationIterator(body, true);
            while (dests.hasNext()) {
                Address d = dests.next();
                for (Reference r : rm.getReferencesTo(d)) {
                    if (body.contains(r.getFromAddress())) continue;
                    if (!again.contains(d)) again.add(d);
                    if (!r.getReferenceType().isJump()) entries.add(d);
                }
            }
            entries.remove(junk.getEntryPoint());
            fm.removeFunction(junk.getEntryPoint());
            AddressSet clear = new AddressSet(body);
            Instruction over = listing.getInstructionContaining(hole);   // a junk instruction straddling the hole
            if (over != null) clear.add(over.getMinAddress(), over.getMaxAddress());
            for (AddressRange r : clear) listing.clearCodeUnits(r.getMinAddress(), r.getMaxAddress(), false);
            for (Address a : again) new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
            for (Address a : entries) if (getInstructionAt(a) != null) new CreateFunctionCmd(a).applyTo(currentProgram, monitor);
            // ExportPcode only writes instructions inside function bodies: grow the functions that flow into the
            // re-decoded code (the hole's own function: the one holding the instruction just before it)
            Set<Function> grow = new HashSet<>();
            Instruction prev = listing.getInstructionContaining(hole.subtract(1));
            if (prev != null && fm.getFunctionContaining(prev.getAddress()) != null) grow.add(fm.getFunctionContaining(prev.getAddress()));
            for (Address a : again)
                for (Reference r : rm.getReferencesTo(a))
                    if (fm.getFunctionContaining(r.getFromAddress()) != null) grow.add(fm.getFunctionContaining(r.getFromAddress()));
            for (Function f : grow) CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
            println("FixHoles: " + hole + " dropped " + junk.getName() + ", re-decoded " + again.size() + " starts, grew " +
                    grow.size() + " function(s)" + (fm.getFunctionContaining(hole) != null ? "" : " (hole still outside any function!)"));
            fixed++;
        }
        println("FixHoles: fixed " + fixed + ", skipped " + skipped);
    }
}
