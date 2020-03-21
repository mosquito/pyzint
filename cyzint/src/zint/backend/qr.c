/* qr.c Handles QR Code, Micro QR Code, UPNQR and rMQR

    libzint - the open source barcode library
    Copyright (C) 2009 - 2019 Robin Stuart <rstuart114@gmail.com>

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:

    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.
    3. Neither the name of the project nor the names of its contributors
       may be used to endorse or promote products derived from this software
       without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
    ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
    FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
    DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
    OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
    HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
    OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
    SUCH DAMAGE.
 */
/* vim: set ts=4 sw=4 et : */

#include <string.h>
#ifdef _MSC_VER
#include <malloc.h>
#endif
#include "common.h"
#include <stdio.h>
#include "sjis.h"
#include "qr.h"
#include "reedsol.h"
#include <stdlib.h>     /* abs */
#include <assert.h>

extern int utf_to_eci(const int eci, const unsigned char source[], unsigned char dest[], size_t *length); /* Convert Unicode to other encodings */

/* Returns true if input glyph is in the Alphanumeric set */
static int in_alpha(const int glyph) {
    int retval = 0;

    if ((glyph >= '0') && (glyph <= '9')) {
        retval = 1;
    }
    if ((glyph >= 'A') && (glyph <= 'Z')) {
        retval = 1;
    }
    switch (glyph) {
        case ' ':
        case '$':
        case '%':
        case '*':
        case '+':
        case '-':
        case '.':
        case '/':
        case ':':
            retval = 1;
            break;
    }

    return retval;
}

static void define_mode(char mode[], const unsigned int jisdata[], const size_t length, const int gs1) {
    /* Values placed into mode[] are: K = Kanji, B = Binary, A = Alphanumeric, N = Numeric */
    size_t i;
    int    mlen, j;

    for (i = 0; i < length; i++) {
        if (jisdata[i] > 0xff) {
            mode[i] = 'K';
        } else {
            mode[i] = 'B';
            if (in_alpha(jisdata[i])) {
                mode[i] = 'A';
            }
            if (gs1 && (jisdata[i] == '[')) {
                mode[i] = 'A';
            }
            if ((jisdata[i] >= '0') && (jisdata[i] <= '9')) {
                mode[i] = 'N';
            }
        }
    }

    /* If less than 6 numeric digits together then don't use numeric mode */
    if (ustrchr_cnt((unsigned char*) mode, length, 'N') != length) {
        for (i = 0; i < length; i++) {
            if (mode[i] == 'N') {
                if (((i != 0) && (mode[i - 1] != 'N')) || (i == 0)) {
                    mlen = 0;
                    while (((mlen + i) < length) && (mode[mlen + i] == 'N')) {
                        mlen++;
                    };
                    if (mlen < 6) {
                        for (j = 0; j < mlen; j++) {
                            mode[i + j] = 'A';
                        }
                    }
                }
            }
        }
    }

    /* If less than 4 alphanumeric characters together then don't use alphanumeric mode */
    if (ustrchr_cnt((unsigned char*) mode, length, 'A') != length) {
        for (i = 0; i < length; i++) {
            if (mode[i] == 'A') {
                if (((i != 0) && (mode[i - 1] != 'A')) || (i == 0)) {
                    mlen = 0;
                    while (((mlen + i) < length) && (mode[mlen + i] == 'A')) {
                        mlen++;
                    };
                    if (mlen < 4) {
                        for (j = 0; j < mlen; j++) {
                            mode[i + j] = 'B';
                        }
                    }
                }
            }
        }
    }
}

/* Choose from three numbers based on version */
static int tribus(const int version,const int a,const int b,const int c) {
    int RetVal;

    RetVal = c;

    if (version < 10) {
        RetVal = a;
    }

    if ((version >= 10) && (version <= 26)) {
        RetVal = b;
    }

    return RetVal;
}

/* Convert input data to a binary stream and add padding */
static void qr_binary(unsigned char datastream[], const int version, const int target_codewords, const char mode[], const unsigned int jisdata[], const size_t length,
            const int gs1, const int eci, const int est_binlen,const int debug) {
    int position = 0;
    int i;
    char padbits;
    int current_binlen, current_bytes;
    int toggle, percent;
    int percent_count;

#ifndef _MSC_VER
    char binary[est_binlen + 12];
#else
    char* binary = (char *) _alloca(est_binlen + 12);
#endif
    strcpy(binary, "");

    if (gs1) {
        if (version < RMQR_VERSION) {
            strcat(binary, "0101"); /* FNC1 */
        } else {
            strcat(binary, "101");
        }
    }

    if (eci != 0) { /* Not applicable to RMQR */
        strcat(binary, "0111"); /* ECI (Table 4) */
        if (eci <= 127) {
            bin_append(eci, 8, binary); /* 000000 to 000127 */
        } else if (eci <= 16383) {
            bin_append(0x8000 + eci, 16, binary); /* 000000 to 016383 */
        } else {
            bin_append(0xC00000 + eci, 24, binary); /* 000000 to 999999 */
        }
    }

    if (debug & ZINT_DEBUG_PRINT) {
        for (i = 0; i < length; i++) {
            printf("%c", mode[i]);
        }
        printf("\n");
    }

    percent = 0;

    do {
        char data_block = mode[position];
        int short_data_block_length = 0;
        int double_byte = 0;
        do {
            if (data_block == 'B' && jisdata[position + short_data_block_length] > 0xFF) {
                double_byte++;
            }
            short_data_block_length++;
        } while (((short_data_block_length + position) < length)
                && (mode[position + short_data_block_length] == data_block));

        switch (data_block) {
            case 'K':
                /* Kanji mode */
                /* Mode indicator */
                if (version < RMQR_VERSION) {
                    strcat(binary, "1000");
                } else {
                    strcat(binary, "100");
                }

                /* Character count indicator */
                if (version < RMQR_VERSION) {
                    bin_append(short_data_block_length, tribus(version, 8, 10, 12), binary);
                } else {
                    bin_append(short_data_block_length, rmqr_kanji_cci[version - RMQR_VERSION], binary);
                }

                if (debug & ZINT_DEBUG_PRINT) {
                    printf("Kanji block (length %d)\n\t", short_data_block_length);
                }

                /* Character representation */
                for (i = 0; i < short_data_block_length; i++) {
                    unsigned int jis = jisdata[position + i];
                    int prod;

                    if (jis >= 0x8140 && jis <= 0x9ffc)
                        jis -= 0x8140;

                    else if (jis >= 0xe040 && jis <= 0xebbf)
                        jis -= 0xc140;

                    prod = ((jis >> 8) * 0xc0) + (jis & 0xff);

                    bin_append(prod, 13, binary);

                    if (debug & ZINT_DEBUG_PRINT) {
                        printf("0x%4X ", prod);
                    }
                }

                if (debug & ZINT_DEBUG_PRINT) {
                    printf("\n");
                }

                break;
            case 'B':
                /* Byte mode */
                /* Mode indicator */
                if (version < RMQR_VERSION) {
                    strcat(binary, "0100");
                } else {
                    strcat(binary, "011");
                }

                /* Character count indicator */
                if (version < RMQR_VERSION) {
                    bin_append(short_data_block_length + double_byte, tribus(version, 8, 16, 16), binary);
                } else {
                    bin_append(short_data_block_length + double_byte, rmqr_byte_cci[version - RMQR_VERSION], binary);
                }

                if (debug & ZINT_DEBUG_PRINT) {
                    printf("Byte block (length %d)\n\t", short_data_block_length + double_byte);
                }

                /* Character representation */
                for (i = 0; i < short_data_block_length; i++) {
                    unsigned int byte = jisdata[position + i];

                    if (gs1 && (byte == '[')) {
                        byte = 0x1d; /* FNC1 */
                    }

                    bin_append(byte, byte > 0xFF ? 16 : 8, binary);

                    if (debug & ZINT_DEBUG_PRINT) {
                        printf("0x%2X(%d) ", byte, byte);
                    }
                }

                if (debug & ZINT_DEBUG_PRINT) {
                    printf("\n");
                }

                break;
            case 'A':
                /* Alphanumeric mode */
                /* Mode indicator */
                if (version < RMQR_VERSION) {
                    strcat(binary, "0010");
                } else {
                    strcat(binary, "010");
                }

                percent_count = 0;
                for (i = 0; i < short_data_block_length; i++) {
                    if (gs1 && (jisdata[position + i] == '%')) {
                        percent_count++;
                    }
                }

                /* Character count indicator */
                if (version < RMQR_VERSION) {
                    bin_append(short_data_block_length + percent_count, tribus(version, 9, 11, 13), binary);
                } else {
                    bin_append(short_data_block_length + percent_count, rmqr_alphanum_cci[version - RMQR_VERSION], binary);
                }

                if (debug & ZINT_DEBUG_PRINT) {
                    printf("Alpha block (length %d)\n\t", short_data_block_length + percent_count);
                }

                /* Character representation */
                i = 0;
                while (i < short_data_block_length) {
                    int count;
                    int first = 0, second = 0, prod;

                    if (percent == 0) {
                        if (gs1 && (jisdata[position + i] == '%')) {
                            first = posn(RHODIUM, '%');
                            second = posn(RHODIUM, '%');
                            count = 2;
                            prod = (first * 45) + second;
                            i++;
                        } else {
                            if (gs1 && (jisdata[position + i] == '[')) {
                                first = posn(RHODIUM, '%'); /* FNC1 */
                            } else {
                                first = posn(RHODIUM, (char) jisdata[position + i]);
                            }
                            count = 1;
                            i++;
                            prod = first;

                            if (i < short_data_block_length && mode[position + i] == 'A') {
                                if (gs1 && (jisdata[position + i] == '%')) {
                                    second = posn(RHODIUM, '%');
                                    count = 2;
                                    prod = (first * 45) + second;
                                    percent = 1;
                                } else {
                                    if (gs1 && (jisdata[position + i] == '[')) {
                                        second = posn(RHODIUM, '%'); /* FNC1 */
                                    } else {
                                        second = posn(RHODIUM, (char) jisdata[position + i]);
                                    }
                                    count = 2;
                                    i++;
                                    prod = (first * 45) + second;
                                }
                            }
                        }
                    } else {
                        first = posn(RHODIUM, '%');
                        count = 1;
                        i++;
                        prod = first;
                        percent = 0;

                        if (i < short_data_block_length && mode[position + i] == 'A') {
                            if (gs1 && (jisdata[position + i] == '%')) {
                                second = posn(RHODIUM, '%');
                                count = 2;
                                prod = (first * 45) + second;
                                percent = 1;
                            } else {
                                if (gs1 && (jisdata[position + i] == '[')) {
                                    second = posn(RHODIUM, '%'); /* FNC1 */
                                } else {
                                    second = posn(RHODIUM, (char) jisdata[position + i]);
                                }
                                count = 2;
                                i++;
                                prod = (first * 45) + second;
                            }
                        }
                    }

                    bin_append(prod, 1 + (5 * count), binary);

                    if (debug & ZINT_DEBUG_PRINT) {
                        printf("0x%4X ", prod);
                    }
                };

                if (debug & ZINT_DEBUG_PRINT) {
                    printf("\n");
                }

                break;
            case 'N':
                /* Numeric mode */
                /* Mode indicator */
                if (version >= RMQR_VERSION) {
                    strcat(binary, "0001");
                } else {
                    strcat(binary, "001");
                }

                /* Character count indicator */
                if (version < RMQR_VERSION) {
                    bin_append(short_data_block_length, tribus(version, 10, 12, 14), binary);
                } else {
                    bin_append(short_data_block_length, rmqr_numeric_cci[version - RMQR_VERSION], binary);
                }

                if (debug & ZINT_DEBUG_PRINT) {
                    printf("Number block (length %d)\n\t", short_data_block_length);
                }

                /* Character representation */
                i = 0;
                while (i < short_data_block_length) {
                    int count;
                    int first = 0, prod;

                    first = posn(NEON, (char) jisdata[position + i]);
                    count = 1;
                    prod = first;

                    if (i + 1 < short_data_block_length && mode[position + i + 1] == 'N') {
                        int second = posn(NEON, (char) jisdata[position + i + 1]);
                        count = 2;
                        prod = (prod * 10) + second;

                        if (i + 2 < short_data_block_length && mode[position + i + 2] == 'N') {
                            int third = posn(NEON, (char) jisdata[position + i + 2]);
                            count = 3;
                            prod = (prod * 10) + third;
                        }
                    }

                    bin_append(prod, 1 + (3 * count), binary);

                    if (debug & ZINT_DEBUG_PRINT) {
                        printf("0x%4X (%d)", prod, prod);
                    }

                    i += count;
                };

                if (debug & ZINT_DEBUG_PRINT) {
                    printf("\n");
                }

                break;
        }

        position += short_data_block_length;
    } while (position < length);

    /* Terminator */
    if (version < RMQR_VERSION) {
        strcat(binary, "0000");
    } else {
        strcat(binary, "000");
    }

    current_binlen = (int)strlen(binary);
    padbits = 8 - (current_binlen % 8);
    if (padbits == 8) {
        padbits = 0;
    }
    current_bytes = (current_binlen + padbits) / 8;

    /* Padding bits */
    for (i = 0; i < padbits; i++) {
        strcat(binary, "0");
    }

    /* Put data into 8-bit codewords */
    for (i = 0; i < current_bytes; i++) {
        int p;
        datastream[i] = 0x00;
        for (p = 0; p < 8; p++) {
            if (binary[i * 8 + p] == '1') {
                datastream[i] += (0x80 >> p);
            }
        }
    }

    /* Add pad codewords */
    toggle = 0;
    for (i = current_bytes; i < target_codewords; i++) {
        if (toggle == 0) {
            datastream[i] = 0xec;
            toggle = 1;
        } else {
            datastream[i] = 0x11;
            toggle = 0;
        }
    }

    if (debug & ZINT_DEBUG_PRINT) {
        printf("Resulting codewords:\n\t");
        for (i = 0; i < target_codewords; i++) {
            printf("0x%2X ", datastream[i]);
        }
        printf("\n");
    }
}

