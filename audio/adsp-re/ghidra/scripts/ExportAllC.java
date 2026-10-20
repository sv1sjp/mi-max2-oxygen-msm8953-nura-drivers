// SPDX-License-Identifier: GPL-2.0-or-later
// Decompile every function to one C file (parallel, via Ghidra's C/C++ exporter),
// and write a function list: entry  size  name.
import ghidra.app.script.GhidraScript;
import ghidra.app.util.exporter.CppExporter;
import ghidra.program.model.listing.*;
import java.io.*;

public class ExportAllC extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outDir = getScriptArgs()[0];
        try (PrintWriter pw = new PrintWriter(new FileWriter(outDir + "/functions.tsv"))) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                pw.println(f.getEntryPoint() + "\t" + f.getBody().getNumAddresses() + "\t" + f.getName());
            }
        }
        println("functions: " + currentProgram.getFunctionManager().getFunctionCount());
        new CppExporter().export(new File(outDir + "/adsp.c"), currentProgram,
                currentProgram.getMemory(), monitor);
        println("export done");
    }
}
