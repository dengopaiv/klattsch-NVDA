/* kl_dict_symbols.c -- the phone symbols of the compiled-in dictionary, and
 * their one-character codes. Shared by the library and by the generator that
 * writes the table (csrc/tools/kl_dict_gen.c), so the two cannot disagree.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Päiv Dengo
 */
#include <string.h>

#include "kl_dict.h"

const char *const kl_dict_symbols[KL_DICT_SYMBOL_COUNT] = {
    "B", "CH", "D", "DH", "F", "G", "HH", "JH", "K", "L", "M", "N", "NG",
    "P", "R", "S", "SH", "T", "TH", "V", "W", "Y", "Z", "ZH",
    "AA0", "AA1", "AA2", "AE0", "AE1", "AE2", "AH0", "AH1", "AH2",
    "AO0", "AO1", "AO2", "AW0", "AW1", "AW2", "AY0", "AY1", "AY2",
    "EH0", "EH1", "EH2", "ER0", "ER1", "ER2", "EY0", "EY1", "EY2",
    "IH0", "IH1", "IH2", "IY0", "IY1", "IY2", "OW0", "OW1", "OW2",
    "OY0", "OY1", "OY2", "UH0", "UH1", "UH2", "UW0", "UW1", "UW2",
};

char kl_dict_code_char(int i)
{
    int c;
    if (i < 0 || i >= KL_DICT_SYMBOL_COUNT)
        return 0;
    c = '#' + i;
    if (c >= '?')
        c++;
    if (c >= '\\')
        c++;
    return (char)c;
}

int kl_dict_code_index(char ch)
{
    int c = (unsigned char)ch, i;
    if (c < '#' || c == '?' || c == '\\')
        return -1;
    i = c - '#';
    if (c > '?')
        i--;
    if (c > '\\')
        i--;
    return i < KL_DICT_SYMBOL_COUNT ? i : -1;
}

int kl_dict_symbol_index(const char *name, size_t len)
{
    int i;
    for (i = 0; i < KL_DICT_SYMBOL_COUNT; i++)
        if (strlen(kl_dict_symbols[i]) == len && memcmp(kl_dict_symbols[i], name, len) == 0)
            return i;
    return -1;
}