/* Split data into blocks, add error correction and then interleave the blocks and error correction data */
static void add_ecc(unsigned char fullstream[], const unsigned char datastream[], const int version, const int data_cw, const int blocks, int debug) {
    int ecc_cw;

    if (version < RMQR_VERSION) {
        ecc_cw = qr_total_codewords[version - 1] - data_cw;
    } else {
        ecc_cw = rmqr_total_codewords[version - RMQR_VERSION] - data_cw;
    }

    int short_data_block_length = data_cw / blocks;
    int qty_long_blocks = data_cw % blocks;
    int qty_short_blocks = blocks - qty_long_blocks;
    int ecc_block_length = ecc_cw / blocks;
    int i, j, length_this_block, posn;

#ifndef _MSC_VER
    unsigned char data_block[short_data_block_length + 2];
    unsigned char ecc_block[ecc_block_length + 2];
    unsigned char interleaved_data[data_cw + 2];
    unsigned char interleaved_ecc[ecc_cw + 2];
#else
    unsigned char* data_block = (unsigned char *) _alloca(short_data_block_length + 2);
    unsigned char* ecc_block = (unsigned char *) _alloca(ecc_block_length + 2);
    unsigned char* interleaved_data = (unsigned char *) _alloca(data_cw + 2);
    unsigned char* interleaved_ecc = (unsigned char *) _alloca(ecc_cw + 2);
#endif

    posn = 0;

    for (i = 0; i < blocks; i++) {
        if (i < qty_short_blocks) {
            length_this_block = short_data_block_length;
        } else {
            length_this_block = short_data_block_length + 1;
        }

        for (j = 0; j < ecc_block_length; j++) {
            ecc_block[j] = 0;
        }

        for (j = 0; j < length_this_block; j++) {
            data_block[j] = datastream[posn + j];
        }

        rs_init_gf(0x11d);
        rs_init_code(ecc_block_length, 0);
        rs_encode(length_this_block, data_block, ecc_block);
        rs_free();

        if (debug & ZINT_DEBUG_PRINT) {
            printf("Block %d: ", i + 1);
            for (j = 0; j < length_this_block; j++) {
                printf("%2X ", data_block[j]);
            }
            if (i < qty_short_blocks) {
                printf("   ");
            }
            printf(" // ");
            for (j = 0; j < ecc_block_length; j++) {
                printf("%2X ", ecc_block[ecc_block_length - j - 1]);
            }
            printf("\n");
        }

        for (j = 0; j < short_data_block_length; j++) {
            interleaved_data[(j * blocks) + i] = data_block[j];
        }

        if (i >= qty_short_blocks) {
            interleaved_data[(short_data_block_length * blocks) + (i - qty_short_blocks)] = data_block[short_data_block_length];
        }

        for (j = 0; j < ecc_block_length; j++) {
            interleaved_ecc[(j * blocks) + i] = ecc_block[ecc_block_length - j - 1];
        }

        posn += length_this_block;
    }

    for (j = 0; j < data_cw; j++) {
        fullstream[j] = interleaved_data[j];
    }
    for (j = 0; j < ecc_cw; j++) {
        fullstream[j + data_cw] = interleaved_ecc[j];
    }

    if (debug & ZINT_DEBUG_PRINT) {
        printf("\nData Stream: \n");
        for (j = 0; j < (data_cw + ecc_cw); j++) {
            printf("%2X ", fullstream[j]);
        }
        printf("\n");
    }
}

static void place_finder(unsigned char grid[],const int size,const int x,const int y) {
    int xp, yp;
    char finder[] = {0x7F, 0x41, 0x5D, 0x5D, 0x5D, 0x41, 0x7F};

    for (xp = 0; xp < 7; xp++) {
        for (yp = 0; yp < 7; yp++) {
            if (finder[yp] & 0x40 >> xp) {
                grid[((yp + y) * size) + (xp + x)] = 0x11;
            } else {
                grid[((yp + y) * size) + (xp + x)] = 0x10;
            }
        }
    }
}

static void place_align(unsigned char grid[],const int size,int x,int y) {
    int xp, yp;
    char alignment[] = {0x1F, 0x11, 0x15, 0x11, 0x1F};

    x -= 2;
    y -= 2; /* Input values represent centre of pattern */

    for (xp = 0; xp < 5; xp++) {
        for (yp = 0; yp < 5; yp++) {
            if (alignment[yp] & 0x10 >> xp) {
                grid[((yp + y) * size) + (xp + x)] = 0x11;
            } else {
                grid[((yp + y) * size) + (xp + x)] = 0x10;
            }
        }
    }
}

static void setup_grid(unsigned char* grid,const int size,const int version) {
    int i, toggle = 1;

    /* Add timing patterns */
    for (i = 0; i < size; i++) {
        if (toggle == 1) {
            grid[(6 * size) + i] = 0x21;
            grid[(i * size) + 6] = 0x21;
            toggle = 0;
        } else {
            grid[(6 * size) + i] = 0x20;
            grid[(i * size) + 6] = 0x20;
            toggle = 1;
        }
    }

    /* Add finder patterns */
    place_finder(grid, size, 0, 0);
    place_finder(grid, size, 0, size - 7);
    place_finder(grid, size, size - 7, 0);

    /* Add separators */
    for (i = 0; i < 7; i++) {
        grid[(7 * size) + i] = 0x10;
        grid[(i * size) + 7] = 0x10;
        grid[(7 * size) + (size - 1 - i)] = 0x10;
        grid[(i * size) + (size - 8)] = 0x10;
        grid[((size - 8) * size) + i] = 0x10;
        grid[((size - 1 - i) * size) + 7] = 0x10;
    }
    grid[(7 * size) + 7] = 0x10;
    grid[(7 * size) + (size - 8)] = 0x10;
    grid[((size - 8) * size) + 7] = 0x10;

    /* Add alignment patterns */
    if (version != 1) {
        /* Version 1 does not have alignment patterns */

        int loopsize = qr_align_loopsize[version - 1];
        int x, y;
        for (x = 0; x < loopsize; x++) {
            for (y = 0; y < loopsize; y++) {
                int xcoord = qr_table_e1[((version - 2) * 7) + x];
                int ycoord = qr_table_e1[((version - 2) * 7) + y];

                if (!(grid[(ycoord * size) + xcoord] & 0x10)) {
                    place_align(grid, size, xcoord, ycoord);
                }
            }
        }
    }

    /* Reserve space for format information */
    for (i = 0; i < 8; i++) {
        grid[(8 * size) + i] += 0x20;
        grid[(i * size) + 8] += 0x20;
        grid[(8 * size) + (size - 1 - i)] = 0x20;
        grid[((size - 1 - i) * size) + 8] = 0x20;
    }
    grid[(8 * size) + 8] += 0x20;
    grid[((size - 1 - 7) * size) + 8] = 0x21; /* Dark Module from Figure 25 */

    /* Reserve space for version information */
    if (version >= 7) {
        for (i = 0; i < 6; i++) {
            grid[((size - 9) * size) + i] = 0x20;
            grid[((size - 10) * size) + i] = 0x20;
            grid[((size - 11) * size) + i] = 0x20;
            grid[(i * size) + (size - 9)] = 0x20;
            grid[(i * size) + (size - 10)] = 0x20;
            grid[(i * size) + (size - 11)] = 0x20;
        }
    }
}

static int cwbit(const unsigned char* fullstream, const int i) {
    int resultant = 0;

    if (fullstream[(i / 8)] & (0x80 >> (i % 8))) {
        resultant = 1;
    }

    return resultant;
}

static void populate_grid(unsigned char* grid, const int h_size, const int v_size, const unsigned char* fullstream, const int cw) {
    int direction = 1; /* up */
    int row = 0; /* right hand side */

    int i, n, y;

    n = cw * 8;
    y = v_size - 1;
    i = 0;
    do {
        int x = (h_size - 2) - (row * 2);

        if ((x < 6) && (v_size == h_size))
            x--; /* skip over vertical timing pattern */

        if (!(grid[(y * h_size) + (x + 1)] & 0xf0)) {
            if (cwbit(fullstream, i)) {
                grid[(y * h_size) + (x + 1)] = 0x01;
            } else {
                grid[(y * h_size) + (x + 1)] = 0x00;
            }
            i++;
        }

        if (i < n) {
            if (!(grid[(y * h_size) + x] & 0xf0)) {
                if (cwbit(fullstream, i)) {
                    grid[(y * h_size) + x] = 0x01;
                } else {
                    grid[(y * h_size) + x] = 0x00;
                }
                i++;
            }
        }

        if (direction) {
            y--;
        } else {
            y++;
        }
        if (y == -1) {
            /* reached the top */
            row++;
            y = 0;
            direction = 0;
        }
        if (y == v_size) {
            /* reached the bottom */
            row++;
            y = v_size - 1;
            direction = 1;
        }
    } while (i < n);
}

#ifdef ZINTLOG

int append_log(char log) {
    FILE *file;

    file = fopen("zintlog.txt", "a+");
    fprintf(file, "%c", log);
    fclose(file);
    return 0;
}

int write_log(char log[]) {
    FILE *file;

    file = fopen("zintlog.txt", "a+");
    fprintf(file, log); /*writes*/
    fprintf(file, "\r\n"); /*writes*/
    fclose(file);
    return 0;
}
#endif

