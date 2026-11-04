import java.io.*;
import java.nio.file.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.listing.Instruction;
public class VerifyBootstrap extends GhidraScript {
 public void run() throws Exception {
  String[] a=getScriptArgs();byte[] bytes=Files.readAllBytes(Path.of(a[0]));Address start=toAddr("02050000");
  MemoryBlock b=currentProgram.getMemory().createInitializedBlock("U3ExperimentalBootstrap",start,new ByteArrayInputStream(bytes),bytes.length,monitor,false);b.setRead(true);b.setExecute(true);b.setWrite(false);
  disassemble(start);
  try(PrintWriter w=new PrintWriter(a[1])){
   w.println("Experimental injected stub; static Ghidra disassembly only. No execution is established.");
   Address at=start;
   while(at.compareTo(start.add(bytes.length))<0){Instruction i=getInstructionAt(at);if(i==null)throw new IllegalStateException("undecoded byte "+at);w.println(i.getAddress()+"  "+i);at=at.add(i.getLength());}
  }
 }
}
