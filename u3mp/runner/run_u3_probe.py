import argparse,hashlib,json,os,shutil,subprocess,time
from u3_child_process import Popen
from pathlib import Path
INSTALL=Path(os.environ.get('UNCHARTED_PC_DIR') or Path(__file__).resolve().parents[2])  # the PC port install (repo root by default)
GAME=Path(os.environ.get('UNCHARTED_GAME_DIR','.'))  # your dumped PS4 game folder (contains eboot.bin and u3data/)
ROOT=Path(__file__).resolve().parent.parent
import sys;sys.path.insert(0,str(ROOT/'helper'))  # build_u3_*.py live in u3mp/helper
p=argparse.ArgumentParser(description='Isolated U3 diagnostic, not playable multiplayer. Never modifies originals.')
p.add_argument('--mode',choices=['baseline','natural','bootstrap','bootstrap-early','bootstrap-pools','menu-entry','menu-callback','menu-flow','lan-profiles'],default='baseline')
p.add_argument('--seconds',type=int,default=35)
p.add_argument('--pulse-cross',action='store_true')
p.add_argument('--input-plan',type=Path,help='JSON list of seconds/button/duration_ms input actions')
p.add_argument('--overlay',type=Path,action='append',default=[],help='Read-only app0 overlay directory for this test')
p.add_argument('--capture-at',type=float,action='append',help='Capture time in seconds (repeatable)')
p.add_argument('--prepared-image',type=Path,help='Use a frozen prepared image instead of rebuilding a diagnostic')
p.add_argument('--expected-sha256',help='Required digest for a frozen prepared image')
p.add_argument('--exclusive-script',action='store_true',help='Override physical pad input with neutral replay during an automated test')
p.add_argument('--profile-store',type=Path,help='Persistent local LAN profile file for this run')
p.add_argument('--ime-text',help='Automated name-entry fixture, never set by the manual launcher')
p.add_argument('--nav',type=Path,help='JSON list of page-aware steps: {page,timeout}|{button,duration_ms,wait}|{capture}|{sleep}|{end}; needs --net-trace')
p.add_argument('--watch',help='Comma-separated image offsets for GOW3_EXEC_WATCH (overrides mode default)')
p.add_argument('--file-trace',default='dc1',help='GOW3_FILE_TRACE path substring')
p.add_argument('--net-trace',action='store_true',help='Log net state globals to net-trace.jsonl every 0.1s')
p.add_argument('--inspect-pages',action='store_true',help='Dump the active menu page controls to page-*.json when it changes')
p.add_argument('--trace-state',help='Hex image offset of the helper ProfileState to trace (frozen builds)')
p.add_argument('--peek',action='append',default=[],help='name=OFF[>OFF...]:SIZE guest memory watch; OFF image-relative hex, each > derefs a u64 and adds the next offset; logs peek.jsonl')
p.add_argument('--windowed',action='store_true',help='Use a private 1280x720 windowed configuration')
a=p.parse_args()
if a.mode=='lan-profiles' and not a.prepared_image:p.error('LAN profiles require a frozen prepared input')
if a.prepared_image and a.mode not in ('baseline','lan-profiles'):p.error('frozen image must use baseline mode, without further patches')
if a.expected_sha256 and not a.prepared_image:p.error('expected digest requires a frozen image')
if a.seconds!=0 and not 5<=a.seconds<=120:p.error('seconds must be 0 (until closed) or 5..120')
plan=[]
if a.pulse_cross:plan=[dict(seconds=t,button='cross',duration_ms=150) for t in [12,20,28]]
if a.input_plan:
 if a.pulse_cross:p.error('choose --input-plan or --pulse-cross')
 plan=json.loads(a.input_plan.read_text(encoding='utf-8'))
 allowed={'cross','circle','square','triangle','up','down','left','right','options','l1','r1'}
 if not isinstance(plan,list):p.error('input plan must be a JSON list')
 for action in plan:
  if not isinstance(action,dict) or action.get('button') not in allowed:p.error('invalid input action/button')
  if not isinstance(action.get('seconds'),(int,float)) or action['seconds']<0:p.error('invalid input timing')
  if not 20<=action.get('duration_ms',150)<=3000:p.error('input pulse must be 20..3000ms')
 plan.sort(key=lambda item:item['seconds'])
 for previous,current in zip(plan,plan[1:]):
  if previous['seconds']+previous.get('duration_ms',150)/1000>=current['seconds']:p.error('input pulses overlap')