static int evaluate(unsigned char *eval,const int size,const int pattern) {
    int x, y, block, weight;
    int result = 0;
    char state;
    int p;
    int dark_mods;
    int percentage, k;
    int a, b, afterCount, beforeCount;
#ifdef ZINTLOG
    int result_b = 0;
    char str[15];
#endif

#ifndef _MSC_VER
    char local[size * size];
#else
    char* local = (char *) _alloca((size * size) * sizeof (char));
#endif


#ifdef ZINTLOG
    write_log("");
    sprintf(str, "%d", pattern);
    write_log(str);
#endif

    /* all eight bitmask variants have been encoded in the 8 bits of the bytes
     * that make up the grid array. select them for evaluation according to the
     * desired pattern.*/
    for (x = 0; x < size; x++) {
        for (y = 0; y < size; y++) {
            if ((eval[(y * size) + x] & (0x01 << pattern)) != 0) {
                local[(y * size) + x] = '1';
            } else {
                local[(y * size) + x] = '0';
            }
        }
    }

#ifdef ZINTLOG
    //bitmask output
    for (y = 0; y < size; y++) {
        strcpy(str, "");
        for (x = 0; x < size; x++) {
            state = local[(y * size) + x];
            append_log(state);
        }
        write_log("");
    }
    write_log("");
#endif

    /* Test 1: Adjacent modules in row/column in same colour */
    /* Vertical */
    for (x = 0; x < size; x++) {
        state = local[x];
        block = 0;
        for (y = 0; y < size; y++) {
            if (local[(y * size) + x] == state) {
                block++;
            } else {
                if (block > 5) {
                    result += (3 + (block - 5));
                }
                block = 0;
                state = local[(y * size) + x];
            }
        }
        if (block > 5) {
            result += (3 + (block - 5));
        }
    }

    /* Horizontal */
    for (y = 0; y < size; y++) {
        state = local[y * size];
        block = 0;
        for (x = 0; x < size; x++) {
            if (local[(y * size) + x] == state) {
                block++;
            } else {
                if (block > 5) {
                    result += (3 + (block - 5));
                }
                block = 0;
                state = local[(y * size) + x];
            }
        }
        if (block > 5) {
            result += (3 + (block - 5));
        }
    }

#ifdef ZINTLOG
    /* output Test 1 */
    sprintf(str, "%d", result);
    result_b = result;
    write_log(str);
#endif

    /* Test 2: Block of modules in same color */
    for (x = 0; x < size - 1; x++) {
        for (y = 0; y < size - 1; y++) {
            if (((local[(y * size) + x] == local[((y + 1) * size) + x]) &&
                    (local[(y * size) + x] == local[(y * size) + (x + 1)])) &&
                    (local[(y * size) + x] == local[((y + 1) * size) + (x + 1)])) {
                result += 3;
            }
        }
    }

#ifdef ZINTLOG
    /* output Test 2 */
    sprintf(str, "%d", result - result_b);
    result_b = result;
    write_log(str);
#endif

    /* Test 3: 1:1:3:1:1 ratio pattern in row/column */
    /* Vertical */
    for (x = 0; x < size; x++) {
        for (y = 0; y < (size - 7); y++) {
            p = 0;
            for (weight = 0; weight < 7; weight++) {
                if (local[((y + weight) * size) + x] == '1') {
                    p += (0x40 >> weight);
                }
            }
            if (p == 0x5d) {
                /* Pattern found, check before and after */
                beforeCount = 0;
                for (b = (y - 4); b < y; b++) {
                    if (b < 0) {
                        beforeCount++;
                    } else {
                        if (local[(b * size) + x] == '0') {
                            beforeCount++;
                        } else {
                            beforeCount = 0;
                        }
                    }
                }

                afterCount = 0;
                for (a = (y + 7); a <= (y + 10); a++) {
                    if (a >= size) {
                        afterCount++;
                    } else {
                        if (local[(a * size) + x] == '0') {
                            afterCount++;
                        } else {
                            afterCount = 0;
                        }
                    }
                }

                if ((beforeCount == 4) || (afterCount == 4)) {
                    /* Pattern is preceeded or followed by light area
                       4 modules wide */
                    result += 40;
                }
            }
        }
    }

    /* Horizontal */
    for (y = 0; y < size; y++) {
        for (x = 0; x < (size - 7); x++) {
            p = 0;
            for (weight = 0; weight < 7; weight++) {
                if (local[(y * size) + x + weight] == '1') {
                    p += (0x40 >> weight);
                }
            }
            if (p == 0x5d) {
                /* Pattern found, check before and after */
                beforeCount = 0;
                for (b = (x - 4); b < x; b++) {
                    if (b < 0) {
                        beforeCount++;
                    } else {
                        if (local[(y * size) + b] == '0') {
                            beforeCount++;
                        } else {
                            beforeCount = 0;
                        }
                    }
                }

                afterCount = 0;
                for (a = (x + 7); a <= (x + 10); a++) {
                    if (a >= size) {
                        afterCount++;
                    } else {
                        if (local[(y * size) + a] == '0') {
                            afterCount++;
                        } else {
                            afterCount = 0;
                        }
                    }
                }

                if ((beforeCount == 4) || (afterCount == 4)) {
                    /* Pattern is preceeded or followed by light area
                       4 modules wide */
                    result += 40;
                }
            }
        }
    }

#ifdef ZINTLOG
    /* output Test 3 */
    sprintf(str, "%d", result - result_b);
    result_b = result;
    write_log(str);
#endif

    /* Test 4: Proportion of dark modules in entire symbol */
    dark_mods = 0;
    for (x = 0; x < size; x++) {
        for (y = 0; y < size; y++) {
            if (local[(y * size) + x] == '1') {
                dark_mods++;
            }
        }
    }
    percentage = 100 * (dark_mods / (size * size));
    if (percentage <= 50) {
        k = ((100 - percentage) - 50) / 5;
    } else {
        k = (percentage - 50) / 5;
    }

    result += 10 * k;

#ifdef ZINTLOG
    /* output Test 4+summary */
    sprintf(str, "%d", result - result_b);
    write_log(str);
    write_log("==========");
    sprintf(str, "%d", result);
    write_log(str);
#endif

    return result;
}

static void add_format_info_eval(unsigned char *eval,const int size,const int ecc_level,const int pattern) {
    /* Add format information to grid */

    int format = pattern;
    unsigned int seq;
    int i;

    switch (ecc_level) {
        case LEVEL_L: format += 0x08;
            break;
        case LEVEL_Q: format += 0x18;
            break;
        case LEVEL_H: format += 0x10;
            break;
    }

    seq = qr_annex_c[format];

    for (i = 0; i < 6; i++) {
        eval[(i * size) + 8] = ((seq >> i) & 0x01) ? (0x01 >> pattern) : 0x00;
    }

    for (i = 0; i < 8; i++) {
        eval[(8 * size) + (size - i - 1)] = ((seq >> i) & 0x01) ? (0x01 >> pattern) : 0x00;
    }

    for (i = 0; i < 6; i++) {
        eval[(8 * size) + (5 - i)] = ((seq >> (i + 9)) & 0x01) ? (0x01 >> pattern) : 0x00;
    }

    for (i = 0; i < 7; i++) {
        eval[(((size - 7) + i) * size) + 8] = ((seq >> (i + 8)) & 0x01) ? (0x01 >> pattern) : 0x00;
    }

    eval[(7 * size) + 8] = ((seq >> 6) & 0x01) ? (0x01 >> pattern) : 0x00;
    eval[(8 * size) + 8] = ((seq >> 7) & 0x01) ? (0x01 >> pattern) : 0x00;
    eval[(8 * size) + 7] = ((seq >> 8) & 0x01) ? (0x01 >> pattern) : 0x00;
}

static int apply_bitmask(unsigned char *grid,const int size,const int ecc_level) {
    int x, y;
    unsigned char p;
    int pattern, penalty[8];
    int best_val, best_pattern;

#ifndef _MSC_VER
    unsigned char mask[size * size];
    unsigned char eval[size * size];
#else
    unsigned char* mask = (unsigned char *) _alloca((size * size) * sizeof (unsigned char));
    unsigned char* eval = (unsigned char *) _alloca((size * size) * sizeof (unsigned char));
#endif

    /* Perform data masking */
    for (x = 0; x < size; x++) {
        for (y = 0; y < size; y++) {
            mask[(y * size) + x] = 0x00;

            // all eight bitmask variants are encoded in the 8 bits of the bytes that make up the mask array.
            if (!(grid[(y * size) + x] & 0xf0)) { // exclude areas not to be masked.
                if (((y + x) & 1) == 0) {
                    mask[(y * size) + x] += 0x01;
                }
                if ((y & 1) == 0) {
                    mask[(y * size) + x] += 0x02;
                }
                if ((x % 3) == 0) {
                    mask[(y * size) + x] += 0x04;
                }
                if (((y + x) % 3) == 0) {
                    mask[(y * size) + x] += 0x08;
                }
                if ((((y / 2) + (x / 3)) & 1) == 0) {
                    mask[(y * size) + x] += 0x10;
                }
                if ((((y * x) & 1) + ((y * x) % 3)) == 0) {
                    mask[(y * size) + x] += 0x20;
                }
                if (((((y * x) & 1) + ((y * x) % 3)) & 1) == 0) {
                    mask[(y * size) + x] += 0x40;
                }
                if (((((y + x) & 1) + ((y * x) % 3)) & 1) == 0) {
                    mask[(y * size) + x] += 0x80;
                }
            }
        }
    }

    // apply data masks to grid, result in eval
    for (x = 0; x < size; x++) {
        for (y = 0; y < size; y++) {
            if (grid[(y * size) + x] & 0x01) {
                p = 0xff;
            } else {
                p = 0x00;
            }

            eval[(y * size) + x] = mask[(y * size) + x] ^ p;
        }
    }


    /* Evaluate result */
    for (pattern = 0; pattern < 8; pattern++) {

        add_format_info_eval(eval, size, ecc_level, pattern);

        penalty[pattern] = evaluate(eval, size, pattern);
    }

    best_pattern = 0;
    best_val = penalty[0];
    for (pattern = 1; pattern < 8; pattern++) {
        if (penalty[pattern] < best_val) {
            best_pattern = pattern;
            best_val = penalty[pattern];
        }
    }

#ifdef ZINTLOG
    char str[15];
    sprintf(str, "%d", best_val);
    write_log("choosed pattern:");
    write_log(str);
#endif

    /* Apply mask */
    for (x = 0; x < size; x++) {
        for (y = 0; y < size; y++) {
            if (mask[(y * size) + x] & (0x01 << best_pattern)) {
                if (grid[(y * size) + x] & 0x01) {
                    grid[(y * size) + x] = 0x00;
                } else {
                    grid[(y * size) + x] = 0x01;
                }
            }
        }
    }

    return best_pattern;
}

/* Add format information to grid */
static void add_format_info(unsigned char *grid,const int size,const int ecc_level,const int pattern) {
    int format = pattern;
    unsigned int seq;
    int i;

    switch (ecc_level) {
        case LEVEL_L: format += 0x08;
            break;
        case LEVEL_Q: format += 0x18;
            break;
        case LEVEL_H: format += 0x10;
            break;
    }

    seq = qr_annex_c[format];

    for (i = 0; i < 6; i++) {
        grid[(i * size) + 8] += (seq >> i) & 0x01;
    }

    for (i = 0; i < 8; i++) {
        grid[(8 * size) + (size - i - 1)] += (seq >> i) & 0x01;
    }

    for (i = 0; i < 6; i++) {
        grid[(8 * size) + (5 - i)] += (seq >> (i + 9)) & 0x01;
    }

    for (i = 0; i < 7; i++) {
        grid[(((size - 7) + i) * size) + 8] += (seq >> (i + 8)) & 0x01;
    }

    grid[(7 * size) + 8] += (seq >> 6) & 0x01;
    grid[(8 * size) + 8] += (seq >> 7) & 0x01;
    grid[(8 * size) + 7] += (seq >> 8) & 0x01;
}

/* Add version information */
static void add_version_info(unsigned char *grid,const int size,const int version) {
    int i;

    long int version_data = qr_annex_d[version - 7];
    for (i = 0; i < 6; i++) {
        grid[((size - 11) * size) + i] += (version_data >> (i * 3)) & 0x41;
        grid[((size - 10) * size) + i] += (version_data >> ((i * 3) + 1)) & 0x41;
        grid[((size - 9) * size) + i] += (version_data >> ((i * 3) + 2)) & 0x41;
        grid[(i * size) + (size - 11)] += (version_data >> (i * 3)) & 0x41;
        grid[(i * size) + (size - 10)] += (version_data >> ((i * 3) + 1)) & 0x41;
        grid[(i * size) + (size - 9)] += (version_data >> ((i * 3) + 2)) & 0x41;
    }
}

/* Implements a custom optimisation algorithm, more efficient than that
   given in Annex J. */
static void applyOptimisation(const int version,char inputMode[], const size_t inputLength) {


    int blockCount = 0, block;
    int i, j;
    char currentMode = ' '; // Null
    int *blockLength;
    char *blockMode;

    for (i = 0; i < inputLength; i++) {
        if (inputMode[i] != currentMode) {
            currentMode = inputMode[i];
            blockCount++;
        }
    }

    blockLength = (int*) malloc(sizeof (int)*blockCount);
    assert(blockLength);
    if (!blockLength) return;
    blockMode = (char*) malloc(sizeof (char)*blockCount);
    assert(blockMode);
    if (!blockMode) {
        free(blockLength);
        return;
    }

    j = -1;
    currentMode = ' '; // Null
    for (i = 0; i < inputLength; i++) {
        if (inputMode[i] != currentMode) {
            j++;
            blockLength[j] = 1;
            blockMode[j] = inputMode[i];
            currentMode = inputMode[i];
        } else {
            blockLength[j]++;
        }
    }

    if (blockCount > 1) {
        // Search forward
        for (i = 0; i <= (blockCount - 2); i++) {
            if (blockMode[i] == 'B') {
                switch (blockMode[i + 1]) {
                    case 'K':
                        if (blockLength[i + 1] < tribus(version, 4, 5, 6)) {
                            blockMode[i + 1] = 'B';
                        }
                        break;
                    case 'A':
                        if (blockLength[i + 1] < tribus(version, 7, 8, 9)) {
                            blockMode[i + 1] = 'B';
                        }
                        break;
                    case 'N':
                        if (blockLength[i + 1] < tribus(version, 3, 4, 5)) {
                            blockMode[i + 1] = 'B';
                        }
                        break;
                }
            }

            if ((blockMode[i] == 'A')
                    && (blockMode[i + 1] == 'N')) {
                if (blockLength[i + 1] < tribus(version, 6, 8, 10)) {
                    blockMode[i + 1] = 'A';
                }
            }
        }

        // Search backward
        for (i = blockCount - 1; i > 0; i--) {
            if (blockMode[i] == 'B') {
                switch (blockMode[i - 1]) {
                    case 'K':
                        if (blockLength[i - 1] < tribus(version, 4, 5, 6)) {
                            blockMode[i - 1] = 'B';
                        }
                        break;
                    case 'A':
                        if (blockLength[i - 1] < tribus(version, 7, 8, 9)) {
                            blockMode[i - 1] = 'B';
                        }
                        break;
                    case 'N':
                        if (blockLength[i - 1] < tribus(version, 3, 4, 5)) {
                            blockMode[i - 1] = 'B';
                        }
                        break;
                }
            }

            if ((blockMode[i] == 'A')
                    && (blockMode[i - 1] == 'N')) {
                if (blockLength[i - 1] < tribus(version, 6, 8, 10)) {
                    blockMode[i - 1] = 'A';
                }
            }
        }
    }

    j = 0;
    for (block = 0; block < blockCount; block++) {
        currentMode = blockMode[block];
        for (i = 0; i < blockLength[block]; i++) {
            inputMode[j] = currentMode;
            j++;
        }
    }

    free(blockLength);
    free(blockMode);
}

