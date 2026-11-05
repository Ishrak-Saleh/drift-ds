//test_keys.c: checks for runtime/keys.c. no framework, prints one line per failure
//build and run from the project folder:
//  clang -Iruntime tests/test_keys.c runtime/keys.c -o tests/test_keys.exe && tests/test_keys.exe
#include "keys.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

//fake settings for bind_load
static const char *fake_values[40][2];
static int fake_count;
static void fake_set(const char *name, const char *value) {
    fake_values[fake_count][0] = name;
    fake_values[fake_count][1] = value;
    fake_count++;
}
static const char *fake_get(const char *name) {
    for (int i = 0; i < fake_count; i++)
        if (!strcmp(fake_values[i][0], name)) return fake_values[i][1];
    return NULL;
}

static uint16_t mask_for(const Bindings *b, int key1, int key2) {
    uint8_t down[TK_COUNT] = {0};
    if (key1) down[key1] = 1;
    if (key2) down[key2] = 1;
    return bind_pressed_mask(b, down);
}

int main(void) {
    //names round trip for every id, ignoring case
    for (int k = 1; k < TK_COUNT; k++) {
        CHECK(*key_name(k) != 0);
        CHECK(key_from_name(key_name(k)) == k);
    }
    CHECK(key_from_name("w") == TK_W);
    CHECK(key_from_name("lshift") == TK_LSHIFT);
    CHECK(key_from_name("") == TK_NONE);
    CHECK(key_from_name("Nonsense") == TK_NONE);
    //reserved keys cannot be named
    CHECK(key_from_name("F1") == TK_NONE);
    CHECK(key_from_name("F10") == TK_NONE);
    CHECK(key_from_name("F11") == TK_NONE);
    CHECK(key_from_name("Esc") == TK_NONE);
    CHECK(key_from_name("Escape") == TK_NONE);
    CHECK(key_from_name("Alt") == TK_NONE);
    CHECK(key_from_name("Mouse1") == TK_NONE);
    CHECK(key_from_name("Mouse2") == TK_MOUSE_RIGHT);
    CHECK(TK_COUNT < 255);   //ids fit in one byte

    //buttons and presets
    CHECK(button_from_name("start") == BTN_START);
    CHECK(button_from_name("Q") == -1);
    CHECK(preset_from_name("WASD") == PRESET_WASD);
    CHECK(preset_from_name("classic") == PRESET_CLASSIC);
    CHECK(preset_from_name("x") == -1);

    //wasd preset: the racing layout
    Bindings b;
    bind_preset(&b, PRESET_WASD);
    CHECK(mask_for(&b, TK_W, 0) == (1u << BTN_A));
    CHECK(mask_for(&b, TK_S, 0) == (1u << BTN_B));
    CHECK(mask_for(&b, TK_A, 0) == (1u << BTN_LEFT));
    CHECK(mask_for(&b, TK_D, 0) == (1u << BTN_RIGHT));
    CHECK(mask_for(&b, TK_LEFT, 0) == (1u << BTN_LEFT));      //arrows still steer
    CHECK(mask_for(&b, TK_RIGHT, 0) == (1u << BTN_RIGHT));
    CHECK(mask_for(&b, TK_SPACE, 0) == (1u << BTN_R));        //drift
    CHECK(mask_for(&b, TK_LSHIFT, 0) == (1u << BTN_L));       //item
    CHECK(mask_for(&b, TK_RSHIFT, 0) == (1u << BTN_L));
    CHECK(mask_for(&b, TK_LSHIFT, TK_UP) == ((1u << BTN_L) | (1u << BTN_UP)));     //forward throw
    CHECK(mask_for(&b, TK_LSHIFT, TK_DOWN) == ((1u << BTN_L) | (1u << BTN_DOWN))); //backward throw
    CHECK(mask_for(&b, TK_ENTER, 0) == (1u << BTN_A));        //menu confirm
    CHECK(mask_for(&b, TK_BACKSPACE, 0) == (1u << BTN_B));    //menu back
    CHECK(mask_for(&b, TK_E, 0) == (1u << BTN_X));
    CHECK(mask_for(&b, TK_Q, 0) == (1u << BTN_Y));
    CHECK(mask_for(&b, TK_TAB, 0) == (1u << BTN_START));
    CHECK(mask_for(&b, TK_RCTRL, 0) == (1u << BTN_SELECT));
    CHECK(mask_for(&b, TK_W, TK_D) == ((1u << BTN_A) | (1u << BTN_RIGHT)));
    CHECK(mask_for(&b, TK_NONE, 0) == 0);
    CHECK(mask_for(&b, TK_Z, 0) == 0);                        //unbound key

    //classic preset: the first port layout
    bind_preset(&b, PRESET_CLASSIC);
    CHECK(mask_for(&b, TK_X, 0) == (1u << BTN_A));
    CHECK(mask_for(&b, TK_Z, 0) == (1u << BTN_B));
    CHECK(mask_for(&b, TK_UP, 0) == (1u << BTN_UP));
    CHECK(mask_for(&b, TK_W, 0) == (1u << BTN_R));
    CHECK(mask_for(&b, TK_Q, 0) == (1u << BTN_L));
    CHECK(mask_for(&b, TK_S, 0) == (1u << BTN_X));
    CHECK(mask_for(&b, TK_A, 0) == (1u << BTN_Y));

    //every wasd binding survives format then parse
    bind_preset(&b, PRESET_WASD);
    for (int i = 0; i < BTN_COUNT; i++) {
        char text[64];
        Bindings copy = b;
        bind_format(&b, (DsButton)i, text, sizeof text);
        bind_parse(&copy, (DsButton)i, text);
        CHECK(!memcmp(copy.key[i], b.key[i], BIND_SLOTS));
    }
    char text[64];
    bind_format(&b, BTN_A, text, sizeof text);
    CHECK(!strcmp(text, "W,Enter"));

    //newest binding wins. a key lives in one slot only
    bind_preset(&b, PRESET_WASD);
    CHECK(bind_set(&b, BTN_X, 1, TK_W) == 1);                 //W moves from A to X slot 2
    CHECK(b.key[BTN_A][0] == TK_NONE);
    CHECK(b.key[BTN_X][1] == TK_W);
    CHECK(mask_for(&b, TK_W, 0) == (1u << BTN_X));
    CHECK(bind_set(&b, BTN_X, 1, TK_NONE) == 0);              //not a key
    CHECK(bind_set(&b, BTN_X, 2, TK_W) == 0);                 //bad slot
    CHECK(bind_set(&b, BTN_COUNT, 0, TK_W) == 0);             //bad button
    CHECK(bind_set(&b, BTN_X, 0, TK_COUNT) == 0);             //bad id
    bind_clear(&b, BTN_X, 1);
    CHECK(b.key[BTN_X][1] == TK_NONE);

    //parse: spaces, case, unknown names and too many names
    bind_parse(&b, BTN_Y, " space , q ");
    CHECK(b.key[BTN_Y][0] == TK_SPACE && b.key[BTN_Y][1] == TK_Q);
    CHECK(bind_parse(&b, BTN_Y, "nope,alsonope") == 0);
    CHECK(b.key[BTN_Y][0] == TK_NONE && b.key[BTN_Y][1] == TK_NONE);
    CHECK(bind_parse(&b, BTN_Y, "F1,Esc") == 0);              //reserved names are rejected
    CHECK(bind_parse(&b, BTN_Y, "") == 0);
    CHECK(bind_parse(&b, BTN_Y, "A,B,C") == 2);               //a third name is ignored
    CHECK(bind_parse(&b, BTN_Y, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") == 0);   //long text does not overflow

    //load: preset first, then TECLA lines on top
    fake_count = 0;
    bind_load(&b, fake_get);
    CHECK(b.key[BTN_A][0] == TK_W);                           //no settings gives the wasd default
    fake_set("PRESET", "classic");
    bind_load(&b, fake_get);
    CHECK(b.key[BTN_A][0] == TK_X);
    fake_set("KEY_A", "Mouse2");
    bind_load(&b, fake_get);
    CHECK(b.key[BTN_A][0] == TK_MOUSE_RIGHT && b.key[BTN_A][1] == TK_NONE);
    CHECK(b.key[BTN_B][0] == TK_Z);                           //other buttons keep the preset
    fake_set("KEY_B", "");
    bind_load(&b, fake_get);
    CHECK(b.key[BTN_B][0] == TK_NONE);                        //an empty line clears the button
    fake_count = 0;
    fake_set("PRESET", "garbage");
    bind_load(&b, fake_get);
    CHECK(b.key[BTN_A][0] == TK_W);                           //bad preset falls back to wasd

    printf(failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
    return failures != 0;
}
