#include "runtime_uc2mp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void preparation(void) {
    Uc2mpProfile a,b,c; Uc2mpLobby l;
    assert(uc2mp_profile(&a,1,"Host")==UC2MP_OK && uc2mp_profile(&b,2,"Client")==UC2MP_OK);
    assert(uc2mp_profile(&c,3,"Late")==UC2MP_OK);
    assert(uc2mp_init(&l,&a)==UC2MP_OK && uc2mp_open(&l)==UC2MP_OK && uc2mp_join(&l,&b)==UC2MP_OK);
    Uc2mpSettings settings={UINT32_C(0xd3b86d60),UINT32_C(0xdbaad5e4),10, 1, 1, 0, 0};
    assert(uc2mp_prepare_begin(&l,2,100,120000)==UC2MP_NOT_HOST);
    /* A new lobby already holds a full match setup (deathmatch, first map), as on PS3. */
    Uc2mpSettings defaults=UC2MP_SETTINGS_DEFAULT(UC2MP_MAX_MEMBERS);
    assert(uc2mp_settings_equal(l.settings,defaults));
    assert(uc2mp_prepare_begin(&l,1,100,120000)==UC2MP_NOT_READY);
    assert(uc2mp_configure(&l,1,settings)==UC2MP_OK);
    assert(uc2mp_prepare_begin(&l,1,100,120000)==UC2MP_NOT_READY);
    assert(uc2mp_ready(&l,1,1)==UC2MP_OK && uc2mp_ready(&l,2,1)==UC2MP_OK);
    assert(uc2mp_prepare_begin(&l,1,100,120001)==UC2MP_INVALID);
    assert(uc2mp_prepare_begin(&l,1,UINT64_MAX,1)==UC2MP_INVALID);
    assert(uc2mp_prepare_begin(&l,1,100,120000)==UC2MP_OK);
    uint64_t id=l.preparation.id, deadline=l.preparation.deadline_ms;
    assert(id && deadline==120100);
    assert(uc2mp_prepare_begin(&l,1,100,100)==UC2MP_BUSY);
    assert(uc2mp_ready(&l,2,0)==UC2MP_BUSY && uc2mp_configure(&l,1,settings)==UC2MP_BUSY);
    assert(uc2mp_join(&l,&c)==UC2MP_BUSY && uc2mp_rename(&l,"Changed")==UC2MP_BUSY);
    assert(uc2mp_prepare_load(&l,1,id,200)==UC2MP_NOT_READY);
    assert(uc2mp_prepare_ack(&l,2,id+1,UC2MP_PHASE_PREPARING)==UC2MP_INVALID);
    assert(uc2mp_prepare_ack(&l,3,id,UC2MP_PHASE_PREPARING)==UC2MP_NOT_FOUND);
    assert(uc2mp_prepare_ack(&l,1,id,UC2MP_PHASE_PREPARING)==UC2MP_OK);
    uint64_t revision=l.revision;
    assert(uc2mp_prepare_ack(&l,1,id,UC2MP_PHASE_PREPARING)==UC2MP_OK && l.revision==revision);
    assert(uc2mp_prepare_load(&l,1,id,200)==UC2MP_NOT_READY);
    assert(uc2mp_prepare_ack(&l,2,id,UC2MP_PHASE_PREPARING)==UC2MP_OK);
    assert(uc2mp_prepare_load(&l,2,id,200)==UC2MP_NOT_HOST);
    assert(uc2mp_prepare_load(&l,1,id,200)==UC2MP_OK && l.preparation.deadline_ms==deadline);
    assert(uc2mp_start(&l,1)==UC2MP_UNSUPPORTED);
    assert(uc2mp_prepare_ack(&l,2,id,UC2MP_PHASE_PREPARING)==UC2MP_INVALID);
    assert(uc2mp_prepare_ack(&l,1,id,UC2MP_PHASE_LOADING)==UC2MP_OK);
    assert(uc2mp_prepare_ack(&l,2,id,UC2MP_PHASE_LOADING)==UC2MP_OK);
    assert(l.preparation.loading==3 && !uc2mp_prepare_expire(&l,deadline-1));
    assert(uc2mp_prepare_expire(&l,deadline) && l.preparation.phase==UC2MP_PHASE_LOBBY);
    assert(l.preparation.id==id && !l.members[0].ready && !l.members[1].ready);
    assert(uc2mp_ready(&l,1,1)==UC2MP_OK && uc2mp_ready(&l,2,1)==UC2MP_OK);
    assert(uc2mp_prepare_begin(&l,1,deadline,1000)==UC2MP_OK && l.preparation.id==id+1);
    assert(uc2mp_prepare_cancel(&l,2,id+1)==UC2MP_NOT_HOST);
    assert(uc2mp_prepare_cancel(&l,1,id)==UC2MP_INVALID);
    assert(uc2mp_leave(&l,2)==UC2MP_OK && l.preparation.phase==UC2MP_PHASE_LOBBY);
    assert(!l.members[0].ready && l.count==1);
    assert(uc2mp_close(&l)==UC2MP_OK && uc2mp_open(&l)==UC2MP_OK && l.preparation.id==id+1);
    assert(uc2mp_configure(&l,1,settings)==UC2MP_OK && uc2mp_ready(&l,1,1)==UC2MP_OK);
    assert(uc2mp_prepare_begin(&l,1,0,10)==UC2MP_OK && l.preparation.id==id+2);
    assert(uc2mp_prepare_ack(&l,1,id+2,UC2MP_PHASE_PREPARING)==UC2MP_OK);
    assert(uc2mp_prepare_load(&l,1,id+2,10)==UC2MP_NOT_READY && l.preparation.phase==UC2MP_PHASE_LOBBY);
}

