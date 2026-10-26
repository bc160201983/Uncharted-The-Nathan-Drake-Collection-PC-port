import os
import pathlib
p=pathlib.Path((os.environ.get('U3MP_PROJECT_DIR','.')+'/reference/previous-project/analysis/ghidra/ps4-u3-bd3bb3b9-users.c'));t=p.read_text();t=t[t.index('void FUN_0064ce90'):];t=t[:t.find('==========')];ls=t.splitlines()
for a,z in [(670,740),(940,1000),(1450,1600)]:
 print('LINES',a,z);print('\n'.join(ls[a:z]))
