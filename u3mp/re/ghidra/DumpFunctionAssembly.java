// @category Uncharted
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.*;
public class DumpFunctionAssembly extends GhidraScript {
 public void run() throws Exception {
  String[] a=getScriptArgs();try(PrintWriter w=new PrintWriter(new FileWriter(a[0]))) {
   for(int i=1;i<a.length;i++) {Function f=getFunctionContaining(toAddr(a[i]));w.println("Function "+f.getName()+" "+f.getEntryPoint()+" "+f.getBody());
    InstructionIterator it=currentProgram.getListing().getInstructions(f.getBody(),true);
    while(it.hasNext()){Instruction x=it.next();StringBuilder h=new StringBuilder();for(byte b:x.getBytes())h.append(String.format("%02x",b&255));w.println(x.getAddress()+" "+h+" "+x);}
   }
  }
 }
}
