// Read-only function/data inventory for reconstructing native widget recipes.
//@category Reverse Engineering
import java.io.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
public class FilePilotUiLocations extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter out = new PrintWriter(new OutputStreamWriter(new FileOutputStream(args[0]), "UTF-8"))) {
            for (int i = 1; i < args.length; ++i) {
                var address = toAddr(args[i]);
                Function f = getFunctionContaining(address);
                out.println(args[i] + " " + (f == null ? "data" : f.getEntryPoint() + " " + f.getBody().getNumAddresses() + " bytes " + f.getName()));
                if (f == null) {
                    byte[] bytes = getBytes(address, 32);
                    for (byte b : bytes) out.printf("%02x ", b & 255);
                    out.println();
                }
            }
        }
    }
}