static size_t blockLength(const size_t start,const char inputMode[],const size_t inputLength) {
    /* Find the length of the block starting from 'start' */
    size_t i;
    int    count;
    char mode = inputMode[start];

    count = 0;
    i = start;

    do {
        count++;
    } while (((i + count) < inputLength) && (inputMode[i + count] == mode));

    return count;
}

static int getBinaryLength(const int version, char inputMode[], const unsigned int inputData[], const size_t inputLength, const int gs1, const int eci) {
    /* Calculate the actual bitlength of the proposed binary string */
    size_t i;
    char currentMode;
    int    j;
    int count = 0;
    int alphalength;
    int percent = 0;

    applyOptimisation(version, inputMode, inputLength);

    currentMode = ' '; // Null

    if (gs1 == 1) {
        if (version < RMQR_VERSION) {
            count += 4;
        } else {
            count += 3;
        }
    }

    if (eci != 0) { // RMQR does not support ECI
        count += 4;
        if (eci <= 127) {
            count += 8;
        } else if (eci <= 16383) {
            count += 16;
        } else {
            count += 24;
        }
    }

    for (i = 0; i < inputLength; i++) {
        if (inputMode[i] != currentMode) {
            count += 4;
            switch (inputMode[i]) {
                case 'K':
                    if (version < RMQR_VERSION) {
                        count += tribus(version, 8, 10, 12);
                    } else {
                        count += 3 + rmqr_kanji_cci[version - RMQR_VERSION];
                    }
                    count += (blockLength(i, inputMode, inputLength) * 13);
                    break;
                case 'B':
                    if (version < RMQR_VERSION) {
                        count += tribus(version, 8, 16, 16);
                    } else {
                        count += 3 + rmqr_byte_cci[version - RMQR_VERSION];
                    }
                    for (j = i; j < (i + blockLength(i, inputMode, inputLength)); j++) {
                        if (inputData[j] > 0xff) {
                            count += 16;
                        } else {
                            count += 8;
                        }
                    }
                    break;
                case 'A':
                    if (version < RMQR_VERSION) {
                        count += tribus(version, 9, 11, 13);
                    } else {
                        count += 3 + rmqr_alphanum_cci[version - RMQR_VERSION];
                    }
                    alphalength = blockLength(i, inputMode, inputLength);
                    // In alphanumeric mode % becomes %%
                    for (j = i; j < (i + alphalength); j++) {
                        if (inputData[j] == '%') {
                            percent++;
                        }
                    }
                    alphalength += percent;
                    switch (alphalength % 2) {
                        case 0:
                            count += (alphalength / 2) * 11;
                            break;
                        case 1:
                            count += ((alphalength - 1) / 2) * 11;
                            count += 6;
                            break;
                    }
                    break;
                case 'N':
                    if (version < RMQR_VERSION) {
                        count += tribus(version, 10, 12, 14);
                    } else {
                        count += 3 + rmqr_numeric_cci[version - RMQR_VERSION];
                    }
                    switch (blockLength(i, inputMode, inputLength) % 3) {
                        case 0:
                            count += (blockLength(i, inputMode, inputLength) / 3) * 10;
                            break;
                        case 1:
                            count += ((blockLength(i, inputMode, inputLength) - 1) / 3) * 10;
                            count += 4;
                            break;
                        case 2:
                            count += ((blockLength(i, inputMode, inputLength) - 2) / 3) * 10;
                            count += 7;
                            break;
                    }
                    break;
            }
            currentMode = inputMode[i];
        }
    }

    return count;
}

static void qr_test_codeword_dump(struct zint_symbol *symbol, unsigned char* codewords, int length) {
    int i;
    for (i = 0; i < length && i < 33; i++) { // 33*3 < errtxt 100 chars
        sprintf(symbol->errtxt + i * 3, "%02X ", codewords[i]);
    }
    symbol->errtxt[strlen(symbol->errtxt) - 1] = '\0';
}

int qr_code(struct zint_symbol *symbol, const unsigned char source[], size_t length) {
    int i, j, est_binlen;
    int ecc_level, autosize, version, max_cw, target_codewords, blocks, size;
    int bitmask, gs1;
    int canShrink;

#ifndef _MSC_VER
    unsigned int jisdata[length + 1];
    char mode[length + 1];
#else
    unsigned char* datastream;
    unsigned char* fullstream;
    unsigned char* grid;
    unsigned int* jisdata = (unsigned int *) _alloca((length + 1) * sizeof (unsigned int));
    char* mode = (char *) _alloca(length + 1);
#endif

    gs1 = ((symbol->input_mode & 0x07) == GS1_MODE);

    if ((symbol->input_mode & 0x07) == DATA_MODE) {
        sjis_cpy(source, &length, jisdata);
    } else {
        int done = 0;
        if (symbol->eci != 20) { /* Unless ECI 20 (Shift JIS) */
            /* Try single byte (Latin) conversion first */
            int error_number = sjis_utf8tosb(symbol->eci && symbol->eci <= 899 ? symbol->eci : 3, source, &length, jisdata);
            if (error_number == 0) {
                done = 1;
            } else if (symbol->eci && symbol->eci <= 899) {
                strcpy(symbol->errtxt, "575: Invalid characters in input data");
                return error_number;
            }
        }
        if (!done) {
            /* Try Shift-JIS */
            int error_number = sjis_utf8tomb(symbol, source, &length, jisdata);
            if (error_number != 0) {
                return error_number;
            }
        }
    }

    define_mode(mode, jisdata, length, gs1);
    est_binlen = getBinaryLength(40, mode, jisdata, length, gs1, symbol->eci);

    ecc_level = LEVEL_L;
    max_cw = 2956;
    if ((symbol->option_1 >= 1) && (symbol->option_1 <= 4)) {
        switch (symbol->option_1) {
            case 1: ecc_level = LEVEL_L;
                max_cw = 2956;
                break;
            case 2: ecc_level = LEVEL_M;
                max_cw = 2334;
                break;
            case 3: ecc_level = LEVEL_Q;
                max_cw = 1666;
                break;
            case 4: ecc_level = LEVEL_H;
                max_cw = 1276;
                break;
        }
    }

    if (est_binlen > (8 * max_cw)) {
        strcpy(symbol->errtxt, "561: Input too long for selected error correction level");
        return ZINT_ERROR_TOO_LONG;
    }

    autosize = 40;
    for (i = 39; i >= 0; i--) {
        switch (ecc_level) {
            case LEVEL_L:
                if ((8 * qr_data_codewords_L[i]) >= est_binlen) {
                    autosize = i + 1;
                }
                break;
            case LEVEL_M:
                if ((8 * qr_data_codewords_M[i]) >= est_binlen) {
                    autosize = i + 1;
                }
                break;
            case LEVEL_Q:
                if ((8 * qr_data_codewords_Q[i]) >= est_binlen) {
                    autosize = i + 1;
                }
                break;
            case LEVEL_H:
                if ((8 * qr_data_codewords_H[i]) >= est_binlen) {
                    autosize = i + 1;
                }
                break;
        }
    }

    // Now see if the optimised binary will fit in a smaller symbol.
    canShrink = 1;

    do {
        if (autosize == 1) {
            canShrink = 0;
        } else {
            est_binlen = getBinaryLength(autosize - 1, mode, jisdata, length, gs1, symbol->eci);

            switch (ecc_level) {
                case LEVEL_L:
                    if ((8 * qr_data_codewords_L[autosize - 2]) < est_binlen) {
                        canShrink = 0;
                    }
                    break;
                case LEVEL_M:
                    if ((8 * qr_data_codewords_M[autosize - 2]) < est_binlen) {
                        canShrink = 0;
                    }
                    break;
                case LEVEL_Q:
                    if ((8 * qr_data_codewords_Q[autosize - 2]) < est_binlen) {
                        canShrink = 0;
                    }
                    break;
                case LEVEL_H:
                    if ((8 * qr_data_codewords_H[autosize - 2]) < est_binlen) {
                        canShrink = 0;
                    }
                    break;
            }

            if (canShrink == 1) {
                // Optimisation worked - data will fit in a smaller symbol
                autosize--;
            } else {
                // Data did not fit in the smaller symbol, revert to original size
                est_binlen = getBinaryLength(autosize, mode, jisdata, length, gs1, symbol->eci);
            }
        }
    } while (canShrink == 1);

    version = autosize;

    if ((symbol->option_2 >= 1) && (symbol->option_2 <= 40)) {
        /* If the user has selected a larger symbol than the smallest available,
         then use the size the user has selected, and re-optimise for this
         symbol size.
         */
        if (symbol->option_2 > version) {
            version = symbol->option_2;
            est_binlen = getBinaryLength(symbol->option_2, mode, jisdata, length, gs1, symbol->eci);
        }

        if (symbol->option_2 < version) {
            strcpy(symbol->errtxt, "569: Input too long for selected symbol size");
            return ZINT_ERROR_TOO_LONG;
        }
    }

    /* Ensure maxium error correction capacity */
    if (est_binlen <= qr_data_codewords_M[version - 1] * 8) {
        ecc_level = LEVEL_M;
    }
    if (est_binlen <= qr_data_codewords_Q[version - 1] * 8) {
        ecc_level = LEVEL_Q;
    }
    if (est_binlen <= qr_data_codewords_H[version - 1] * 8) {
        ecc_level = LEVEL_H;
    }

    target_codewords = qr_data_codewords_L[version - 1];
    blocks = qr_blocks_L[version - 1];
    switch (ecc_level) {
        case LEVEL_M: target_codewords = qr_data_codewords_M[version - 1];
            blocks = qr_blocks_M[version - 1];
            break;
        case LEVEL_Q: target_codewords = qr_data_codewords_Q[version - 1];
            blocks = qr_blocks_Q[version - 1];
            break;
        case LEVEL_H: target_codewords = qr_data_codewords_H[version - 1];
            blocks = qr_blocks_H[version - 1];
            break;
    }

#ifndef _MSC_VER
    unsigned char datastream[target_codewords + 1];
    unsigned char fullstream[qr_total_codewords[version - 1] + 1];
#else
    datastream = (unsigned char *) _alloca(target_codewords + 1);
    fullstream = (unsigned char *) _alloca(qr_total_codewords[version - 1] + 1);
#endif

    qr_binary(datastream, version, target_codewords, mode, jisdata, length, gs1, symbol->eci, est_binlen, symbol->debug);
    if (symbol->debug & ZINT_DEBUG_TEST) qr_test_codeword_dump(symbol, datastream, target_codewords);
    add_ecc(fullstream, datastream, version, target_codewords, blocks, symbol->debug);

    size = qr_sizes[version - 1];
#ifndef _MSC_VER
    unsigned char grid[size * size];
#else
    grid = (unsigned char *) _alloca((size * size) * sizeof (unsigned char));
#endif

    for (i = 0; i < size; i++) {
        for (j = 0; j < size; j++) {
            grid[(i * size) + j] = 0;
        }
    }

    setup_grid(grid, size, version);
    populate_grid(grid, size, size, fullstream, qr_total_codewords[version - 1]);

    if (version >= 7) {
        add_version_info(grid, size, version);
    }

    bitmask = apply_bitmask(grid, size, ecc_level);

    add_format_info(grid, size, ecc_level, bitmask);



    symbol->width = size;
    symbol->rows = size;

    for (i = 0; i < size; i++) {
        for (j = 0; j < size; j++) {
            if (grid[(i * size) + j] & 0x01) {
                set_module(symbol, i, j);
            }
        }
        symbol->row_height[i] = 1;
    }

    return 0;
}

