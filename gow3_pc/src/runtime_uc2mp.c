#include "runtime_uc2mp.h"
#include "runtime_uc2mp_catalog.h"
#include <string.h>

/* Reject truncation, controls, invalid UTF-8, overlong encodings, surrogates
 * and code points outside Unicode before changing any existing state. */
static int valid_name(const char *name) {
    if (!name) return 0;
    size_t n = 0;
    while (n < UC2MP_NAME_BYTES && name[n]) ++n;
    if (!n || n == UC2MP_NAME_BYTES) return 0;
    int visible = 0;
    for (size_t i = 0; i < n;) {
        unsigned c = (unsigned char)name[i++], code, extra, minimum;
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f) return 0;
            if (c != ' ') visible = 1;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) { code = c & 31; extra = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { code = c & 15; extra = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { code = c & 7; extra = 3; minimum = 0x10000; }
        else return 0;
        if (i + extra > n) return 0;
        while (extra--) {
            unsigned next = (unsigned char)name[i++];
            if ((next & 0xc0) != 0x80) return 0;
            code = (code << 6) | (next & 63);
        }
        if (code < minimum || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff) ||
            (code >= 0x80 && code <= 0x9f)) return 0;
        visible = 1;
    }
    return visible;
}

Uc2mpResult uc2mp_profile(Uc2mpProfile *out, uint64_t id, const char *name) {
    if (!out || !id || !valid_name(name)) return UC2MP_INVALID;
    Uc2mpProfile next = {0};
    next.id = id;
    memcpy(next.name, name, strlen(name) + 1);
    *out = next;
    return UC2MP_OK;
}

Uc2mpResult uc2mp_init(Uc2mpLobby *lobby, const Uc2mpProfile *local) {
    if (!lobby || !local) return UC2MP_INVALID;
    Uc2mpProfile checked;
    if (uc2mp_profile(&checked, local->id, local->name) != UC2MP_OK) return UC2MP_INVALID;
    memset(lobby, 0, sizeof(*lobby));
    lobby->local = checked;
    lobby->settings = UC2MP_SETTINGS_DEFAULT(UC2MP_MAX_MEMBERS);
    return UC2MP_OK;
}

Uc2mpResult uc2mp_open(Uc2mpLobby *lobby) {
    if (!lobby || !lobby->local.id) return UC2MP_INVALID;
    if (lobby->open) return UC2MP_OK; /* repeated entry cannot duplicate the host */
    memset(lobby->members, 0, sizeof(lobby->members));
    lobby->settings = UC2MP_SETTINGS_DEFAULT(UC2MP_MAX_MEMBERS);
    lobby->members[0].profile = lobby->local;
    lobby->host_id = lobby->local.id;
    lobby->count = 1;
    lobby->open = 1;
    ++lobby->generation;
    ++lobby->revision;
    return UC2MP_OK;
}

Uc2mpResult uc2mp_close(Uc2mpLobby *lobby) {
    if (!lobby || !lobby->local.id) return UC2MP_INVALID;
    if (!lobby->open) return UC2MP_OK;
    memset(lobby->members, 0, sizeof(lobby->members));
    lobby->settings = UC2MP_SETTINGS_DEFAULT(UC2MP_MAX_MEMBERS);
    lobby->count = 0;
    lobby->host_id = 0;
    lobby->open = 0;
    lobby->preparation = (Uc2mpPreparation){.id=lobby->preparation.id};
    ++lobby->revision;
    return UC2MP_OK;
}

static int member_index(const Uc2mpLobby *lobby, uint64_t id) {
    for (unsigned i = 0; i < lobby->count; ++i)
        if (lobby->members[i].profile.id == id) return (int)i;
    return -1;
}

Uc2mpResult uc2mp_rename(Uc2mpLobby *lobby, const char *name) {
    if (!lobby || !lobby->local.id) return UC2MP_INVALID;
    if (lobby->preparation.phase!=UC2MP_PHASE_LOBBY) return UC2MP_BUSY;
    Uc2mpProfile next;
    if (uc2mp_profile(&next, lobby->local.id, name) != UC2MP_OK) return UC2MP_INVALID;
    if (!strcmp(lobby->local.name, next.name)) return UC2MP_OK;
    lobby->local = next;
    int index = member_index(lobby, next.id);
    if (index >= 0) lobby->members[index].profile = next;
    ++lobby->revision;
    return UC2MP_OK;
}

