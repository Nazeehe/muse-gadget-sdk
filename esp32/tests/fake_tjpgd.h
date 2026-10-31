/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

/*
 * A stand-in for ESP-IDF's rom/tjpgd.h (components/esp_rom/esp32s3/include/esp32s3/rom/tjpgd.h: ChaN's TJpgDec R0.01c, in the
 * ESP32-S3's ROM) for apps_jpeg.c on the host. tests/test_muse_apps_jpeg.py stages rom/tjpgd.h as one line that includes this. The
 * declarations are the ROM header's: the same configuration, types, result codes, rectangle, session and the two functions, so
 * what compiles here compiles against the real header (the device build checks it). The ROM is fixed silicon: its header never
 * changes for the chip.
 *
 * What it decodes is not JPEG but a test format that says which picture to make, so the glue (apps_jpeg.c: the scale, the
 * pixels, the circle, the errors, the memory) is tested without vendoring tjpgd. The real decode is checked on the board.
 *   bytes 0-3    "FJPG"
 *   byte 4       'B' baseline; 'P' progressive (jd_prepare returns JDR_FMT3, as the ROM's does for SOF2); 'C' a stream that breaks
 *                after its first MCU (jd_decomp returns JDR_FMT1 there); anything else is JDR_FMT1
 *   bytes 5, 6   the MCU's width and height in blocks of 8 px: 1 or 2 each, as TJpgDec's msx and msy
 *   byte 7       0
 *   bytes 8-11   the width and the height, little-endian, 1 to 65535
 *   bytes 12-13  n, little-endian: jd_prepare then skips n bytes through the input function (infunc(jd, NULL, n))
 *   n bytes      anything
 *   4 bytes      "END!", read by jd_decomp before its first MCU
 * A stream that ends early is JDR_INP, as the ROM's is. jd_prepare needs a pool of at least 3,100 bytes (JDR_MEM1 otherwise), and
 * fills all of it, as the ROM's fills it with its tables.
 *
 * jd_decomp gives the output function what TJpgDec's mcu_output gives it: the MCUs in raster order, each clipped at the right and
 * bottom edges of the picture, then scaled (its width and height shifted right by scale, and an MCU left with none skipped; its
 * corner shifted too), as a rectangle and an RGB888 bitmap of exactly its pixels, in a block of exactly that size (so a read past
 * it is AddressSanitizer's). The pixel at (x, y) of the output is the picture's at (x << scale, y << scale), and the picture's at
 * (X, Y) is R = (7X + 3Y) & 255, G = (5X ^ 11Y) & 255, B = (X + 13Y) & 255. An output function that returns 0 interrupts it
 * (JDR_INTR).
 */

#define JD_SZBUF 512   /* Size of stream input buffer */
#define JD_FORMAT 0    /* Output pixel format 0:RGB888 (3 BYTE/pix), 1:RGB565 (1 WORD/pix) */
#define JD_USE_SCALE 1 /* Use descaling feature for output */
#define JD_TBLCLIP 1   /* Use table for saturation */

typedef int INT;
typedef unsigned int UINT;
typedef char CHAR;
typedef unsigned char UCHAR;
typedef unsigned char BYTE;
typedef short SHORT;
typedef unsigned short USHORT;
typedef unsigned short WORD;
typedef unsigned short WCHAR;
typedef long LONG;
typedef unsigned long ULONG;
typedef unsigned long DWORD;

typedef enum {
    JDR_OK = 0, /* 0: Succeeded */
    JDR_INTR,   /* 1: Interrupted by output function */
    JDR_INP,    /* 2: Device error or wrong termination of input stream */
    JDR_MEM1,   /* 3: Insufficient memory pool for the image */
    JDR_MEM2,   /* 4: Insufficient stream input buffer */
    JDR_PAR,    /* 5: Parameter error */
    JDR_FMT1,   /* 6: Data format error (may be damaged data) */
    JDR_FMT2,   /* 7: Right format but not supported */
    JDR_FMT3    /* 8: Not supported JPEG standard */
} JRESULT;

typedef struct {
    WORD left, right, top, bottom;
} JRECT;

typedef struct JDEC JDEC;
struct JDEC {
    UINT dctr;            /* Number of bytes available in the input buffer */
    BYTE *dptr;           /* Current data read ptr */
    BYTE *inbuf;          /* Bit stream input buffer */
    BYTE dmsk;            /* Current bit in the current read byte */
    BYTE scale;           /* Output scaling ratio */
    BYTE msx, msy;        /* MCU size in unit of block (width, height) */
    BYTE qtid[3];         /* Quantization table ID of each component */
    SHORT dcv[3];         /* Previous DC element of each component */
    WORD nrst;            /* Restart interval */
    UINT width, height;   /* Size of the input image (pixel) */
    BYTE *huffbits[2][2]; /* Huffman bit distribution tables [id][dcac] */
    WORD *huffcode[2][2]; /* Huffman code word tables [id][dcac] */
    BYTE *huffdata[2][2]; /* Huffman decoded data tables [id][dcac] */
    LONG *qttbl[4];       /* Dequaitizer tables [id] */
    void *workbuf;        /* Working buffer for IDCT and RGB output */
    BYTE *mcubuf;         /* Working buffer for the MCU */
    void *pool;           /* Pointer to available memory pool */
    UINT sz_pool;         /* Size of memory pool (bytes available) */
    UINT (*infunc)(JDEC *, BYTE *, UINT); /* Pointer to jpeg stream input function */
    void *device;         /* Pointer to I/O device identifier for the session */
};

JRESULT jd_prepare(JDEC *, UINT (*)(JDEC *, BYTE *, UINT), void *, UINT, void *);
JRESULT jd_decomp(JDEC *, UINT (*)(JDEC *, void *, JRECT *), BYTE);

/* The fake's own, for the harness: the scale the last jd_decomp was given (-1 before any), and the rectangles it gave out. */
extern int fake_tjpgd_scale;
extern int fake_tjpgd_rects;