overlays=[]
for overlay in a.overlay:
 if not overlay.is_dir():p.error('overlay directory is missing: '+str(overlay))
 overlays.append(overlay.resolve())
out=ROOT/'work/runtime-tests'/(a.mode+'-'+str(time.time_ns()));out.mkdir(parents=True)
(out/'user').mkdir();(out/'dumps').mkdir();(out/'pad.txt').write_text('')
prepared=INSTALL/'analysis/big3-ps4_Shipping'
profile=INSTALL/'analysis/uncharted-user'
if (profile/'savedata').exists():shutil.copytree(profile/'savedata',out/'user/savedata')
if (profile/'probe-u3.ini').exists():shutil.copyfile(profile/'probe-u3.ini',out/'user/probe-u3.ini')
if a.windowed:
 config=out/'user/probe-u3.ini'
 lines=config.read_text().splitlines() if config.exists() else []
 lines=[line for line in lines if line.split('=',1)[0].strip() not in ('display_mode','output_res')]
 config.write_text('\n'.join(lines+['display_mode=windowed','output_res=1280x720'])+'\n')
if a.profile_store and a.profile_store.exists():
 (out/'user/data').mkdir(exist_ok=True)
 shutil.copyfile(a.profile_store,out/'user/data/u3-lan-profiles.v1')
image_source=a.prepared_image.resolve() if a.prepared_image else prepared/'boot-linked.bin'
d=bytearray(image_source.read_bytes());source_sha=hashlib.sha256(d).hexdigest()
if a.expected_sha256 and source_sha!=a.expected_sha256.lower():raise ValueError('Frozen test image digest mismatch; refusing to launch')
if a.prepared_image:
 from build_u3_bootstrap_probe import parse
 parse(d)
if a.mode=='lan-profiles':
 import build_u3_profile_probe
 built,manifest=build_u3_profile_probe.build(bytes(d),(ROOT/'work/u3_lan_profiles.bin').read_bytes());d=bytearray(built)
 (out/'patch-manifest.json').write_text(json.dumps(manifest,indent=2))
old=bytes.fromhex('498b87b0000000488b80c00000008b7818c745dc8fb4516e')
if a.mode in ('menu-entry','menu-callback','menu-flow'):
 import build_u3_menu_probe
 built,manifest=build_u3_menu_probe.build(bytes(d),(ROOT/'work/u3_menu_entry.bin').read_bytes(),callback=a.mode!='menu-entry',flow_blob=(ROOT/'work/u3_menu_flow.bin').read_bytes() if a.mode=='menu-flow' else None);d=bytearray(built)
 (out/'patch-manifest.json').write_text(json.dumps(manifest,indent=2))
if a.mode in ('bootstrap','bootstrap-early','bootstrap-pools'):
 import build_u3_bootstrap_probe
 blob=(ROOT/'work/u3_pool_init.bin').read_bytes() if a.mode=='bootstrap-pools' else None
 built,manifest,stub=build_u3_bootstrap_probe.build(bytes(d),entry="early" if a.mode in ('bootstrap-early','bootstrap-pools') else "natural",pool_blob=blob);d=bytearray(built)
 # build() returns the whole container, matching baseline d.
 (out/'patch-manifest.json').write_text(json.dumps(manifest,indent=2))
if a.mode=='natural':
 if d.count(old)!=1:raise ValueError('Refusing incompatible image: signature not unique')
 site=d.index(old);d[site:site+17]=bytes.fromhex('bf67c9b3b7')+b'\x90'*12