Uc2mpResult uc2mp_configure(Uc2mpLobby *lobby, uint64_t actor, Uc2mpSettings settings) {
    if (!lobby) return UC2MP_INVALID;
    if (!lobby->open) return UC2MP_CLOSED;
    if (actor != lobby->host_id) return UC2MP_NOT_HOST;
    if (lobby->preparation.phase!=UC2MP_PHASE_LOBBY) return UC2MP_BUSY;
    if (!settings.capacity || settings.capacity > UC2MP_MAX_MEMBERS ||
        settings.capacity < lobby->count || !uc2mp_rules_valid(settings)) return UC2MP_INVALID;
    if (uc2mp_settings_equal(lobby->settings, settings)) return UC2MP_OK;
    lobby->settings = settings;
    for (unsigned i = 0; i < lobby->count; ++i) lobby->members[i].ready = 0;
    ++lobby->revision;
    return UC2MP_OK;
}

Uc2mpResult uc2mp_join(Uc2mpLobby *lobby, const Uc2mpProfile *profile) {
    if (!lobby || !profile) return UC2MP_INVALID;
    if (!lobby->open) return UC2MP_CLOSED;
    Uc2mpProfile checked;
    if (uc2mp_profile(&checked, profile->id, profile->name) != UC2MP_OK) return UC2MP_INVALID;
    int index = member_index(lobby, checked.id);
    if (index >= 0) return !strcmp(lobby->members[index].profile.name, checked.name)
                              ? UC2MP_OK : UC2MP_INVALID;
    if (lobby->preparation.phase!=UC2MP_PHASE_LOBBY) return UC2MP_BUSY;
    if (lobby->count >= lobby->settings.capacity) return UC2MP_FULL;
    lobby->members[lobby->count++] = (Uc2mpMember){checked, 0};
    ++lobby->revision;
    return UC2MP_OK;
}

Uc2mpResult uc2mp_leave(Uc2mpLobby *lobby, uint64_t member) {
    if (!lobby) return UC2MP_INVALID;
    if (!lobby->open) return UC2MP_CLOSED;
    int index = member_index(lobby, member);
    if (index < 0) return UC2MP_NOT_FOUND;
    /* Local-only scope: leaving as host closes the room, no fake migration. */
    if (member == lobby->host_id) return uc2mp_close(lobby);
    if (lobby->preparation.phase!=UC2MP_PHASE_LOBBY)
        (void)uc2mp_prepare_cancel(lobby,lobby->host_id,lobby->preparation.id);
    memmove(&lobby->members[index], &lobby->members[index + 1],
            (lobby->count - (unsigned)index - 1) * sizeof(lobby->members[0]));
    memset(&lobby->members[--lobby->count], 0, sizeof(lobby->members[0]));
    ++lobby->revision;
    return UC2MP_OK;
}

Uc2mpResult uc2mp_ready(Uc2mpLobby *lobby, uint64_t member, int ready) {
    if (!lobby || (ready != 0 && ready != 1)) return UC2MP_INVALID;
    if (!lobby->open) return UC2MP_CLOSED;
    if (lobby->preparation.phase!=UC2MP_PHASE_LOBBY) return UC2MP_BUSY;
    int index = member_index(lobby, member);
    if (index < 0) return UC2MP_NOT_FOUND;
    if (lobby->members[index].ready != ready) {
        lobby->members[index].ready = ready;
        ++lobby->revision;
    }
    return UC2MP_OK;
}

const Uc2mpMember *uc2mp_member(const Uc2mpLobby *lobby, unsigned index) {
    return lobby && lobby->open && index < lobby->count ? &lobby->members[index] : NULL;
}

Uc2mpResult uc2mp_start(const Uc2mpLobby *lobby, uint64_t actor) {
    if (!lobby) return UC2MP_INVALID;
    if (!lobby->open) return UC2MP_CLOSED;
    if (actor != lobby->host_id) return UC2MP_NOT_HOST;
    /* Only a verified map-loading adapter may implement this transition. */
    return UC2MP_UNSUPPORTED;
}

const char *uc2mp_result_text(Uc2mpResult result) {
    static const char *const text[] = {
        "ok", "invalid", "closed", "full", "not found", "not host", "unsupported", "busy", "not ready"
    };
    return (unsigned)result < sizeof(text) / sizeof(*text) ? text[result] : "unknown";
}

