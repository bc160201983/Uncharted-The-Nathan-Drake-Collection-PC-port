/* Levels a multiplayer map loads with it: its sky (sky, light and reflections) and its wide/vehicle
 * parts. Same lists as PS3 and the Collection's dc1/ps4/net.bin (competitive groups at 0x11188:
 * name pointers, terminated per map). The game's command line takes several "level" arguments
 * (1.02 0x3f2976 appends each one to the request list that GameInit 0x56bf60 loads); the map
 * itself stays first, since other code reads the first requested level as the map. */
#ifndef UC2MP_MAP_LEVELS_H
#define UC2MP_MAP_LEVELS_H
#include <stddef.h>
#include <string.h>

#define UC2MP_MAP_LEVELS_MAX 3
typedef struct { const char *map; const char *levels[UC2MP_MAP_LEVELS_MAX + 1]; } Uc2mpMapLevels;
static const Uc2mpMapLevels uc2mp_map_levels[] = {
    {"warzone-multi", {"sky-multi-warzone", NULL}},
    {"village-multi", {"village-multi-wide", "sky-village-multi", "village-multi-tank", NULL}},
    {"shambhala-multi", {"sky-sham-descent", "sham-multi-wide", NULL}},
    {"treasure-multi", {"sky-village-pre", NULL}},
    {"gompa-multi", {"gompa-multi-wide", "sky-gompa", NULL}},
    {"train-wreck-multi", {"sky-train-wreck", NULL}},
    {"ice-cave-multi", {"sky-ice-cave", NULL}},
    /* the-fort-multi (sky-fort-multi, the-fort-multi-wide): the Collection does not declare
     * those levels; the map loads alone. */
};

/* NULL-terminated companion levels of map (an empty list for other maps). */
static inline const char *const *uc2mp_map_companions(const char *map) {
    static const char *const none[] = {NULL};
    for (size_t i = 0; map && i < sizeof(uc2mp_map_levels) / sizeof(*uc2mp_map_levels); ++i)
        if (!strcmp(map, uc2mp_map_levels[i].map)) return uc2mp_map_levels[i].levels;
    return none;
}
static inline int uc2mp_map_companion(const char *map, const char *level) {
    for (const char *const *l = uc2mp_map_companions(map); level && *l; ++l)
        if (!strcmp(*l, level)) return 1;
    return 0;
}
#endif