(out/'boot-linked.bin').write_bytes(d);shutil.copyfile(prepared/'content.bin',out/'content.bin')
env={k:v for k,v in os.environ.items() if not k.startswith(('GOW3_','UNCHARTED_'))}
env.update(GOW3_CONFIG=str(out/'user/probe-u3.ini'),GOW3_PAD_FILE=str(out/'pad.txt'),GOW3_DRAW_PIPE='0',GOW3_VK_RECORD_THREAD='0',GOW3_NATIVE_TSC='1',GOW3_ASYNC_READBACK='1',GOW3_COPY64_EOP_READBACK='1',GOW3_TINY_IMAGE_READBACK='1',GOW3_MIP_TARGETS='1',GOW3_FILE_TRACE=a.file_trace,GOW3_EXEC_WATCH='66d8c0,99610b',GOW3_EXEC_WATCH_SECONDS=str(a.seconds or 600),GOW3_FRAME_DUMP_TRIGGER=str(out/'capture.trigger'),GOW3_DUMP_DIR=str(out/'dumps'))
if a.ime_text is not None:env['GOW3_IME_TEXT']=a.ime_text
if a.mode=='lan-profiles':env['GOW3_EXEC_WATCH']='672c60,651940'
if a.watch:env['GOW3_EXEC_WATCH']=a.watch
if a.exclusive_script:
 if (not plan and not a.nav) or not a.seconds:p.error('exclusive scripted input requires a timed input plan')
 (out/'neutral-replay.txt').write_text('0 0 128 128 128 128 0 0\n10000000 0 128 128 128 128 0 0\n')
 (out/'pad.txt').write_text('replay')
 env['GOW3_PAD_REPLAY']=str(out/'neutral-replay.txt')
if a.mode=='bootstrap-pools':env['GOW3_EXEC_WATCH']='205005e,73e6ef,678e84,66d8c0'
if a.mode in ('menu-entry','menu-callback'):env['GOW3_EXEC_WATCH']='6496ec,6496f1,66e500,651940'
if a.mode=='menu-flow':env['GOW3_EXEC_WATCH']='66e500,66e510,651940,671d00'
cmd=[str(INSTALL/'gow3_pc/out/Uncharted.exe'),str(out/'boot-linked.bin'),'--no-game-hooks','--low-image','--content-profile',str(out/'content.bin'),'--app0',str(GAME),'--user',str(out/'user'),'--guest-path','eboot.bin','--guest-arg','ispartofuccollection']
for overlay in overlays:cmd.extend(['--app0-overlay',str(overlay)])
r=dict(mode=a.mode,image_source=str(image_source),source_sha256=source_sha,patched_sha256=hashlib.sha256(d).hexdigest(),command=cmd,environment={k:v for k,v in env.items() if k.startswith('GOW3_')},actions=[],claim='Diagnostic only; no multiplayer or transport success implied')
print(out,flush=True);start=time.monotonic();timeout=False;pulses=list(plan);captures=sorted(a.capture_at or [10,24]);release=None

nav=json.loads(a.nav.read_text(encoding='utf-8')) if a.nav else [];nav_i=0;nav_next=0;nav_deadline=None;cur_page=None;r_nav=[]
net_last=None;peek_next=0;peek_last=None;trace_base=None;trace_last=None;trace_next=0;inspect_next=0;inspect_last=None;inspect_force=0
if a.mode=='lan-profiles' or a.trace_state:
 import ctypes as ct,struct,re
 trace_kernel=ct.WinDLL('kernel32',use_last_error=True)
 trace_kernel.ReadProcessMemory.argtypes=[ct.c_void_p,ct.c_void_p,ct.c_void_p,ct.c_size_t,ct.POINTER(ct.c_size_t)]
 trace_kernel.ReadProcessMemory.restype=ct.c_int
 trace_address=int(a.trace_state,16) if a.trace_state else int(manifest['state'],16)
 def trace_read(handle,address,size):
  b=ct.create_string_buffer(size);got=ct.c_size_t()
  if not trace_kernel.ReadProcessMemory(handle,address,b,size,ct.byref(got)):return None
  return b.raw