static int micro_qr_intermediate(char binary[], const unsigned int jisdata[], const char mode[], const size_t length, int *kanji_used, int *alphanum_used, int *byte_used, const int debug) {
    /* Convert input data to an "intermediate stage" where data is binary encoded but
       control information is not */
    int position = 0;
    int i;
    char buffer[2];

    strcpy(binary, "");

    if (debug & ZINT_DEBUG_PRINT) {
        for (i = 0; i < length; i++) {
            printf("%c", mode[i]);
        }
        printf("\n");
    }

    do {
        char data_block;
        int short_data_block_length = 0;
        int double_byte = 0;
        if (strlen(binary) > 128) {
            return ZINT_ERROR_TOO_LONG;
        }

        data_block = mode[position];
        do {
            if (data_block == 'B' && jisdata[position + short_data_block_length] > 0xFF) {
                double_byte++;
            }
            short_data_block_length++;
        } while (((short_data_block_length + position) < length) && (mode[position + short_data_block_length] == data_block));

        switch (data_block) {
            case 'K':
                /* Kanji mode */
                /* Mode indicator */
                strcat(binary, "K");
                *kanji_used = 1;

                /* Character count indicator */
                buffer[0] = short_data_block_length;
                buffer[1] = '\0';
                strcat(binary, buffer);

                if (debug & ZINT_DEBUG_PRINT) {
                    printf("Kanji block (length %d)\n\t", short_data_block_length);
                }

                /* Character representation */
                for (i = 0; i < short_data_block_length; i++) {
                    int jis = jisdata[position + i];
                    int prod;

                    if (jis >= 0x8140 && jis <= 0x9ffc)
                        jis -= 0x8140;

                    else if (jis >= 0xe040 && jis <= 0xebbf)
                        jis -= 0xc140;

                    prod = ((jis >> 8) * 0xc0) + (jis & 0xff);

                    bin_append(prod, 13, binary);

                    if (debug & ZINT_DEBUG_PRINT) {
                        printf("0x%4X ", prod);
                    }

                    if (strlen(binary) > 128) {
                        return ZINT_ERROR_TOO_LONG;
                    }
                }

                if (debug & ZINT_DEBUG_PRINT) {
                    printf("\n");
                }

                break;
            case 'B':
                /* Byte mode */
                /* Mode indicator */
                strcat(binary, "B");
                *byte_used = 1;

                /* Character count indicator */
                buffer[0] = short_data_block_length + double_byte;
                buffer[1] = '\0';
                strcat(binary, buffer);

                if (debug & ZINT_DEBUG_PRINT) {
                    printf("Byte block (length %d)\n\t", short_data_block_length + double_byte);
                }

                /* Character representation */
                for (i = 0; i < short_data_block_length; i++) {
                    int byte = jisdata[position + i];

                    bin_append(byte, byte > 0xFF ? 16 : 8, binary);

                    if (debug & ZINT_DEBUG_PRINT) {
                        printf("0x%4X ", byte);
                    }

                    if (strlen(binary) > 128) {
                        return ZINT_ERROR_TOO_LONG;
                    }
                }

                if (debug & ZINT_DEBUG_PRINT) {
                    printf("\n");
                }

                break;
            case 'A':
                /* Alphanumeric mode */
                /* Mode indicator */
                strcat(binary, "A");
                *alphanum_used = 1;

                /* Character count indicator */
                buffer[0] = short_data_block_length;
                buffer[1] = '\0';
                strcat(binary, buffer);

                if (debug & ZINT_DEBUG_PRINT) {
                    printf("Alpha block (length %d)\n\t", short_data_block_length);
                }

                /* Character representation */
                i = 0;
                while (i < short_data_block_length) {
                    int count;
                    int first = 0, prod;

                    first = posn(RHODIUM, (char) jisdata[position + i]);
                    count = 1;
                    prod = first;

                    if (i + 1 < short_data_block_length && mode[position + i + 1] == 'A') {
                        int second = posn(RHODIUM, (char) jisdata[position + i + 1]);
                        count = 2;
                        prod = (first * 45) + second;
                    }

                    bin_append(prod, 1 + (5 * count), binary);

                    if (debug & ZINT_DEBUG_PRINT) {
                        printf("0x%4X ", prod);
                    }

                    if (strlen(binary) > 128) {
                        return ZINT_ERROR_TOO_LONG;
                    }

                    i += 2;
                };

                if (debug & ZINT_DEBUG_PRINT) {
                    printf("\n");
                }

                break;
            case 'N':
                /* Numeric mode */
                /* Mode indicator */
                strcat(binary, "N");

                /* Character count indicator */
                buffer[0] = short_data_block_length;
                buffer[1] = '\0';
                strcat(binary, buffer);

                if (debug & ZINT_DEBUG_PRINT) {
                    printf("Number block (length %d)\n\t", short_data_block_length);
                }

                /* Character representation */
                i = 0;
                while (i < short_data_block_length) {
                    int count;
                    int first = 0, prod;

                    first = posn(NEON, (char) jisdata[position + i]);
                    count = 1;
                    prod = first;

                    if (i + 1 < short_data_block_length && mode[position + i + 1] == 'N') {
                        int second = posn(NEON, (char) jisdata[position + i + 1]);
                        count = 2;
                        prod = (prod * 10) + second;
                    }

                    if (i + 2 < short_data_block_length && mode[position + i + 2] == 'N') {
                        int third = posn(NEON, (char) jisdata[position + i + 2]);
                        count = 3;
                        prod = (prod * 10) + third;
                    }

                    bin_append(prod, 1 + (3 * count), binary);

                    if (debug & ZINT_DEBUG_PRINT) {
                        printf("0x%4X (%d)", prod, prod);
                    }

                    if (strlen(binary) > 128) {
                        return ZINT_ERROR_TOO_LONG;
                    }

                    i += 3;
                };

                if (debug & ZINT_DEBUG_PRINT) {
                    printf("\n");
                }

                break;
        }

        position += short_data_block_length;
    } while (position < length);

    return 0;
}

static void get_bitlength(int count[],const char stream[]) {
    size_t length;
    int    i;

    length = strlen(stream);

    for (i = 0; i < 4; i++) {
        count[i] = 0;
    }

    i = 0;
    do {
        if ((stream[i] == '0') || (stream[i] == '1')) {
            count[0]++;
            count[1]++;
            count[2]++;
            count[3]++;
            i++;
        } else {
            switch (stream[i]) {
                case 'K':
                    count[2] += 5;
                    count[3] += 7;
                    i += 2;
                    break;
                case 'B':
                    count[2] += 6;
                    count[3] += 8;
                    i += 2;
                    break;
                case 'A':
                    count[1] += 4;
                    count[2] += 6;
                    count[3] += 8;
                    i += 2;
                    break;
                case 'N':
                    count[0] += 3;
                    count[1] += 5;
                    count[2] += 7;
                    count[3] += 9;
                    i += 2;
                    break;
            }
        }
    } while (i < length);
}

static void microqr_expand_binary(const char binary_stream[], char full_stream[],const int version) {
    int    i;
    size_t length;

    length = strlen(binary_stream);

    i = 0;
    do {
        switch (binary_stream[i]) {
            case '1': strcat(full_stream, "1");
                i++;
                break;
            case '0': strcat(full_stream, "0");
                i++;
                break;
            case 'N':
                /* Numeric Mode */
                /* Mode indicator */
                switch (version) {
                    case 1: strcat(full_stream, "0");
                        break;
                    case 2: strcat(full_stream, "00");
                        break;
                    case 3: strcat(full_stream, "000");
                        break;
                }

                /* Character count indicator */
                bin_append(binary_stream[i + 1], 3 + version, full_stream); /* version = 0..3 */

                i += 2;
                break;
            case 'A':
                /* Alphanumeric Mode */
                /* Mode indicator */
                switch (version) {
                    case 1: strcat(full_stream, "1");
                        break;
                    case 2: strcat(full_stream, "01");
                        break;
                    case 3: strcat(full_stream, "001");
                        break;
                }

                /* Character count indicator */
                bin_append(binary_stream[i + 1], 2 + version, full_stream); /* version = 1..3 */

                i += 2;
                break;
            case 'B':
                /* Byte Mode */
                /* Mode indicator */
                switch (version) {
                    case 2: strcat(full_stream, "10");
                        break;
                    case 3: strcat(full_stream, "010");
                        break;
                }

                /* Character count indicator */
                bin_append(binary_stream[i + 1], 2 + version, full_stream); /* version = 2..3 */

                i += 2;
                break;
            case 'K':
                /* Kanji Mode */
                /* Mode indicator */
                switch (version) {
                    case 2: strcat(full_stream, "11");
                        break;
                    case 3: strcat(full_stream, "011");
                        break;
                }

                /* Character count indicator */
                bin_append(binary_stream[i + 1], 1 + version, full_stream); /* version = 2..3 */

                i += 2;
                break;
        }

    } while (i < length);
}

static void micro_qr_m1(struct zint_symbol *symbol, char binary_data[]) {
    int i, j, latch;
    int bits_total, bits_left;
    int data_codewords, ecc_codewords;
    unsigned char data_blocks[4], ecc_blocks[3];

    bits_total = 20;
    latch = 0;

    /* Add terminator */
    bits_left = bits_total - (int)strlen(binary_data);
    if (bits_left <= 3) {
        for (i = 0; i < bits_left; i++) {
            strcat(binary_data, "0");
        }
        latch = 1;
    } else {
        strcat(binary_data, "000");
    }

    if (latch == 0) {
        /* Manage last (4-bit) block */
        bits_left = bits_total - (int)strlen(binary_data);
        if (bits_left <= 4) {
            for (i = 0; i < bits_left; i++) {
                strcat(binary_data, "0");
            }
            latch = 1;
        }
    }

    if (latch == 0) {
        /* Complete current byte */
        int remainder = 8 - (strlen(binary_data) % 8);
        if (remainder == 8) {
            remainder = 0;
        }
        for (i = 0; i < remainder; i++) {
            strcat(binary_data, "0");
        }

        /* Add padding */
        bits_left = bits_total - (int)strlen(binary_data);
        if (bits_left > 4) {
            remainder = (bits_left - 4) / 8;
            for (i = 0; i < remainder; i++) {
                strcat(binary_data, (i & 1) ? "00010001" : "11101100");
            }
        }
        bin_append(0, 4, binary_data);
    }

    data_codewords = 3;
    ecc_codewords = 2;

    /* Copy data into codewords */
    for (i = 0; i < (data_codewords - 1); i++) {
        data_blocks[i] = 0;
        for (j = 0; j < 8; j++) {
            if (binary_data[(i * 8) + j] == '1') {
                data_blocks[i] += 0x80 >> j;
            }
        }
    }
    data_blocks[2] = 0;
    for (j = 0; j < 4; j++) {
        if (binary_data[16 + j] == '1') {
            data_blocks[2] += 0x80 >> j;
        }
    }

    if (symbol->debug & ZINT_DEBUG_TEST) qr_test_codeword_dump(symbol, data_blocks, data_codewords);

    /* Calculate Reed-Solomon error codewords */
    rs_init_gf(0x11d);
    rs_init_code(ecc_codewords, 0);
    rs_encode(data_codewords, data_blocks, ecc_blocks);
    rs_free();

    /* Add Reed-Solomon codewords to binary data */
    for (i = 0; i < ecc_codewords; i++) {
        bin_append(ecc_blocks[ecc_codewords - i - 1], 8, binary_data);
    }
}

static void micro_qr_m2(struct zint_symbol *symbol, char binary_data[], const int ecc_mode) {
    int i, j, latch;
    int bits_total=0, bits_left;
    int data_codewords=0, ecc_codewords=0;
    unsigned char data_blocks[6], ecc_blocks[7];

    latch = 0;

    if (ecc_mode == LEVEL_L) {
        bits_total = 40;
    }
    else if (ecc_mode == LEVEL_M) {
        bits_total = 32;
    }
    else assert(0);

    /* Add terminator */
    bits_left = bits_total - (int)strlen(binary_data);
    if (bits_left <= 5) {
        for (i = 0; i < bits_left; i++) {
            strcat(binary_data, "0");
        }
        latch = 1;
    } else {
        bin_append(0, 5, binary_data);
    }

    if (latch == 0) {
        /* Complete current byte */
        int remainder = 8 - (strlen(binary_data) % 8);
        if (remainder == 8) {
            remainder = 0;
        }
        for (i = 0; i < remainder; i++) {
            strcat(binary_data, "0");
        }

        /* Add padding */
        bits_left = bits_total - (int)strlen(binary_data);
        remainder = bits_left / 8;
        for (i = 0; i < remainder; i++) {
            strcat(binary_data, (i & 1) ? "00010001" : "11101100");
        }
    }

    if (ecc_mode == LEVEL_L) {
        data_codewords = 5;
        ecc_codewords = 5;
    }
    else if (ecc_mode == LEVEL_M) {
        data_codewords = 4;
        ecc_codewords = 6;
    }
    else assert(0);

    /* Copy data into codewords */
    for (i = 0; i < data_codewords; i++) {
        data_blocks[i] = 0;

        for (j = 0; j < 8; j++) {
            if (binary_data[(i * 8) + j] == '1') {
                data_blocks[i] += 0x80 >> j;
            }
        }
    }

    if (symbol->debug & ZINT_DEBUG_TEST) qr_test_codeword_dump(symbol, data_blocks, data_codewords);

    /* Calculate Reed-Solomon error codewords */
    rs_init_gf(0x11d);
    rs_init_code(ecc_codewords, 0);
    rs_encode(data_codewords, data_blocks, ecc_blocks);
    rs_free();

    /* Add Reed-Solomon codewords to binary data */
    for (i = 0; i < ecc_codewords; i++) {
        bin_append(ecc_blocks[ecc_codewords - i - 1], 8, binary_data);
    }

    return;
}

