// Split the PS3 RW segment and mark pointer-table/TOC regions read-only so the decompiler folds them.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.*;
import ghidra.program.model.address.*;
public class PS3ReadOnly extends GhidraScript {
  public void run() throws Exception {
    Memory m=currentProgram.getMemory();
    long lo=0x111b5a8L, hi=0x11bdaf0L;
    MemoryBlock b=m.getBlock(toAddr(lo));
    println("block "+b.getName()+" "+b.getStart()+"-"+b.getEnd());
    if(!b.getStart().equals(toAddr(lo))){ m.split(b,toAddr(lo)); }
    b=m.getBlock(toAddr(lo));
    if(b.getEnd().getOffset()>=hi){ m.split(b,toAddr(hi)); }
    b=m.getBlock(toAddr(lo));
    b.setWrite(false);
    println("read-only "+b.getStart()+"-"+b.getEnd());
  }
}