int uc2mp_rules_valid(Uc2mpSettings settings) {
    size_t score, time, friendly, rounds;
    uc2mp_rule_choices(60,&score); uc2mp_rule_choices(61,&time);
    uc2mp_rule_choices(62,&friendly); uc2mp_rule_choices(63,&rounds);
    return settings.score_limit<score && settings.time_limit<time &&
        settings.friendly_fire<friendly && settings.rounds<rounds;
}
int uc2mp_settings_equal(Uc2mpSettings a, Uc2mpSettings b) {
    return a.mode_id==b.mode_id && a.map_id==b.map_id && a.capacity==b.capacity &&
        a.score_limit==b.score_limit && a.time_limit==b.time_limit && a.rounds==b.rounds &&
        a.friendly_fire==b.friendly_fire;
}
int uc2mp_match_settings_valid(Uc2mpSettings settings) {
    return settings.mode_id && settings.map_id && settings.capacity &&
        settings.capacity<=UC2MP_MAX_MEMBERS && uc2mp_catalog_id(57,settings.mode_id) &&
        uc2mp_catalog_id(58,settings.map_id) && uc2mp_rules_valid(settings);
}
Uc2mpResult uc2mp_prepare_begin(Uc2mpLobby *b,uint64_t actor,uint64_t now,uint32_t timeout) {
    if (!b || !timeout || timeout>UC2MP_PREPARE_MAX_MS || now>UINT64_MAX-timeout) return UC2MP_INVALID;
    if (!b->open) return UC2MP_CLOSED;
    if (actor!=b->host_id) return UC2MP_NOT_HOST;
    if (b->preparation.phase!=UC2MP_PHASE_LOBBY) return UC2MP_BUSY;
    if (!uc2mp_match_settings_valid(b->settings) || !b->count || b->count>b->settings.capacity ||
        b->preparation.id==UINT64_MAX) return UC2MP_INVALID;
    for (unsigned i=0;i<b->count;++i) if (!b->members[i].ready) return UC2MP_NOT_READY;
    b->preparation=(Uc2mpPreparation){.id=b->preparation.id+1,.deadline_ms=now+timeout,
                                     .phase=UC2MP_PHASE_PREPARING};
    ++b->revision;
    return UC2MP_OK;
}
Uc2mpResult uc2mp_prepare_ack(Uc2mpLobby *b,uint64_t member,uint64_t id,Uc2mpPhase phase) {
    if (!b || !id || (phase!=UC2MP_PHASE_PREPARING && phase!=UC2MP_PHASE_LOADING)) return UC2MP_INVALID;
    if (!b->open) return UC2MP_CLOSED;
    if (id!=b->preparation.id || phase!=b->preparation.phase) return UC2MP_INVALID;
    int index=member_index(b,member);
    if (index<0) return UC2MP_NOT_FOUND;
    uint16_t *mask=phase==UC2MP_PHASE_PREPARING ? &b->preparation.prepared : &b->preparation.loading;
    uint16_t bit=(uint16_t)(1u<<index);
    if (!(*mask&bit)) { *mask|=bit; ++b->revision; }
    return UC2MP_OK;
}
Uc2mpResult uc2mp_prepare_cancel(Uc2mpLobby *b,uint64_t actor,uint64_t id) {
    if (!b || !id) return UC2MP_INVALID;
    if (!b->open) return UC2MP_CLOSED;
    if (actor!=b->host_id) return UC2MP_NOT_HOST;
    if (id!=b->preparation.id) return UC2MP_INVALID;
    if (b->preparation.phase==UC2MP_PHASE_LOBBY) return UC2MP_OK;
    b->preparation=(Uc2mpPreparation){.id=id};
    for (unsigned i=0;i<b->count;++i) b->members[i].ready=0;
    ++b->revision;
    return UC2MP_OK;
}
int uc2mp_prepare_expire(Uc2mpLobby *b,uint64_t now) {
    if (!b || !b->open || b->preparation.phase==UC2MP_PHASE_LOBBY || now<b->preparation.deadline_ms) return 0;
    return uc2mp_prepare_cancel(b,b->host_id,b->preparation.id)==UC2MP_OK;
}
Uc2mpResult uc2mp_prepare_load(Uc2mpLobby *b,uint64_t actor,uint64_t id,uint64_t now) {
    if (!b || !id) return UC2MP_INVALID;
    if (!b->open) return UC2MP_CLOSED;
    if (actor!=b->host_id) return UC2MP_NOT_HOST;
    if (uc2mp_prepare_expire(b,now)) return UC2MP_NOT_READY;
    if (id!=b->preparation.id || b->preparation.phase!=UC2MP_PHASE_PREPARING) return UC2MP_INVALID;
    if (b->preparation.prepared!=(uint16_t)((1u<<b->count)-1)) return UC2MP_NOT_READY;
    b->preparation.phase=UC2MP_PHASE_LOADING;
    ++b->revision;
    return UC2MP_OK;
}
