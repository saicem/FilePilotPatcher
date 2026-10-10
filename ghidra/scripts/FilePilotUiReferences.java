// Export callers for native UI integration points without changing the project.
//@category Reverse Engineering
import java.io.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
public class FilePilotUiReferences extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter out = new PrintWriter(new OutputStreamWriter(new FileOutputStream(args[0]), "UTF-8"))) {
            for (int i = 1; i < args.length; ++i) {
                out.println("TARGET " + args[i]);
                for (Reference ref : getReferencesTo(toAddr(args[i]))) {
                    Function f = getFunctionContaining(ref.getFromAddress());
                    out.println(ref.getFromAddress() + " " + ref.getReferenceType() + " " + (f == null ? "data" : f.getEntryPoint() + " " + f.getName()));
                }
            }
        }
    }
}
