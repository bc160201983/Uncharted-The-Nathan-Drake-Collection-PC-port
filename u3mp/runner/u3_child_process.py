"""Launch the U3 runtime with a process-local address layout compatible with its guest reserve.
No image, registry, or system mitigation setting is changed. Only this child gets policy 0x220000.
"""
import ctypes as c
from ctypes import wintypes as w
import msvcrt,os,subprocess,time
from pathlib import Path

_POLICY=0x220000
_RUNTIME=Path(os.environ.get('UNCHARTED_PC_DIR') or Path(__file__).resolve().parents[2])/'gow3_pc/out/Uncharted.exe'
_k=c.WinDLL('kernel32',use_last_error=True)
class _SI(c.Structure):
 _fields_=[('cb',w.DWORD),('lpReserved',w.LPWSTR),('lpDesktop',w.LPWSTR),('lpTitle',w.LPWSTR),('dwX',w.DWORD),('dwY',w.DWORD),('dwXSize',w.DWORD),('dwYSize',w.DWORD),('dwXCountChars',w.DWORD),('dwYCountChars',w.DWORD),('dwFillAttribute',w.DWORD),('dwFlags',w.DWORD),('wShowWindow',w.WORD),('cbReserved2',w.WORD),('lpReserved2',c.POINTER(c.c_byte)),('hStdInput',w.HANDLE),('hStdOutput',w.HANDLE),('hStdError',w.HANDLE)]
class _SIX(c.Structure):_fields_=[('StartupInfo',_SI),('lpAttributeList',c.c_void_p)]
class _PI(c.Structure):_fields_=[('hProcess',w.HANDLE),('hThread',w.HANDLE),('dwProcessId',w.DWORD),('dwThreadId',w.DWORD)]
class _MBI(c.Structure):_fields_=[('BaseAddress',c.c_void_p),('AllocationBase',c.c_void_p),('AllocationProtect',w.DWORD),('PartitionId',w.WORD),('RegionSize',c.c_size_t),('State',w.DWORD),('Protect',w.DWORD),('Type',w.DWORD)]
_k.InitializeProcThreadAttributeList.argtypes=[c.c_void_p,w.DWORD,w.DWORD,c.POINTER(c.c_size_t)];_k.InitializeProcThreadAttributeList.restype=w.BOOL
_k.UpdateProcThreadAttribute.argtypes=[c.c_void_p,w.DWORD,c.c_size_t,c.c_void_p,c.c_size_t,c.c_void_p,c.c_void_p];_k.UpdateProcThreadAttribute.restype=w.BOOL
_k.DeleteProcThreadAttributeList.argtypes=[c.c_void_p]
_k.CreateProcessW.argtypes=[w.LPCWSTR,w.LPWSTR,c.c_void_p,c.c_void_p,w.BOOL,w.DWORD,c.c_void_p,w.LPCWSTR,c.c_void_p,c.POINTER(_PI)];_k.CreateProcessW.restype=w.BOOL
_k.VirtualQueryEx.argtypes=[w.HANDLE,c.c_void_p,c.POINTER(_MBI),c.c_size_t];_k.VirtualQueryEx.restype=c.c_size_t
_k.TerminateProcess.argtypes=[w.HANDLE,w.UINT];_k.TerminateProcess.restype=w.BOOL
_k.WaitForSingleObject.argtypes=[w.HANDLE,w.DWORD];_k.WaitForSingleObject.restype=w.DWORD
_k.CloseHandle.argtypes=[w.HANDLE];_k.CloseHandle.restype=w.BOOL
_k.ResumeThread.argtypes=[w.HANDLE];_k.ResumeThread.restype=w.DWORD
_k.GetExitCodeProcess.argtypes=[w.HANDLE,c.POINTER(w.DWORD)];_k.GetExitCodeProcess.restype=w.BOOL
_k.GetCurrentProcess.restype=w.HANDLE
_k.DuplicateHandle.argtypes=[w.HANDLE,w.HANDLE,w.HANDLE,c.POINTER(w.HANDLE),w.DWORD,w.BOOL,w.DWORD];_k.DuplicateHandle.restype=w.BOOL

def _check(ok):
 if not ok:raise c.WinError(c.get_last_error())
def _duplicate(file):
 result=w.HANDLE();this=_k.GetCurrentProcess()
 _check(_k.DuplicateHandle(this,msvcrt.get_osfhandle(file.fileno()),this,c.byref(result),0,True,2))
 return result

def _conflicts(handle):
 at=0x1000000000;end=0xfc00000000;rows=[]
 while at<end:
  m=_MBI();_check(_k.VirtualQueryEx(handle,at,c.byref(m),c.sizeof(m)))
  if not m.RegionSize:raise RuntimeError('VirtualQueryEx returned an empty region')
  if m.State!=0x10000:rows.append(dict(base=hex(m.BaseAddress or 0),size=hex(m.RegionSize),state=hex(m.State)))
  at=(m.BaseAddress or 0)+m.RegionSize
 return rows

