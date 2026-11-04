// Set PS3 TOC (r2) and seed functions from .opd descriptors.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import java.math.BigInteger;
public class PS3Setup extends GhidraScript {
  public void run() throws Exception {
    long toc=0x11c2070L, opd=0x1168bd8L; int n=0x51498/8;
    Register r2=currentProgram.getRegister("r2");
    ProgramContext ctx=currentProgram.getProgramContext();
    for (MemoryBlock b: currentProgram.getMemory().getBlocks()) {
      if (b.isExecute()) ctx.setValue(r2,b.getStart(),b.getEnd(),BigInteger.valueOf(toc));
    }
    Memory m=currentProgram.getMemory();
    int made=0;
    for (int i=0;i<n;i++){
      Address a=toAddr(opd+i*8L);
      long code=m.getInt(a)&0xffffffffL;
      Address c=toAddr(code);
      if (!m.contains(c)) continue;
      createLabel(c,"f_"+Long.toHexString(code),true);
      disassemble(c);
      if (getFunctionAt(c)==null && createFunction(c,null)!=null) made++;
    }
    println("PS3Setup: functions seeded "+made);
  }
}
