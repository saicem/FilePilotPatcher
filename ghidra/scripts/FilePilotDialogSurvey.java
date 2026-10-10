// Read-only evidence for a File Pilot Open/Save picker implementation.
//@category Reverse Engineering
import java.io.*;
import java.util.*;
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class FilePilotDialogSurvey extends GhidraScript {
    private PrintWriter out;
    private void refs(Address target) {
        for (Reference r : currentProgram.getReferenceManager().getReferencesTo(target)) {
            Function f = getFunctionContaining(r.getFromAddress());
            out.printf("  %s %s %s%n", r.getFromAddress(), r.getReferenceType(), f == null ? "data" : f.getName()+" @ "+f.getEntryPoint());
        }
    }
    public void run() throws Exception {
        String[] args=getScriptArgs();
        try (PrintWriter w=new PrintWriter(new OutputStreamWriter(new FileOutputStream(args[0]), "UTF-8"))) {
            out=w;
            out.printf("Program=%s SHA256=%s base=%s%n", currentProgram.getName(), currentProgram.getExecutableSHA256(), currentProgram.getImageBase());
            SymbolIterator symbols=currentProgram.getSymbolTable().getExternalSymbols();
            while(symbols.hasNext()) {
                Symbol s=symbols.next(); String n=s.getName(true);
                if(n.matches("(?i).*(dialog|filename|shellexecute|cocreate|classobject|createwindow|windowlong|commandline|postmessage).*")) {
                    out.println("IMPORT "+n+" "+s.getAddress()); refs(s.getAddress());
                }
            }
            DataIterator data=currentProgram.getListing().getDefinedData(true);
            while(data.hasNext()) {
                Data d=data.next(); if(!d.hasStringValue())continue;
                String v=String.valueOf(d.getValue());
                if(v.matches("(?is).*(filedialog|save as|filename|file name|openwith|open with|FilePilot-OpenWindowFormat|Panel_Filter|SelectAll|--root|--select).*")) {
                    out.println("STRING "+d.getAddress()+" "+v);refs(d.getAddress());
                }
            }
            DecompInterface dc=new DecompInterface();dc.openProgram(currentProgram);
            for(int i=1;i<args.length;i++) {
                Address a=toAddr(args[i]);Function f=getFunctionContaining(a);
                out.println("\nFUNCTION REQUEST "+a);
                if(f==null){out.println("No function");continue;}
                out.printf("%s size=%d%n", f.getName(),f.getBody().getNumAddresses());refs(f.getEntryPoint());
                DecompileResults r=dc.decompileFunction(f,90,monitor);
                out.println(r.decompileCompleted()&&r.getDecompiledFunction()!=null?r.getDecompiledFunction().getC():r.getErrorMessage());
            }
            dc.dispose();
        }
    }
}
