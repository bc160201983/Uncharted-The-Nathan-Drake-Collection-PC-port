import os
import sys,pathlib,json
old=pathlib.Path((os.environ.get('U3MP_PROJECT_DIR','.')+'/reference/previous-project'))
sys.path.insert(0,str(old/'tools'))
import ps3_string_xrefs as p
elf=p.ELFFile(str(pathlib.Path((os.environ.get('U3_PS3_GAME_DIR','.')+"/PS3_GAME/USRDIR/EBOOT.elf"))));elf.load()
rows=p.segment_rows(elf);targets={0x122ec60};slots=p.find_pointer_slots(rows,targets,'>');functions=p.load_functions(old/'analysis/ps3-u3-1.19/functions.json');tocs,freq=p.collect_tocs(elf,rows,'>')
x=p.symbolic_xrefs(rows,functions,tocs,freq,set(slots)|targets,slots,targets,True,'>')
result={'targets':[hex(t) for t in targets],'pointer_slots':{hex(k):[hex(t) for t in v] for k,v in slots.items()},'xrefs':{hex(k):v for k,v in x.items()}}
pathlib.Path('work/ps3-session-global-xrefs.json').write_text(json.dumps(result,indent=2))
print(json.dumps({'functions':sorted({r['function'] for a in x.values() for r in a}),'count':sum(len(a) for a in x.values())},indent=2))
