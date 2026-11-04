// Decompile PS3 functions whose switch uses a "bctr" with a relative table placed right after it.
// Args: <output-file> <func>:<count> ...  (count = number of 32-bit table entries)
// The table holds signed offsets relative to the table start. Adds COMPUTED_JUMP references,
// disassembles the targets, fixes the function body, then decompiles (in-memory, read-only run).
// @category Uncharted

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.pcode.JumpTable;
import java.util.ArrayList;

public class DecompileJT extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File output = new File(args[0]);
        DecompInterface decompiler = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        opts.grabFromProgram(currentProgram);
        opts.setRespectReadOnly(true);
        decompiler.setOptions(opts);
        decompiler.toggleCCode(true);
        decompiler.toggleSyntaxTree(true);
        decompiler.openProgram(currentProgram);
        try (BufferedWriter w = new BufferedWriter(new FileWriter(output))) {
            for (int i = 1; i < args.length; i++) {
                String[] p = args[i].split(":");
                Address fa = toAddr(p[0]);
                int count = Integer.parseInt(p[1]);
                Function f = getFunctionAt(fa);
                if (f == null) { disassemble(fa); f = createFunction(fa, null); }
                // find bctr in the first body range
                Instruction bctr = null;
                Instruction ins = getInstructionAt(fa);
                for (int k = 0; ins != null && k < 400; k++) {
                    if (ins.getMnemonicString().equals("bctr")) { bctr = ins; break; }
                    ins = ins.getNext();
                }
                w.write("========== " + fa + " ==========\n");
                if (bctr == null) { w.write("no bctr\n"); continue; }
                Address tbl = bctr.getAddress().add(4);
                clearListing(tbl, tbl.add(count * 4 - 1));
                ArrayList<Address> targets = new ArrayList<>();
                for (int k = 0; k < count; k++) {
                    int off = getInt(tbl.add(k * 4));
                    Address t = tbl.add(off);
                    bctr.addMnemonicReference(t, RefType.COMPUTED_JUMP, SourceType.USER_DEFINED);
                    if (getInstructionAt(t) == null) disassemble(t);
                    targets.add(t);
                }
                CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
                new JumpTable(bctr.getAddress(), targets, true, 0).writeOverride(f);
                w.write("Body: " + f.getBody() + "\n");
                DecompileResults r = decompiler.decompileFunction(f, 300, monitor);
                if (!r.decompileCompleted()) { w.write("Decompile failed: " + r.getErrorMessage() + "\n"); continue; }
                w.write(r.getDecompiledFunction().getC());
                w.write("\n\n");
            }
        } finally {
            decompiler.dispose();
        }
    }
}
