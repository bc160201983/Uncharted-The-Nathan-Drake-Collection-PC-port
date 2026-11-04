/* System services contracts: dialogs (progress bars run until closed), PlayGo, trophies. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_services.c"
#include <assert.h>

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) { (void)table; (void)count; (void)nid; return 0; }
const char *runtime_symbol(const char *nid) { return nid; }
int gow3gpu_text_input_begin(const char *text, const char *prompt) { (void)text; (void)prompt; return 0; }
int gow3gpu_text_input_poll(char *text, uint64_t size) { (void)text; (void)size; return 0; }
int64_t runtime_file_open(const char *path, int flags, int mode) { (void)path; (void)flags; (void)mode; return -1; }
int64_t runtime_file_read(int fd, void *buffer, uint64_t size) { (void)fd; (void)buffer; (void)size; return -1; }
int64_t runtime_file_close(int fd) { (void)fd; return 0; }
static int choice_next=-1;
int gow3gpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus) { (void)title; (void)items; (void)images; (void)focus; return count>0; }
int gow3gpu_choice_poll(void) { return choice_next; }
int runtime_savedata_param(int32_t user, const char *dir, void *param) { (void)user; (void)dir; (void)param; return -1; }
int runtime_savedata_icon(int32_t user, const char *dir, char *out, size_t size) { (void)user; (void)dir; (void)out; (void)size; return -1; }
static int trophy_lists_shown, trophies_shown, trophy_last_shown=-1;
void gow3gpu_trophy_list(const char *pack, int list, const uint32_t earned[4]) { (void)pack; (void)list; (void)earned; ++trophy_lists_shown; }
void gow3gpu_trophy_earned(int id) { ++trophies_shown; trophy_last_shown=id; }
static char test_user[64];
const char *runtime_file_user_dir(void) { return test_user; }
int runtime_file_translate(const char *guest, char *out, size_t size) { snprintf(out,size,"%s",guest); return 0; }

int main(void) {
    unsigned char param[128]={0}, result[64]={0}; /* the game zeroes the pointers it does not want */
    uint32_t value;

    /* MsgDialog: a user message finishes with OK; a progress bar runs until closed. */
    assert(msg_init()==0);
    memset(param+56,0,4); param[56]=1;
    assert(msg_open(param)==0 && msg_status()==DIALOG_FINISHED);
    assert(msg_result(result)==0);
    memcpy(&value,result+4,4); assert(value==1);
    param[56]=2;
    assert(msg_open(param)==0 && msg_status()==DIALOG_RUNNING);
    assert(msg_result(result)==DIALOG_NOT_FINISHED);
    assert(msg_progress(0,50)==0 && msg_progress(1,50)==DIALOG_PARAM_INVALID);
    assert(msg_close()==0 && msg_status()==DIALOG_FINISHED && msg_close()==DIALOG_NOT_RUNNING);
    assert(msg_progress(0,100)==DIALOG_NOT_RUNNING);
    assert(msg_result(NULL)==DIALOG_ARG_NULL);
    assert(msg_term()==0 && msg_open(NULL)==DIALOG_ARG_NULL);

    /* SaveDataDialog: mode at offset 52 (5 = progress bar); result after the mode field. */
    assert(save_init()==0 && save_ready()==1);
    param[52]=5;
    assert(save_open(param)==0 && save_status()==DIALOG_RUNNING && save_progress(0,10)==0);
    assert(save_close()==0 && save_result(result)==0);
    memcpy(&value,result+8,4); assert(value==0); /* closed by the game: no button */
    param[52]=2;
    assert(save_open(param)==0 && save_status()==DIALOG_FINISHED && save_result(result)==0);
    memcpy(&value,result+8,4); assert(value==1);
    assert(save_term()==0);

    /* Save list: the player's choice comes back as the save directory. */
    char names[2][32]={"SAVE0001","SAVE0002"}, chosen[32];
    unsigned char items[64]={0}, list[128]={0}, out[64]={0};
    const char *names_ptr=names[0]; uint32_t name_count=2; const char *new_title="New";
    memcpy(items+16,&names_ptr,8); memcpy(items+24,&name_count,4); memcpy(items+32,&new_title,8);
    unsigned char *items_ptr=items; char *chosen_ptr=chosen;
    list[52]=1; list[56]=2; memcpy(list+72,&items_ptr,8);
    memcpy(out+16,&chosen_ptr,8);
    assert(save_init()==0 && save_open(list)==0 && save_status()==DIALOG_RUNNING);
    choice_next=2; /* entries: New save, SAVE0001, SAVE0002 */
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0 && !strcmp(chosen,"SAVE0002"));
    memcpy(&value,out+4,4); assert(value==0);
    memcpy(&value,out+8,4); assert(value==1);
    assert(save_open(list)==0);
    choice_next=0;
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0 && chosen[0]==0); /* new save */
    assert(save_open(list)==0);
    choice_next=-2;
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0);
    memcpy(&value,out+4,4); assert(value==SAVE_RESULT_CANCELED);
    assert(save_term()==0);

    /* PlayGo: installed, full speed, progress never divides by zero. */
    int32_t handle=0; uint64_t progress[2]; int32_t speed; uint16_t chunk=0;
    assert(playgo_init(NULL)==0 && playgo_open(&handle,NULL)==0);
    assert(playgo_get_speed(handle,&speed)==0 && speed==2);
    assert(playgo_progress(handle,&chunk,1,progress)==0 && progress[0]==progress[1] && progress[1]);
    assert(playgo_progress(handle,&chunk,0,progress)==PLAYGO_BAD_SIZE);
    assert(playgo_todo(handle,&chunk,1)==0 && playgo_todo(handle,NULL,1)==PLAYGO_BAD_POINTER);
    uint64_t mask=0;
    assert(playgo_get_language(handle,&mask)==0 && mask==(UINT64_C(1)<<62));
    assert(playgo_set_language(handle,UINT64_C(1)<<55)==0);
    assert(playgo_get_language(handle,&mask)==0 && mask==(UINT64_C(1)<<55));
    assert(playgo_get_language(handle,NULL)==PLAYGO_BAD_POINTER);
    int64_t eta=-1;
    assert(playgo_eta(handle,&chunk,1,&eta)==0 && eta==0);
    uint16_t invalid_chunk=100;
    eta=42;
    assert(playgo_eta(handle,&invalid_chunk,1,&eta)==PLAYGO_BAD_CHUNK && eta==42);
    assert(playgo_prefetch(handle,&chunk,1,3)==0);
    assert(playgo_prefetch(handle,&invalid_chunk,1,3)==PLAYGO_BAD_CHUNK);
    uint32_t pending=7; unsigned char untouched[4]={1,2,3,4};
    assert(playgo_get_todo(handle,untouched,1,&pending)==0 && pending==0 && untouched[0]==1);
    assert(playgo_get_todo(handle,untouched,0,&pending)==PLAYGO_BAD_SIZE);
    assert(playgo_close(handle)==0 && playgo_close(handle)==PLAYGO_BAD_HANDLE && playgo_terminate()==0);

    /* Trophies: none until earned; an earned one is saved, reported back and shown once. */
    snprintf(test_user,sizeof(test_user),"services-test-user-%d",(int)getpid());
    mkdir(test_user,0755);
    uint32_t flags[4]={~0u,~0u,~0u,~0u}, count=7;
    assert(trophy_unlock_state(1,1,flags,&count)==0 && !flags[0] && !flags[3] && count==0);
    assert(trophy_unlock_state(1,1,NULL,&count)==TROPHY_INVALID && trophy_release(0)==TROPHY_INVALID);
    int32_t trophy_ctx=0, trophy_hd=0, platinum=0;
    assert(trophy_context(&trophy_ctx,1,2,0)==0 && trophy_handle(&trophy_hd)==0);
    assert(trophy_register(trophy_ctx,trophy_hd,0)==0 && trophy_lists_shown==1);
    assert(trophy_unlock(trophy_ctx,trophy_hd,5,&platinum)==0 && platinum==-1 && trophies_shown==1 && trophy_last_shown==5);
    assert(trophy_unlock(trophy_ctx,trophy_hd,5,&platinum)==0 && trophies_shown==1);
    assert(trophy_unlock(trophy_ctx,trophy_hd,128,&platinum)==TROPHY_INVALID);
    assert(trophy_unlock_state(trophy_ctx,trophy_hd,flags,&count)==0 && flags[0]==1u<<5 && !flags[1] && count==1);
    /* The next start reads it back. */
    memset(trophy_flags,0,sizeof(trophy_flags)); memset(trophy_loaded,0,sizeof(trophy_loaded));
    memset(trophy_contexts,0,sizeof(trophy_contexts));
    assert(trophy_context(&trophy_ctx,1,2,0)==0);
    assert(trophy_unlock_state(trophy_ctx,trophy_hd,flags,&count)==0 && flags[0]==1u<<5 && count==1);
    char trophy_path[128];
    trophy_file(2,trophy_path,sizeof(trophy_path)); remove(trophy_path);
    snprintf(trophy_path,sizeof(trophy_path),"%s/trophy",test_user); rmdir(trophy_path); rmdir(test_user);

    float ratio=0; assert(safe_area(&ratio)==0 && ratio==1.0f && safe_area(NULL)==SYSTEM_PARAMETER);
    puts("services test: ok");
    return 0;
}
