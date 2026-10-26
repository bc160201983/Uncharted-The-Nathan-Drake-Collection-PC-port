import java.io.*;
import java.util.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
public class FindPoolInitializers extends GhidraScript {
 public void run() throws Exception {
  Map<String,List<String>> groups=new LinkedHashMap<>();
  InstructionIterator it=currentProgram.getListing().getInstructions(true);
  while(it.hasNext()){
   Instruction i=it.next();if(i.getAddress().compareTo(toAddr("013a0000"))>=0)break;
   if(!i.getMnemonicString().equals("MOV"))continue;
   String x=i.toString();
   if(!x.contains("+ 0x40],0x1"))continue;
   Function f=getFunctionContaining(i.getAddress());if(f==null)continue;
   List<String> lines=new ArrayList<>();boolean count=false,capacity=false;
   InstructionIterator body=currentProgram.getListing().getInstructions(f.getBody(),true);
   while(body.hasNext()){
    Instruction j=body.next();String t=j.toString();
    if(t.startsWith("MOV")&&t.contains("+ 0x3c],"))count=true;
    if(t.startsWith("MOV")&&t.contains("+ 0x14],"))capacity=true;
    lines.add(j.getAddress()+" "+t);
   }
   if(count&&capacity)groups.put(f.getEntryPoint().toString(),lines);
  }
  try(PrintWriter w=new PrintWriter(getScriptArgs()[0])){
   for(Map.Entry<String,List<String>> e:groups.entrySet()){
    w.println("FUNCTION "+e.getKey());for(String line:e.getValue())w.println(line);w.println();
   }
  }
 }
}