with (out/'boot.log').open('wb') as log:
 proc=Popen(cmd,cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT);r['pid']=proc.pid;r['child_layout_policy']=proc.layout_policy;r['initial_address_conflicts']=proc.initial_address_conflicts
 try:
  while proc.poll() is None:
   t=time.monotonic()-start

   if a.net_trace and trace_base is not None:
    try:
     g=lambda off,n:trace_read(proc._handle,trace_base+off,n)
     rd=lambda off,f:struct.unpack('<'+f,g(off,struct.calcsize(f)))[0]
     depth=rd(0x17bd080+0xdb0,'I');top=0
     if depth<160:
      pg=rd(0x17bd080+0x8b0+depth*8,'Q');pb=trace_read(proc._handle,pg,0x14) if pg else None;top=struct.unpack_from('<I',pb,0x10)[0] if pb else 0
     cur_page=top
     lp=rd(0x17cb3e0,'Q');hb=g(trace_address+1900,36)
     cnt=struct.unpack_from('<I',hb,0)[0];ring=hb[4:];hist=[ring[i%32] for i in range(max(0,cnt-32),cnt)]
     lm=g(0x1afcf10,0x48);levels=[]
     try:
      if lm:
       arr0=struct.unpack_from('<Q',lm,0)[0];bits0=struct.unpack_from('<4Q',lm,8)
       levels.append(dict(kind='hdr',want=struct.unpack_from('<I',lm,0x28)[0],actors=struct.unpack_from('<I',lm,0x38)[0],bits=[hex(x) for x in bits0]))
       wn=struct.unpack_from('<I',lm,0x28)[0];wp=struct.unpack_from('<Q',lm,0x30)[0]
       for i in range(min(wn,48)):
        eb=trace_read(proc._handle,wp+i*0x18,0x18)
        if eb:
         np_=struct.unpack_from('<Q',eb,0)[0];nb=trace_read(proc._handle,np_,64) if np_ else None
         levels.append(dict(kind='want',name=(nb.split(b'\0')[0].decode(errors='replace') if nb else None),id=hex(struct.unpack_from('<I',eb,8)[0]),flags=hex(eb[0xe])))
       for w in range(4):
        for b in range(64):
         if bits0[w]>>b&1 and arr0:
          sb=trace_read(proc._handle,arr0+(w*64+b)*0x250,0x100)
          if sb:
           pk=[]
           if struct.unpack_from('<H',sb,0x30)[0] in (1,2,3):
            pc=struct.unpack_from('<i',sb,0x78)[0]
            for k in range(max(0,min(pc,12))):
             pp=struct.unpack_from('<Q',sb,0x80+8*k)[0]
             pb=trace_read(proc._handle,pp,0x80) if pp else None
             if pb:
              import re as _re3
              pk.append(dict(state=struct.unpack_from('<I',pb,0x20)[0],txt=[m.decode() for m in _re3.findall(rb'[a-z0-9][a-z0-9_\-/]{4,60}',pb)][:2]))
           nm=sb[0x34:0x74].split(b'\0')[0].decode(errors='replace')
           levels.append(dict(kind='slot',slot=w*64+b,name=nm,state=struct.unpack_from('<H',sb,0x30)[0],sub=struct.unpack_from('<H',sb,0x32)[0],flags=hex(struct.unpack_from('<H',sb,0)[0]),pk=pk))
     except Exception as e:levels.append(dict(kind='err',err=repr(e)))
     if lm:
      arr=struct.unpack_from('<Q',lm,0)[0];bits=struct.unpack_from('<4Q',lm,8);rc=struct.unpack_from('<I',lm,0x28)[0];rl=struct.unpack_from('<Q',lm,0x30)[0]
      wc=struct.unpack_from('<I',lm,0x38)[0];wl=struct.unpack_from('<Q',lm,0x40)[0]
      slots={}
      for w in range(4):
       for b in range(64):
        if bits[w]>>b&1 and arr:
         sb=g(arr+(w*64+b)*0x250,0x250)
         if sb:
          import re as _re
          txt=[m.decode() for m in _re.findall(rb'[a-z0-9][a-z0-9_\-]{4,60}',sb)][:2]
          slots[struct.unpack_from('<I',sb,0x74)[0]]=dict(slot=w*64+b,type=struct.unpack_from('<h',sb,0)[0],state=struct.unpack_from('<I',sb,0x30)[0],text=txt)
      for lst,cnt,kind in ((rl,rc,'req'),(wl,wc,'want')):
       for i in range(min(cnt,24)):
        eb=g(lst+i*0x18,0x18)
        if eb:
         lid=struct.unpack_from('<I',eb,8)[0];levels.append(dict(kind=kind,id=hex(lid),flags=eb[0xe],**slots.get(lid,{})))
     icon_diag=None
     try:
      ic=g(trace_address+0x2300,52)
      if ic:icon_diag=[hex(x) for x in struct.unpack_from('<13I',ic,0)]
     except Exception as e:icon_diag=repr(e)
     emblem_diag=None
     try:
      eg=g(trace_address+0x2400,22*8)
      if eg:emblem_diag=[hex(x) for x in struct.unpack_from('<22Q',eg,0)]
     except Exception as e:emblem_diag=repr(e)
     spawn_gate=None
     try:
      if lp:
       pb=trace_read(proc._handle,lp,0x9c0)
       if pb:
        B=lambda o:pb[o];I=lambda o:struct.unpack_from('<i',pb,o)[0];Q=lambda o:struct.unpack_from('<Q',pb,o)[0]
        spawn_gate=dict(obj=hex(Q(0x10)),objid=I(0x18),active_e1=B(0xe1),id_1e4=hex(I(0x1e4)),team=I(0x21c),t39c=I(0x39c),t3a0=I(0x3a0),t3a8=I(0x3a8),req_420=B(0x420),leave_5e4=B(0x5e4),f5ec=B(0x5ec),loaded_5ed=B(0x5ed),f5ee=B(0x5ee),f5f9=B(0x5f9),f5fa=B(0x5fa),f5fd=B(0x5fd),f600=I(0x600),f770=B(0x770),p430=B(0x430),g1a84=rd(0x17d1a84,'B'),g1a91=rd(0x17d1a91,'B'),g7b05=rd(0x17d7b05,'B'))
       spawn_gate['screen0']=hex(rd(0x17cb3d0,'Q'));spawn_gate['screen1']=hex(rd(0x17cb3d8,'Q'));spawn_gate['host']=hex(rd(0x17cb3e0,'Q'))
       pls=[]
       for i in range(3):
        pp=g(0x17cb3f0+i*0x9c0,0x9c0)
        if pp:pls.append(dict(i=i,act=pp[0xe1],id=hex(struct.unpack_from('<I',pp,0x1e4)[0]),scr=struct.unpack_from('<I',pp,0x1e8)[0],bot=pp[0x1ec],leave=pp[0x5e4],req=pp[0x420]))
       spawn_gate['players']=pls
       dc=g(trace_address+0x2200,15*16)
       if dc:spawn_gate['chain']=[list(struct.unpack_from('<Iii',dc,16*i)) for i in range(15)]
     except Exception as e:spawn_gate=repr(e)
     netgame=None
     try:
      ga=struct.unpack_from('<Q',g(trace_address+1280+208,8),0)[0]
      if ga:
       import re as _re2
       cnt=struct.unpack_from('<I',g(ga,4),0)[0];arrp=struct.unpack_from('<Q',g(ga+8,8),0)[0];sel=rd(0x17d167c,'h')
       ent=g(arrp+sel*0xd0,0xd0);netgame=dict(count=cnt,sel=sel,type=struct.unpack_from('<I',ent,0xc)[0],fields={})
       for off in range(0,0xd0,8):
        v=struct.unpack_from('<Q',ent,off)[0]
        if 0x10000<v<0x7fffffffffff:
         b1=g(v,0x80)
         if not b1:continue
         st=[m.decode() for m in _re2.findall(rb'[a-z][a-z0-9_\-]{3,60}',b1)][:3]
         sub=[]
         for o2 in range(0,0x40,8):
          v2=struct.unpack_from('<Q',b1,o2)[0]
          if 0x10000<v2<0x7fffffffffff:
           b2=g(v2,0x80)
           if b2:
            sub+=[m.decode() for m in _re2.findall(rb'[a-z][a-z0-9_\-]{3,60}',b2)][:3]
            for o3 in range(0,0x40,8):
             v3=struct.unpack_from('<Q',b2,o3)[0]
             if 0x10000<v3<0x7fffffffffff:
              b3=g(v3,0x40)
              if b3:sub+=[m.decode() for m in _re2.findall(rb'[a-z][a-z0-9_\-]{3,60}',b3)][:2]
         if st or sub:netgame['fields']['+%x'%off]=dict(str=st,deeper=sub[:12])
     except Exception as e:netgame=repr(e)
     snap=dict(net_state=rd(0x1b68810,'I'),net_error=rd(0x17d5c80,'B'),leave_flag=rd(0x17c3868,'B'),relaunch=rd(0x17d7758,'B'),local_player=hex(lp),local_player_5e4=spawn_gate,session_ready=rd(0x17d64c1,'B'),session_result=rd(0x17d64c4,'i'),mp_active=rd(0x17d5c61,'B'),page=hex(top),depth=depth,slots=hist,levels=levels,netgame=netgame,icon=icon_diag,emblem=emblem_diag)
     if snap!=net_last:
      net_last=snap
      with (out/'net-trace.jsonl').open('a') as f:f.write(json.dumps(dict(seconds=round(t,2),**snap))+'\n')
    except Exception as e:(out/'net-trace-error.txt').write_text(repr(e))
   if a.inspect_pages and trace_base is not None and t>=inspect_next:
    inspect_next=t+0.5
    try:
     menu=trace_base+0x17bd080
     raw=trace_read(proc._handle,menu+0xdb0,4)
     depth=struct.unpack('<I',raw)[0] if raw else 999
     if depth<160:
      pg=struct.unpack('<Q',trace_read(proc._handle,menu+0x8b0+depth*8,8))[0]
      head=trace_read(proc._handle,pg,0x100) if pg else None
      if head:
       pid=struct.unpack_from('<I',head,0x10)[0];n=struct.unpack_from('<I',head,0xac)[0];cp=struct.unpack_from('<Q',head,0xb0)[0]
       key=(pid,n)
       if key!=inspect_last or t>=inspect_force:
        inspect_last=key;inspect_force=t+20
        ctl=[]
        if cp and n<64:
         ptrs=trace_read(proc._handle,cp,8*n) or b''
         for i in range(len(ptrs)//8):
          c=struct.unpack_from('<Q',ptrs,i*8)[0]
          cr=trace_read(proc._handle,c,0x200) if c else None
          if cr:
           src=struct.unpack_from('<Q',cr,0x18)[0]
           sr=trace_read(proc._handle,src,0x40) if src else None
           ctl.append(dict(index=i,vtable=hex(struct.unpack_from('<Q',cr,0)[0]-trace_base),id=hex(struct.unpack_from('<I',cr,0x10)[0]),focus=cr[8],flags=hex(struct.unpack_from('<I',cr,0xa8)[0]),raw=cr.hex(),source_vtable=hex(struct.unpack_from('<Q',sr,0)[0]-trace_base) if sr else None,source_raw=sr.hex() if sr else None))
        (out/('page-%08x-%d.json'%(pid,int(t)))).write_text(json.dumps(dict(seconds=round(t,1),page=hex(pid),depth=depth,active=head[8],count=n,head=head.hex(),controls=ctl),indent=1))
    except Exception as e:
     (out/'inspect-error.txt').write_text(repr(e))
   if (a.mode=='lan-profiles' or a.trace_state) and t>=trace_next:
    trace_next=t+1
    if trace_base is None:
     found=re.search(r'Guest image base: ([0-9a-fA-F]+)',(out/'boot.log').read_text(errors='replace'))
     if found:trace_base=int(found[1],16)
    if trace_base is not None:
     state_bytes=trace_read(proc._handle,trace_base+trace_address,1536)
     menu_bytes=trace_read(proc._handle,trace_base+0x17bd088,4)
     if state_bytes and menu_bytes:
      phase=struct.unpack_from('<4I',state_bytes,72);startup=struct.unpack_from('<4I',state_bytes,680)
      snapshot=dict(phase=phase[1],profile_error=phase[2],lobby_action=phase[3],startup_phase=startup[0],startup_error=startup[1],startup_ticks=startup[2],lobby_selected=startup[3],menu_state=struct.unpack('<I',menu_bytes)[0])
      extra=struct.unpack_from('<10I',state_bytes,696);snapshot.update(focus_done=extra[0],focus_moves=extra[2],k6_calls=min(extra[4],1),k6_last=extra[5],focus_tries=min(extra[6],1),focus_fail=extra[7],lobby_row=struct.unpack_from('<I',state_bytes,84)[0],lobby_error=extra[8])
      pr=state_bytes[1280:1280+208] if len(state_bytes)>=1488 else b''
      if len(pr)==208 and struct.unpack_from('<I',pr,0)[0]==0x424f5250:
       v=struct.unpack_from('<16I',pr,0);txt=lambda o:pr[o:o+32].split(b'\0')[0].decode('utf-8','replace')
       snapshot.update(probe=dict(nmh_count=v[2],selected_netgame=v[3],mode=v[4],lan=v[5],flag65=v[6],gametype_max=v[7],gametype_sel=v[8],map_max=v[9],map_sel=v[10],game_array=v[11],game_count=v[12],name_array=v[13],name_count=v[14],party=v[15],gametype0=txt(64),gametype1=txt(96),map0=txt(128)))
       w=struct.unpack_from('<8I',pr,160);snapshot['probe'].update(time_seconds=round(struct.unpack_from('<f',pr,160)[0],2),score_raw=w[1],start_calls=w[2],create_calls=w[3],backend_calls=min(w[4],1),last_slot=w[5],net_state=w[6],session_ready=w[7])
       w=struct.unpack_from('<3I',pr,192);snapshot['probe'].update(allocators='%08x%08x'%(w[1],w[0]),network_alias=w[2])
      if snapshot!=trace_last:
       with (out/'restoration-state.jsonl').open('a') as trace_log:trace_log.write(json.dumps(dict(seconds=round(t,3),**snapshot))+'\n')
       trace_last=snapshot

   if a.peek and trace_base is not None and t>=peek_next:
    peek_next=t+0.5;psnap={}
    for spec in a.peek:
     try:
      pname,rest=spec.split('=',1);pexpr,psize=rest.rsplit(':',1);toks=pexpr.split('>');paddr=trace_base+int(toks[0],16)
      for tk in toks[1:]:
       pb=trace_read(proc._handle,paddr,8);paddr=(struct.unpack('<Q',pb)[0] if pb else 0)+int(tk,16)
       if paddr<0x10000:break
      pb=trace_read(proc._handle,paddr,int(psize,0)) if paddr>=0x10000 else None
      psnap[pname]=pb.hex() if pb else None
     except Exception as pe:psnap[spec]=repr(pe)
    if psnap!=peek_last:
     with (out/'peek.jsonl').open('a') as pf:pf.write(json.dumps(dict(seconds=round(t,2),**psnap))+'\n')
     peek_last=psnap
   if a.seconds and t>=a.seconds:timeout=True;proc.terminate();break
   if nav_i<len(nav) and t>=nav_next:
    st=nav[nav_i]
    if 'if_page' in st and cur_page!=int(st['if_page'],16):r_nav.append(dict(step=nav_i,skipped=st['if_page'],t=round(t,2),page=hex(cur_page or 0)));nav_i+=1;nav_next=t
    elif 'page' in st:
     if nav_deadline is None:nav_deadline=t+st.get('timeout',30)
     if cur_page==int(st['page'],16):r_nav.append(dict(step=nav_i,page=st['page'],t=round(t,2)));nav_i+=1;nav_deadline=None;nav_next=t+st.get('settle',1.0)
     elif t>nav_deadline:r_nav.append(dict(step=nav_i,page=st['page'],t=round(t,2),failed=True,current=hex(cur_page or 0)));nav_i=len(nav)
    elif 'button' in st and release is None:
     dur=st.get('duration_ms',150);(out/'pad.txt').write_text(st['button']);release=t+dur/1000
     r_nav.append(dict(step=nav_i,button=st['button'],t=round(t,2),page=hex(cur_page or 0)));nav_i+=1;nav_next=t+dur/1000+st.get('wait',0.8)
    elif 'capture' in st:
     (out/'capture.trigger').touch();r_nav.append(dict(step=nav_i,capture=st['capture'],t=round(t,2),page=hex(cur_page or 0)));nav_i+=1;nav_next=t+1.5
    elif 'sleep' in st:nav_i+=1;nav_next=t+st['sleep']
    elif 'end' in st:r_nav.append(dict(step=nav_i,end=True,t=round(t,2)));proc.terminate();break
   if release is not None and t>=release:(out/'pad.txt').write_text('');release=None
   if pulses and t>=pulses[0]['seconds']:
    action=pulses.pop(0);duration=action.get('duration_ms',150)
    (out/'pad.txt').write_text(action['button']);release=t+duration/1000
    r['actions'].append(dict(seconds=action['seconds'],actual_seconds=round(t,3),pad=action['button'],duration_ms=duration))
   if captures and t>=captures[0]:captures.pop(0);(out/'capture.trigger').touch()
   time.sleep(.05 if a.net_trace else .1)
  proc.wait(timeout=10)
 finally:
  (out/'pad.txt').write_text('')
  if proc.poll() is None:proc.kill();proc.wait()
if a.profile_store and (out/'user/data/u3-lan-profiles.v1').exists():
 import struct
 data=(out/'user/data/u3-lan-profiles.v1').read_bytes()
 valid=len(data)==528
 if valid:
  magic,version,count,selected=struct.unpack_from('<4I',data)
  valid=magic==0x504c3355 and version==1 and count<=8 and selected<=count
  for i in range(min(count,8)):
   name=data[16+i*64:16+(i+1)*64]
   if not name[0] or b'\0' not in name:valid=False
   try:name.split(b'\0',1)[0].decode('utf-8')
   except UnicodeDecodeError:valid=False
 if valid:
  a.profile_store.parent.mkdir(parents=True,exist_ok=True)
  temporary=a.profile_store.with_suffix(a.profile_store.suffix+'.tmp');temporary.write_bytes(data);temporary.replace(a.profile_store)
  r['profile_store']=str(a.profile_store.resolve());r['profile_count']=count
 else:r['profile_store_error']='invalid profile file; persistent copy left unchanged'
r.update(exit_code=proc.returncode,timed_out=timeout,elapsed_seconds=round(time.monotonic()-start,3))
r['nav']=r_nav
(out/'result.json').write_text(json.dumps(r,indent=2));print(json.dumps(dict(directory=str(out),exit_code=proc.returncode,timed_out=timeout)),flush=True)