static void micro_qr_m3(struct zint_symbol *symbol, char binary_data[], const int ecc_mode) {
    int i, j, latch;
    int bits_total=0, bits_left;
    int data_codewords=0, ecc_codewords=0;
    unsigned char data_blocks[12], ecc_blocks[9];

    latch = 0;

    if (ecc_mode == LEVEL_L) {
        bits_total = 84;
    }
    else if (ecc_mode == LEVEL_M) {
        bits_total = 68;
    }
    else assert(0);

    /* Add terminator */
    bits_left = bits_total - (int)strlen(binary_data);
    if (bits_left <= 7) {
        for (i = 0; i < bits_left; i++) {
            strcat(binary_data, "0");
        }
        latch = 1;
    } else {
        bin_append(0, 7, binary_data);
    }

    if (latch == 0) {
        /* Manage last (4-bit) block */
        bits_left = bits_total - (int)strlen(binary_data);
        if (bits_left <= 4) {
            for (i = 0; i < bits_left; i++) {
                strcat(binary_data, "0");
            }
            latch = 1;
        }
    }

    if (latch == 0) {
        /* Complete current byte */
        int remainder = 8 - (strlen(binary_data) % 8);
        if (remainder == 8) {
            remainder = 0;
        }
        for (i = 0; i < remainder; i++) {
            strcat(binary_data, "0");
        }

        /* Add padding */
        bits_left = bits_total - (int)strlen(binary_data);
        if (bits_left > 4) {
            remainder = (bits_left - 4) / 8;
            for (i = 0; i < remainder; i++) {
                strcat(binary_data, (i & 1) ? "00010001" : "11101100");
            }
        }
        bin_append(0, 4, binary_data);
    }

    if (ecc_mode == LEVEL_L) {
        data_codewords = 11;
        ecc_codewords = 6;
    }
    else if (ecc_mode == LEVEL_M) {
        data_codewords = 9;
        ecc_codewords = 8;
    }
    else assert(0);

    /* Copy data into codewords */
    for (i = 0; i < (data_codewords - 1); i++) {
        data_blocks[i] = 0;

        for (j = 0; j < 8; j++) {
            if (binary_data[(i * 8) + j] == '1') {
                data_blocks[i] += 0x80 >> j;
            }
        }
    }

    if (ecc_mode == LEVEL_L) {
        data_blocks[10] = 0;
        for (j = 0; j < 4; j++) {
            if (binary_data[80 + j] == '1') {
                data_blocks[10] += 0x80 >> j;
            }
        }
    }

    if (ecc_mode == LEVEL_M) {
        data_blocks[8] = 0;
        for (j = 0; j < 4; j++) {
            if (binary_data[64 + j] == '1') {
                data_blocks[8] += 0x80 >> j;
            }
        }
    }

    if (symbol->debug & ZINT_DEBUG_TEST) qr_test_codeword_dump(symbol, data_blocks, data_codewords);

    /* Calculate Reed-Solomon error codewords */
    rs_init_gf(0x11d);
    rs_init_code(ecc_codewords, 0);
    rs_encode(data_codewords, data_blocks, ecc_blocks);
    rs_free();

    /* Add Reed-Solomon codewords to binary data */
    for (i = 0; i < ecc_codewords; i++) {
        bin_append(ecc_blocks[ecc_codewords - i - 1], 8, binary_data);
    }

    return;
}

static void micro_qr_m4(struct zint_symbol *symbol, char binary_data[], const int ecc_mode) {
    int i, j, latch;
    int bits_total=0, bits_left;
    int data_codewords=0, ecc_codewords=0;
    unsigned char data_blocks[17], ecc_blocks[15];

    latch = 0;

    if (ecc_mode == LEVEL_L) {
        bits_total = 128;
    }
    else if (ecc_mode == LEVEL_M) {
        bits_total = 112;
    }
    else if (ecc_mode == LEVEL_Q) {
        bits_total = 80;
    }
    else assert(0);

    /* Add terminator */
    bits_left = bits_total - (int)strlen(binary_data);
    if (bits_left <= 9) {
        for (i = 0; i < bits_left; i++) {
            strcat(binary_data, "0");
        }
        latch = 1;
    } else {
        bin_append(0, 9, binary_data);
    }

    if (latch == 0) {
        /* Complete current byte */
        int remainder = 8 - (strlen(binary_data) % 8);
        if (remainder == 8) {
            remainder = 0;
        }
        for (i = 0; i < remainder; i++) {
            strcat(binary_data, "0");
        }

        /* Add padding */
        bits_left = bits_total - (int)strlen(binary_data);
        remainder = bits_left / 8;
        for (i = 0; i < remainder; i++) {
            strcat(binary_data, (i & 1) ? "00010001" : "11101100");
        }
    }

    if (ecc_mode == LEVEL_L) {
        data_codewords = 16;
        ecc_codewords = 8;
    }
    else if (ecc_mode == LEVEL_M) {
        data_codewords = 14;
        ecc_codewords = 10;
    }
    else if (ecc_mode == LEVEL_Q) {
        data_codewords = 10;
        ecc_codewords = 14;
    }
    else assert(0);

    /* Copy data into codewords */
    for (i = 0; i < data_codewords; i++) {
        data_blocks[i] = 0;

        for (j = 0; j < 8; j++) {
            if (binary_data[(i * 8) + j] == '1') {
                data_blocks[i] += 0x80 >> j;
            }
        }
    }

    if (symbol->debug & ZINT_DEBUG_TEST) qr_test_codeword_dump(symbol, data_blocks, data_codewords);

    /* Calculate Reed-Solomon error codewords */
    rs_init_gf(0x11d);
    rs_init_code(ecc_codewords, 0);
    rs_encode(data_codewords, data_blocks, ecc_blocks);
    rs_free();

    /* Add Reed-Solomon codewords to binary data */
    for (i = 0; i < ecc_codewords; i++) {
        bin_append(ecc_blocks[ecc_codewords - i - 1], 8, binary_data);
    }
}

static void micro_setup_grid(unsigned char* grid,const int size) {
    int i, toggle = 1;

    /* Add timing patterns */
    for (i = 0; i < size; i++) {
        if (toggle == 1) {
            grid[i] = 0x21;
            grid[(i * size)] = 0x21;
            toggle = 0;
        } else {
            grid[i] = 0x20;
            grid[(i * size)] = 0x20;
            toggle = 1;
        }
    }

    /* Add finder patterns */
    place_finder(grid, size, 0, 0);

    /* Add separators */
    for (i = 0; i < 7; i++) {
        grid[(7 * size) + i] = 0x10;
        grid[(i * size) + 7] = 0x10;
    }
    grid[(7 * size) + 7] = 0x10;


    /* Reserve space for format information */
    for (i = 0; i < 8; i++) {
        grid[(8 * size) + i] += 0x20;
        grid[(i * size) + 8] += 0x20;
    }
    grid[(8 * size) + 8] += 20;
}

static void micro_populate_grid(unsigned char* grid,const int size,const char full_stream[]) {
    int direction = 1; /* up */
    int row = 0; /* right hand side */
    size_t n;
    int i, y;

    n = strlen(full_stream);
    y = size - 1;
    i = 0;
    do {
        int x = (size - 2) - (row * 2);

        if (!(grid[(y * size) + (x + 1)] & 0xf0)) {
            if (full_stream[i] == '1') {
                grid[(y * size) + (x + 1)] = 0x01;
            } else {
                grid[(y * size) + (x + 1)] = 0x00;
            }
            i++;
        }

        if (i < n) {
            if (!(grid[(y * size) + x] & 0xf0)) {
                if (full_stream[i] == '1') {
                    grid[(y * size) + x] = 0x01;
                } else {
                    grid[(y * size) + x] = 0x00;
                }
                i++;
            }
        }

        if (direction) {
            y--;
        } else {
            y++;
        }
        if (y == 0) {
            /* reached the top */
            row++;
            y = 1;
            direction = 0;
        }
        if (y == size) {
            /* reached the bottom */
            row++;
            y = size - 1;
            direction = 1;
        }
    } while (i < n);
}

static int micro_evaluate(const unsigned char *grid,const int size,const int pattern) {
    int sum1, sum2, i, filter = 0, retval;

    switch (pattern) {
        case 0: filter = 0x01;
            break;
        case 1: filter = 0x02;
            break;
        case 2: filter = 0x04;
            break;
        case 3: filter = 0x08;
            break;
    }

    sum1 = 0;
    sum2 = 0;
    for (i = 1; i < size; i++) {
        if (grid[(i * size) + size - 1] & filter) {
            sum1++;
        }
        if (grid[((size - 1) * size) + i] & filter) {
            sum2++;
        }
    }

    if (sum1 <= sum2) {
        retval = (sum1 * 16) + sum2;
    } else {
        retval = (sum2 * 16) + sum1;
    }

    return retval;
}

static int micro_apply_bitmask(unsigned char *grid,const int size) {
    int x, y;
    unsigned char p;
    int pattern, value[8];
    int best_val, best_pattern;

#ifndef _MSC_VER
    unsigned char mask[size * size];
    unsigned char eval[size * size];
#else
    unsigned char* mask = (unsigned char *) _alloca((size * size) * sizeof (unsigned char));
    unsigned char* eval = (unsigned char *) _alloca((size * size) * sizeof (unsigned char));
#endif

    /* Perform data masking */
    for (x = 0; x < size; x++) {
        for (y = 0; y < size; y++) {
            mask[(y * size) + x] = 0x00;

            if (!(grid[(y * size) + x] & 0xf0)) {
                if ((y & 1) == 0) {
                    mask[(y * size) + x] += 0x01;
                }

                if ((((y / 2) + (x / 3)) & 1) == 0) {
                    mask[(y * size) + x] += 0x02;
                }

                if (((((y * x) & 1) + ((y * x) % 3)) & 1) == 0) {
                    mask[(y * size) + x] += 0x04;
                }

                if (((((y + x) & 1) + ((y * x) % 3)) & 1) == 0) {
                    mask[(y * size) + x] += 0x08;
                }
            }
        }
    }

    for (x = 0; x < size; x++) {
        for (y = 0; y < size; y++) {
            if (grid[(y * size) + x] & 0x01) {
                p = 0xff;
            } else {
                p = 0x00;
            }

            eval[(y * size) + x] = mask[(y * size) + x] ^ p;
        }
    }


    /* Evaluate result */
    for (pattern = 0; pattern < 8; pattern++) {
        value[pattern] = micro_evaluate(eval, size, pattern);
    }

    best_pattern = 0;
    best_val = value[0];
    for (pattern = 1; pattern < 4; pattern++) {
        if (value[pattern] > best_val) {
            best_pattern = pattern;
            best_val = value[pattern];
        }
    }

    /* Apply mask */
    for (x = 0; x < size; x++) {
        for (y = 0; y < size; y++) {
            if (mask[(y * size) + x] & (0x01 << best_pattern)) {
                if (grid[(y * size) + x] & 0x01) {
                    grid[(y * size) + x] = 0x00;
                } else {
                    grid[(y * size) + x] = 0x01;
                }
            }
        }
    }

    return best_pattern;
}

