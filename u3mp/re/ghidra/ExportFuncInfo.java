// Export per-function: entry, size, referenced strings, called functions, referenced data addresses.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;
import ghidra.program.model.mem.*;
import java.io.*;
import java.util.*;
public class ExportFuncInfo extends GhidraScript {
  String str(Address a){
    try{
      Data d=getDataAt(a);
      if(d!=null && d.getValue() instanceof String) return (String)d.getValue();
      Memory m=currentProgram.getMemory();
      StringBuilder sb=new StringBuilder();
      for(int i=0;i<120;i++){int c=m.getByte(a.add(i))&0xff; if(c==0)break; if(c<32||c>126) return null; sb.append((char)c);}
      return sb.length()>=4?sb.toString():null;
    }catch(Exception e){return null;}
  }
  public void run() throws Exception {
    String out=getScriptArgs()[0];
    PrintWriter w=new PrintWriter(new FileWriter(out));
    FunctionIterator it=currentProgram.getFunctionManager().getFunctions(true);
    ReferenceManager rm=currentProgram.getReferenceManager();
    while(it.hasNext() && !monitor.isCancelled()){
      Function f=it.next();
      StringBuilder sb=new StringBuilder();
      sb.append(Long.toHexString(f.getEntryPoint().getOffset())).append('\t').append(f.getBody().getNumAddresses()).append('\t');
      Set<String> strs=new LinkedHashSet<>(); Set<String> calls=new LinkedHashSet<>(); Set<String> data=new LinkedHashSet<>();
      for(Address a: f.getBody().getAddresses(true)){
        for(Reference r: rm.getReferencesFrom(a)){
          Address t=r.getToAddress();
          if(r.getReferenceType().isCall()){ calls.add(Long.toHexString(t.getOffset())); continue; }
          if(!t.isMemoryAddress()) continue;
          String s=str(t);
          if(s!=null) strs.add(s.replace('\t',' ').replace('\n',' '));
          else if(r.getReferenceType().isData()) data.add(Long.toHexString(t.getOffset()));
        }
      }
      sb.append(String.join("\u0001",strs)).append('\t').append(String.join(",",calls)).append('\t').append(String.join(",",data));
      w.println(sb);
    }
    w.close();
  }
}
