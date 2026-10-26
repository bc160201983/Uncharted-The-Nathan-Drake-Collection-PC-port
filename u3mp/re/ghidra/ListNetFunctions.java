import java.io.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
public class ListNetFunctions extends GhidraScript {
 public void run()throws Exception {
  try(PrintWriter w=new PrintWriter(getScriptArgs()[0])){
   FunctionIterator fs=currentProgram.getFunctionManager().getFunctions(toAddr("0071fe20"),true);
   while(fs.hasNext()){
    Function f=fs.next();if(f.getEntryPoint().compareTo(toAddr("007230f0"))>0)break;
    w.println(f.getEntryPoint()+" "+f.getName()+" bytes="+f.getBody().getNumAddresses());
    if(f.getBody().getNumAddresses()<48){InstructionIterator it=currentProgram.getListing().getInstructions(f.getBody(),true);while(it.hasNext())w.println("  "+it.next());}
   }
  }
 }
}
