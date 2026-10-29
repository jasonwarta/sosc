// Headless: decompile every function and dump C plus call sites of timing/registry imports.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class ExportDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outDir = getScriptArgs().length > 0 ? getScriptArgs()[0] : ".";
        new File(outDir).mkdirs();

        try (PrintWriter x = new PrintWriter(new FileWriter(new File(outDir, "import_xrefs.txt")))) {
            String[] names = {"GetTickCount", "timeGetTime", "QueryPerformanceCounter", "QueryPerformanceFrequency",
                "timeSetEvent", "timeBeginPeriod", "Sleep", "RegOpenKeyA", "RegQueryValueExA",
                "GetPrivateProfileStringA", "GetPrivateProfileIntA", "GetModuleFileNameA", "GetDriveTypeA",
                "GetLogicalDriveStringsA", "DirectDrawCreate", "DirectSoundCreate", "_grSstWinOpen@28", "wglCreateContext"};
            for (String n : names) {
                for (Symbol s : currentProgram.getSymbolTable().getSymbols(n)) {
                    for (Reference r : getReferencesTo(s.getAddress())) {
                        Function f = getFunctionContaining(r.getFromAddress());
                        x.println(n + "\t" + r.getFromAddress() + "\t" + (f == null ? "?" : f.getName() + "@" + f.getEntryPoint()));
                    }
                    // thunks/pointer refs: follow refs to IAT entries
                }
            }
        }

        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        int n = 0;
        try (PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter(new File(outDir, "streets.c"))))) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (monitor.isCancelled()) break;
                DecompileResults r = d.decompileFunction(f, 60, monitor);
                w.println("// ==== " + f.getName() + " @ " + f.getEntryPoint());
                if (r != null && r.decompileCompleted()) w.println(r.getDecompiledFunction().getC());
                else w.println("// decompile failed");
                if (++n % 500 == 0) println("decompiled " + n);
            }
        }
        println("done: " + n + " functions");
    }
}
