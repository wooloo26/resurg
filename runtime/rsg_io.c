#include "rsg_io.h"

#include <stdio.h>

// ── print (no trailing newline) ────────────────────────────────────────

void rsgu_print_str(RsgStr src) {
    fwrite(src.data, 1, src.len, stdout);
}

void rsgu_print_i32(int32_t value) {
    printf("%d", value);
}

void rsgu_print_u32(uint32_t value) {
    printf("%u", value);
}

void rsgu_print_f64(double value) {
    printf("%g", value);
}

void rsgu_print_bool(bool value) {
    printf("%s", value ? "true" : "false");
}

void rsgu_print_char(char value) {
    putchar(value);
}

// ── println (with trailing newline) ────────────────────────────────────

void rsgu_println_str(RsgStr src) {
    fwrite(src.data, 1, src.len, stdout);
    putchar('\n');
}

void rsgu_println_i32(int32_t value) {
    printf("%d\n", value);
}

void rsgu_println_u32(uint32_t value) {
    printf("%u\n", value);
}

void rsgu_println_f64(double value) {
    printf("%g\n", value);
}

void rsgu_println_bool(bool value) {
    printf("%s\n", value ? "true" : "false");
}

void rsgu_println_char(char value) {
    printf("%c\n", value);
}
