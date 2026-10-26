/* GOW3_PAD_MAP / GOW3_KEY_MAP parsing (src/runtime_pad.c). */
#define _GNU_SOURCE
#include <assert.h>
#include "../src/runtime_pad.c"

int gow3gpu_overlay_captures_input(void) { return 0; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

/* PS4 buttons a host source presses in `map`. */
static uint32_t pressed_by(const ButtonMap *map, int source) {
    uint32_t ps=0;
    for (size_t i=0;i<map->count;++i) if (map->entries[i].source==source) ps|=map->entries[i].ps;
    return ps;
}

int main(void) {
    StickKeyMap sticks=default_stick_keys;
    ButtonMap keys=default_key_map;
    assert(apply_button_map(&keys,&sticks,NULL,MAP_KEYBOARD)==0 && apply_button_map(&keys,&sticks,"",MAP_KEYBOARD)==0);
    assert(keys.count==default_key_map.count);

    /* Swap: Z presses Cross, Space presses Triangle; Space no longer presses Cross. */
    assert(apply_button_map(&keys,&sticks," cross = Z , triangle=Space",MAP_KEYBOARD)==2);
    assert(pressed_by(&keys,SDL_SCANCODE_Z)==BTN_CROSS);
    assert(pressed_by(&keys,SDL_SCANCODE_SPACE)==BTN_TRIANGLE);
    assert(pressed_by(&keys,SDL_SCANCODE_F)==BTN_SQUARE);

    /* Names with spaces, any case; repeated entries add multiple sources for a button. */
    ButtonMap again=default_key_map;
    assert(apply_button_map(&again,&sticks,"CIRCLE=left shift,circle=Left Ctrl",MAP_KEYBOARD)==2);
    assert(pressed_by(&again,SDL_SCANCODE_LCTRL)==BTN_CIRCLE);
    assert(pressed_by(&again,SDL_SCANCODE_LSHIFT)==(BTN_CIRCLE|BTN_L3));

    /* Bad entries are skipped and leave the defaults alone. */
    ButtonMap bad=default_key_map;
    assert(apply_button_map(&bad,&sticks,"bogus=Q,cross=NotAKey,circle,=Q,square=NotAKey,,",MAP_KEYBOARD)==0);
    assert(bad.count==default_key_map.count && pressed_by(&bad,SDL_SCANCODE_SPACE)==BTN_CROSS);

    /* Gamepad: Xbox layout swap; 'touchpad' is the left click, Back stays the right click and
     * the real touchpad click keeps its fingers. */
    ButtonMap pad=default_pad_map;
    assert(apply_button_map(&pad,&sticks,"cross=b,circle=a,touchpad=misc1",MAP_PAD)==3);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_EAST)==BTN_CROSS);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_SOUTH)==BTN_CIRCLE);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_MISC1)==CLICK_LEFT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_BACK)==CLICK_RIGHT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_TOUCHPAD)==BTN_TOUCHPAD);
    assert(apply_button_map(&pad,&sticks,"touchpad_right=guide,touchpad_left=paddle1",MAP_PAD)==2);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_BACK)==0 && pressed_by(&pad,SDL_GAMEPAD_BUTTON_GUIDE)==CLICK_RIGHT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_MISC1)==0 && pressed_by(&pad,SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1)==CLICK_LEFT);
    assert(apply_button_map(&pad,&sticks,"cross=Space",MAP_PAD)==0); /* a key name is no gamepad button */
    /* Keyboard: each side has a key of its own. */
    assert(pressed_by(&default_key_map,SDL_SCANCODE_TAB)==CLICK_LEFT && pressed_by(&default_key_map,SDL_SCANCODE_BACKSPACE)==CLICK_RIGHT);
    ButtonMap sides=default_key_map;
    assert(apply_button_map(&sides,&sticks,"touchpad_right=Delete",MAP_KEYBOARD)==1);
    assert(pressed_by(&sides,SDL_SCANCODE_BACKSPACE)==0 && pressed_by(&sides,SDL_SCANCODE_DELETE)==CLICK_RIGHT);
    puts("PASS: pad and key maps");
}
