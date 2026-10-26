// Decompile selected entry addresses in a headless Ghidra project.
// @category Uncharted

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileAt extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            throw new IllegalArgumentException("usage: DecompileAt <output-file> <address> [address ...]");
        }

        File output = new File(args[0]);
        File parent = output.getParentFile();
        if (parent != null) {
            parent.mkdirs();
        }

        DecompInterface decompiler = new DecompInterface();
        decompiler.toggleCCode(true);
        decompiler.toggleSyntaxTree(true);
        if (!decompiler.openProgram(currentProgram)) {
            throw new IllegalStateException("could not open program in decompiler");
        }

        try (BufferedWriter writer = new BufferedWriter(new FileWriter(output))) {
            writer.write("Program: " + currentProgram.getName() + "\n");
            writer.write("Language: " + currentProgram.getLanguageID() + "\n\n");

            for (int i = 1; i < args.length; i++) {
                Address address = toAddr(args[i]);
                Function function = getFunctionAt(address);
                if (function == null) {
                    function = getFunctionContaining(address);
                }
                if (function == null) {
                    disassemble(address);
                    function = createFunction(address, null);
                }

                writer.write("========== " + address + " ==========\n");
                if (function == null) {
                    writer.write("No function could be created.\n\n");
                    continue;
                }
                writer.write("Function: " + function.getName() + " @ " + function.getEntryPoint() + "\n");
                writer.write("Body: " + function.getBody() + "\n");
                DecompileResults result = decompiler.decompileFunction(function, 180, monitor);
                if (!result.decompileCompleted()) {
                    writer.write("Decompile failed: " + result.getErrorMessage() + "\n\n");
                    continue;
                }
                writer.write(result.getDecompiledFunction().getC());
                writer.write("\n\n");
            }
        } finally {
            decompiler.dispose();
        }
        println("Wrote decompilation to " + output.getAbsolutePath());
    }
}
