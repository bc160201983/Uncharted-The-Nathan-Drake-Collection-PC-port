#include "../u3_lan_profiles.c"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
static ProfileFile disk,tmp;
static size_t disk_size,tmp_size;
static int fail_write,cancelled,dialog_status=2,init_calls,term_calls;
static const uint16_t fixture[]={ 'J','o','n','a','s','.','L','A','N',0 };
static int mock_init(const ImeParam*p,const void*extended){assert(!extended);assert(p->user==1&&p->max_length==15);memcpy(p->buffer,fixture,sizeof fixture);init_calls++;return 0;}
static int mock_status(void){return dialog_status;}
static int mock_result(uint32_t*r){*r=(uint32_t)cancelled;return 0;}
static int mock_term(void){term_calls++;return 0;}
static int64_t mock_open(const char*p,int flags,int mode){(void)p;(void)mode;if(flags==0)return disk_size?10:-2;assert(flags==0x601);tmp_size=0;return 11;}
static int64_t mock_read(int fd,void*p,uint64_t n){assert(fd==10);if(n>disk_size)n=disk_size;memcpy(p,&disk,(size_t)n);return (int64_t)n;}
static int64_t mock_write(int fd,const void*p,uint64_t n){assert(fd==11);if(fail_write)return -5;if(n>23)n=23;memcpy((char*)&tmp+tmp_size,p,(size_t)n);tmp_size+=(size_t)n;return (int64_t)n;}
static int64_t mock_close(int fd){assert(fd==10||fd==11);return 0;}
static int64_t mock_rename(const char*a,const char*b){assert(strstr(a,".tmp")&&!strstr(b,".tmp"));disk=tmp;disk_size=tmp_size;return 0;}
static ProfileState state(void){ProfileState s={0};s.init=(uintptr_t)mock_init;s.status=(uintptr_t)mock_status;s.result=(uintptr_t)mock_result;s.term=(uintptr_t)mock_term;s.open=(uintptr_t)mock_open;s.read=(uintptr_t)mock_read;s.write=(uintptr_t)mock_write;s.close=(uintptr_t)mock_close;s.rename=(uintptr_t)mock_rename;return s;}

