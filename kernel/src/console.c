#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef __linux__
#include <string.h>
#endif
#include "console.h"
#include "vfs.h"
#include "pmm.h"

/* Auto-generated tiny 8x16 bitmap font (ASCII 0..127).
   Each glyph is 16 rows, each row is 8 bits (bit 7 = leftmost pixel). */
static const uint8_t fifi_font8x16[128][16] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*   0 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*   1 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*   2 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*   3 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*   4 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*   5 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*   6 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*   7 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*   8 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*   9 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  10 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  11 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  12 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  13 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  14 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  15 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  16 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  17 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  18 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  19 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  20 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  21 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  22 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  23 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  24 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  25 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  26 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  27 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  28 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  29 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  30 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  31 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  32 ' ' */
    {0x00, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00}, /*  33 '!' */
    {0x00, 0x66, 0x66, 0x66, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  34 '"' */
    {0x00, 0x00, 0x36, 0x36, 0x7F, 0x36, 0x36, 0x7F, 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  35 '#' */
    {0x00, 0x18, 0x3E, 0x63, 0x43, 0x03, 0x3E, 0x60, 0x61, 0x63, 0x3E, 0x18, 0x18, 0x00, 0x00, 0x00}, /*  36 '$' */
    {0x00, 0x00, 0x63, 0x63, 0x30, 0x18, 0x0C, 0x06, 0x63, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  37 '%' */
    {0x00, 0x1C, 0x36, 0x36, 0x1C, 0x0E, 0x3B, 0x33, 0x33, 0x33, 0x6E, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  38 '&' */
    {0x00, 0x18, 0x18, 0x18, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  39 ''' */
    {0x00, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  40 '(' */
    {0x00, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  41 ')' */
    {0x00, 0x00, 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  42 '*' */
    {0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0xFF, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  43 '+' */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0x0C, 0x00, 0x00, 0x00}, /*  44 ',' */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  45 '-' */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  46 '.' */
    {0x00, 0x60, 0x60, 0x30, 0x30, 0x18, 0x18, 0x0C, 0x0C, 0x06, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  47 '/' */
    {0x00, 0x3E, 0x63, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x63, 0x63, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  48 '0' */
    {0x00, 0x18, 0x1C, 0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  49 '1' */
    {0x00, 0x3E, 0x63, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x63, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  50 '2' */
    {0x00, 0x3E, 0x63, 0x60, 0x60, 0x3C, 0x60, 0x60, 0x60, 0x63, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  51 '3' */
    {0x00, 0x30, 0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x30, 0x30, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  52 '4' */
    {0x00, 0x7F, 0x03, 0x03, 0x03, 0x3F, 0x60, 0x60, 0x60, 0x63, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  53 '5' */
    {0x00, 0x3C, 0x06, 0x03, 0x03, 0x3F, 0x63, 0x63, 0x63, 0x63, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  54 '6' */
    {0x00, 0x7F, 0x63, 0x60, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  55 '7' */
    {0x00, 0x3E, 0x63, 0x63, 0x63, 0x3E, 0x63, 0x63, 0x63, 0x63, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  56 '8' */
    {0x00, 0x3E, 0x63, 0x63, 0x63, 0x7E, 0x60, 0x60, 0x60, 0x30, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  57 '9' */
    {0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  58 ':' */
    {0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00}, /*  59 ';' */
    {0x00, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  60 '<' */
    {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  61 '=' */
    {0x00, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  62 '>' */
    {0x00, 0x3E, 0x63, 0x60, 0x30, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  63 '?' */
    {0x00, 0x3E, 0x63, 0x63, 0x7B, 0x7B, 0x7B, 0x7B, 0x03, 0x63, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  64 '@' */
    {0x00, 0x1C, 0x36, 0x63, 0x63, 0x63, 0x7F, 0x63, 0x63, 0x63, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  65 'A' */
    {0x00, 0x3F, 0x66, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x66, 0x66, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  66 'B' */
    {0x00, 0x3C, 0x66, 0x63, 0x03, 0x03, 0x03, 0x03, 0x63, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  67 'C' */
    {0x00, 0x1F, 0x36, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  68 'D' */
    {0x00, 0x7F, 0x06, 0x06, 0x06, 0x3E, 0x06, 0x06, 0x06, 0x06, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  69 'E' */
    {0x00, 0x7F, 0x06, 0x06, 0x06, 0x3E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  70 'F' */
    {0x00, 0x3C, 0x66, 0x63, 0x03, 0x03, 0x73, 0x63, 0x63, 0x66, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  71 'G' */
    {0x00, 0x63, 0x63, 0x63, 0x63, 0x7F, 0x63, 0x63, 0x63, 0x63, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  72 'H' */
    {0x00, 0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  73 'I' */
    {0x00, 0x78, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  74 'J' */
    {0x00, 0x63, 0x33, 0x1B, 0x0F, 0x07, 0x0F, 0x1B, 0x33, 0x63, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  75 'K' */
    {0x00, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  76 'L' */
    {0x00, 0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x63, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  77 'M' */
    {0x00, 0x63, 0x67, 0x6F, 0x7F, 0x7B, 0x73, 0x63, 0x63, 0x63, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  78 'N' */
    {0x00, 0x3E, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  79 'O' */
    {0x00, 0x3F, 0x66, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  80 'P' */
    {0x00, 0x3E, 0x63, 0x63, 0x63, 0x63, 0x63, 0x6B, 0x7B, 0x33, 0x5E, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  81 'Q' */
    {0x00, 0x3F, 0x66, 0x66, 0x66, 0x3E, 0x1E, 0x36, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  82 'R' */
    {0x00, 0x3E, 0x63, 0x03, 0x03, 0x1E, 0x30, 0x60, 0x60, 0x63, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  83 'S' */
    {0x00, 0x7E, 0x5A, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  84 'T' */
    {0x00, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  85 'U' */
    {0x00, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x36, 0x36, 0x1C, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  86 'V' */
    {0x00, 0x63, 0x63, 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x7F, 0x77, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  87 'W' */
    {0x00, 0x63, 0x63, 0x36, 0x36, 0x1C, 0x1C, 0x36, 0x36, 0x63, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  88 'X' */
    {0x00, 0x66, 0x66, 0x66, 0x3C, 0x3C, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  89 'Y' */
    {0x00, 0x7F, 0x63, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x63, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  90 'Z' */
    {0x00, 0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  91 '[' */
    {0x00, 0x06, 0x06, 0x0C, 0x0C, 0x18, 0x18, 0x30, 0x30, 0x60, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  92 '\\' */
    {0x00, 0x3C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  93 ']' */
    {0x00, 0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  94 '^' */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00}, /*  95 '_' */
    {0x00, 0x18, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  96 '`' */
    {0x00, 0x00, 0x00, 0x00, 0x3C, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  97 'a' */
    {0x00, 0x06, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  98 'b' */
    {0x00, 0x00, 0x00, 0x00, 0x3C, 0x66, 0x06, 0x06, 0x06, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00}, /*  99 'c' */
    {0x00, 0x60, 0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 100 'd' */
    {0x00, 0x00, 0x00, 0x00, 0x3C, 0x66, 0x66, 0x7E, 0x06, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 101 'e' */
    {0x00, 0x38, 0x0C, 0x0C, 0x3E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 102 'f' */
    {0x00, 0x00, 0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x7C, 0x60, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 103 'g' */
    {0x00, 0x06, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 104 'h' */
    {0x00, 0x18, 0x18, 0x00, 0x1C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 105 'i' */
    {0x00, 0x30, 0x30, 0x00, 0x38, 0x30, 0x30, 0x30, 0x30, 0x33, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 106 'j' */
    {0x00, 0x06, 0x06, 0x06, 0x66, 0x36, 0x1E, 0x0E, 0x1E, 0x36, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 107 'k' */
    {0x00, 0x1C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 108 'l' */
    {0x00, 0x00, 0x00, 0x00, 0x36, 0x7F, 0x6B, 0x6B, 0x6B, 0x6B, 0x6B, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 109 'm' */
    {0x00, 0x00, 0x00, 0x00, 0x3E, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 110 'n' */
    {0x00, 0x00, 0x00, 0x00, 0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 111 'o' */
    {0x00, 0x00, 0x00, 0x00, 0x3E, 0x66, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 112 'p' */
    {0x00, 0x00, 0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 113 'q' */
    {0x00, 0x00, 0x00, 0x00, 0x3E, 0x66, 0x06, 0x06, 0x06, 0x06, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 114 'r' */
    {0x00, 0x00, 0x00, 0x00, 0x7C, 0x06, 0x3C, 0x60, 0x60, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 115 's' */
    {0x00, 0x0C, 0x0C, 0x0C, 0x3E, 0x0C, 0x0C, 0x0C, 0x0C, 0x6C, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 116 't' */
    {0x00, 0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 117 'u' */
    {0x00, 0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 118 'v' */
    {0x00, 0x00, 0x00, 0x00, 0x63, 0x63, 0x6B, 0x6B, 0x7F, 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 119 'w' */
    {0x00, 0x00, 0x00, 0x00, 0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 120 'x' */
    {0x00, 0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x7C, 0x60, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 121 'y' */
    {0x00, 0x00, 0x00, 0x00, 0x7E, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 122 'z' */
    {0x00, 0x70, 0x18, 0x18, 0x18, 0x0E, 0x18, 0x18, 0x18, 0x18, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 123 '{' */
    {0x00, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 124 '|' */
    {0x00, 0x0E, 0x18, 0x18, 0x18, 0x70, 0x18, 0x18, 0x18, 0x18, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 125 '}' */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x3B, 0x6E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 126 '~' */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* 127 */
};

/* ── Dynamic font state ──────────────────────────────────────────────────
 * Defaults to the built-in 8×16 LSB-first font.  Replaced by console_load_psf(). */

#define FONT_BUF_SIZE 65536u
static uint8_t   g_font_buf[FONT_BUF_SIZE];

static uint32_t       g_fw      = 8u;
static uint32_t       g_fh      = 16u;
static uint32_t       g_fbpg    = 16u;   /* bytes per glyph */
static uint32_t       g_fglyphs = 128u;
static bool           g_fmsb    = false; /* false=LSB-first (built-in), true=MSB-first (PSF) */
static const uint8_t *g_fdata   = (const uint8_t *)fifi_font8x16;
static char           g_fname[64] = "default";

/* ── Cell buffer ─────────────────────────────────────────────────────────
 * All text is stored in a RAM cell buffer. Scrolling shifts the cell buffer
 * (fast — it's regular RAM) and re-renders to the framebuffer using WRITE-ONLY
 * access. This avoids reading from video RAM, which is extremely slow on real
 * hardware (every read is an uncached PCIe round-trip that causes visible
 * "raster scanning" artifacts during scroll). */

#define CELL_MAX_COLS 240
#define CELL_MAX_ROWS 135   /* enough for 1920x1080 at 8x16 = 67 rows */

static struct {
    volatile uint32_t *pix;
    uint64_t pitch32;
    uint64_t w, h;

    uint64_t cx, cy;        /* cursor in character cells */
    uint64_t cols, rows;    /* visible cell grid dimensions */
    uint64_t y_offset;      /* reserved pixel rows at top (status bar / window y) */
    uint64_t x_off;         /* pixel x where text area starts */
    uint64_t vp_h;          /* viewport height in pixels */
    uint32_t fg;
    uint32_t bg;
    bool initialized;
} con;

static uint8_t cell_buf[CELL_MAX_ROWS][CELL_MAX_COLS];

/* ── Double buffer ───────────────────────────────────────────────────────────
 * g_back: regular RAM mirror of the framebuffer.  All rendering goes here.
 * console_flip_if_dirty() copies it to VRAM in one pass to eliminate tearing. */
static uint32_t *g_back     = NULL;
static bool      g_dirty    = false;
static uint32_t  g_dirty_y0 = 0xFFFFFFFFu;  /* min dirty row */
static uint32_t  g_dirty_y1 = 0u;            /* max dirty row (exclusive) */

/* ── Terminal scrollback ring buffer ────────────────────────────────────── */
static uint8_t  g_tsb_ring[CONSOLE_TSB_CAP];
static uint32_t g_tsb_head  = 0;  /* oldest byte index in ring */
static uint32_t g_tsb_used  = 0;  /* bytes currently stored */
static bool     g_tsb_suppress = false;

/* ── ANSI VT100 SGR colour state machine ─────────────────────────────────
 * Parses ESC [ <param> ; ... m   (SGR sequences only).
 * Supported codes:
 *   0          reset to defaults
 *   1          bold → use bright-fg variant
 *   30-37      set fg (standard 8 colours)
 *   40-47      set bg (standard 8 colours)
 *   90-97      set fg (bright 8 colours)
 *   100-107    set bg (bright 8 colours)   */
typedef enum { ANSI_NORM, ANSI_ESC, ANSI_CSI, ANSI_OSC, ANSI_CHARSET } ansi_state_t;
static ansi_state_t g_ansi_st   = ANSI_NORM;
static uint8_t      g_ansi_buf[64];
static uint8_t      g_ansi_len  = 0;
static uint32_t     g_ansi_fg0  = 0x00FFFFFFu; /* default fg */
static uint32_t     g_ansi_bg0  = 0x00101010u; /* default bg */
static bool         g_ansi_bold = false;

/* Standard 8 ANSI foreground colours (dark theme palette) */
static const uint32_t ansi_fg_std[8] = {
    0x00202020u, /* 0: black  */
    0x00cc4444u, /* 1: red    */
    0x0044cc66u, /* 2: green  */
    0x00ddaa22u, /* 3: yellow */
    0x004488ddu, /* 4: blue   */
    0x00bb66ddu, /* 5: magenta*/
    0x0033bbddu, /* 6: cyan   */
    0x00ccccccu, /* 7: white  */
};
/* Bright variants */
static const uint32_t ansi_fg_brt[8] = {
    0x00505050u, /* 0: bright black (gray) */
    0x00ff6666u, /* 1: bright red   */
    0x0066ff88u, /* 2: bright green */
    0x00ffdd44u, /* 3: bright yellow*/
    0x0066aaffu, /* 4: bright blue  */
    0x00dd88ffu, /* 5: bright magenta */
    0x0055eeffu, /* 6: bright cyan  */
    0x00ffffffu, /* 7: bright white */
};
/* Background colours (darker versions) */
static const uint32_t ansi_bg_std[8] = {
    0x00000000u, /* 0: black  */
    0x00330808u, /* 1: dark red   */
    0x00083308u, /* 2: dark green */
    0x00332200u, /* 3: dark yellow*/
    0x00081830u, /* 4: dark blue  */
    0x00200830u, /* 5: dark magenta*/
    0x00083028u, /* 6: dark cyan  */
    0x00202020u, /* 7: gray       */
};
static const uint32_t ansi_bg_brt[8] = {
    0x00303030u, /* bright black */
    0x00661010u, /* bright red   */
    0x00106610u, /* bright green */
    0x00664400u, /* bright yellow*/
    0x00103060u, /* bright blue  */
    0x00401060u, /* bright magenta*/
    0x00106050u, /* bright cyan  */
    0x00606060u, /* bright white */
};

static uint32_t ansi_256_color(uint8_t n) {
    if (n < 8u)  return ansi_fg_std[n];
    if (n < 16u) return ansi_fg_brt[n - 8u];
    if (n < 232u) {
        uint8_t i = n - 16u;
        uint8_t b = i % 6u, g = (i / 6u) % 6u, r = i / 36u;
        uint8_t rv = r ? (uint8_t)(95u + (r-1u)*40u) : 0u;
        uint8_t gv = g ? (uint8_t)(95u + (g-1u)*40u) : 0u;
        uint8_t bv = b ? (uint8_t)(95u + (b-1u)*40u) : 0u;
        return ((uint32_t)rv << 16) | ((uint32_t)gv << 8) | bv;
    }
    uint8_t v = (uint8_t)(8u + (uint8_t)(n - 232u) * 10u);
    return ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
}

static void ansi_apply_sgr(uint8_t *params, int n) {
    for (int pi = 0; pi < n; pi++) {
        uint8_t p = params[pi];
        if (p == 0) {
            con.fg = g_ansi_fg0; con.bg = g_ansi_bg0; g_ansi_bold = false;
        } else if (p == 1) {
            g_ansi_bold = true;
        } else if (p == 22u) {
            g_ansi_bold = false;
        } else if (p >= 30u && p <= 37u) {
            con.fg = g_ansi_bold ? ansi_fg_brt[p - 30u] : ansi_fg_std[p - 30u];
        } else if (p == 39u) {
            con.fg = g_ansi_fg0;
        } else if (p >= 40u && p <= 47u) {
            con.bg = ansi_bg_std[p - 40u];
        } else if (p == 49u) {
            con.bg = g_ansi_bg0;
        } else if (p >= 90u && p <= 97u) {
            con.fg = ansi_fg_brt[p - 90u];
        } else if (p >= 100u && p <= 107u) {
            con.bg = ansi_bg_brt[p - 100u];
        } else if ((p == 38u || p == 48u) && pi + 1 < n) {
            bool is_fg = (p == 38u);
            uint8_t sub = params[pi + 1];
            if (sub == 5u && pi + 2 < n) {
                uint32_t c = ansi_256_color(params[pi + 2]);
                if (is_fg) con.fg = c; else con.bg = c;
                pi += 2;
            } else if (sub == 2u && pi + 4 < n) {
                uint32_t c = ((uint32_t)params[pi+2] << 16) | ((uint32_t)params[pi+3] << 8) | params[pi+4];
                if (is_fg) con.fg = c; else con.bg = c;
                pi += 4;
            }
        }
    }
}

/* Forward declarations for functions defined later in this file */
static void fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t c);
static void render_char(uint64_t cell_x, uint64_t cell_y, unsigned char uc);

/* Alt-screen state (for ?1049h/l) */
static uint8_t  g_alt_buf[CELL_MAX_ROWS][CELL_MAX_COLS];
static uint64_t g_alt_cx = 0, g_alt_cy = 0;
static uint32_t g_alt_fg = 0x00FFFFFFu, g_alt_bg = 0x00101010u;
static bool     g_in_altscreen = false;
static bool     g_cursor_vis = true;

static void ansi_process_csi(void) {
    uint16_t params[16] = {0};
    int      np = 0;
    uint16_t cur = 0;
    bool     private_mode = false;
    for (int i = 0; i < (int)g_ansi_len - 1; i++) {
        uint8_t c2 = g_ansi_buf[i];
        if (c2 == '?') { private_mode = true; continue; }
        if (c2 >= '0' && c2 <= '9') {
            cur = (uint16_t)(cur * 10u + (c2 - '0'));
        } else if (c2 == ';') {
            if (np < 16) params[np++] = cur;
            cur = 0;
        }
    }
    if (np < 16) params[np++] = cur;

    uint8_t final_ch = g_ansi_len > 0 ? g_ansi_buf[g_ansi_len - 1u] : 0u;

    if (private_mode) {
        if (final_ch == 'h' || final_ch == 'l') {
            bool on = (final_ch == 'h');
            for (int pi = 0; pi < np; pi++) {
                switch (params[pi]) {
                case 25u:   g_cursor_vis = on; break;
                case 1049u:
                    if (on) {
                        /* Enter alt screen: save main screen, clear */
                        memcpy(g_alt_buf, cell_buf, sizeof(cell_buf));
                        g_alt_cx = con.cx; g_alt_cy = con.cy;
                        g_alt_fg = con.fg; g_alt_bg = con.bg;
                        g_in_altscreen = true;
                        fill_rect(con.x_off, con.y_offset, con.cols * g_fw, con.vp_h, con.bg);
                        memset(cell_buf, ' ', sizeof(cell_buf));
                        con.cx = 0; con.cy = 0;
                    } else {
                        /* Exit alt screen: restore main screen */
                        memcpy(cell_buf, g_alt_buf, sizeof(cell_buf));
                        con.cx = g_alt_cx; con.cy = g_alt_cy;
                        con.fg = g_alt_fg; con.bg = g_alt_bg;
                        g_in_altscreen = false;
                        fill_rect(con.x_off, con.y_offset, con.cols * g_fw, con.vp_h, con.bg);
                        uint64_t vr = (con.rows < CELL_MAX_ROWS) ? con.rows : CELL_MAX_ROWS;
                        uint64_t vc = (con.cols < CELL_MAX_COLS) ? con.cols : CELL_MAX_COLS;
                        for (uint64_t r = 0; r < vr; r++)
                            for (uint64_t c = 0; c < vc; c++)
                                render_char(c, r, cell_buf[r][c]);
                    }
                    break;
                }
            }
        }
        return;
    }

    if (final_ch == 'm') {
        uint8_t p8[16];
        for (int i = 0; i < np; i++) p8[i] = (uint8_t)(params[i] > 255u ? 255u : params[i]);
        ansi_apply_sgr(p8, np);
    } else if (final_ch == 'A') {          /* CUU — cursor up */
        uint16_t n = (params[0] > 0u) ? params[0] : 1u;
        con.cy = (con.cy >= n) ? con.cy - n : 0;
    } else if (final_ch == 'B') {          /* CUD — cursor down */
        uint16_t n = (params[0] > 0u) ? params[0] : 1u;
        con.cy += n;
        if (con.rows > 0 && con.cy >= con.rows) con.cy = con.rows - 1;
    } else if (final_ch == 'C') {          /* CUF — cursor forward */
        uint16_t n = (params[0] > 0u) ? params[0] : 1u;
        con.cx += n;
        if (con.cols > 0 && con.cx >= con.cols) con.cx = con.cols - 1;
    } else if (final_ch == 'D') {          /* CUB — cursor back */
        uint16_t n = (params[0] > 0u) ? params[0] : 1u;
        con.cx = (con.cx >= n) ? con.cx - n : 0;
    } else if (final_ch == 'E') {          /* CNL — cursor next line */
        uint16_t n = (params[0] > 0u) ? params[0] : 1u;
        con.cx = 0; con.cy += n;
        if (con.rows > 0 && con.cy >= con.rows) con.cy = con.rows - 1;
    } else if (final_ch == 'F') {          /* CPL — cursor prev line */
        uint16_t n = (params[0] > 0u) ? params[0] : 1u;
        con.cx = 0; con.cy = (con.cy >= n) ? con.cy - n : 0;
    } else if (final_ch == 'G') {          /* CHA — cursor horizontal absolute */
        uint16_t col = (params[0] > 0u) ? (uint16_t)(params[0] - 1u) : 0u;
        con.cx = (con.cols > 0 && col < con.cols) ? col : (con.cols > 0 ? con.cols - 1 : 0);
    } else if (final_ch == 'd') {          /* VPA — vertical position absolute */
        uint16_t row = (params[0] > 0u) ? (uint16_t)(params[0] - 1u) : 0u;
        con.cy = (con.rows > 0 && row < con.rows) ? row : (con.rows > 0 ? con.rows - 1 : 0);
    } else if (final_ch == 'H' || final_ch == 'f') {  /* CUP / HVP — cursor position */
        uint16_t row = (np >= 1 && params[0] > 0u) ? (uint16_t)(params[0] - 1u) : 0u;
        uint16_t col = (np >= 2 && params[1] > 0u) ? (uint16_t)(params[1] - 1u) : 0u;
        con.cy = (con.rows > 0 && row < con.rows) ? row : (con.rows > 0 ? con.rows - 1 : 0);
        con.cx = (con.cols > 0 && col < con.cols) ? col : (con.cols > 0 ? con.cols - 1 : 0);
    } else if (final_ch == 's') {          /* SCP — save cursor (ANSI) */
        g_alt_cx = con.cx; g_alt_cy = con.cy;
    } else if (final_ch == 'u') {          /* RCP — restore cursor (ANSI) */
        if (g_alt_cx < con.cols) con.cx = g_alt_cx;
        if (g_alt_cy < con.rows) con.cy = g_alt_cy;
    } else if (final_ch == 'J') {          /* ED — erase display */
        uint16_t mode = params[0];
        if (mode == 2u || mode == 3u) {
            fill_rect(con.x_off, con.y_offset, con.cols * g_fw, con.vp_h, con.bg);
            con.cx = 0; con.cy = 0;
            memset(cell_buf, ' ', sizeof(cell_buf));
        } else if (mode == 0u) {
            if (con.cols > con.cx)
                fill_rect(con.x_off + con.cx * g_fw, con.y_offset + con.cy * g_fh,
                          (con.cols - con.cx) * g_fw, g_fh, con.bg);
            for (uint64_t c = con.cx; c < con.cols && c < CELL_MAX_COLS; c++)
                cell_buf[con.cy][c] = ' ';
            if (con.cy + 1 < con.rows) {
                fill_rect(con.x_off, con.y_offset + (con.cy + 1u) * g_fh,
                          con.cols * g_fw, (con.rows - con.cy - 1u) * g_fh, con.bg);
                for (uint64_t r = con.cy + 1; r < con.rows && r < CELL_MAX_ROWS; r++)
                    for (uint64_t c = 0; c < con.cols && c < CELL_MAX_COLS; c++)
                        cell_buf[r][c] = ' ';
            }
        } else if (mode == 1u) {
            if (con.cy > 0) {
                fill_rect(con.x_off, con.y_offset, con.cols * g_fw, con.cy * g_fh, con.bg);
                for (uint64_t r = 0; r < con.cy && r < CELL_MAX_ROWS; r++)
                    for (uint64_t c = 0; c < con.cols && c < CELL_MAX_COLS; c++)
                        cell_buf[r][c] = ' ';
            }
            fill_rect(con.x_off, con.y_offset + con.cy * g_fh, (con.cx + 1u) * g_fw, g_fh, con.bg);
            for (uint64_t c = 0; c <= con.cx && c < CELL_MAX_COLS; c++)
                cell_buf[con.cy][c] = ' ';
        }
    } else if (final_ch == 'K') {          /* EL — erase line */
        uint16_t mode = params[0];
        if (mode == 0u) {
            if (con.cols > con.cx)
                fill_rect(con.x_off + con.cx * g_fw, con.y_offset + con.cy * g_fh,
                          (con.cols - con.cx) * g_fw, g_fh, con.bg);
            for (uint64_t c = con.cx; c < con.cols && c < CELL_MAX_COLS; c++)
                cell_buf[con.cy][c] = ' ';
        } else if (mode == 1u) {
            fill_rect(con.x_off, con.y_offset + con.cy * g_fh, (con.cx + 1u) * g_fw, g_fh, con.bg);
            for (uint64_t c = 0; c <= con.cx && c < CELL_MAX_COLS; c++)
                cell_buf[con.cy][c] = ' ';
        } else if (mode == 2u) {
            fill_rect(con.x_off, con.y_offset + con.cy * g_fh, con.cols * g_fw, g_fh, con.bg);
            for (uint64_t c = 0; c < con.cols && c < CELL_MAX_COLS; c++)
                cell_buf[con.cy][c] = ' ';
        }
    } else if (final_ch == 'X') {              /* ECH — erase character */
        uint16_t n = (params[0] > 0u) ? params[0] : 1u;
        for (uint64_t i = con.cx; i < con.cx + n && i < con.cols && i < CELL_MAX_COLS; i++) {
            fill_rect(con.x_off + i * g_fw, con.y_offset + con.cy * g_fh, g_fw, g_fh, con.bg);
            cell_buf[con.cy][i] = ' ';
        }
    } else if (final_ch == 'L') {              /* IL — insert line */
        uint16_t n = (params[0] > 0u) ? params[0] : 1u;
        if (con.cy < con.rows && n > 0) {
            uint64_t move = (con.rows - con.cy > n) ? (con.rows - con.cy - n) : 0;
            if (move > 0)
                memmove(&cell_buf[con.cy + n], &cell_buf[con.cy],
                        sizeof(cell_buf[0]) * move);
            for (uint16_t i = 0; i < n && con.cy + i < con.rows; i++)
                memset(cell_buf[con.cy + i], ' ', sizeof(cell_buf[0]));
        }
    } else if (final_ch == 'M') {              /* DL — delete line */
        uint16_t n = (params[0] > 0u) ? params[0] : 1u;
        if (con.cy < con.rows && n > 0) {
            uint64_t move = (con.rows > con.cy + n) ? (con.rows - con.cy - n) : 0;
            if (move > 0)
                memmove(&cell_buf[con.cy], &cell_buf[con.cy + n],
                        sizeof(cell_buf[0]) * move);
            for (uint64_t i = (con.cy + move); i < con.rows && i < CELL_MAX_ROWS; i++)
                memset(cell_buf[i], ' ', sizeof(cell_buf[0]));
        }
    }
    /* Other CSI sequences silently consumed */
}

void console_set_suppress_draw(bool on) { g_tsb_suppress = on; }

static inline uint8_t tsb_at(uint32_t pos) {
    return g_tsb_ring[(g_tsb_head + pos) % CONSOLE_TSB_CAP];
}

static void tsb_push(uint8_t c) {
    if (g_tsb_used < CONSOLE_TSB_CAP) {
        g_tsb_ring[(g_tsb_head + g_tsb_used) % CONSOLE_TSB_CAP] = c;
        g_tsb_used++;
    } else {
        /* Ring full: overwrite oldest */
        g_tsb_ring[g_tsb_head] = c;
        g_tsb_head = (g_tsb_head + 1u) % CONSOLE_TSB_CAP;
    }
}

int console_tsb_count_lines(void) {
    int n = 0;
    for (uint32_t i = 0; i < g_tsb_used; i++)
        if (tsb_at(i) == '\n') n++;
    return n;
}

int console_tsb_get_line(int line_from_end, char *buf, int maxlen) {
    if (maxlen <= 0 || !buf || g_tsb_used == 0) { if (buf && maxlen > 0) buf[0]='\0'; return 0; }
    int32_t pos = (int32_t)g_tsb_used - 1;
    /* Skip trailing newline(s) at end of buffer */
    while (pos >= 0 && tsb_at((uint32_t)pos) == '\n') pos--;
    /* Walk back counting newlines to find our target line */
    int nl_seen = 0;
    while (pos >= 0 && nl_seen < line_from_end) {
        if (tsb_at((uint32_t)pos) == '\n') nl_seen++;
        pos--;
    }
    if (pos < 0) { buf[0] = '\0'; return 0; }
    /* pos is now at the last char of the target line */
    int32_t end = pos;
    /* Walk back to find the start of this line */
    while (pos > 0 && tsb_at((uint32_t)(pos - 1)) != '\n') pos--;
    int32_t line_start = pos;
    int len = (int)(end - line_start + 1);
    if (len < 0) len = 0;
    if (len > maxlen - 1) len = maxlen - 1;
    for (int i = 0; i < len; i++)
        buf[i] = (char)tsb_at((uint32_t)(line_start + (int32_t)i));
    buf[len] = '\0';
    return len;
}

/* ── Framebuffer rendering (write-only — never reads video RAM) ─────────── */

static void render_char(uint64_t cell_x, uint64_t cell_y, unsigned char uc) {
    const uint64_t px = con.x_off + cell_x * g_fw;
    const uint64_t py = cell_y * g_fh + con.y_offset;
    if (px + g_fw > con.w || py + g_fh > con.h) return;

    uint32_t gi = (uc < g_fglyphs) ? uc : ('?' < g_fglyphs ? '?' : 0u);
    const uint8_t *glyph = g_fdata + (uint64_t)gi * g_fbpg;
    uint32_t bpr = (g_fw + 7u) / 8u;

    if (g_back) {
        g_dirty = true;
        if ((uint32_t)py < g_dirty_y0) g_dirty_y0 = (uint32_t)py;
        if ((uint32_t)(py + g_fh) > g_dirty_y1) g_dirty_y1 = (uint32_t)(py + g_fh);
        for (uint32_t y = 0; y < g_fh; y++) {
            uint32_t *row = g_back + (py + y) * con.pitch32 + px;
            const uint8_t *scan = glyph + y * bpr;
            for (uint32_t x = 0; x < g_fw; x++) {
                uint8_t b = scan[x >> 3];
                uint32_t bit = g_fmsb ? ((b >> (7u - (x & 7u))) & 1u)
                                      : ((b >>        (x & 7u) ) & 1u);
                row[x] = bit ? con.fg : con.bg;
            }
        }
    } else {
        for (uint32_t y = 0; y < g_fh; y++) {
            volatile uint32_t *row = con.pix + (py + y) * con.pitch32 + px;
            const uint8_t *scan = glyph + y * bpr;
            for (uint32_t x = 0; x < g_fw; x++) {
                uint8_t b = scan[x >> 3];
                uint32_t bit = g_fmsb ? ((b >> (7u - (x & 7u))) & 1u)
                                      : ((b >>        (x & 7u) ) & 1u);
                row[x] = bit ? con.fg : con.bg;
            }
        }
    }
}

static void fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t c) {
    if (x >= con.w || y >= con.h || w == 0 || h == 0) return;
    if (x + w > con.w) w = con.w - x;
    if (y + h > con.h) h = con.h - y;
    if (g_back) {
        g_dirty = true;
        if ((uint32_t)y < g_dirty_y0) g_dirty_y0 = (uint32_t)y;
        if ((uint32_t)(y + h) > g_dirty_y1) g_dirty_y1 = (uint32_t)(y + h);
        for (uint64_t yy = 0; yy < h; yy++) {
            uint32_t *row = g_back + (y + yy) * con.pitch32 + x;
            for (uint64_t xx = 0; xx < w; xx++)
                row[xx] = c;
        }
    } else {
        for (uint64_t yy = 0; yy < h; yy++) {
            volatile uint32_t *row = con.pix + (y + yy) * con.pitch32 + x;
            for (uint64_t xx = 0; xx < w; xx++)
                row[xx] = c;
        }
    }
}

/* ── Scroll: shift cell buffer in RAM, re-render to framebuffer ─────────── */

static void scroll_one_line(void) {
    if (con.rows <= 1) return;

    uint64_t vr = (con.rows < CELL_MAX_ROWS) ? con.rows : CELL_MAX_ROWS;

#ifdef __linux__
    /* Fast path: shift pixels directly in the back buffer, no glyph decode */
    memmove(cell_buf[0], cell_buf[1], (vr - 1) * CELL_MAX_COLS);
    memset(cell_buf[vr - 1], ' ', CELL_MAX_COLS);

    if (g_back && g_fh > 0) {
        uint64_t vp_w   = con.cols * g_fw;
        uint64_t stride = con.pitch32 * sizeof(uint32_t);

        /* Shift all pixel rows in the terminal viewport up by one character row */
        for (uint64_t r = 0; r < vr - 1; r++) {
            uint8_t *dst = (uint8_t *)(g_back + (con.y_offset + r * g_fh) * con.pitch32 + con.x_off);
            const uint8_t *src = (const uint8_t *)(g_back + (con.y_offset + (r + 1) * g_fh) * con.pitch32 + con.x_off);
            for (uint32_t yy = 0; yy < g_fh; yy++)
                memcpy(dst + yy * stride, src + yy * stride, vp_w * sizeof(uint32_t));
        }
        /* Clear last character row */
        fill_rect(con.x_off, con.y_offset + (vr - 1) * g_fh, vp_w, g_fh, con.bg);
        /* Mark full viewport dirty — memmove above shifted all pixel rows */
        if ((uint32_t)con.y_offset < g_dirty_y0) g_dirty_y0 = (uint32_t)con.y_offset;
        uint32_t _sy1 = (uint32_t)(con.y_offset + vr * g_fh);
        if (_sy1 > g_dirty_y1) g_dirty_y1 = _sy1;
    } else {
        /* No back buffer: re-render from cell buf (bare-metal path) */
        uint64_t vc = (con.cols < CELL_MAX_COLS) ? con.cols : CELL_MAX_COLS;
        for (uint64_t r = 0; r < vr; r++)
            for (uint64_t c = 0; c < vc; c++)
                render_char(c, r, cell_buf[r][c]);
    }
#else
    uint64_t vc = (con.cols < CELL_MAX_COLS) ? con.cols : CELL_MAX_COLS;
    for (uint64_t r = 0; r + 1 < vr; r++)
        for (uint64_t c = 0; c < vc; c++)
            cell_buf[r][c] = cell_buf[r + 1][c];
    for (uint64_t c = 0; c < vc; c++)
        cell_buf[vr - 1][c] = ' ';
    for (uint64_t r = 0; r < vr; r++)
        for (uint64_t c = 0; c < vc; c++)
            render_char(c, r, cell_buf[r][c]);
#endif

    /* Clear any fractional pixel rows below the last cell row */
    uint64_t used_h = vr * g_fh;
    if (used_h < con.vp_h)
        fill_rect(con.x_off, con.y_offset + used_h,
                  con.cols * g_fw, con.vp_h - used_h, con.bg);
}

static void ensure_cursor_visible(void) {
    if (con.cols == 0 || con.rows == 0) return;

    if (con.cx >= con.cols) {
        con.cx = 0;
        con.cy++;
    }

    while (con.cy >= con.rows) {
        scroll_one_line();
        con.cy = con.rows - 1;
    }
}

static void draw_char_at(uint64_t cell_x, uint64_t cell_y, unsigned char uc) {
    if (cell_x < CELL_MAX_COLS && cell_y < CELL_MAX_ROWS)
        cell_buf[cell_y][cell_x] = uc;
    if (con.cols == 0 || con.rows == 0 || cell_x >= con.cols || cell_y >= con.rows) return;
    render_char(cell_x, cell_y, uc);
}

bool console_ready(void) {
    return con.initialized;
}

void console_set_colors(uint32_t fg, uint32_t bg) {
    con.fg = fg;
    con.bg = bg;
}

void console_clear(void) {
    if (!con.initialized) return;
    fill_rect(con.x_off, con.y_offset, con.cols * g_fw, con.vp_h, con.bg);
    con.cx = 0;
    con.cy = 0;
    for (uint64_t r = 0; r < CELL_MAX_ROWS; r++)
        for (uint64_t c = 0; c < CELL_MAX_COLS; c++)
            cell_buf[r][c] = ' ';
}

void console_init(struct limine_framebuffer *fb) {
    con.pix = (volatile uint32_t *)fb->address;
    con.pitch32 = fb->pitch / 4;
    con.w = fb->width;
    con.h = fb->height;
    con.y_offset = 0;
    con.x_off = 0;
    con.vp_h = con.h;
    con.cols = con.w / g_fw;
    con.rows = con.h / g_fh;
    con.cx = 0;
    con.cy = 0;

    if (con.cols > CELL_MAX_COLS) con.cols = CELL_MAX_COLS;
    if (con.rows > CELL_MAX_ROWS) con.rows = CELL_MAX_ROWS;

    con.fg = 0x00FFFFFFu; /* white */
    con.bg = 0x00101010u; /* near-black */
    g_ansi_fg0 = con.fg;
    g_ansi_bg0 = con.bg;
    g_ansi_st  = ANSI_NORM;
    g_ansi_len = 0;

    con.initialized = true;
    console_clear();
}

/* ── Status bar support ───────────────────────────────────────────────────── */

void console_set_y_offset(uint64_t offset) {
    if (!con.initialized) return;
    con.y_offset = offset;
    con.vp_h = con.h - offset;
    con.cols = (con.w - con.x_off) / g_fw;
    con.rows = con.vp_h / g_fh;
    if (con.cols > CELL_MAX_COLS) con.cols = CELL_MAX_COLS;
    if (con.rows > CELL_MAX_ROWS) con.rows = CELL_MAX_ROWS;
    if (con.cy >= con.rows && con.rows > 0) con.cy = con.rows - 1;
    /* Re-render cell buffer at the new pixel offset */
    for (uint64_t r = 0; r < con.rows; r++)
        for (uint64_t c = 0; c < con.cols; c++)
            render_char(c, r, cell_buf[r][c]);
}

void console_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t color) {
    fill_rect(x, y, w, h, color);
}

/* Alpha-blend a colored rect over the current backbuffer contents.
 * alpha 0..255 (255 = opaque). Falls back to a solid fill without a backbuffer
 * (blending against VRAM reads would be slow on real hardware). */
void console_blend_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h,
                        uint32_t color, uint8_t alpha) {
    if (x >= con.w || y >= con.h || w == 0 || h == 0) return;
    if (x + w > con.w) w = con.w - x;
    if (y + h > con.h) h = con.h - y;
    if (!g_back || alpha >= 250u) { fill_rect(x, y, w, h, color); return; }
    if (alpha == 0u) return;
    uint32_t a  = alpha, ia = 255u - alpha;
    uint32_t sr = ((color >> 16) & 0xffu) * a;
    uint32_t sg = ((color >>  8) & 0xffu) * a;
    uint32_t sb = ( color        & 0xffu) * a;
    g_dirty = true;
    if ((uint32_t)y < g_dirty_y0) g_dirty_y0 = (uint32_t)y;
    if ((uint32_t)(y + h) > g_dirty_y1) g_dirty_y1 = (uint32_t)(y + h);
    for (uint64_t yy = 0; yy < h; yy++) {
        uint32_t *row = g_back + (y + yy) * con.pitch32 + x;
        for (uint64_t xx = 0; xx < w; xx++) {
            uint32_t d = row[xx];
            uint32_t r = (sr + ((d >> 16) & 0xffu) * ia) >> 8;
            uint32_t g = (sg + ((d >>  8) & 0xffu) * ia) >> 8;
            uint32_t b = (sb + ( d        & 0xffu) * ia) >> 8;
            row[xx] = (r << 16) | (g << 8) | b;
        }
    }
}

/* Vertical gradient fill: c0 at the top row, c1 at the bottom row. */
void console_fill_vgrad(uint64_t x, uint64_t y, uint64_t w, uint64_t h,
                        uint32_t c0, uint32_t c1) {
    if (h == 0) return;
    uint32_t r0 = (c0 >> 16) & 0xffu, g0 = (c0 >> 8) & 0xffu, b0 = c0 & 0xffu;
    uint32_t r1 = (c1 >> 16) & 0xffu, g1 = (c1 >> 8) & 0xffu, b1 = c1 & 0xffu;
    uint64_t hm1 = h > 1u ? h - 1u : 1u;
    for (uint64_t yy = 0; yy < h; yy++) {
        uint32_t r = (uint32_t)(r0 + (int64_t)((int64_t)r1 - (int64_t)r0) * (int64_t)yy / (int64_t)hm1);
        uint32_t g = (uint32_t)(g0 + (int64_t)((int64_t)g1 - (int64_t)g0) * (int64_t)yy / (int64_t)hm1);
        uint32_t b = (uint32_t)(b0 + (int64_t)((int64_t)b1 - (int64_t)b0) * (int64_t)yy / (int64_t)hm1);
        fill_rect(x, y + yy, w, 1u, (r << 16) | (g << 8) | b);
    }
}

/* Render a single glyph at absolute pixel coordinates (bypasses cell buffer). */
void console_render_glyph(uint64_t px, uint64_t py, unsigned char ch, uint32_t fg, uint32_t bg) {
    if (!con.initialized) return;
    uint32_t gi = (ch < g_fglyphs) ? ch : ('?' < g_fglyphs ? '?' : 0u);
    if (px + g_fw > con.w || py + g_fh > con.h) return;
    const uint8_t *glyph = g_fdata + (uint64_t)gi * g_fbpg;
    uint32_t bpr = (g_fw + 7u) / 8u;
    if (g_back) {
        g_dirty = true;
        if ((uint32_t)py < g_dirty_y0) g_dirty_y0 = (uint32_t)py;
        if ((uint32_t)(py + g_fh) > g_dirty_y1) g_dirty_y1 = (uint32_t)(py + g_fh);
        for (uint32_t y = 0; y < g_fh; y++) {
            uint32_t *row = g_back + (py + y) * con.pitch32 + px;
            const uint8_t *scan = glyph + y * bpr;
            for (uint32_t x = 0; x < g_fw; x++) {
                uint8_t b = scan[x >> 3];
                uint32_t bit = g_fmsb ? ((b >> (7u - (x & 7u))) & 1u)
                                      : ((b >>        (x & 7u) ) & 1u);
                row[x] = bit ? fg : bg;
            }
        }
    } else {
        for (uint32_t y = 0; y < g_fh; y++) {
            volatile uint32_t *row = con.pix + (py + y) * con.pitch32 + px;
            const uint8_t *scan = glyph + y * bpr;
            for (uint32_t x = 0; x < g_fw; x++) {
                uint8_t b = scan[x >> 3];
                uint32_t bit = g_fmsb ? ((b >> (7u - (x & 7u))) & 1u)
                                      : ((b >>        (x & 7u) ) & 1u);
                row[x] = bit ? fg : bg;
            }
        }
    }
}

/* Transparent-background glyph: writes ONLY the lit (foreground) pixels, leaving the
 * existing background untouched. Used for title-bar text so the glyph cell's background
 * never paints past the title bar, and it's cheaper (skips background writes). */
void console_render_glyph_fg(uint64_t px, uint64_t py, unsigned char ch, uint32_t fg) {
    if (!con.initialized) return;
    uint32_t gi = (ch < g_fglyphs) ? ch : ('?' < g_fglyphs ? '?' : 0u);
    if (px + g_fw > con.w || py + g_fh > con.h) return;
    const uint8_t *glyph = g_fdata + (uint64_t)gi * g_fbpg;
    uint32_t bpr = (g_fw + 7u) / 8u;
    uint32_t *dst_base = g_back ? g_back : NULL;
    if (dst_base) {
        g_dirty = true;
        if ((uint32_t)py < g_dirty_y0) g_dirty_y0 = (uint32_t)py;
        if ((uint32_t)(py + g_fh) > g_dirty_y1) g_dirty_y1 = (uint32_t)(py + g_fh);
    }
    for (uint32_t y = 0; y < g_fh; y++) {
        const uint8_t *scan = glyph + y * bpr;
        if (dst_base) {
            uint32_t *row = dst_base + (py + y) * con.pitch32 + px;
            for (uint32_t x = 0; x < g_fw; x++) {
                uint8_t b = scan[x >> 3];
                uint32_t bit = g_fmsb ? ((b >> (7u - (x & 7u))) & 1u) : ((b >> (x & 7u)) & 1u);
                if (bit) row[x] = fg;
            }
        } else {
            volatile uint32_t *row = con.pix + (py + y) * con.pitch32 + px;
            for (uint32_t x = 0; x < g_fw; x++) {
                uint8_t b = scan[x >> 3];
                uint32_t bit = g_fmsb ? ((b >> (7u - (x & 7u))) & 1u) : ((b >> (x & 7u)) & 1u);
                if (bit) row[x] = fg;
            }
        }
    }
}

uint64_t console_fb_width(void)           { return con.w; }
uint64_t console_fb_height(void)          { return con.h; }
uint64_t console_cols(void)               { return con.cols; }
uint64_t console_rows(void)               { return con.rows; }
uint64_t console_viewport_x(void)         { return con.x_off; }
uint64_t console_viewport_y(void)         { return con.y_offset; }
volatile uint32_t *console_fb_ptr(void)   { return con.pix; }
uint64_t           console_pitch32(void)  { return con.pitch32; }

/* Render a glyph at (scale)x size — each font pixel becomes a scale×scale block. */
void console_render_glyph_scaled(uint64_t px, uint64_t py, unsigned char ch,
                                  uint64_t scale, uint32_t fg, uint32_t bg) {
    if (!con.initialized || scale == 0) return;
    uint32_t gi = (ch < g_fglyphs) ? ch : ('?' < g_fglyphs ? '?' : 0u);
    const uint8_t *glyph = g_fdata + (uint64_t)gi * g_fbpg;
    uint32_t bpr = (g_fw + 7u) / 8u;
    uint32_t *target_back = g_back;
    if (target_back) {
        g_dirty = true;
        if ((uint32_t)py < g_dirty_y0) g_dirty_y0 = (uint32_t)py;
        uint32_t _rsy1 = (uint32_t)(py + g_fh * scale);
        if (_rsy1 > g_dirty_y1) g_dirty_y1 = _rsy1;
    }
    for (uint32_t r = 0; r < g_fh; r++) {
        const uint8_t *scan = glyph + r * bpr;
        for (uint32_t col = 0; col < g_fw; col++) {
            uint8_t b = scan[col >> 3];
            uint32_t bit = g_fmsb ? ((b >> (7u - (col & 7u))) & 1u)
                                  : ((b >>        (col & 7u) ) & 1u);
            uint32_t color = bit ? fg : bg;
            for (uint64_t dy = 0; dy < scale; dy++) {
                for (uint64_t dx = 0; dx < scale; dx++) {
                    uint64_t ppx = px + col * scale + dx;
                    uint64_t ppy = py + r   * scale + dy;
                    if (ppx < con.w && ppy < con.h) {
                        if (target_back)
                            target_back[ppy * con.pitch32 + ppx] = color;
                        else
                            con.pix[ppy * con.pitch32 + ppx] = color;
                    }
                }
            }
        }
    }
}

/* UTF-8 decoder state. Accumulates continuation bytes. */
static uint32_t g_utf8_cp;
static int g_utf8_remain;
/* Display width of the most-recently decoded Unicode codepoint (1 or 2).
 * Set by the UTF-8 completion path so the cursor-advance step uses the
 * TRUE width of the original codepoint, NOT cp_width() of the ASCII fallback
 * that unicode_to_ascii() maps it to (which would always return 1). */
static int g_cell_w = 1;

/* ESC 7/8 save-restore cursor */
static uint64_t g_saved_cx = 0, g_saved_cy = 0;

/* Map Unicode codepoint to displayable ASCII (space for anything unmapped). */
static uint8_t unicode_to_ascii(uint32_t cp) {
    if (cp < 0x80u) return (uint8_t)cp;
    /* Box drawing — all corners/junctions → + */
    if (cp >= 0x250Cu && cp <= 0x254Bu) return '+';
    /* Double-line box drawing */
    if (cp >= 0x2552u && cp <= 0x256Cu) return '+';
    /* Rounded corners ╭ ╮ ╯ ╰ */
    if (cp >= 0x256Du && cp <= 0x2570u) return '+';
    /* Diagonals ╱ ╲ ╳ */
    if (cp == 0x2571u) return '/';
    if (cp == 0x2572u) return '\\';
    if (cp == 0x2573u) return 'X';
    /* Partial box lines ╴..╿ — horizontal (even) → -, vertical (odd) → | */
    if (cp >= 0x2574u && cp <= 0x257Fu) return (cp & 1u) ? '|' : '-';
    /* Braille patterns (progress spinners) */
    if (cp >= 0x2800u && cp <= 0x28FFu) return '*';
    /* Latin-1 accented letters → base ASCII */
    if (cp == 0x00A0u) return ' ';
    if (cp >= 0x00C0u && cp <= 0x00C6u) return 'A';
    if (cp == 0x00C7u) return 'C';
    if (cp >= 0x00C8u && cp <= 0x00CBu) return 'E';
    if (cp >= 0x00CCu && cp <= 0x00CFu) return 'I';
    if (cp == 0x00D0u) return 'D';
    if (cp == 0x00D1u) return 'N';
    if (cp >= 0x00D2u && cp <= 0x00D6u) return 'O';
    if (cp == 0x00D7u) return 'x';
    if (cp == 0x00D8u) return 'O';
    if (cp >= 0x00D9u && cp <= 0x00DCu) return 'U';
    if (cp == 0x00DDu) return 'Y';
    if (cp == 0x00DFu) return 's';
    if (cp >= 0x00E0u && cp <= 0x00E6u) return 'a';
    if (cp == 0x00E7u) return 'c';
    if (cp >= 0x00E8u && cp <= 0x00EBu) return 'e';
    if (cp >= 0x00ECu && cp <= 0x00EFu) return 'i';
    if (cp == 0x00F0u) return 'd';
    if (cp == 0x00F1u) return 'n';
    if (cp >= 0x00F2u && cp <= 0x00F6u) return 'o';
    if (cp == 0x00F7u) return '/';
    if (cp == 0x00F8u) return 'o';
    if (cp >= 0x00F9u && cp <= 0x00FCu) return 'u';
    if (cp == 0x00FDu || cp == 0x00FFu) return 'y';
    switch (cp) {
    /* Box line variants */
    case 0x2500u: case 0x2501u:
    case 0x2504u: case 0x2505u: case 0x2508u: case 0x2509u:
    case 0x254Cu: case 0x254Du: case 0x2550u: return '-';
    case 0x2502u: case 0x2503u:
    case 0x2506u: case 0x2507u: case 0x250Au: case 0x250Bu:
    case 0x254Eu: case 0x254Fu: case 0x2551u: return '|';
    /* Block elements */
    case 0x2580u: case 0x2584u: case 0x2588u:
    case 0x258Cu: case 0x2590u:
    case 0x2591u: case 0x2592u: case 0x2593u: case 0x2594u: case 0x2595u:
    case 0x25A0u: case 0x25A1u: return '#';
    /* Arrows */
    case 0x2190u: case 0x21D0u: case 0x27F5u: return '<';
    case 0x2192u: case 0x21D2u: case 0x27F6u: return '>';
    case 0x2191u: case 0x21D1u: return '^';
    case 0x2193u: case 0x21D3u: return 'v';
    case 0x21B5u: case 0x23CEu: return '<';
    /* Geometric shapes */
    case 0x25C6u: case 0x25C7u: case 0x25C8u: case 0x25C9u: return '*';
    case 0x25CFu: case 0x25CBu: case 0x25CCu: return 'o';
    case 0x25B2u: case 0x25B3u: return '^';
    case 0x25BCu: case 0x25BDu: return 'v';
    case 0x25B6u: return '>';
    case 0x25C0u: return '<';
    /* Bullets and dots */
    case 0x2022u: case 0x2023u: case 0x00B7u: case 0x2027u: case 0x2219u: return '.';
    /* Check marks, crosses */
    case 0x2713u: case 0x2714u: case 0x2705u: return '+';
    case 0x2717u: case 0x2718u: case 0x274Cu: return 'x';
    /* Misc punctuation */
    case 0x2026u: return '.';
    case 0x2014u: case 0x2013u: return '-';
    case 0x2018u: case 0x2019u: case 0x201Cu: case 0x201Du: return '"';
    /* Stars */
    case 0x2605u: case 0x2606u: return '*';
    /* Warning / info */
    case 0x26A0u: return '!';
    case 0x2139u: return 'i';
    /* Misc symbols Claude Code uses */
    case 0x25E6u: return 'o';
    case 0x2794u: return '>';
    default: return ' ';
    }
}

/* Display width: 0=combining/zero-width, 1=normal, 2=wide (CJK/emoji). */
static int cp_width(uint32_t cp) {
    /* Zero-width: combining marks, joiners, variation selectors, ZWSP/BOM */
    if (cp == 0x200Bu || cp == 0x200Cu || cp == 0x200Du || cp == 0xFEFFu ||
        (cp >= 0x0300u && cp <= 0x036Fu) || (cp >= 0x0483u && cp <= 0x0489u) ||
        (cp >= 0x0591u && cp <= 0x05BDu) || (cp >= 0x0610u && cp <= 0x061Au) ||
        (cp >= 0x064Bu && cp <= 0x065Fu) || (cp >= 0x06D6u && cp <= 0x06DCu) ||
        (cp >= 0x1AB0u && cp <= 0x1AFFu) || (cp >= 0x1DC0u && cp <= 0x1DFFu) ||
        (cp >= 0x20D0u && cp <= 0x20FFu) || (cp >= 0xFE20u && cp <= 0xFE2Fu) ||
        (cp >= 0xFE00u && cp <= 0xFE0Fu) || (cp >= 0xE0100u && cp <= 0xE01EFu))
        return 0;
    /* Wide: East-Asian Wide & Fullwidth + supplementary-plane emoji */
    if ((cp >= 0x1100u && cp <= 0x115Fu) ||
        cp == 0x2329u || cp == 0x232Au ||
        (cp >= 0x2E80u && cp <= 0x303Eu) ||
        (cp >= 0x3041u && cp <= 0x33FFu) ||
        (cp >= 0x3400u && cp <= 0x4DBFu) ||
        (cp >= 0x4E00u && cp <= 0x9FFFu) ||
        (cp >= 0xA000u && cp <= 0xA4CFu) ||
        (cp >= 0xAC00u && cp <= 0xD7A3u) ||
        (cp >= 0xF900u && cp <= 0xFAFFu) ||
        (cp >= 0xFE10u && cp <= 0xFE19u) ||
        (cp >= 0xFE30u && cp <= 0xFE4Fu) ||
        (cp >= 0xFF00u && cp <= 0xFF60u) ||
        (cp >= 0xFFE0u && cp <= 0xFFE6u) ||
        (cp >= 0x1F000u && cp <= 0x1FAFFu) ||
        (cp >= 0x20000u && cp <= 0x3FFFDu))
        return 2;
    /* BMP emoji with default emoji presentation (Claude Code emits these standalone) */
    if (cp == 0x231Au || cp == 0x231Bu || (cp >= 0x23E9u && cp <= 0x23ECu) ||
        cp == 0x23F0u || cp == 0x23F3u || cp == 0x25FDu || cp == 0x25FEu ||
        cp == 0x2614u || cp == 0x2615u || (cp >= 0x2648u && cp <= 0x2653u) ||
        cp == 0x267Fu || cp == 0x2693u || cp == 0x26A1u ||
        cp == 0x26AAu || cp == 0x26ABu || cp == 0x26BDu || cp == 0x26BEu ||
        cp == 0x26C4u || cp == 0x26C5u || cp == 0x26CEu || cp == 0x26D4u ||
        cp == 0x26EAu || cp == 0x26F2u || cp == 0x26F3u || cp == 0x26F5u ||
        cp == 0x26FAu || cp == 0x26FDu || cp == 0x2705u ||
        cp == 0x270Au || cp == 0x270Bu || cp == 0x2728u ||
        cp == 0x274Cu || cp == 0x274Eu || (cp >= 0x2753u && cp <= 0x2755u) ||
        cp == 0x2757u || (cp >= 0x2795u && cp <= 0x2797u) ||
        cp == 0x27B0u || cp == 0x27BFu || cp == 0x2B1Bu || cp == 0x2B1Cu ||
        cp == 0x2B50u || cp == 0x2B55u)
        return 2;
    return 1;
}

void console_putc(char c) {
    if (!con.initialized) return;
    uint8_t uc = (uint8_t)c;

    /* ── UTF-8 decoder ── */
    if (uc >= 0x80) {
        /* Continuation byte or start byte */
        if (uc >= 0xC0) {   /* Start byte */
            if (g_utf8_remain > 0) g_utf8_cp = 0;  /* Drop incomplete sequence */
            if (uc < 0xE0) { g_utf8_cp = uc & 0x1F; g_utf8_remain = 1; }
            else if (uc < 0xF0) { g_utf8_cp = uc & 0x0F; g_utf8_remain = 2; }
            else { g_utf8_cp = uc & 0x07; g_utf8_remain = 3; }
            return;
        } else if (g_utf8_remain > 0) {  /* Continuation byte */
            g_utf8_cp = (g_utf8_cp << 6) | (uc & 0x3F);
            g_utf8_remain--;
            if (g_utf8_remain == 0) {
                uint32_t cp = g_utf8_cp;
                g_utf8_cp = 0;
                int cpw = cp_width(cp);
                if (cpw == 0) return;   /* zero-width combining mark: skip entirely */
                g_cell_w = cpw;         /* remember true width for cursor-advance below */
                uc = unicode_to_ascii(cp);
            } else {
                return;  /* Need more bytes */
            }
        } else {
            uc = '?';  /* Orphan continuation — replace with ? */
        }
    }

    /* ── ANSI escape sequence state machine ── */
    if (g_ansi_st == ANSI_ESC) {
        g_ansi_st = ANSI_NORM;
        switch (uc) {
        case '[': g_ansi_st = ANSI_CSI; g_ansi_len = 0; break;
        case ']': g_ansi_st = ANSI_OSC; break;
        case '\\': break;  /* ST string terminator — consume */
        case 'M':  /* RI — reverse index */
            if (con.cy > 0) con.cy--;
            break;
        case '7':  g_saved_cx = con.cx; g_saved_cy = con.cy; break;
        case '8':
            if (g_saved_cx < con.cols) con.cx = g_saved_cx;
            if (g_saved_cy < con.rows) con.cy = g_saved_cy;
            break;
        case '(': case ')': case '*': case '+':
            g_ansi_st = ANSI_CHARSET; break;  /* charset designator follows */
        default: break;  /* unknown ESC — consume */
        }
        return;
    } else if (g_ansi_st == ANSI_OSC) {
        if (uc == 0x07u) { g_ansi_st = ANSI_NORM; }   /* BEL terminates OSC */
        else if (uc == 0x1Bu) { g_ansi_st = ANSI_ESC; }  /* ESC starts potential ST */
        return;  /* consume all OSC payload */
    } else if (g_ansi_st == ANSI_CHARSET) {
        g_ansi_st = ANSI_NORM;  /* consume the one-byte charset designator */
        return;
    } else if (g_ansi_st == ANSI_CSI) {
        if ((uc >= 0x40u && uc <= 0x7Eu) || g_ansi_len >= 63u) {
            /* Final byte of CSI sequence */
            if (g_ansi_len < 63u) g_ansi_buf[g_ansi_len++] = uc;
            ansi_process_csi();
            g_ansi_st = ANSI_NORM;
        } else {
            g_ansi_buf[g_ansi_len++] = uc;   /* collect param bytes */
        }
        return;
    } else if (uc == 0x1Bu) {   /* ESC */
        g_ansi_st = ANSI_ESC;
        return;
    }

    /* Always capture to scrollback ring (skip control codes except \n) */
    if (uc >= 0x20u || uc == '\n') tsb_push(uc);

    if (g_tsb_suppress) {
        int w = g_cell_w; g_cell_w = 1;
        if (c == '\n') { con.cx = 0; con.cy++; }
        else if (c == '\r') con.cx = 0;
        else con.cx += (uint64_t)w;
        return;
    }

    if (c == '\x7f' || c == '\b') {
        if (con.cx > 0) con.cx--;
        return;
    }

    if (c == '\n') {
        con.cx = 0;
        con.cy++;
        ensure_cursor_visible();
        return;
    }

    if (c == '\r') {
        con.cx = 0;
        return;
    }

    if (c == '\t') {
        for (int i = 0; i < 4; i++) console_putc(' ');
        return;
    }

    int w = g_cell_w; g_cell_w = 1;
    /* Wide char straddling the right margin: wrap to next line first */
    if (w == 2 && con.cx + 1 >= con.cols) {
        con.cx = 0; con.cy++;
    }
    ensure_cursor_visible();
    draw_char_at(con.cx, con.cy, (unsigned char)uc);
    con.cx += (uint64_t)w;
    /* Blank the phantom second column so later erases/scrolls stay aligned */
    if (w == 2 && con.cx < con.cols)
        draw_char_at(con.cx, con.cy, ' ');
    ensure_cursor_visible();
}

void console_write(const char *s) {
    for (size_t i = 0; s[i]; i++) console_putc(s[i]);
}

void console_get_cursor(uint32_t *x, uint32_t *y) {
    if (x) *x = (uint32_t)con.cx;
    if (y) *y = (uint32_t)con.cy;
}

void console_set_cursor(uint32_t x, uint32_t y) {
    if (con.cols == 0 || con.rows == 0) return;
    if (x >= con.cols) x = (uint32_t)(con.cols - 1);
    if (y >= con.rows) y = (uint32_t)(con.rows - 1);
    con.cx = x;
    con.cy = y;
}

uint32_t    console_font_width(void)  { return g_fw; }
uint32_t    console_font_height(void) { return g_fh; }
const char *console_font_name(void)   { return g_fname; }

bool console_load_psf(const char *path) {
    const void *raw = NULL;
    uint64_t raw_size = 0;
    if (vfs_read(path, &raw, &raw_size) != 0 || !raw || raw_size < 4)
        return false;

    const uint8_t *d = (const uint8_t *)raw;
    uint32_t new_fw, new_fh, new_fbpg, new_nglyphs, data_off;

    if (d[0] == 0x36u && d[1] == 0x04u) {
        /* PSF1: 4-byte header, always 8px wide */
        if (raw_size < 4u) return false;
        new_fw      = 8u;
        new_fh      = d[3];                       /* charsize = height */
        new_fbpg    = d[3];
        new_nglyphs = (d[2] & 0x01u) ? 512u : 256u;
        data_off    = 4u;
    } else if (d[0] == 0x72u && d[1] == 0xb5u && d[2] == 0x4au && d[3] == 0x86u) {
        /* PSF2: 32-byte header */
        if (raw_size < 32u) return false;
        /* fields are little-endian uint32 */
        #define LE32(off) ((uint32_t)d[off] | ((uint32_t)d[(off)+1]<<8) | \
                           ((uint32_t)d[(off)+2]<<16) | ((uint32_t)d[(off)+3]<<24))
        data_off    = LE32(8);
        new_nglyphs = LE32(16);
        new_fbpg    = LE32(20);
        new_fh      = LE32(24);
        new_fw      = LE32(28);
        #undef LE32
    } else {
        return false;
    }

    if (new_fw == 0u || new_fw > 64u) return false;
    if (new_fh == 0u || new_fh > 64u) return false;
    if (new_nglyphs == 0u || new_nglyphs > 512u) return false;
    uint64_t glyph_bytes = (uint64_t)new_nglyphs * new_fbpg;
    if ((uint64_t)data_off + glyph_bytes > raw_size) return false;
    if (glyph_bytes > FONT_BUF_SIZE) return false;

    const uint8_t *src = d + data_off;
    for (uint64_t i = 0; i < glyph_bytes; i++)
        g_font_buf[i] = src[i];

    g_fw      = new_fw;
    g_fh      = new_fh;
    g_fbpg    = new_fbpg;
    g_fglyphs = new_nglyphs;
    g_fmsb    = true;
    g_fdata   = g_font_buf;

    /* store basename as display name */
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/') base = p + 1;
    uint32_t ni = 0;
    while (base[ni] && ni < 63u) { g_fname[ni] = base[ni]; ni++; }
    g_fname[ni] = '\0';

    /* recalculate grid and redraw */
    if (con.initialized) {
        con.cols = (con.w - con.x_off) / g_fw;
        con.rows = con.vp_h / g_fh;
        if (con.cols > CELL_MAX_COLS) con.cols = CELL_MAX_COLS;
        if (con.rows > CELL_MAX_ROWS) con.rows = CELL_MAX_ROWS;
        con.cx = 0; con.cy = 0;
        console_clear();
    }
    return true;
}

/* Update viewport coords and re-render the cell buffer.
 * Used when opening/reopening the window — cell-buffer text is restored. */
void console_set_viewport(uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    if (!con.initialized) return;
    con.x_off    = x;
    con.y_offset = y;
    con.vp_h     = h;
    con.cols     = w / g_fw;
    con.rows     = h / g_fh;
    if (con.cols > CELL_MAX_COLS) con.cols = CELL_MAX_COLS;
    if (con.rows > CELL_MAX_ROWS) con.rows = CELL_MAX_ROWS;
    if (con.cols > 0 && con.cx >= con.cols) con.cx = con.cols - 1;
    if (con.rows > 0 && con.cy >= con.rows) con.cy = con.rows - 1;
    for (uint64_t r = 0; r < con.rows; r++)
        for (uint64_t c = 0; c < con.cols; c++)
            render_char(c, r, cell_buf[r][c]);
}

/* Update viewport coords only — no framebuffer re-render.
 * Used after a pixel blit where the framebuffer already has correct content. */
void console_set_viewport_norender(uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    if (!con.initialized) return;
    con.x_off    = x;
    con.y_offset = y;
    con.vp_h     = h;
    con.cols     = w / g_fw;
    con.rows     = h / g_fh;
    if (con.cols > CELL_MAX_COLS) con.cols = CELL_MAX_COLS;
    if (con.rows > CELL_MAX_ROWS) con.rows = CELL_MAX_ROWS;
    if (con.cols > 0 && con.cx >= con.cols) con.cx = con.cols - 1;
    if (con.rows > 0 && con.cy >= con.rows) con.cy = con.rows - 1;
}

/* Allocate the double-buffer from PMM.  Call after pmm_init() / vmm_init().
 * Until this is called, fill_rect/render_char fall back to writing directly
 * to VRAM (g_back == NULL branch). */
void console_backbuf_init(void) {
    if (!con.initialized || g_back) return;
    uint64_t npix   = con.pitch32 * con.h;
    uint64_t nbytes = npix * sizeof(uint32_t);
    uint64_t npages = (nbytes + 4095u) / 4096u;
    uint64_t phys   = pmm_alloc_pages(npages);
    if (!phys) return;
    g_back = (uint32_t *)pmm_phys_to_virt(phys);
#ifdef __linux__
    memset(g_back, 0, npix * sizeof(uint32_t));
#else
    for (uint64_t i = 0; i < npix; i++) g_back[i] = 0;
#endif
    g_dirty    = false;
    g_dirty_y0 = 0xFFFFFFFFu;
    g_dirty_y1 = 0u;
}

/* Expand the dirty range so the cursor layer can force-flush its own rows
 * without having to draw anything into the back buffer. */
void console_mark_dirty_rows(uint32_t y0, uint32_t y1) {
    if (!g_back) return;
    g_dirty = true;
    if (y0 < g_dirty_y0) g_dirty_y0 = y0;
    if (y1 > g_dirty_y1) g_dirty_y1 = y1;
}

uint32_t *console_backbuf_ptr(void)     { return g_back; }
uint64_t  console_backbuf_pitch32(void) { return g_back ? con.pitch32 : 0; }

/* Copy only dirty rows of backbuf → VRAM.  Returns true when a flip happened. */
bool console_flip_if_dirty(void) {
    if (!g_back || !g_dirty) return false;
    g_dirty = false;
    uint32_t y0 = g_dirty_y0, y1 = g_dirty_y1;
    g_dirty_y0 = 0xFFFFFFFFu; g_dirty_y1 = 0u;
    if (y0 >= y1) return true;
    if (y1 > (uint32_t)con.h) y1 = (uint32_t)con.h;
    uint64_t n = (uint64_t)(y1 - y0) * con.pitch32;
    const uint32_t *src = g_back + (uint64_t)y0 * con.pitch32;
    uint32_t *dst = (uint32_t *)(uintptr_t)con.pix + (uint64_t)y0 * con.pitch32;
#ifdef __linux__
    memcpy(dst, src, n * sizeof(uint32_t));
#else
    for (uint64_t i = 0; i < n; i++) dst[i] = src[i];
#endif
    return true;
}

/* Read a rectangle of pixels from the back buffer into buf (caller-allocated,
 * must be w*h uint32_t elements).  Returns false if no back buffer. */
bool console_capture_rect(uint32_t *buf, uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    if (!g_back || !buf) return false;
    for (uint64_t yy = 0; yy < h && y + yy < con.h; yy++) {
        const uint32_t *src = g_back + (y + yy) * con.pitch32 + x;
        uint32_t       *dst = buf + yy * w;
        for (uint64_t xx = 0; xx < w && x + xx < con.w; xx++)
            dst[xx] = src[xx];
    }
    return true;
}

/* Write a rectangle of pixels from buf into the back buffer. */
void console_paste_rect(const uint32_t *buf, uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    if (!g_back || !buf) return;
    g_dirty = true;
    if ((uint32_t)y < g_dirty_y0) g_dirty_y0 = (uint32_t)y;
    uint32_t _pry1 = (uint32_t)(y + h);
    if (_pry1 > g_dirty_y1) g_dirty_y1 = _pry1;
    for (uint64_t yy = 0; yy < h && y + yy < con.h; yy++) {
        const uint32_t *src = buf + yy * w;
        uint32_t       *dst = g_back + (y + yy) * con.pitch32 + x;
        for (uint64_t xx = 0; xx < w && x + xx < con.w; xx++)
            dst[xx] = src[xx];
    }
}

/* Blit a sub-rectangle of a larger source image. src points at the full image with
 * row stride src_stride (in pixels); (sx,sy,w,h) selects the region to copy to
 * screen (dx,dy). Used to crop a CSD window's content out of its shadow margin. */
void console_paste_subrect(const uint32_t *src, uint64_t src_stride,
                           uint64_t sx, uint64_t sy, uint64_t w, uint64_t h,
                           uint64_t dx, uint64_t dy) {
    if (!g_back || !src) return;
    g_dirty = true;
    if ((uint32_t)dy < g_dirty_y0) g_dirty_y0 = (uint32_t)dy;
    uint32_t _sy1 = (uint32_t)(dy + h);
    if (_sy1 > g_dirty_y1) g_dirty_y1 = _sy1;
    for (uint64_t yy = 0; yy < h && dy + yy < con.h; yy++) {
        const uint32_t *s = src + (sy + yy) * src_stride + sx;
        uint32_t       *d = g_back + (dy + yy) * con.pitch32 + dx;
        for (uint64_t xx = 0; xx < w && dx + xx < con.w; xx++)
            d[xx] = s[xx];
    }
}

/* True alpha compositing: blend each ARGB source pixel over the current back-buffer
 * contents: out = src*a + dst*(1-a). Transparent pixels leave the background
 * untouched; semi-transparent shadow blends softly instead of stamping black. */
void console_paste_rect_blend(const uint32_t *buf, uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    if (!g_back || !buf) return;
    g_dirty = true;
    if ((uint32_t)y < g_dirty_y0) g_dirty_y0 = (uint32_t)y;
    uint32_t _by1 = (uint32_t)(y + h);
    if (_by1 > g_dirty_y1) g_dirty_y1 = _by1;
    for (uint64_t yy = 0; yy < h && y + yy < con.h; yy++) {
        const uint32_t *src = buf + yy * w;
        uint32_t       *dst = g_back + (y + yy) * con.pitch32 + x;
        for (uint64_t xx = 0; xx < w && x + xx < con.w; xx++) {
            uint32_t s = src[xx];
            uint32_t a = s >> 24;
            if (a == 0) continue;             /* fully transparent — keep background */
            if (a == 0xFF) { dst[xx] = s | 0xFF000000u; continue; }
            uint32_t d = dst[xx];
            uint32_t na = 255u - a;
            uint32_t r = (((s >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * na) / 255u;
            uint32_t g = (((s >> 8)  & 0xFF) * a + ((d >> 8)  & 0xFF) * na) / 255u;
            uint32_t b = (((s)       & 0xFF) * a + ((d)       & 0xFF) * na) / 255u;
            dst[xx] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

/* Draw only (near-)fully-opaque source pixels; skip anything with partial alpha.
 * Used for CSD windows so the whole shadow (transparent margin + semi-transparent
 * gradient) is skipped, leaving just the opaque window content — no shadow at all. */
void console_paste_rect_opaque(const uint32_t *buf, uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    if (!g_back || !buf) return;
    g_dirty = true;
    if ((uint32_t)y < g_dirty_y0) g_dirty_y0 = (uint32_t)y;
    uint32_t _oy1 = (uint32_t)(y + h);
    if (_oy1 > g_dirty_y1) g_dirty_y1 = _oy1;
    for (uint64_t yy = 0; yy < h && y + yy < con.h; yy++) {
        const uint32_t *src = buf + yy * w;
        uint32_t       *dst = g_back + (y + yy) * con.pitch32 + x;
        for (uint64_t xx = 0; xx < w && x + xx < con.w; xx++) {
            uint32_t px = src[xx];
            if ((px >> 24) < 0xF0u) continue;   /* not fully opaque → shadow, skip */
            dst[xx] = px | 0xFF000000u;
        }
    }
}

/* Like console_paste_rect but treats the source as ARGB: fully-transparent source
 * pixels (alpha == 0) are skipped rather than stamped as black. Used for popups /
 * subsurfaces so an empty/transparent overlay surface doesn't paint a black box. */
void console_paste_rect_alpha(const uint32_t *buf, uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    if (!g_back || !buf) return;
    g_dirty = true;
    if ((uint32_t)y < g_dirty_y0) g_dirty_y0 = (uint32_t)y;
    uint32_t _pry1 = (uint32_t)(y + h);
    if (_pry1 > g_dirty_y1) g_dirty_y1 = _pry1;
    for (uint64_t yy = 0; yy < h && y + yy < con.h; yy++) {
        const uint32_t *src = buf + yy * w;
        uint32_t       *dst = g_back + (y + yy) * con.pitch32 + x;
        for (uint64_t xx = 0; xx < w && x + xx < con.w; xx++) {
            uint32_t px = src[xx];
            if ((px >> 24) == 0) continue;   /* fully transparent — leave background */
            dst[xx] = px | 0xFF000000u;      /* force opaque (we don't alpha-blend) */
        }
    }
}

/* Scale-blit src (sw×sh pixels) into the back buffer at (dx,dy) with size dw×dh.
 * Nearest-neighbor interpolation. Used for wallpaper images. */
void console_blit_scaled(const uint32_t *src, uint64_t sw, uint64_t sh,
                         uint64_t dx, uint64_t dy, uint64_t dw, uint64_t dh) {
    if (!g_back || !src || !sw || !sh || !dw || !dh) return;
    g_dirty = true;
    if ((uint32_t)dy < g_dirty_y0) g_dirty_y0 = (uint32_t)dy;
    uint32_t _bsy1 = (uint32_t)(dy + dh);
    if (_bsy1 > g_dirty_y1) g_dirty_y1 = _bsy1;
    for (uint64_t y = 0; y < dh && dy + y < con.h; y++) {
        uint64_t sy = y * sh / dh;
        const uint32_t *src_row = src + sy * sw;
        uint32_t *dst_row = g_back + (dy + y) * con.pitch32 + dx;
        for (uint64_t x = 0; x < dw && dx + x < con.w; x++) {
            uint64_t sx = x * sw / dw;
            dst_row[x] = src_row[sx];
        }
    }
}

/* Scale-blit ARGB source with per-pixel alpha blending over the backbuffer.
 * Nearest-neighbor. Used for app-icon logos over the wallpaper. */
void console_blit_scaled_alpha(const uint32_t *src, uint64_t sw, uint64_t sh,
                               uint64_t dx, uint64_t dy, uint64_t dw, uint64_t dh) {
    if (!g_back || !src || !sw || !sh || !dw || !dh) return;
    g_dirty = true;
    if ((uint32_t)dy < g_dirty_y0) g_dirty_y0 = (uint32_t)dy;
    uint32_t _bay1 = (uint32_t)(dy + dh);
    if (_bay1 > g_dirty_y1) g_dirty_y1 = _bay1;
    for (uint64_t y = 0; y < dh && dy + y < con.h; y++) {
        uint64_t sy = y * sh / dh;
        const uint32_t *src_row = src + sy * sw;
        uint32_t *dst_row = g_back + (dy + y) * con.pitch32 + dx;
        for (uint64_t x = 0; x < dw && dx + x < con.w; x++) {
            uint32_t s = src_row[x * sw / dw];
            uint32_t a = s >> 24;
            if (a == 0) continue;
            if (a >= 0xF8u) { dst_row[x] = s & 0x00ffffffu; continue; }
            uint32_t ia = 255u - a;
            uint32_t d  = dst_row[x];
            uint32_t r = ((((s >> 16) & 0xffu) * a) + (((d >> 16) & 0xffu) * ia)) >> 8;
            uint32_t g = ((((s >>  8) & 0xffu) * a) + (((d >>  8) & 0xffu) * ia)) >> 8;
            uint32_t b = (((s         & 0xffu) * a) + ((d         & 0xffu) * ia)) >> 8;
            dst_row[x] = (r << 16) | (g << 8) | b;
        }
    }
}
