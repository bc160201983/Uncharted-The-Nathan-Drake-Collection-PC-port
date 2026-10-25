#ifndef RUNTIME_UC2MP_H
#define RUNTIME_UC2MP_H
#include <stddef.h>
#include <stdint.h>

/* Host-owned data, NOT the PS4 object ABI. The menu adapter must copy values;
 * never give these addresses to guest player/session methods. All access to an
 * instance belongs to one thread (the menu thread once an adapter is installed). */
#define UC2MP_MAX_MEMBERS 10
#define UC2MP_NAME_BYTES 32
typedef enum {
    UC2MP_OK, UC2MP_INVALID, UC2MP_CLOSED, UC2MP_FULL,
    UC2MP_NOT_FOUND, UC2MP_NOT_HOST, UC2MP_UNSUPPORTED, UC2MP_BUSY, UC2MP_NOT_READY
} Uc2mpResult;
typedef struct {
    uint64_t id;
    char name[UC2MP_NAME_BYTES]; /* NUL-terminated UTF-8 */
} Uc2mpProfile;
typedef struct {
    Uc2mpProfile profile;
    int ready;
} Uc2mpMember;
typedef struct {
    uint32_t mode_id, map_id; /* opaque IDs; zero means not selected */
    unsigned capacity;
    /* Match rules: indices into runtime_uc2mp_catalog.h's choice lists. */
    uint8_t score_limit, time_limit, rounds, friendly_fire;
} Uc2mpSettings;
/* The PS3 custom game's opening choice: deathmatch on the catalogue's first map (village),
 * 50 kills, 20 minutes, 1 round, friendly fire off. */
#define UC2MP_SETTINGS_DEFAULT(capacity_) \
    ((Uc2mpSettings){UINT32_C(0xd3b86d60), UINT32_C(0xdbaad5e4), (capacity_), 1, 3, 0, 0})

#define UC2MP_PREPARE_MAX_MS 120000u
typedef enum { UC2MP_PHASE_LOBBY, UC2MP_PHASE_PREPARING, UC2MP_PHASE_LOADING } Uc2mpPhase;
typedef struct {
    uint64_t id; /* Increasing attempt ID, retained after cancel/close. */
    uint64_t deadline_ms; /* Local monotonic clock; never extended by ACK. */
    Uc2mpPhase phase;
    uint16_t prepared, loading; /* Bits index the frozen roster. */
} Uc2mpPreparation;
typedef struct {
    Uc2mpProfile local;
    Uc2mpMember members[UC2MP_MAX_MEMBERS];
    Uc2mpSettings settings;
    uint64_t host_id, generation, revision;
    unsigned count;
    int open;
    Uc2mpPreparation preparation;
} Uc2mpLobby;

Uc2mpResult uc2mp_profile(Uc2mpProfile *out, uint64_t id, const char *name);
Uc2mpResult uc2mp_init(Uc2mpLobby *lobby, const Uc2mpProfile *local);
Uc2mpResult uc2mp_open(Uc2mpLobby *lobby);
Uc2mpResult uc2mp_close(Uc2mpLobby *lobby);
Uc2mpResult uc2mp_rename(Uc2mpLobby *lobby, const char *name);
Uc2mpResult uc2mp_configure(Uc2mpLobby *lobby, uint64_t actor,
                          Uc2mpSettings settings);
/* In-memory roster operations. No network transport or remote authentication
 * is provided by these functions. A future transport must authorize requests. */
Uc2mpResult uc2mp_join(Uc2mpLobby *lobby, const Uc2mpProfile *profile);
Uc2mpResult uc2mp_leave(Uc2mpLobby *lobby, uint64_t member);
Uc2mpResult uc2mp_ready(Uc2mpLobby *lobby, uint64_t member, int ready);
const Uc2mpMember *uc2mp_member(const Uc2mpLobby *lobby, unsigned index);
Uc2mpResult uc2mp_start(const Uc2mpLobby *lobby, uint64_t actor);
/* Preparation only: no engine calls, avatar spawning, or gameplay transition.
 * timeout_ms is 1..120000. All participants (including host) explicitly ACK
 * PREPARING, then host may authorize LOADING. LOADING ACK means permission
 * received, not that an engine has loaded. No automatic start on any timeout. */
int uc2mp_match_settings_valid(Uc2mpSettings settings);
int uc2mp_settings_equal(Uc2mpSettings a, Uc2mpSettings b);
int uc2mp_rules_valid(Uc2mpSettings settings);
Uc2mpResult uc2mp_prepare_begin(Uc2mpLobby *lobby, uint64_t actor, uint64_t now_ms, uint32_t timeout_ms);
Uc2mpResult uc2mp_prepare_ack(Uc2mpLobby *lobby, uint64_t member, uint64_t id, Uc2mpPhase phase);
Uc2mpResult uc2mp_prepare_load(Uc2mpLobby *lobby, uint64_t actor, uint64_t id, uint64_t now_ms);
Uc2mpResult uc2mp_prepare_cancel(Uc2mpLobby *lobby, uint64_t actor, uint64_t id);
int uc2mp_prepare_expire(Uc2mpLobby *lobby, uint64_t now_ms);
const char *uc2mp_result_text(Uc2mpResult result);
#endif