static uint32_t pages_seen[8];static unsigned pages_seen_count,module_loaded,loader_calls,pool_calls;static int loader_fail;
static int mock_show(void*menu,uint32_t page,int replace,int silent){(void)menu;(void)silent;assert(!replace);assert(pages_seen_count<8);pages_seen[pages_seen_count++]=page;return 1;}
static void **mock_lookup(uint32_t hash){static void*available=(void*)1;assert(hash==0xbd3bb3b9);return module_loaded?&available:0;}
static void mock_loader(uint32_t hash,uint32_t*tag){assert(hash==0x4240ef2e&&*tag==0x12);loader_calls++;if(!loader_fail)module_loaded=1;}
static int mock_pool(uintptr_t base){assert(base);pool_calls++;return 1;}
static void startup_tests(void){
 ProfileState s=state();s.initialized=1;s.profiles.magic=MAGIC;s.profiles.version=1;s.profiles.count=1;s.profiles.selected=1;strcpy(s.profiles.names[0],"Johnny");s.phase=2;
 s.show_page=(uintptr_t)mock_show;s.lookup=(uintptr_t)mock_lookup;s.loader=(uintptr_t)mock_loader;s.pool_init=(uintptr_t)mock_pool;
 uint8_t *image=calloc(1,0x17c0000);assert(image);uintptr_t base=(uintptr_t)image;
 _Alignas(8) int32_t event[48]={6};
 u3_profile_event(base,event,1,&s);assert(s.startup_phase==1&&pages_seen_count==1&&pages_seen[0]==0x9e1863b1);
 *(uint32_t*)(image+0x17bd080+8)=7;u3_profile_event(base,event,3,&s);assert(loader_calls==0);
 *(uint32_t*)(image+0x17bd080+8)=12;u3_profile_event(base,event,3,&s);assert(loader_calls==1&&pool_calls==1&&s.startup_phase==2);
 u3_profile_event(base,event,3,&s);assert(s.startup_phase==3&&pages_seen_count==3&&pages_seen[1]==0x4608c1ad&&pages_seen[2]==0x53cbb5f2&&s.phase==0);
 s.startup_phase=1;module_loaded=0;loader_fail=1;u3_profile_event(base,event,3,&s);assert(s.startup_error==2&&s.startup_phase==4&&pages_seen_count==3);loader_fail=0;
 uint8_t source[32]={0};*(uint32_t*)(source+0x1c)=0x4c414eb3;
 memset(event,0,sizeof event);*(void**)((char*)event+0x98)=source;event[0]=2;assert(u3_profile_event(base,event,2,&s));assert(!strcmp(*(char**)((char*)event+0x40),"Custom Game"));
 event[0x15]=1;u3_profile_event(base,event,2,&s);assert(!strcmp(*(char**)((char*)event+0x40),""));event[0x15]=0;
  event[0]=6;u3_profile_event(base,event,2,&s);assert(event[1]==5);
 event[0]=11;event[1]=4;event[14]=0;u3_profile_event(base,event,2,&s);assert(event[14]==1);event[1]=6;u3_profile_event(base,event,2,&s);assert(event[14]==0);
 *(uint32_t*)(source+0x1c)=0x4c414ead;event[0]=2;event[1]=0;u3_profile_event(base,event,2,&s);assert(!strcmp(*(char**)((char*)event+0x40),"Johnny"));
 _Alignas(8) uint8_t page_data[149*0x80]={0},items[4*0x68]={0};struct{uint32_t count,reserved;void*items;} header={149,0,page_data},collection={4,0,items};
 *(uint32_t*)(page_data+8)=0x53cbb5f2;*(void**)(page_data+0x48)=&collection;
 *(uint32_t*)(items+8)=0xf75ffe57;*(uint32_t*)(items+0x30)=21;*(uint32_t*)(items+0x34)=0xb3;
 *(uint32_t*)(items+2*0x68+8)=0x3406420c;*(uint32_t*)(items+2*0x68+0x30)=21;*(uint32_t*)(items+2*0x68+0x34)=0xad;
 assert(u3_profile_event(base,(int32_t*)&header,8,&s));assert(*(uint32_t*)(items+0x34)==0xcf&&*(uint32_t*)(items+0x40)==0x4c414eb3);assert(*(uint32_t*)(items+2*0x68+0x40)==0x4c414ead);
 assert(u3_profile_event(base,(int32_t*)&header,8,&s));
 uint32_t *n=(uint32_t*)(image+0x17be5dc);uint8_t *registry=image+0x17be5e0;
 assert(!register_lobby_click(base)&&*n==0);
 *n=1;*(uint32_t*)registry=0xbe55365f;*(uintptr_t*)(registry+8)=base+0x672c60;
 assert(register_lobby_click(base)&&*n==2&&*(uint32_t*)(registry+24)==0xf75ffe57);
 assert(register_lobby_click(base)&&*n==2);
 *(uintptr_t*)(registry+32)=1;assert(!register_lobby_click(base)&&*n==2);
 *(uint32_t*)(registry+24)=0;*n=128;assert(!register_lobby_click(base)&&*n==128);
 uint8_t lobby_page[32]={0};*(uint32_t*)(lobby_page+16)=0x53cbb5f2;
 *(uint32_t*)(image+0x17bd080+0xdb0)=0;*(void**)(image+0x17bd080+0x8b0)=lobby_page;
 event[0]=0;event[1]=4;u3_profile_event(base,event,0,&s);assert(pages_seen_count==4&&pages_seen[3]==0xc7597732&&s.reserved==5);
 event[1]=0;u3_profile_event(base,event,0,&s);assert(pages_seen_count==4&&s.reserved==1);
 free(image);
 puts("PASS: guarded/idempotent lobby click registration; Options routes without a session backend; unsupported actions do not invent sessions");
 puts("PASS: selected profile -> native Connecting -> real resource/pool work -> lobby; failure stays out of lobby; tagged native menu/name adapters");
}

int main(void){
 _Alignas(8) int32_t event[48]={0};ProfileState s=state();
 dialog_status=1;assert(u3_profile_event(0,event,0,&s)==1);assert(s.phase==1&&disk_size==0);
 dialog_status=2;event[0]=6;assert(u3_profile_event(0,event,1,&s)==1);
 assert(s.profiles.count==1&&s.profiles.selected==1&&s.phase==2&&disk_size==528);
 assert(!strcmp(s.profiles.names[0],"Player*LAN")&&term_calls==1);
 memset(event,0,sizeof event);event[0]=6;u3_profile_event(0,event,2,&s);assert(event[1]==1);
 memset(event,0,sizeof event);event[0]=2;event[1]=1;assert(u3_profile_event(0,event,2,&s)==1);assert(!strcmp(*(char**)((char*)event+0x40),"Player*LAN"));
 ProfileState reloaded=state();memset(event,0,sizeof event);event[0]=6;u3_profile_event(0,event,2,&reloaded);assert(reloaded.profiles.count==1&&event[1]==1);
 memset(event,0,sizeof event);u3_profile_event(0,event,0,&s);assert(s.profiles.count==1); /* duplicate selects existing */
 cancelled=1;u3_profile_event(0,event,0,&s);assert(s.profiles.count==1&&s.phase==0);cancelled=0;
 ProfileFile before=disk;fail_write=1;u3_profile_event(0,event,0,&s);assert(s.error==4&&!memcmp(&disk,&before,sizeof disk));fail_write=0;
 disk.version=99;ProfileState corrupt=state();int prior=init_calls;u3_profile_event(0,event,0,&corrupt);assert(corrupt.error==1&&init_calls==prior&&disk.version==99);
 uint16_t bad[]={0xdc00,0};char name[64];assert(!u3_profile_name(bad,name));uint16_t blank[]={' ',' ',0};assert(!u3_profile_name(blank,name));
 startup_tests();
 puts("PASS: IME lifecycle, partial-write save, normalization, native list text/count, reload, duplicate handling, cancel, write failure and corrupt-store preservation");
}