class Popen:
 """Small Popen-compatible interface for this specific Uncharted runtime and file logging."""
 def __init__(self,args,*,cwd=None,env=None,stdout=None,stderr=subprocess.STDOUT):
  if not args or Path(args[0]).resolve()!=_RUNTIME.resolve():raise ValueError('This launcher is restricted to the configured Uncharted runtime')
  if stdout is None or stderr!=subprocess.STDOUT:raise ValueError('stdout must be a log file and stderr must be STDOUT')
  self.args=list(map(str,args));self.pid=None;self.returncode=None;self._handle=None
  self.layout_policy=hex(_POLICY);self.initial_address_conflicts=[]
  si=_SIX();si.StartupInfo.cb=c.sizeof(si);si.StartupInfo.dwFlags=0x100
  size=c.c_size_t();attr=None;initialized=False;pi=_PI();out_handle=None;in_handle=None
  try:
   out_handle=_duplicate(stdout)
   with open(os.devnull,'rb') as null:in_handle=_duplicate(null)
   si.StartupInfo.hStdInput=in_handle;si.StartupInfo.hStdOutput=out_handle;si.StartupInfo.hStdError=out_handle
   _k.InitializeProcThreadAttributeList(None,2,0,c.byref(size));attr=c.create_string_buffer(size.value)
   _check(_k.InitializeProcThreadAttributeList(attr,2,0,c.byref(size)));initialized=True
   policy=c.c_uint64(_POLICY)
   _check(_k.UpdateProcThreadAttribute(attr,0,0x20007,c.byref(policy),c.sizeof(policy),None,None))
   handles=(w.HANDLE*2)(in_handle.value,out_handle.value)
   _check(_k.UpdateProcThreadAttribute(attr,0,0x20002,handles,c.sizeof(handles),None,None))
   si.lpAttributeList=c.cast(attr,c.c_void_p)
   values=os.environ if env is None else env
   pairs=[]
   for key in sorted(values,key=str.upper):
    value=str(values[key])
    if not key or '\0' in key or '=' in key or '\0' in value:raise ValueError('Invalid environment entry')
    pairs.append(str(key)+'='+value)
   block=c.create_unicode_buffer('\0'.join(pairs)+'\0\0')
   cmd=c.create_unicode_buffer(subprocess.list2cmdline(self.args))
   _check(_k.CreateProcessW(str(_RUNTIME),cmd,None,None,True,0x00080000|0x400|4|0x08000000,block,str(cwd) if cwd else None,c.byref(si),c.byref(pi)))
   self.pid=pi.dwProcessId
   self.initial_address_conflicts=_conflicts(pi.hProcess)
   if self.initial_address_conflicts:raise RuntimeError('Child address layout conflicts with PS4 reserve: '+repr(self.initial_address_conflicts))
   if _k.ResumeThread(pi.hThread)==0xffffffff:raise c.WinError(c.get_last_error())
   self._handle=pi.hProcess;pi.hProcess=None
  finally:
   if pi.hThread:_k.CloseHandle(pi.hThread)
   if pi.hProcess:_k.TerminateProcess(pi.hProcess,1);_k.WaitForSingleObject(pi.hProcess,10000);_k.CloseHandle(pi.hProcess)
   if initialized:_k.DeleteProcThreadAttributeList(attr)
   if out_handle:_k.CloseHandle(out_handle)
   if in_handle:_k.CloseHandle(in_handle)
 def poll(self):
  if self._handle is None:return self.returncode
  status=_k.WaitForSingleObject(self._handle,0)
  if status==0:
   code=w.DWORD();_check(_k.GetExitCodeProcess(self._handle,c.byref(code)))
   self.returncode=code.value;self.close()
  elif status!=258:raise c.WinError(c.get_last_error())
  return self.returncode
 def wait(self,timeout=None):
  if self.returncode is not None:return self.returncode
  millis=0xffffffff if timeout is None else min(0xfffffffe,max(0,int(timeout*1000)))
  status=_k.WaitForSingleObject(self._handle,millis)
  if status==258:raise subprocess.TimeoutExpired(self.args,timeout)
  if status!=0:raise c.WinError(c.get_last_error())
  return self.poll()
 def terminate(self):
  if self.poll() is None:_check(_k.TerminateProcess(self._handle,1))
 kill=terminate
 def close(self):
  if self._handle:_k.CloseHandle(self._handle);self._handle=None
 def __enter__(self):return self
 def __exit__(self,*exc):
  if self.poll() is None:self.terminate()
  self.wait()
