#ifndef RUNTIME_UC2MP_CATALOG_H
#define RUNTIME_UC2MP_CATALOG_H
#include <stdint.h>
#include <stddef.h>

/* Host-owned configuration IDs. CRCs of stable keys, NOT game-manager enum
 * indices. A future launch adapter must translate the selected key explicitly.
 * Map keys/packages are present in this installation's net.bin and pak files.
 * Catalog presence does not imply that a playable match has been validated. */
typedef struct { uint32_t id; const char *key, *label; } Uc2mpCatalogEntry;
static const Uc2mpCatalogEntry uc2mp_modes[] = {
    {UINT32_C(0xd3b86d60),"deathmatch","Match \xc3\xa0 mort"}
};
static const Uc2mpCatalogEntry uc2mp_maps[] = {
    /* Every map the Collection kept with PS3 online spawners (uc2mp_extracted_spawn_catalog.h).
     * Labels translate the PS3 map titles (text1 <lang>.networking, which the Collection lost;
     * net.bin's map record +0 is the title SID): The Village a036040e, The Plaza 23480697,
     * The Sanctuary b5b2acd0, The Ice Cave 1dcca0f1, The Lost City 9ddf8b90, The Fort d08e0327,
     * The Train Wreck 3af91b54, The Temple 8f4a3a42. No PS3 disc carries French titles. */
    {UINT32_C(0xdbaad5e4),"village-multi","Le Village"},
    {UINT32_C(0x64b8d44b),"warzone-multi","La Place"},
    {UINT32_C(0x92a1a069),"gompa-multi","Le Sanctuaire"},
    {UINT32_C(0x031b911b),"ice-cave-multi","La Grotte de glace"},
    {UINT32_C(0xb31a0522),"shambhala-multi","La Cit\xc3\xa9 perdue"},
    {UINT32_C(0x800e878e),"the-fort-multi","Le Fort"},
    {UINT32_C(0xd0f7b257),"train-wreck-multi","L'\xc3\x89pave du train"},
    {UINT32_C(0x78d64959),"treasure-multi","Le Temple"}
};
/* Match rules of PAGE_LOBBY's rows (commands 60 score limit, 61 time limit, 63 rounds,
 * 62 friendly fire); Uc2mpSettings keeps the index, the match logic reads the value
 * (time in minutes, 0 = none). Defaults: UC2MP_SETTINGS_DEFAULT in runtime_uc2mp.h. */
typedef struct { unsigned value; const char *label; } Uc2mpRuleChoice;
/* Worded like the PS3 lobby ("[A] kills", "20 minutes", "Off"). */
static const Uc2mpRuleChoice uc2mp_score_limits[] = {
    {25,"25 \xc3\xa9liminations"},{50,"50 \xc3\xa9liminations"},{75,"75 \xc3\xa9liminations"},
    {100,"100 \xc3\xa9liminations"}
};
static const Uc2mpRuleChoice uc2mp_time_limits[] = {
    {5,"5 minutes"},{10,"10 minutes"},{15,"15 minutes"},{20,"20 minutes"},{0,"Aucune"}
};
static const Uc2mpRuleChoice uc2mp_round_limits[] = {{1,"1"},{2,"2"},{3,"3"},{5,"5"}};
static const Uc2mpRuleChoice uc2mp_friendly_fire[] = {{0,"D\xc3\xa9sactiv\xc3\xa9"},{1,"Activ\xc3\xa9"}};
static inline const Uc2mpRuleChoice *uc2mp_rule_choices(unsigned command, size_t *count) {
#define UC2MP_RULE(list) (*count=sizeof(list)/sizeof(*list), list)
    switch (command) {
        case 60: return UC2MP_RULE(uc2mp_score_limits);
        case 61: return UC2MP_RULE(uc2mp_time_limits);
        case 62: return UC2MP_RULE(uc2mp_friendly_fire);
        case 63: return UC2MP_RULE(uc2mp_round_limits);
        default: *count=0; return NULL;
    }
#undef UC2MP_RULE
}
static inline const Uc2mpCatalogEntry *uc2mp_catalog(unsigned command, size_t *count) {
    if (command == 57) { *count=sizeof(uc2mp_modes)/sizeof(*uc2mp_modes); return uc2mp_modes; }
    if (command == 58) { *count=sizeof(uc2mp_maps)/sizeof(*uc2mp_maps); return uc2mp_maps; }
    *count=0; return NULL;
}
static inline const Uc2mpCatalogEntry *uc2mp_catalog_id(unsigned command, uint32_t id) {
    size_t count;
    const Uc2mpCatalogEntry *entries=uc2mp_catalog(command,&count);
    for (size_t i=0; i<count; ++i) if (entries[i].id==id) return entries+i;
    return NULL;
}
#endif
