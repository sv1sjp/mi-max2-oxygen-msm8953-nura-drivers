// SPDX-License-Identifier: GPL-2.0-or-later
// Decomp.java OUTFILE ADDR... : create a function at each ADDR if none contains it,
// decompile it, append C to OUTFILE. Changes are saved to the project.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.*;

public class Decomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter pw = new PrintWriter(new FileWriter(a[0], true))) {
            for (int i = 1; i < a.length; i++) {
                Address ad = toAddr(a[i]);
                Function f = getFunctionContaining(ad);
                if (f == null) {
                    disassemble(ad);
                    new CreateFunctionCmd(ad).applyTo(currentProgram, monitor);
                    f = getFunctionAt(ad);
                }
                if (f == null) { pw.println("== " + a[i] + ": could not create function"); continue; }
                DecompileResults r = di.decompileFunction(f, 120, monitor);
                pw.println("== " + f.getName() + " @ " + f.getEntryPoint() + " (asked " + a[i] + ")");
                pw.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "decompile failed: " + r.getErrorMessage());
            }
        }
    }
}
