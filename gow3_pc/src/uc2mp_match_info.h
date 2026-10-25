/* What a restarted match process shows while its map loads (the overlay's multiplayer loading
 * screen): the lobby writes it next to the launcher handoff (GOW3_LOADEXEC_FILE + ".uc2mp")
 * before LoadExec, the match process reads it at start. Lines: map key, mode, map name, rules.
 * The map picture is the overlay's uc2mp/loading/<map key>.png (tools/uc2mp_loading_art.py). */
#ifndef UC2MP_MATCH_INFO_H
#define UC2MP_MATCH_INFO_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime_uc2mp.h"
#include "runtime_uc2mp_catalog.h"

typedef struct { char map[48], mode[64], name[64], rules[192]; } Uc2mpMatchInfo;

static inline int uc2mp_match_info_path(char *out, size_t size) {
    const char *handoff = getenv("GOW3_LOADEXEC_FILE");
    return handoff && *handoff && snprintf(out, size, "%s.uc2mp", handoff) < (int)size;
}

/* "Score : 50 \xc3\xa9liminations - Temps : 20 minutes - Parties : 1 - Tir alli\xc3\xa9 : D\xc3\xa9sactiv\xc3\xa9"
 * from the lobby's rules. */
static inline void uc2mp_match_info_rules(Uc2mpSettings settings, char *out, size_t size) {
    size_t score, time, friendly, rounds;
    const Uc2mpRuleChoice *s = uc2mp_rule_choices(60, &score), *t = uc2mp_rule_choices(61, &time);
    const Uc2mpRuleChoice *f = uc2mp_rule_choices(62, &friendly), *r = uc2mp_rule_choices(63, &rounds);
    if (!uc2mp_rules_valid(settings)) { if (size) *out = 0; return; }
    snprintf(out, size, "Score : %s - Temps : %s - Parties : %s - Tir alli\xc3\xa9 : %s",
             s[settings.score_limit].label, t[settings.time_limit].label,
             r[settings.rounds].label, f[settings.friendly_fire].label);
}

static inline int uc2mp_match_info_from(Uc2mpSettings settings, Uc2mpMatchInfo *info) {
    const Uc2mpCatalogEntry *mode = uc2mp_catalog_id(57, settings.mode_id);
    const Uc2mpCatalogEntry *map = uc2mp_catalog_id(58, settings.map_id);
    if (!mode || !map || !mode->id || !map->id) return 0;
    memset(info, 0, sizeof(*info));
    snprintf(info->map, sizeof(info->map), "%s", map->key);
    snprintf(info->mode, sizeof(info->mode), "%s", mode->label);
    snprintf(info->name, sizeof(info->name), "%s", map->label);
    uc2mp_match_info_rules(settings, info->rules, sizeof(info->rules));
    return 1;
}

static inline int uc2mp_match_info_write(const Uc2mpMatchInfo *info) {
    char path[1024];
    FILE *f = uc2mp_match_info_path(path, sizeof(path)) ? fopen(path, "wb") : NULL;
    if (!f) return 0;
    fprintf(f, "%s\n%s\n%s\n%s\n", info->map, info->mode, info->name, info->rules);
    return fclose(f) == 0;
}

static inline void uc2mp_match_info_line(FILE *f, char *out, size_t size) {
    if (!fgets(out, (int)size, f)) { *out = 0; return; }
    out[strcspn(out, "\r\n")] = 0;
}

/* The written match of this map, else a plain one named after the map key. */
static inline void uc2mp_match_info_read(const char *map, Uc2mpMatchInfo *info) {
    char path[1024];
    memset(info, 0, sizeof(*info));
    FILE *f = uc2mp_match_info_path(path, sizeof(path)) ? fopen(path, "rb") : NULL;
    if (f) {
        uc2mp_match_info_line(f, info->map, sizeof(info->map));
        uc2mp_match_info_line(f, info->mode, sizeof(info->mode));
        uc2mp_match_info_line(f, info->name, sizeof(info->name));
        uc2mp_match_info_line(f, info->rules, sizeof(info->rules));
        fclose(f);
    }
    if (!map || strcmp(info->map, map)) {
        memset(info, 0, sizeof(*info));
        snprintf(info->map, sizeof(info->map), "%s", map ? map : "");
        snprintf(info->mode, sizeof(info->mode), "Multijoueur");
        size_t count;
        const Uc2mpCatalogEntry *maps = uc2mp_catalog(58, &count);
        snprintf(info->name, sizeof(info->name), "%s", info->map);
        for (size_t i = 0; map && i < count; ++i)
            if (!strcmp(maps[i].key, map)) snprintf(info->name, sizeof(info->name), "%s", maps[i].label);
    }
}
#endif
