// Export native UI functions without changing the project.
//@category Reverse Engineering
import java.io.*;
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
public class FilePilotNativeUi extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter out = new PrintWriter(new OutputStreamWriter(new FileOutputStream(args[0]), "UTF-8"))) {
            DecompInterface dc = new DecompInterface();
            dc.openProgram(currentProgram);
            for (int i = 1; i < args.length; ++i) {
                Function f = getFunctionContaining(toAddr(args[i]));
                out.println("\nFUNCTION " + args[i]); out.flush();
                if (f == null) { out.println("No function"); continue; }
                DecompileResults r = dc.decompileFunction(f, 90, monitor);
                out.println(r.decompileCompleted() && r.getDecompiledFunction() != null
                    ? r.getDecompiledFunction().getC() : r.getErrorMessage());
                out.flush();
            }
            dc.dispose();
        }
    }
}
