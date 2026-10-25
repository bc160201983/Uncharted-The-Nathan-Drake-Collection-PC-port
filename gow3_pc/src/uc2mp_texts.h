/* Uncharted 2 multiplayer texts the Collection removed from its text files (text1/ps4/N.common)
 * while code and menus still ask for them. uc2mp_text_bridge.h answers the game's text lookup
 * with these; uc2mp_menu_bridge.h uses the same lookup for the multiplayer page's buttons.
 * The language follows the game's active text: its "Multiplayer" (id f75b3a65). */
#ifndef UC2MP_TEXTS_H
#define UC2MP_TEXTS_H
#include <stdint.h>
#include <string.h>

enum { UC2MP_LANG_EN, UC2MP_LANG_FR, UC2MP_LANG_IT, UC2MP_LANG_DE, UC2MP_LANG_ES, UC2MP_LANG_PT,
       UC2MP_LANG_NL, UC2MP_LANG_COUNT };
#define UC2MP_TEXT_MULTIPLAYER UINT32_C(0xf75b3a65)
/* Ids of our own (CRC of these names): the PS3 ids of these two labels are not known. */
#define UC2MP_TEXT_LEADERBOARDS UINT32_C(0xbd24f140) /* "UC2MP_LEADERBOARDS" */
#define UC2MP_TEXT_TROPHIES UINT32_C(0xbf5af37d)     /* "UC2MP_TROPHIES" */

typedef struct { uint32_t id; const char *text[UC2MP_LANG_COUNT]; } Uc2mpText;
static const Uc2mpText uc2mp_texts[] = {
    /* Profile stats panel of PAGE_MULTIPLAYER (renderer 59ddb0, rows 1-3; row 4 is Co-op). */
    {UINT32_C(0xf67d9ad2), {"Competitive Wins", "Victoires en comp\xc3\xa9tition", "Vittorie competitive",
                            "Wettkampf-Siege", "Victorias competitivas", "Vit\xc3\xb3rias competitivas",
                            "Competitieve overwinningen"}},
    {UINT32_C(0x70c81d2e), {"Competitive Kills", "Victimes en comp\xc3\xa9tition", "Uccisioni competitive",
                            "Wettkampf-Abschuss", "Bajas competitivas", "Abates competitivos",
                            "Competitieve kills"}},
    {UINT32_C(0xc0cd1b00), {"Money", "Argent", "Denaro", "Geld", "Dinero", "Dinheiro", "Geld"}},
    /* PAGE_LOBBY rule rows whose label the Collection lost: MENU_MULTIPLAYER_CUSTOMTIMELIMIT
     * (command 61) and MENU_MULTIPLAYER_CUSTOMFRIENDLYFIRE (command 62). */
    {UINT32_C(0x07774a0d), {"Time Limit", "Limite de temps", "Limite di tempo", "Zeitlimit",
                            "L\xc3\xadmite de tiempo", "Limite de tempo", "Tijdslimiet"}},
    {UINT32_C(0x9b7aa1a9), {"Friendly Fire", "Tir alli\xc3\xa9", "Fuoco amico", "Eigenbeschuss",
                            "Fuego amigo", "Fogo amigo", "Eigen vuur"}},
    /* PAGE_MULTIPLAYER buttons whose text the Collection lost (commands 131 and 136). */
    {UC2MP_TEXT_LEADERBOARDS, {"Leaderboards", "Classements", "Classifiche", "Bestenlisten",
                               "Clasificaciones", "Classifica\xc3\xa7\xc3\xb5" "es", "Ranglijsten"}},
    {UC2MP_TEXT_TROPHIES, {"Trophies", "Troph\xc3\xa9" "es", "Trofei", "Troph\xc3\xa4" "en", "Trofeos",
                           "Trof\xc3\xa9us", "Trofee\xc3\xabn"}},
};

static inline const Uc2mpText *uc2mp_text_find(uint32_t id) {
    for (size_t i = 0; i < sizeof(uc2mp_texts) / sizeof(*uc2mp_texts); ++i)
        if (uc2mp_texts[i].id == id) return &uc2mp_texts[i];
    return NULL;
}
/* multiplayer: the active text's "Multiplayer" (NULL: English). */
static inline int uc2mp_text_language(const char *multiplayer) {
    static const char *const words[UC2MP_LANG_COUNT] = {
        "Multiplayer", "Multijoueur", "Multigiocatore", "Mehrspieler", "Multijugador", "Multijogador", NULL
    };
    if (!multiplayer) return UC2MP_LANG_EN;
    for (int i = 1; i < UC2MP_LANG_COUNT; ++i)
        if (words[i] && !strcmp(multiplayer, words[i])) return i;
    return UC2MP_LANG_EN; /* Dutch shares "Multiplayer" with English; other languages: English */
}
#endif