int microqr(struct zint_symbol *symbol, const unsigned char source[], size_t length) {
    size_t i;
    int    j, size;
    char binary_stream[200];
    char full_stream[200];

    unsigned int jisdata[40];
    char mode[40];
    int error_number, kanji_used = 0, alphanum_used = 0, byte_used = 0;
    int version_valid[4];
    int binary_count[4];
    int ecc_level, autoversion, version;
    int n_count, a_count, bitmask, format, format_full;
#ifdef _MSC_VER
    unsigned char* grid;
#endif

    if (length > 35) {
        strcpy(symbol->errtxt, "562: Input data too long");
        return ZINT_ERROR_TOO_LONG;
    }

    /* Check option 1 in combination with option 2 */
    ecc_level = LEVEL_L;
    if (symbol->option_1 >= 1 && symbol->option_1 <= 4) {
        if (symbol->option_1 == 4) {
            strcpy(symbol->errtxt, "566: Error correction level H not available");
            return ZINT_ERROR_INVALID_OPTION;
        }
        if (symbol->option_2 >= 1 && symbol->option_2 <= 4) {
            if (symbol->option_2 == 1 && symbol->option_1 != 1) {
                strcpy(symbol->errtxt, "574: Version M1 supports error correction level L only");
                return ZINT_ERROR_INVALID_OPTION;
            }
            if (symbol->option_2 != 4 && symbol->option_1 == 3) {
                strcpy(symbol->errtxt, "575: Error correction level Q requires Version M4");
                return ZINT_ERROR_INVALID_OPTION;
            }
        }
        ecc_level = symbol->option_1;
    }

    for (i = 0; i < 4; i++) {
        version_valid[i] = 1;
    }

    if ((symbol->input_mode & 0x07) == DATA_MODE) {
        sjis_cpy(source, &length, jisdata);
    } else {
        /* Try ISO 8859-1 conversion first */
        int error_number = sjis_utf8tosb(3, source, &length, jisdata);
        if (error_number != 0) {
            /* Try Shift-JIS */
            int error_number = sjis_utf8tomb(symbol, source, &length, jisdata);
            if (error_number != 0) {
                return error_number;
            }
        }
    }

    define_mode(mode, jisdata, length, 0);

    n_count = 0;
    a_count = 0;
    for (i = 0; i < length; i++) {
        if ((jisdata[i] >= '0') && (jisdata[i] <= '9')) {
            n_count++;
        }
        if (in_alpha(jisdata[i])) {
            a_count++;
        }
    }

    if (a_count == length) {
        /* All data can be encoded in Alphanumeric mode */
        for (i = 0; i < length; i++) {
            mode[i] = 'A';
        }
    }

    if (n_count == length) {
        /* All data can be encoded in Numeric mode */
        for (i = 0; i < length; i++) {
            mode[i] = 'N';
        }
    }

    error_number = micro_qr_intermediate(binary_stream, jisdata, mode, length, &kanji_used, &alphanum_used, &byte_used, symbol->debug);
    if (error_number != 0) {
        strcpy(symbol->errtxt, "564: Input data too long");
        return error_number;
    }

    get_bitlength(binary_count, binary_stream);

    /* Eliminate possible versions depending on type of content */
    if (byte_used) {
        version_valid[0] = 0;
        version_valid[1] = 0;
    }

    if (alphanum_used) {
        version_valid[0] = 0;
    }

    if (kanji_used) {
        version_valid[0] = 0;
        version_valid[1] = 0;
    }

    /* Eliminate possible versions depending on length of binary data */
    if (binary_count[0] > 20) {
        version_valid[0] = 0;
    }
    if (binary_count[1] > 40) {
        version_valid[1] = 0;
    }
    if (binary_count[2] > 84) {
        version_valid[2] = 0;
    }
    if (binary_count[3] > 128) {
        strcpy(symbol->errtxt, "565: Input data too long");
        return ZINT_ERROR_TOO_LONG;
    }

    /* Eliminate possible versions depending on error correction level specified */
    if (ecc_level == LEVEL_Q) {
        version_valid[0] = 0;
        version_valid[1] = 0;
        version_valid[2] = 0;
        if (binary_count[3] > 80) {
            strcpy(symbol->errtxt, "567: Input data too long");
            return ZINT_ERROR_TOO_LONG;
        }
    }

    if (ecc_level == LEVEL_M) {
        version_valid[0] = 0;
        if (binary_count[1] > 32) {
            version_valid[1] = 0;
        }
        if (binary_count[2] > 68) {
            version_valid[2] = 0;
        }
        if (binary_count[3] > 112) {
            strcpy(symbol->errtxt, "568: Input data too long");
            return ZINT_ERROR_TOO_LONG;
        }
    }

    autoversion = 3;
    if (version_valid[2]) {
        autoversion = 2;
    }
    if (version_valid[1]) {
        autoversion = 1;
    }
    if (version_valid[0]) {
        autoversion = 0;
    }

    version = autoversion;
    /* Get version from user */
    if ((symbol->option_2 >= 1) && (symbol->option_2 <= 4)) {
        if (symbol->option_2 - 1 >= autoversion) {
            version = symbol->option_2 - 1;
        } else {
            strcpy(symbol->errtxt, "570: Input too long for selected symbol size");
            return ZINT_ERROR_TOO_LONG;
        }
    }

    /* If there is enough unused space then increase the error correction level */
    if (version == 3) {
        if (binary_count[3] <= 112) {
            ecc_level = LEVEL_M;
        }
        if (binary_count[3] <= 80) {
            ecc_level = LEVEL_Q;
        }
    }

    if (version == 2) {
        if (binary_count[2] <= 68) {
            ecc_level = LEVEL_M;
        }
    }

    if (version == 1) {
        if (binary_count[1] <= 32) {
            ecc_level = LEVEL_M;
        }
    }

    strcpy(full_stream, "");
    microqr_expand_binary(binary_stream, full_stream, version);

    switch (version) {
        case 0: micro_qr_m1(symbol, full_stream);
            break;
        case 1: micro_qr_m2(symbol, full_stream, ecc_level);
            break;
        case 2: micro_qr_m3(symbol, full_stream, ecc_level);
            break;
        case 3: micro_qr_m4(symbol, full_stream, ecc_level);
            break;
    }

    size = micro_qr_sizes[version];
#ifndef _MSC_VER
    unsigned char grid[size * size];
#else
    grid = (unsigned char *) _alloca((size * size) * sizeof (unsigned char));
#endif

    for (i = 0; i < size; i++) {
        for (j = 0; j < size; j++) {
            grid[(i * size) + j] = 0;
        }
    }

    micro_setup_grid(grid, size);
    micro_populate_grid(grid, size, full_stream);
    bitmask = micro_apply_bitmask(grid, size);

    /* Add format data */
    format = 0;
    switch (version) {
        case 1: switch (ecc_level) {
                case 1: format = 1;
                    break;
                case 2: format = 2;
                    break;
            }
            break;
        case 2: switch (ecc_level) {
                case 1: format = 3;
                    break;
                case 2: format = 4;
                    break;
            }
            break;
        case 3: switch (ecc_level) {
                case 1: format = 5;
                    break;
                case 2: format = 6;
                    break;
                case 3: format = 7;
                    break;
            }
            break;
    }

    format_full = qr_annex_c1[(format << 2) + bitmask];

    if (format_full & 0x4000) {
        grid[(8 * size) + 1] += 0x01;
    }
    if (format_full & 0x2000) {
        grid[(8 * size) + 2] += 0x01;
    }
    if (format_full & 0x1000) {
        grid[(8 * size) + 3] += 0x01;
    }
    if (format_full & 0x800) {
        grid[(8 * size) + 4] += 0x01;
    }
    if (format_full & 0x400) {
        grid[(8 * size) + 5] += 0x01;
    }
    if (format_full & 0x200) {
        grid[(8 * size) + 6] += 0x01;
    }
    if (format_full & 0x100) {
        grid[(8 * size) + 7] += 0x01;
    }
    if (format_full & 0x80) {
        grid[(8 * size) + 8] += 0x01;
    }
    if (format_full & 0x40) {
        grid[(7 * size) + 8] += 0x01;
    }
    if (format_full & 0x20) {
        grid[(6 * size) + 8] += 0x01;
    }
    if (format_full & 0x10) {
        grid[(5 * size) + 8] += 0x01;
    }
    if (format_full & 0x08) {
        grid[(4 * size) + 8] += 0x01;
    }
    if (format_full & 0x04) {
        grid[(3 * size) + 8] += 0x01;
    }
    if (format_full & 0x02) {
        grid[(2 * size) + 8] += 0x01;
    }
    if (format_full & 0x01) {
        grid[(1 * size) + 8] += 0x01;
    }

    symbol->width = size;
    symbol->rows = size;

    for (i = 0; i < size; i++) {
        for (j = 0; j < size; j++) {
            if (grid[(i * size) + j] & 0x01) {
                set_module(symbol, i, j);
            }
        }
        symbol->row_height[i] = 1;
    }

    return 0;
}

/* For UPNQR the symbol size and error correction capacity is fixed */
int upnqr(struct zint_symbol *symbol, const unsigned char source[], size_t length) {
    int i, j, est_binlen;
    int ecc_level, version, target_codewords, blocks, size;
    int bitmask, error_number;

#ifndef _MSC_VER
    unsigned int jisdata[length + 1];
    char mode[length + 1];
#else
    unsigned char* datastream;
    unsigned char* fullstream;
    unsigned char* grid;
    unsigned int* jisdata = (unsigned int *) _alloca((length + 1) * sizeof (unsigned int));
    char* mode = (char *) _alloca(length + 1);
#endif

#ifndef _MSC_VER
    unsigned char preprocessed[length + 1];
#else
    unsigned char* preprocessed = (unsigned char*) _alloca(length + 1);
#endif

    symbol->eci = 4; /* Set before any processing */

    switch (symbol->input_mode & 0x07) {
        case DATA_MODE:
            /* Input is already in ISO-8859-2 format */
            for (i = 0; i < length; i++) {
                jisdata[i] = source[i];
                mode[i] = 'B';
            }
            break;
        case GS1_MODE:
            strcpy(symbol->errtxt, "571: UPNQR does not support GS-1 encoding");
            return ZINT_ERROR_INVALID_OPTION;
            break;
        case UNICODE_MODE:
            error_number = utf_to_eci(4, source, preprocessed, &length);
            if (error_number != 0) {
                strcpy(symbol->errtxt, "572: Invalid characters in input data");
                return error_number;
            }
            for (i = 0; i < length; i++) {
                jisdata[i] = preprocessed[i];
                mode[i] = 'B';
            }
            break;
    }

    est_binlen = getBinaryLength(15, mode, jisdata, length, 0, symbol->eci);

    ecc_level = LEVEL_M;

    if (est_binlen > 3320) {
        strcpy(symbol->errtxt, "573: Input too long for selected symbol");
        return ZINT_ERROR_TOO_LONG;
    }

    version = 15; // 77 x 77

    target_codewords = qr_data_codewords_M[version - 1];
    blocks = qr_blocks_M[version - 1];
#ifndef _MSC_VER
    unsigned char datastream[target_codewords + 1];
    unsigned char fullstream[qr_total_codewords[version - 1] + 1];
#else
    datastream = (unsigned char *) _alloca(target_codewords + 1);
    fullstream = (unsigned char *) _alloca(qr_total_codewords[version - 1] + 1);
#endif

    qr_binary(datastream, version, target_codewords, mode, jisdata, length, 0, symbol->eci, est_binlen, symbol->debug);
    if (symbol->debug & ZINT_DEBUG_TEST) qr_test_codeword_dump(symbol, datastream, target_codewords);
    add_ecc(fullstream, datastream, version, target_codewords, blocks, symbol->debug);

    size = qr_sizes[version - 1];
#ifndef _MSC_VER
    unsigned char grid[size * size];
#else
    grid = (unsigned char *) _alloca((size * size) * sizeof (unsigned char));
#endif

    for (i = 0; i < size; i++) {
        for (j = 0; j < size; j++) {
            grid[(i * size) + j] = 0;
        }
    }

    setup_grid(grid, size, version);
    populate_grid(grid, size, size, fullstream, qr_total_codewords[version - 1]);

    add_version_info(grid, size, version);

    bitmask = apply_bitmask(grid, size, ecc_level);

    add_format_info(grid, size, ecc_level, bitmask);

    symbol->width = size;
    symbol->rows = size;

    for (i = 0; i < size; i++) {
        for (j = 0; j < size; j++) {
            if (grid[(i * size) + j] & 0x01) {
                set_module(symbol, i, j);
            }
        }
        symbol->row_height[i] = 1;
    }

    return 0;
}

