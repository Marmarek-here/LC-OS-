#ifndef LCOS_TYPES_H
#define LCOS_TYPES_H

typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;

#define INPUT_MAX 255

enum key_type {
    KEY_NONE = 0,
    KEY_CHAR,
    KEY_CTRL_C,
    KEY_CTRL_S,
    KEY_CTRL_E,
    KEY_ENTER,
    KEY_BACKSPACE,
    KEY_TAB,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_CAPSLOCK
};

struct key_event {
    enum key_type type;
    char ch;
    uint8_t code;
    uint8_t has_code;
    const char *label;
};

#endif