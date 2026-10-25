/* Opt-in (UNCHARTED_MP_MENU=1) answers to the game's text lookup for the multiplayer texts the
 * Collection removed (uc2mp_texts.h). Uncharted 2 v1.02 TextManager lookup 0xcddc00(manager,
 * id) returns the UTF-8 text or NULL; callers fall back to "UNTRANSLATED STRING!!!". Its entry
 * becomes a jump to uc2mp_text_lookup, which answers the listed ids and otherwise runs the
 * original through a trampoline. Include from probe.c after allocate_near_image(). */
#ifndef UC2MP_TEXT_BRIDGE_H
#define UC2MP_TEXT_BRIDGE_H
#include "uc2mp_texts.h"

typedef const char *(ABI *Uc2mpTextLookup)(void *, uint32_t);
static Uc2mpTextLookup uc2mp_text_original;

static ABI const char *uc2mp_text_lookup(void *manager, uint32_t id) {
    const Uc2mpText *text = uc2mp_text_find(id);
    if (!text) return uc2mp_text_original(manager, id);
    const char *language = uc2mp_text_original(manager, UC2MP_TEXT_MULTIPLAYER);
    return text->text[uc2mp_text_language(language)];
}

/* 0=not requested, 1=installed, -1=requested but rejected. Before image protections. */
static int install_uc2mp_text_bridge(Segment *segments, uint64_t count, uint64_t size) {
    const char *enabled = getenv("UNCHARTED_MP_MENU");
    if (!enabled || strcmp(enabled, "1")) return 0;
    /* push rbp; mov rbp,rsp; push r14 | push rbx; mov ebx,esi; mov r14,rdi; mov rdi,[r14+58] */
    static const unsigned char entry[] = {
        0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x89,0xf3,0x49,0x89,0xfe,0x49,0x8b,0x7e,0x58
    };
    const uint64_t at = 0xcddc00;
    int mapped = 0;
    for (uint64_t i = 0; i < count; ++i)
        if ((segments[i].flags & 5) == 5 && at >= segments[i].address &&
            sizeof(entry) <= segments[i].size && at - segments[i].address <= segments[i].size - sizeof(entry))
            mapped = 1;
    if (!image || at + sizeof(entry) > size || !mapped || memcmp(image + at, entry, sizeof(entry))) {
        fprintf(stderr, "UC2MP text: unsupported image or signature; missing texts stay missing\n");
        return -1;
    }
    unsigned char *stub = allocate_near_image(size);
    const int64_t displacement = stub ? (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + at + 5) : 0;
    if (!stub || displacement < INT32_MIN || displacement > INT32_MAX) {
        fprintf(stderr, "UC2MP text: no trampoline page in rel32 range\n");
        return -1;
    }
    /* stub+0: jmp [rip] -> uc2mp_text_lookup. stub+16: the six entry bytes (whole instructions:
     * push rbp; mov rbp,rsp; push r14), then jmp [rip] -> original +6. */
    const unsigned char absolute_jump[6] = {0xff,0x25,0,0,0,0};
    const uintptr_t hook = (uintptr_t)uc2mp_text_lookup, resume = (uintptr_t)(image + at + 6);
    memcpy(stub, absolute_jump, sizeof(absolute_jump));
    memcpy(stub + 6, &hook, sizeof(hook));
    memcpy(stub + 16, image + at, 6);
    memcpy(stub + 22, absolute_jump, sizeof(absolute_jump));
    memcpy(stub + 28, &resume, sizeof(resume));
    protect(stub, page_size, 5);
    uc2mp_text_original = (Uc2mpTextLookup)(stub + 16);
    const int32_t relative = (int32_t)displacement;
    image[at] = 0xe9;
    memcpy(image + at + 1, &relative, sizeof(relative));
    puts("UC2MP text: removed multiplayer texts answered (stats labels, leaderboards, trophies)");
    return 1;
}
#endif