int main(void) {
    preparation();
    Uc2mpProfile local;
    assert(uc2mp_profile(&local, 1, "Tony") == UC2MP_OK);
    Uc2mpLobby lobby;
    assert(uc2mp_init(&lobby, &local) == UC2MP_OK);
    assert(!uc2mp_member(&lobby, 0));
    assert(uc2mp_start(&lobby, 1) == UC2MP_CLOSED);
    for (unsigned cycle = 0; cycle < 50; ++cycle) {
        assert(uc2mp_open(&lobby) == UC2MP_OK);
        assert(lobby.generation == cycle + 1 && lobby.count == 1 && lobby.host_id == 1);
        uint64_t revision = lobby.revision;
        assert(uc2mp_open(&lobby) == UC2MP_OK && lobby.revision == revision);
        assert(!uc2mp_member(&lobby, 1) && !uc2mp_member(&lobby, UINT32_MAX));
        assert(uc2mp_join(&lobby, &lobby.local) == UC2MP_OK && lobby.count == 1);
        Uc2mpProfile other;
        for (unsigned i = 2; i <= UC2MP_MAX_MEMBERS; ++i) {
            assert(uc2mp_profile(&other, i, "Test") == UC2MP_OK);
            assert(uc2mp_join(&lobby, &other) == UC2MP_OK);
        }
        assert(uc2mp_profile(&other, 20, "Test") == UC2MP_OK);
        Uc2mpLobby before = lobby;
        assert(uc2mp_join(&lobby, &other) == UC2MP_FULL);
        assert(!memcmp(&before, &lobby, sizeof(lobby)));
        assert(uc2mp_configure(&lobby, 2, (Uc2mpSettings){1, 2, 10, 1, 1, 0, 0}) == UC2MP_NOT_HOST);
        assert(uc2mp_configure(&lobby, 1, (Uc2mpSettings){1, 2, 9, 1, 1, 0, 0}) == UC2MP_INVALID);
        assert(uc2mp_ready(&lobby, 1, 1) == UC2MP_OK);
        assert(uc2mp_configure(&lobby, 1, (Uc2mpSettings){1, 2, 10, 1, 1, 0, 0}) == UC2MP_OK);
        assert(!uc2mp_member(&lobby, 0)->ready);
        assert(uc2mp_leave(&lobby, 5) == UC2MP_OK);
        assert(lobby.count == 9 && uc2mp_member(&lobby, 4)->profile.id == 6);
        assert(uc2mp_leave(&lobby, 5) == UC2MP_NOT_FOUND);
        assert(uc2mp_rename(&lobby, "Ton\xc3\xa9") == UC2MP_OK);
        assert(!strcmp(lobby.local.name, uc2mp_member(&lobby, 0)->profile.name));
        assert(uc2mp_start(&lobby, 1) == UC2MP_UNSUPPORTED);
        assert(uc2mp_leave(&lobby, 1) == UC2MP_OK);
        assert(!lobby.open && !lobby.count && !lobby.host_id);
        assert(!uc2mp_member(&lobby, 0));
        assert(uc2mp_close(&lobby) == UC2MP_OK);
    }
    const char *invalid[] = {"", "   ", "bad\nname", "\xc0\xaf", "\xed\xa0\x80",
                             "\xf4\x90\x80\x80", "\xe2\x82", "\xc2\x85",
                             "12345678901234567890123456789012"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        Uc2mpLobby before = lobby;
        assert(uc2mp_rename(&lobby, invalid[i]) == UC2MP_INVALID);
        assert(!memcmp(&before, &lobby, sizeof(lobby)));
    }
    assert(uc2mp_profile(&local, 0, "Tony") == UC2MP_INVALID);
    assert(uc2mp_open(NULL) == UC2MP_INVALID);
    assert(uc2mp_configure(NULL, 1, (Uc2mpSettings){0}) == UC2MP_INVALID);
    puts("UC2MP: profile validation, roster, host controls and 50 lobby lifecycles passed");
    puts("UC2MP: preparation validation, frozen settings, ACKs, cancel and fixed timeout passed");
    return 0;
}
