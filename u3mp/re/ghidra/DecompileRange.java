// @category Uncharted
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class DecompileRange extends GhidraScript {
 public void run() throws Exception {
  String[] a=getScriptArgs();
  DecompInterface d=new DecompInterface();d.openProgram(currentProgram);
  try(PrintWriter w=new PrintWriter(new FileWriter(a[0]))) {
   FunctionIterator fs=currentProgram.getFunctionManager().getFunctions(toAddr(a[1]),true);
   while(fs.hasNext()) {Function f=fs.next();if(f.getEntryPoint().compareTo(toAddr(a[2]))>=0)break;
    w.println("========== "+f.getEntryPoint()+" "+f.getName()+" "+f.getBody()+" ==========");
    DecompileResults r=d.decompileFunction(f,45,monitor);w.println(r.decompileCompleted()?r.getDecompiledFunction().getC():r.getErrorMessage());
   }
  }finally{d.dispose();}
 }
}