static void setup_rmqr_grid(unsigned char* grid,const int h_size,const int v_size,const int version) {
    int i, j;
    char alignment[] = {0x1F, 0x11, 0x15, 0x11, 0x1F};
    int h_version, finder_position;

    /* Add timing patterns - top and bottom */
    for (i = 0; i < h_size; i++) {
        if (i % 2) {
            grid[i] = 0x20;
            grid[((v_size - 1) * h_size) + i] = 0x20;
        } else {
            grid[i] = 0x21;
            grid[((v_size - 1) * h_size) + i] = 0x21;
        }
    }

    /* Add timing patterns - left and right */
    for (i = 0; i < v_size; i++) {
        if (i % 2) {
            grid[i * h_size] = 0x20;
            grid[(i * h_size) + (h_size - 1)] = 0x20;
        } else {
            grid[i * h_size] = 0x21;
            grid[(i * h_size) + (h_size - 1)] = 0x21;
        }
    }

    /* Add finder pattern */
    place_finder(grid, h_size, 0, 0); // This works because finder is always top left

    /* Add finder sub-pattern to bottom right */
    for (i = 0; i < 5; i++) {
        for (j = 0; j < 5; j++) {
            if (alignment[j] & 0x10 >> i) {
                grid[((v_size - 5) * h_size) + (h_size * i) + (h_size - 5) + j] = 0x11;
            } else {
                grid[((v_size - 5) * h_size) + (h_size * i) + (h_size - 5) + j] = 0x10;
            }
        }
    }

    /* Add corner finder pattern - bottom left */
    grid[(v_size - 2) * h_size] = 0x11;
    grid[((v_size - 2) * h_size) + 1] = 0x10;
    grid[((v_size - 1) * h_size) + 1] = 0x11;

    /* Add corner finder pattern - top right */
    grid[h_size - 2] = 0x11;
    grid[(h_size * 2) - 2] = 0x10;
    grid[(h_size * 2) - 1] = 0x11;

    /* Add seperator */
    for (i = 0; i < 7; i++) {
        grid[(i * h_size) + 7] = 0x20;
    }
    if (v_size > 7) {
        // Note for v_size = 9 this overrides the bottom right corner finder pattern
        for(i = 0; i < 8; i++) {
            grid[(7 * h_size) + i] = 0x20;
        }
    }

    /* Add alignment patterns */
    if (h_size > 27) {
        for(i = 0; i < 5; i++) {
            if (h_size == rmqr_width[i]) {
                h_version = i;
            }
        }

        for(i = 0; i < 4; i++) {
            finder_position = rmqr_table_d1[(h_version * 4) + i];

            if (finder_position != 0) {
                for (j = 0; j < v_size; j++) {
                    if (j % 2) {
                        grid[(j * h_size) + finder_position] = 0x10;
                    } else {
                        grid[(j * h_size) + finder_position] = 0x11;
                    }
                }

                // Top square
                grid[h_size + finder_position - 1] = 0x11;
                grid[(h_size * 2) + finder_position - 1] = 0x11;
                grid[h_size + finder_position + 1] = 0x11;
                grid[(h_size * 2) + finder_position + 1] = 0x11;

                // Bottom square
                grid[(h_size * (v_size - 3)) + finder_position - 1] = 0x11;
                grid[(h_size * (v_size - 2)) + finder_position - 1] = 0x11;
                grid[(h_size * (v_size - 3)) + finder_position + 1] = 0x11;
                grid[(h_size * (v_size - 2)) + finder_position + 1] = 0x11;
            }
        }
    }

    /* Reserve space for format information */
    for (i = 0; i < 5; i++) {
        for (j = 0; j < 3; j++) {
            grid[(h_size * (i + 1)) + j + 8] = 0x20;
            grid[(h_size * (v_size - 6)) + (h_size * i) + j + (h_size - 8)] = 0x20;
        }
    }
    grid[(h_size * 1) + 11] = 0x20;
    grid[(h_size * 2) + 11] = 0x20;
    grid[(h_size * 3) + 11] = 0x20;
    grid[(h_size * (v_size - 6)) + (h_size - 5)] = 0x20;
    grid[(h_size * (v_size - 6)) + (h_size - 4)] = 0x20;
    grid[(h_size * (v_size - 6)) + (h_size - 3)] = 0x20;
}

/* rMQR according to 2018 draft standard */
int rmqr(struct zint_symbol *symbol, const unsigned char source[], size_t length) {
    int i, j, est_binlen;
    int ecc_level, autosize, version, max_cw, target_codewords, blocks, h_size, v_size;
    int gs1;
    int footprint, best_footprint, format_data;
    unsigned int left_format_info, right_format_info;

#ifndef _MSC_VER
    unsigned int jisdata[length + 1];
    char mode[length + 1];
#else
    unsigned char* datastream;
    unsigned char* fullstream;
    unsigned char* grid;
    unsigned int* jisdata = (unsigned int *) _alloca((length + 1) * sizeof (unsigned int));
    char* mode = (char *) _alloca(length + 1);
#endif

    gs1 = ((symbol->input_mode & 0x07) == GS1_MODE);

    if ((symbol->input_mode & 0x07) == DATA_MODE) {
        sjis_cpy(source, &length, jisdata);
    } else {
        int done = 0;
        if (symbol->eci != 20) { /* Unless ECI 20 (Shift JIS) */
            /* Try single byte (Latin) conversion first */
            int error_number = sjis_utf8tosb(symbol->eci && symbol->eci <= 899 ? symbol->eci : 3, source, &length, jisdata);
            if (error_number == 0) {
                done = 1;
            } else if (symbol->eci && symbol->eci <= 899) {
                strcpy(symbol->errtxt, "575: Invalid characters in input data");
                return error_number;
            }
        }
        if (!done) {
            /* Try Shift-JIS */
            int error_number = sjis_utf8tomb(symbol, source, &length, jisdata);
            if (error_number != 0) {
                return error_number;
            }
        }
    }

    define_mode(mode, jisdata, length, gs1);
    est_binlen = getBinaryLength(RMQR_VERSION + 31, mode, jisdata, length, gs1, symbol->eci);

    ecc_level = LEVEL_M;
    max_cw = 152;
    if (symbol->option_1 == 1) {
        strcpy(symbol->errtxt, "576: Error correction level L not available in rMQR");
        return ZINT_ERROR_INVALID_OPTION;
    }

    if (symbol->option_1 == 3) {
        strcpy(symbol->errtxt, "577: Error correction level Q not available in rMQR");
        return ZINT_ERROR_INVALID_OPTION;
    }

    if (symbol->option_1 == 4) {
        ecc_level = LEVEL_H;
        max_cw = 76;
    }

    if (est_binlen > (8 * max_cw)) {
        strcpy(symbol->errtxt, "578: Input too long for selected error correction level");
        return ZINT_ERROR_TOO_LONG;
    }

    if ((symbol->option_2 < 0) || (symbol->option_2 > 38)) {
        strcpy(symbol->errtxt, "579: Invalid rMQR symbol size");
        return ZINT_ERROR_INVALID_OPTION;
    }

    version = 31; // Set default to keep compiler happy

    if (symbol->option_2 == 0) {
        // Automatic symbol size
        autosize = 31;
        best_footprint = rmqr_height[31] * rmqr_width[31];
        for (version = 30; version >= 0; version--) {
            est_binlen = getBinaryLength(RMQR_VERSION + version, mode, jisdata, length, gs1, symbol->eci);
            footprint = rmqr_height[version] * rmqr_width[version];
            if (ecc_level == LEVEL_M) {
                if (8 * rmqr_data_codewords_M[version] >= est_binlen) {
                    if (footprint < best_footprint) {
                        autosize = version;
                        best_footprint = footprint;
                    }
                }
            } else {
                if (8 * rmqr_data_codewords_H[version] >= est_binlen) {
                    if (footprint < best_footprint) {
                        autosize = version;
                        best_footprint = footprint;
                    }
                }
            }
        }
        version = autosize;
        est_binlen = getBinaryLength(RMQR_VERSION + version, mode, jisdata, length, gs1, symbol->eci);
    }

    if ((symbol->option_2 >= 1) && (symbol->option_2 <= 32)) {
        // User specified symbol size
        version = symbol->option_2 - 1;
    }

    if (symbol->option_2 >= 33) {
        // User has specified symbol height only
        version = rmqr_fixed_height_upper_bound[symbol->option_2 - 32];
        for(i = version - 1; i > rmqr_fixed_height_upper_bound[symbol->option_2 - 33]; i--) {
            est_binlen = getBinaryLength(RMQR_VERSION + i, mode, jisdata, length, gs1, symbol->eci);
            if (ecc_level == LEVEL_M) {
                if (8 * rmqr_data_codewords_M[i] >= est_binlen) {
                    version = i;
                }
            } else {
                if (8 * rmqr_data_codewords_H[i] >= est_binlen) {
                    version = i;
                }
            }
        }
        est_binlen = getBinaryLength(RMQR_VERSION + version, mode, jisdata, length, gs1, symbol->eci);
    }

    if (symbol->option_1 == -1) {
        // Detect if there is enough free space to increase ECC level
        if (est_binlen < (rmqr_data_codewords_H[version] * 8)) {
            ecc_level = LEVEL_H;
        }
    }

    if (ecc_level == LEVEL_M) {
        target_codewords = rmqr_data_codewords_M[version];
        blocks = rmqr_blocks_M[version];
    } else {
        target_codewords = rmqr_data_codewords_H[version];
        blocks = rmqr_blocks_H[version];
    }

    if (est_binlen > (target_codewords * 8)) {
        // User has selected a symbol too small for the data
        strcpy(symbol->errtxt, "580: Input too long for selected symbol size");
        return ZINT_ERROR_TOO_LONG;
    }

    if (symbol->debug) {
        printf("Minimum codewords = %d\n", est_binlen / 8);
        printf("Selected version: %d = R%dx%d-", (version + 1), rmqr_height[version], rmqr_width[version]);
        if (ecc_level == LEVEL_M) {
            printf("M\n");
        } else {
            printf("H\n");
        }
        printf("Number of data codewords in symbol = %d\n", target_codewords);
        printf("Number of ECC blocks = %d\n", blocks);
    }

#ifndef _MSC_VER
    unsigned char datastream[target_codewords + 1];
    unsigned char fullstream[rmqr_total_codewords[version] + 1];
#else
    datastream = (unsigned char *) _alloca((target_codewords + 1) * sizeof (unsigned char));
    fullstream = (unsigned char *) _alloca((rmqr_total_codewords[version] + 1) * sizeof (unsigned char));
#endif

    qr_binary(datastream, RMQR_VERSION + version, target_codewords, mode, jisdata, length, gs1, symbol->eci, est_binlen, symbol->debug);
    add_ecc(fullstream, datastream, RMQR_VERSION + version, target_codewords, blocks, symbol->debug);

    h_size = rmqr_width[version];
    v_size = rmqr_height[version];

#ifndef _MSC_VER
    unsigned char grid[h_size * v_size];
#else
    grid = (unsigned char *) _alloca((h_size * v_size) * sizeof (unsigned char));
#endif

    for (i = 0; i < v_size; i++) {
        for (j = 0; j < h_size; j++) {
            grid[(i * h_size) + j] = 0;
        }
    }

    setup_rmqr_grid(grid, h_size, v_size, version);
    populate_grid(grid, h_size, v_size, fullstream, rmqr_total_codewords[version]);

    /* apply bitmask */
    for (i = 0; i < v_size; i++) {
        for (j = 0; j < h_size; j++) {
            if ((grid[(i * h_size) + j] & 0xf0) == 0) {
                // This is a data module
                if (((i / 2) + (j / 3)) % 2 == 0) { // < This is the data mask from section 7.8.2
                    // This module needs to be changed
                    if (grid[(i * h_size) + j] == 0x01) {
                        grid[(i * h_size) + j] = 0x00;
                    } else {
                        grid[(i * h_size) + j] = 0x01;
                    }
                }
            }
        }
    }

    /* add format information */
    format_data = version;
    if (ecc_level == LEVEL_H) {
        format_data += 32;
    }
    left_format_info = rmqr_format_info_left[format_data];
    right_format_info = rmqr_format_info_right[format_data];

    for (i = 0; i < 5; i++) {
        for (j = 0; j < 3; j++) {
            grid[(h_size * (i + 1)) + j + 8] = (left_format_info >> ((j * 5) + i)) & 0x01;
            grid[(h_size * (v_size - 6)) + (h_size * i) + j + (h_size - 8)] = (right_format_info >> ((j * 5) + i)) & 0x01;
        }
    }
    grid[(h_size * 1) + 11] = (left_format_info >> 15) & 0x01;
    grid[(h_size * 2) + 11] = (left_format_info >> 16) & 0x01;
    grid[(h_size * 3) + 11] = (left_format_info >> 17) & 0x01;
    grid[(h_size * (v_size - 6)) + (h_size - 5)] = (right_format_info >> 15) & 0x01;
    grid[(h_size * (v_size - 6)) + (h_size - 4)] = (right_format_info >> 16) & 0x01;
    grid[(h_size * (v_size - 6)) + (h_size - 3)] = (right_format_info >> 17) & 0x01;


    symbol->width = h_size;
    symbol->rows = v_size;

    for (i = 0; i < v_size; i++) {
        for (j = 0; j < h_size; j++) {
            if (grid[(i * h_size) + j] & 0x01) {
                set_module(symbol, i, j);
            }
        }
        symbol->row_height[i] = 1;
    }

    return 0;
}