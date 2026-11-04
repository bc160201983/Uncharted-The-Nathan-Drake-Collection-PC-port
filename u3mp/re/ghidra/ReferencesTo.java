// Lists Ghidra references to selected addresses, including the containing function.
// @category Analysis

import java.io.BufferedWriter;
import java.io.FileWriter;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class ReferencesTo extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            throw new IllegalArgumentException("usage: ReferencesTo.java <output> <address>...");
        }
        try (PrintWriter writer = new PrintWriter(new BufferedWriter(new FileWriter(args[0])))) {
            writer.println("Program: " + currentProgram.getName());
            writer.println("Language: " + currentProgram.getLanguageID());
            writer.println();
            for (int i = 1; i < args.length; ++i) {
                Address target = toAddr(args[i]);
                writer.println("========== " + target + " ==========");
                ReferenceIterator references = currentProgram.getReferenceManager().getReferencesTo(target);
                int count = 0;
                while (references.hasNext()) {
                    Reference reference = references.next();
                    Address from = reference.getFromAddress();
                    Function function = getFunctionContaining(from);
                    Instruction instruction = getInstructionAt(from);
                    writer.printf(
                        "%s  %-20s  function=%s  instruction=%s%n",
                        from,
                        reference.getReferenceType(),
                        function == null ? "<none>" : function.getName() + "@" + function.getEntryPoint(),
                        instruction == null ? "<none>" : instruction.toString()
                    );
                    ++count;
                }
                writer.println("count=" + count);
                writer.println();
            }
        }
        println("Wrote references to " + args[0]);
    }
}
