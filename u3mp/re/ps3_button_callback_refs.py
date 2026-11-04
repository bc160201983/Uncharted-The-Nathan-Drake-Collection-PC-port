import os
import sys,pathlib,json
old=pathlib.Path((os.environ.get('U3MP_PROJECT_DIR','.')+'/reference/previous-project'))
sys.path.insert(0,str(old/'tools'))
import ps3_string_xrefs as p
elf=p.ELFFile((os.environ.get('U3_PS3_GAME_DIR','.')+"/PS3_GAME/USRDIR/EBOOT.elf"));elf.load()
rows=p.segment_rows(elf);targets={0x1178d70,0x1178d60,0x1178d68};slots=p.find_pointer_slots(rows,targets,'>')
funcs=p.load_functions(old/'analysis/ps3-u3-1.19/functions.json');tocs,freq=p.collect_tocs(elf,rows,'>')
x=p.symbolic_xrefs(rows,funcs,tocs,freq,set(slots)|targets,slots,targets,True,'>')
result={hex(k):v for k,v in x.items()}
pathlib.Path('outputs/evidence/ps3-entry-callback-descriptor-refs.json').write_text(json.dumps(result,indent=2))
print(json.dumps(result,indent=2))
