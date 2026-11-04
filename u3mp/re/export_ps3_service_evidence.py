import os
from pathlib import Path
import struct,json,hashlib
old=Path((os.environ.get('U3MP_PROJECT_DIR','.')+'/reference/previous-project'));r=Path.cwd();d=(old/'analysis/ps3-u3-1.19/eboot-image.bin').read_bytes();u=lambda a:struct.unpack_from('>I',d,a)[0];t=u(0x11ba558)
meta={'ps3_memory_image_sha256':hashlib.sha256(d).hexdigest(),'main_toc':'0x11c2070','frontend_secondary_toc':hex(t),'globals':{name:hex(u(t+off)) for name,off in [('net_info',-0x7e08),('match_session',-0x7d80),('party_session',-0x7de0),('backend_active',-0x7afc),('backend_online',-0x7b04),('backend_lan',-0x7b00)]},'netinit':'0x2901e0','netinit_worker':'0x290ca0','worker_function_descriptor':'0x1178d80','backends':json.loads((r/'work/ps3-backend-vtables.json').read_text()),'binding':{'wrapper':'0x374100','field_offset':4,'online_slot_0xc':'0x371ebc','lan_slot_0xc':'0x370fe4','max_sessions':4},'affected_method':{'ps4_vtable_offset':'0x58','ps3_vtable_offset':'0x2c','ps3_wrapper':'0x3745c4','online_implementation':'0x3726ac','lan_implementation':'0x370088','wire_message_type':'0x140','wire_message_bytes':16,'session_room_id_offset':'0x10','session_flags_offset':'0x1f4','session_dirty_offset':'0xe0','name_inferred':'SetRoomFlags'}}
meta['backends'][0].update({'allocation_bytes':0x24d60,'constructor':'0x373ed8','init':'0x371a98','transport':'TCP custom matchmaking','allocation_tag':27})
meta['backends'][1].update({'allocation_bytes':0x261e0,'constructor':'0x371920','init':'0x370f98','transport':'UDP local discovery/session','udp_bind_port':0x2334,'allocation_tag':27})
(r/'outputs/evidence/ps3-network-service-bootstrap.json').write_text(json.dumps(meta,indent=2))
parts=['PS3 reference service construction and method semantics. Fresh Ghidra 12.2 read-only decompilation.\n\n']
for f in ['ps3-session-backend-constructors.c','ps3-backend-method-semantics.c','ps3-backend-methods-extra.c','ps3-service-backend-candidate.c','ps3-backend-lifecycle.c']:
 s=(r/'work'/f).read_text()
 if f=='ps3-backend-lifecycle.c':s=s[:s.index('========== 0028cb80')]
 if f=='ps3-service-backend-candidate.c':s=s[:s.index('========== 003233ec')]
 parts.append('SOURCE '+f+'\n'+s+'\n')
(r/'outputs/evidence/ps3-network-service-bootstrap.c').write_text(''.join(parts))
(r/'outputs/evidence/ps3-network-service-bootstrap-worker.c').write_text((r/'work/ps3-netinit-worker.c').read_text().split('========== 00373f00')[0])
(r/'outputs/evidence/ps3-network-service-bootstrap-shutdown.c').write_text((r/'work/ps3-backend-lifecycle.c').read_text().split('========== 0028cb80')[1])
print(json.dumps(meta['globals'],indent=2))
