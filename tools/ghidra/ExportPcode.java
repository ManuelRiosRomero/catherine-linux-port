// Dumps every function's raw p-code (Ghidra's exact per-instruction semantics) to a compact text file.
// Usage (headless): -postScript ExportPcode.java <out.txt>
// @category CatherineRecomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.pcode.*;
import java.io.*;

public class ExportPcode extends GhidraScript {

    private static String vn(Varnode v) {
        if (v == null) return "-";
        AddressSpace s = v.getAddress().getAddressSpace();
        String k;
        if (v.isConstant()) k = "c";
        else if (v.isRegister()) k = "r";
        else if (v.isUnique()) k = "u";
        else if (s.isMemorySpace()) k = "m";
        else k = "?" + s.getName();
        return k + Long.toHexString(v.getOffset()) + ":" + v.getSize();
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args[0]);
        Language lang = currentProgram.getLanguage();
        Listing listing = currentProgram.getListing();
        long fnCount = 0, insCount = 0;

        try (PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter(out), 1 << 22))) {
            w.println("LANG " + lang.getLanguageID() + " " + currentProgram.getCompilerSpec().getCompilerSpecID());
            for (Register r : lang.getRegisters()) {
                w.println("REG " + r.getName() + " " + Long.toHexString(r.getOffset()) + " " + r.getMinimumByteSize());
            }
            for (int i = 0; i < lang.getNumberOfUserDefinedOpNames(); i++) {
                w.println("USEROP " + i + " " + lang.getUserDefinedOpName(i));
            }

            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (monitor.isCancelled()) break;
                fnCount++;
                w.println("FN " + Long.toHexString(f.getEntryPoint().getOffset()) + " " + f.getName() + " "
                        + (f.isThunk() ? 1 : 0));
                for (AddressRange r : f.getBody()) {
                    w.println("BODY " + Long.toHexString(r.getMinAddress().getOffset()) + " "
                            + Long.toHexString(r.getMaxAddress().getOffset()));
                }
                InstructionIterator it = listing.getInstructions(f.getBody(), true);
                while (it.hasNext()) {
                    Instruction ins = it.next();
                    insCount++;
                    Address ft = ins.getFallThrough();
                    StringBuilder flows = new StringBuilder();
                    for (Address a : ins.getFlows()) {
                        if (flows.length() > 0) flows.append(',');
                        flows.append(Long.toHexString(a.getOffset()));
                    }
                    w.println("INS " + Long.toHexString(ins.getAddress().getOffset()) + " " + ins.getLength() + " "
                            + (ft == null ? "-" : Long.toHexString(ft.getOffset())) + " "
                            + (flows.length() == 0 ? "-" : flows) + " | " + ins);
                    for (PcodeOp op : ins.getPcode()) {
                        StringBuilder sb = new StringBuilder("OP ");
                        sb.append(op.getMnemonic()).append(' ').append(vn(op.getOutput()));
                        for (int i = 0; i < op.getNumInputs(); i++) sb.append(' ').append(vn(op.getInput(i)));
                        w.println(sb);
                    }
                }
            }
            w.println("END " + fnCount + " " + insCount);
        }
        println("ExportPcode: " + fnCount + " functions, " + insCount + " instructions -> " + out);
    }
}
