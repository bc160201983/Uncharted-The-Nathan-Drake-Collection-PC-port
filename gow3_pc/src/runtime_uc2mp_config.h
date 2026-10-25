#ifndef RUNTIME_UC2MP_CONFIG_H
#define RUNTIME_UC2MP_CONFIG_H
#include "runtime_uc2mp.h"
#include <stdlib.h>
#include <string.h>

static inline int uc2mp_config_u64(const char *text, uint64_t *result) {
    if (!text || !*text || !result) return 0;
    uint64_t value = 0;
    for (const unsigned char *c = (const unsigned char *)text; *c; ++c) {
        if (*c < '0' || *c > '9' || value > (UINT64_MAX - (*c - '0')) / 10) return 0;
        value = value * 10 + (*c - '0');
    }
    *result = value;
    return 1;
}

static inline int uc2mp_config_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline int uc2mp_config_hex(const char *text, unsigned char *out, size_t count) {
    if (!text || !out || strlen(text) != 2 * count) return 0;
    for (size_t i = 0; i < 2 * count; ++i)
        if (uc2mp_config_nibble(text[i]) < 0) return 0;
    for (size_t i = 0; i < count; ++i)
        out[i] = (unsigned char)((uc2mp_config_nibble(text[2*i]) << 4) |
                                uc2mp_config_nibble(text[2*i+1]));
    return 1;
}

static inline Uc2mpResult uc2mp_config_profile(Uc2mpProfile *profile) {
    uint64_t id = 1;
    const char *id_text = getenv("UC2MP_LOCAL_ID");
    if (id_text && (!uc2mp_config_u64(id_text, &id) || !id)) return UC2MP_INVALID;
    const char *hex = getenv("UC2MP_PLAYER_UTF8");
    char name[UC2MP_NAME_BYTES] = {0};
    if (hex) {
        size_t bytes = strlen(hex) / 2;
        if (!bytes || bytes >= sizeof(name) ||
            !uc2mp_config_hex(hex, (unsigned char *)name, bytes) ||
            memchr(name, 0, bytes)) return UC2MP_INVALID;
    } else {
        const char *plain = getenv("UC2MP_PLAYER_NAME");
        if (!plain || !*plain) plain = "Joueur";
        if (strlen(plain) >= sizeof(name)) return UC2MP_INVALID;
        memcpy(name, plain, strlen(plain) + 1);
    }
    return uc2mp_profile(profile, id, name);
}
#endif
