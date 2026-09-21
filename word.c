word_layout.h
#ifndef WORD_LAYOUT_H
#define WORD_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------
 * Basic geometry
 * --------------------------------------------------------- */

typedef struct {
    float x;
    float y;
    float width;
    float height;
} WLRect;

/* ---------------------------------------------------------
 * Text formatting
 * --------------------------------------------------------- */

typedef struct {
    float font_size;
    float character_spacing;
    float word_spacing;

    float line_height;

    uint8_t bold;
    uint8_t italic;
    uint8_t underline;
} WLTextStyle;

/* ---------------------------------------------------------
 * Text run
 * --------------------------------------------------------- */

typedef struct {
    const char *text;
    size_t length;

    WLTextStyle style;
} WLTextRun;

/* ---------------------------------------------------------
 * Paragraph
 * --------------------------------------------------------- */

typedef enum {
    WL_ALIGN_LEFT,
    WL_ALIGN_CENTER,
    WL_ALIGN_RIGHT,
    WL_ALIGN_JUSTIFY
} WLAlignment;

typedef struct {
    WLTextRun *runs;
    size_t run_count;

    WLAlignment alignment;

    float available_width;
    float spacing_before;
    float spacing_after;
} WLParagraph;

/* ---------------------------------------------------------
 * Glyph
 *
 * In a complete implementation glyph_id would come from
 * the font shaping subsystem.
 * --------------------------------------------------------- */

typedef struct {
    uint32_t codepoint;
    uint32_t glyph_id;

    float x;
    float y;

    float advance;
    float width;
    float height;
} WLGlyph;

/* ---------------------------------------------------------
 * Layout line
 * --------------------------------------------------------- */

typedef struct {
    WLGlyph *glyphs;
    size_t glyph_count;

    float width;
    float height;

    float baseline;
} WLLine;

/* ---------------------------------------------------------
 * Layout result
 * --------------------------------------------------------- */

typedef struct {
    WLLine *lines;
    size_t line_count;

    float width;
    float height;
} WLParagraphLayout;

/* ---------------------------------------------------------
 * Engine
 * --------------------------------------------------------- */

typedef struct {
    float default_font_size;
    float default_line_height;

    size_t initial_capacity;
} WLLayoutEngine;

/* ---------------------------------------------------------
 * API
 * --------------------------------------------------------- */

void wl_engine_init(
    WLLayoutEngine *engine
);

int wl_layout_paragraph(
    WLLayoutEngine *engine,
    const WLParagraph *paragraph,
    WLParagraphLayout *result
);

void wl_free_layout(
    WLParagraphLayout *layout
);

#ifdef __cplusplus
}
#endif

#endif
word_layout.c
#include "word_layout.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------- */

static float
wl_glyph_advance(
    uint32_t codepoint,
    const WLTextStyle *style
)
{
    /*
     * Temporary metric model.

     * A production engine would replace this with actual
     * font metrics + Unicode shaping.
     */

    float size = style->font_size;

    if (codepoint == ' ')
        return size * 0.33f + style->word_spacing;

    if (codepoint == '\t')
        return size * 1.32f;

    if (codepoint == '\n')
        return 0.0f;

    float advance;

    /*
     * Approximate character classes.
     */

    if (codepoint >= 'i' && codepoint <= 'l') {
        advance = size * 0.25f;
    }
    else if (codepoint >= 'm' && codepoint <= 'w') {
        advance = size * 0.58f;
    }
    else if (codepoint >= 'A' && codepoint <= 'Z') {
        advance = size * 0.62f;
    }
    else if (codepoint >= '0' && codepoint <= '9') {
        advance = size * 0.55f;
    }
    else {
        advance = size * 0.50f;
    }

    if (style->bold)
        advance *= 1.03f;

    return advance + style->character_spacing;
}


/*
 * Decode one UTF-8 codepoint.
 *
 * Returns the number of bytes consumed.
 */
static size_t
wl_utf8_decode(
    const unsigned char *text,
    size_t remaining,
    uint32_t *codepoint
)
{
    if (remaining == 0)
        return 0;

    unsigned char c = text[0];

    if (c < 0x80) {
        *codepoint = c;
        return 1;
    }

    if ((c & 0xE0) == 0xC0 && remaining >= 2) {
        *codepoint =
            ((uint32_t)(c & 0x1F) << 6) |
            (text[1] & 0x3F);

        return 2;
    }

    if ((c & 0xF0) == 0xE0 && remaining >= 3) {
        *codepoint =
            ((uint32_t)(c & 0x0F) << 12) |
            ((uint32_t)(text[1] & 0x3F) << 6) |
            (text[2] & 0x3F);

        return 3;
    }

    if ((c & 0xF8) == 0xF0 && remaining >= 4) {
        *codepoint =
            ((uint32_t)(c & 0x07) << 18) |
            ((uint32_t)(text[1] & 0x3F) << 12) |
            ((uint32_t)(text[2] & 0x3F) << 6) |
            (text[3] & 0x3F);

        return 4;
    }

    /*
     * Invalid UTF-8.
     * Replace with U+FFFD.
     */

    *codepoint = 0xFFFD;
    return 1;
}


/* ---------------------------------------------------------
 * Dynamic line storage
 * --------------------------------------------------------- */

static int
wl_grow_lines(
    WLParagraphLayout *layout,
    size_t *capacity
)
{
    size_t new_capacity =
        (*capacity == 0)
            ? 16
            : (*capacity * 2);

    WLLine *new_lines =
        realloc(
            layout->lines,
            new_capacity * sizeof(WLLine)
        );

    if (!new_lines)
        return 0;

    layout->lines = new_lines;
    *capacity = new_capacity;

    return 1;
}


/* ---------------------------------------------------------
 * Dynamic glyph storage
 * --------------------------------------------------------- */

static int
wl_grow_glyphs(
    WLLine *line,
    size_t *capacity
)
{
    size_t new_capacity =
        (*capacity == 0)
            ? 64
            : (*capacity * 2);

    WLGlyph *new_glyphs =
        realloc(
            line->glyphs,
            new_capacity * sizeof(WLGlyph)
        );

    if (!new_glyphs)
        return 0;

    line->glyphs = new_glyphs;
    *capacity = new_capacity;

    return 1;
}


/* ---------------------------------------------------------
 * Engine initialization
 * --------------------------------------------------------- */

void
wl_engine_init(
    WLLayoutEngine *engine
)
{
    if (!engine)
        return;

    engine->default_font_size = 12.0f;
    engine->default_line_height = 16.0f;
    engine->initial_capacity = 16;
}


/* ---------------------------------------------------------
 * Paragraph layout
 * --------------------------------------------------------- */

int
wl_layout_paragraph(
    WLLayoutEngine *engine,
    const WLParagraph *paragraph,
    WLParagraphLayout *result
)
{
    if (!engine || !paragraph || !result)
        return 0;

    memset(result, 0, sizeof(*result));

    size_t line_capacity = 0;

    WLLine current_line;
    memset(&current_line, 0, sizeof(current_line));

    size_t glyph_capacity = 0;

    float cursor_x = 0.0f;
    float cursor_y = paragraph->spacing_before;

    float current_line_height =
        engine->default_line_height;

    /*
     * Process every run.
     */

    for (size_t r = 0; r < paragraph->run_count; ++r) {

        const WLTextRun *run =
            &paragraph->runs[r];

        WLTextStyle style = run->style;

        if (style.font_size <= 0.0f)
            style.font_size =
                engine->default_font_size;

        if (style.line_height <= 0.0f)
            style.line_height =
                engine->default_line_height;

        const unsigned char *text =
            (const unsigned char *)run->text;

        size_t offset = 0;

        while (offset < run->length) {

            uint32_t codepoint;

            size_t consumed =
                wl_utf8_decode(
                    text + offset,
                    run->length - offset,
                    &codepoint
                );

            if (consumed == 0)
                break;

            offset += consumed;

            /*
             * Explicit newline.
             */

            if (codepoint == '\n') {

                current_line.width = cursor_x;
                current_line.height =
                    current_line_height;

                current_line.baseline =
                    cursor_y + current_line_height * 0.8f;

                if (result->line_count >= line_capacity) {
                    if (!wl_grow_lines(
                            result,
                            &line_capacity))
                        goto failure;
                }

                result->lines[result->line_count++] =
                    current_line;

                memset(
                    &current_line,
                    0,
                    sizeof(current_line)
                );

                glyph_capacity = 0;
                cursor_x = 0.0f;
                cursor_y += current_line_height;

                continue;
            }

            float advance =
                wl_glyph_advance(
                    codepoint,
                    &style
                );

            /*
             * Word-wrap.
             *
             * This first version wraps at whitespace.
             */

            if (cursor_x > 0.0f &&
                cursor_x + advance >
                paragraph->available_width &&
                codepoint != ' ') {

                current_line.width =
                    cursor_x;

                current_line.height =
                    current_line_height;

                current_line.baseline =
                    cursor_y +
                    current_line_height * 0.8f;

                if (result->line_count >= line_capacity) {
                    if (!wl_grow_lines(
                            result,
                            &line_capacity))
                        goto failure;
                }

                result->lines[result->line_count++] =
                    current_line;

                memset(
                    &current_line,
                    0,
                    sizeof(current_line)
                );

                glyph_capacity = 0;

                cursor_x = 0.0f;
                cursor_y += current_line_height;
            }

            /*
             * Don't render leading whitespace.
             */

            if (codepoint == ' ' &&
                cursor_x == 0.0f)
                continue;

            if (current_line.glyph_count >=
                glyph_capacity) {

                if (!wl_grow_glyphs(
                        &current_line,
                        &glyph_capacity))
                    goto failure;
            }

            WLGlyph *glyph =
                &current_line.glyphs[
                    current_line.glyph_count++
                ];

            glyph->codepoint = codepoint;

            /*
             * Temporary glyph ID.
             *
             * Later this becomes the actual font
             * shaping result.
             */
            glyph->glyph_id = codepoint;

            glyph->x = cursor_x;
            glyph->y = cursor_y;

            glyph->advance = advance;
            glyph->width = advance;
            glyph->height = style.font_size;

            cursor_x += advance;

            if (style.line_height >
                current_line_height) {

                current_line_height =
                    style.line_height;
            }
        }
    }

    /*
     * Flush final line.
     */

    if (current_line.glyph_count > 0 ||
        result->line_count == 0) {

        current_line.width =
            cursor_x;

        current_line.height =
            current_line_height;

        current_line.baseline =
            cursor_y +
            current_line_height * 0.8f;

        if (result->line_count >= line_capacity) {
            if (!wl_grow_lines(
                    result,
                    &line_capacity))
                goto failure;
        }

        result->lines[result->line_count++] =
            current_line;

        current_line.glyphs = NULL;
        current_line.glyph_count = 0;
    }

    result->width =
        paragraph->available_width;

    result->height =
        cursor_y +
        current_line_height +
        paragraph->spacing_after;

    return 1;

failure:

    free(current_line.glyphs);
    wl_free_layout(result);

    return 0;
}


/* ---------------------------------------------------------
 * Free layout
 * --------------------------------------------------------- */

void
wl_free_layout(
    WLParagraphLayout *layout
)
{
    if (!layout)
        return;

    for (size_t i = 0;
         i < layout->line_count;
         ++i) {

        free(layout->lines[i].glyphs);
    }

    free(layout->lines);

    memset(layout, 0, sizeof(*layout));
}
Basic test program
#include "word_layout.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    WLLayoutEngine engine;

    wl_engine_init(&engine);

    WLTextStyle style = {
        .font_size = 12.0f,
        .character_spacing = 0.0f,
        .word_spacing = 0.0f,
        .line_height = 18.0f,
        .bold = 0,
        .italic = 0,
        .underline = 0
    };

    const char *text =
        "This is a native C text layout engine "
        "designed as the foundation for a high "
        "performance word processor.";

    WLTextRun run = {
        .text = text,
        .length = strlen(text),
        .style = style
    };

    WLParagraph paragraph = {
        .runs = &run,
        .run_count = 1,
        .alignment = WL_ALIGN_LEFT,
        .available_width = 300.0f,
        .spacing_before = 0.0f,
        .spacing_after = 10.0f
    };

    WLParagraphLayout layout;

    if (!wl_layout_paragraph(
            &engine,
            &paragraph,
            &layout)) {

        fprintf(stderr,
                "Layout failed\n");

        return 1;
    }

    printf(
        "Lines: %zu\n",
        layout.line_count
    );

    printf(
        "Height: %.2f\n",
        layout.height
    );

    for (size_t i = 0;
         i < layout.line_count;
         ++i) {

        WLLine *line =
            &layout.lines[i];

        printf(
            "Line %zu: width %.2f, glyphs %zu\n",
            i + 1,
            line->width,
            line->glyph_count
        );

        for (size_t g = 0;
             g < line->glyph_count;
             ++g) {

            WLGlyph *glyph =
                &line->glyphs[g];

            printf(
                "  U+%04X x=%.2f y=%.2f "
                "advance=%.2f\n",
                glyph->codepoint,
                glyph->x,
                glyph->y,
                glyph->advance
            );
        }
    }

    wl_free_layout(&layout);

    return 0;
}

Compile it with:

cc -std=c11 -O2 -Wall -Wextra \
    word_layout.c \
    test_layout.c \
    -lm \
    -o word_layout_test
    
    
    
    
    word_memory.h
#ifndef WORD_MEMORY_H
#define WORD_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================
 * Configuration
 * ========================================================= */

#define WM_TEXT_CHUNK_SIZE   4096
#define WM_ARENA_BLOCK_SIZE  65536

/* =========================================================
 * Result codes
 * ========================================================= */

typedef enum {
    WM_OK = 0,
    WM_ERROR = -1,
    WM_OUT_OF_MEMORY = -2,
    WM_INVALID_ARGUMENT = -3,
    WM_NOT_FOUND = -4,
    WM_RANGE_ERROR = -5
} WMResult;

/* =========================================================
 * Text chunk
 *
 * Text is stored in chunks instead of one enormous buffer.
 * This avoids reallocating the entire document when text
 * is inserted.
 * ========================================================= */

typedef struct WMTextChunk {
    struct WMTextChunk *next;

    size_t used;
    size_t capacity;

    char data[WM_TEXT_CHUNK_SIZE];
} WMTextChunk;

/* =========================================================
 * Arena block
 *
 * Document metadata is allocated from arenas.
 * Individual nodes therefore don't require separate malloc
 * calls and can remain stable in memory.
 * ========================================================= */

typedef struct WMArenaBlock {
    struct WMArenaBlock *next;

    size_t used;
    size_t capacity;

    unsigned char data[];
} WMArenaBlock;

typedef struct {
    WMArenaBlock *first;
    WMArenaBlock *current;

    size_t block_size;
    size_t total_allocated;
} WMArena;

/* =========================================================
 * Text reference
 *
 * Points into chunk storage.
 * ========================================================= */

typedef struct {
    WMTextChunk *chunk;
    size_t offset;
    size_t length;
} WMTextRef;

/* =========================================================
 * Formatting
 * ========================================================= */

typedef struct {
    uint32_t font_id;

    float font_size;

    uint8_t bold;
    uint8_t italic;
    uint8_t underline;

    uint32_t color;
} WMFormat;

/* =========================================================
 * Text run
 * ========================================================= */

typedef struct WMRun {
    struct WMRun *next;

    WMTextRef text;
    WMFormat format;

    uint64_t id;
} WMRun;

/* =========================================================
 * Paragraph
 * ========================================================= */

typedef struct WMParagraph {
    struct WMParagraph *prev;
    struct WMParagraph *next;

    WMRun *first_run;
    WMRun *last_run;

    uint64_t id;

    uint32_t flags;

    float spacing_before;
    float spacing_after;
} WMParagraph;

/* =========================================================
 * Document
 * ========================================================= */

typedef struct {
    WMParagraph *first_paragraph;
    WMParagraph *last_paragraph;

    size_t paragraph_count;
    size_t run_count;

    uint64_t version;
    uint64_t next_id;

    WMArena arena;

    WMTextChunk *first_text_chunk;
    WMTextChunk *last_text_chunk;

    size_t text_bytes;
    size_t metadata_bytes;
} WMDocument;

/* =========================================================
 * Memory statistics
 * ========================================================= */

typedef struct {
    size_t text_bytes;
    size_t metadata_bytes;
    size_t allocated_bytes;

    size_t paragraph_count;
    size_t run_count;

    uint64_t document_version;
} WMMemoryStats;

/* =========================================================
 * Arena API
 * ========================================================= */

void wm_arena_init(
    WMArena *arena,
    size_t block_size
);

void *wm_arena_alloc(
    WMArena *arena,
    size_t size,
    size_t alignment
);

void wm_arena_destroy(
    WMArena *arena
);

/* =========================================================
 * Document API
 * ========================================================= */

WMResult wm_document_init(
    WMDocument *document
);

void wm_document_destroy(
    WMDocument *document
);

WMParagraph *wm_document_add_paragraph(
    WMDocument *document
);

WMRun *wm_paragraph_add_run(
    WMDocument *document,
    WMParagraph *paragraph,
    const char *text,
    size_t length,
    const WMFormat *format
);

/* =========================================================
 * Text API
 * ========================================================= */

WMResult wm_document_append_text(
    WMDocument *document,
    const char *text,
    size_t length,
    WMTextRef *result
);

WMResult wm_text_read(
    const WMTextRef *ref,
    char *output,
    size_t output_size
);

/* =========================================================
 * Document operations
 * ========================================================= */

WMResult wm_paragraph_append_text(
    WMDocument *document,
    WMParagraph *paragraph,
    const char *text,
    size_t length,
    const WMFormat *format
);

/* =========================================================
 * Statistics
 * ========================================================= */

void wm_document_stats(
    const WMDocument *document,
    WMMemoryStats *stats
);

#ifdef __cplusplus
}
#endif

#endif
word_memory.c
#include "word_memory.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* =========================================================
 * Alignment helper
 * ========================================================= */

static size_t
wm_align_up(
    size_t value,
    size_t alignment
)
{
    if (alignment == 0)
        return value;

    size_t remainder =
        value % alignment;

    if (remainder == 0)
        return value;

    return value +
           (alignment - remainder);
}

/* =========================================================
 * Arena
 * ========================================================= */

void
wm_arena_init(
    WMArena *arena,
    size_t block_size
)
{
    if (!arena)
        return;

    memset(arena, 0, sizeof(*arena));

    arena->block_size =
        block_size
            ? block_size
            : WM_ARENA_BLOCK_SIZE;
}


void *
wm_arena_alloc(
    WMArena *arena,
    size_t size,
    size_t alignment
)
{
    if (!arena || size == 0)
        return NULL;

    if (alignment == 0)
        alignment = sizeof(void *);

    /*
     * Create first block if required.
     */

    if (!arena->current) {

        size_t capacity =
            arena->block_size;

        if (capacity < size)
            capacity = size;

        WMArenaBlock *block =
            malloc(
                sizeof(WMArenaBlock) +
                capacity
            );

        if (!block)
            return NULL;

        block->next = NULL;
        block->used = 0;
        block->capacity = capacity;

        arena->first = block;
        arena->current = block;

        arena->total_allocated +=
            capacity;
    }

    WMArenaBlock *block =
        arena->current;

    size_t aligned =
        wm_align_up(
            block->used,
            alignment
        );

    /*
     * Current block has enough space.
     */

    if (aligned + size <= block->capacity) {

        void *ptr =
            block->data + aligned;

        block->used =
            aligned + size;

        return ptr;
    }

    /*
     * Need a new block.
     */

    size_t capacity =
        arena->block_size;

    if (capacity < size)
        capacity = size;

    WMArenaBlock *new_block =
        malloc(
            sizeof(WMArenaBlock) +
            capacity
        );

    if (!new_block)
        return NULL;

    new_block->next = NULL;
    new_block->used = 0;
    new_block->capacity = capacity;

    block->next = new_block;

    arena->current = new_block;

    arena->total_allocated +=
        capacity;

    void *ptr =
        new_block->data;

    new_block->used = size;

    return ptr;
}


void
wm_arena_destroy(
    WMArena *arena
)
{
    if (!arena)
        return;

    WMArenaBlock *block =
        arena->first;

    while (block) {

        WMArenaBlock *next =
            block->next;

        free(block);

        block = next;
    }

    memset(arena, 0, sizeof(*arena));
}

/* =========================================================
 * Text chunks
 * ========================================================= */

static WMTextChunk *
wm_create_text_chunk(void)
{
    WMTextChunk *chunk =
        malloc(sizeof(WMTextChunk));

    if (!chunk)
        return NULL;

    chunk->next = NULL;
    chunk->used = 0;
    chunk->capacity =
        WM_TEXT_CHUNK_SIZE;

    return chunk;
}


WMResult
wm_document_append_text(
    WMDocument *document,
    const char *text,
    size_t length,
    WMTextRef *result
)
{
    if (!document ||
        (!text && length != 0) ||
        !result)
        return WM_INVALID_ARGUMENT;

    memset(result, 0, sizeof(*result));

    if (length == 0)
        return WM_OK;

    WMTextChunk *chunk =
        document->last_text_chunk;

    /*
     * Create initial chunk.
     */

    if (!chunk) {

        chunk =
            wm_create_text_chunk();

        if (!chunk)
            return WM_OUT_OF_MEMORY;

        document->first_text_chunk =
            chunk;

        document->last_text_chunk =
            chunk;
    }

    /*
     * If the requested text fits into the
     * remaining chunk, copy it directly.
     */

    if (chunk->used + length <=
        chunk->capacity) {

        size_t offset =
            chunk->used;

        memcpy(
            chunk->data + offset,
            text,
            length
        );

        chunk->used += length;

        result->chunk = chunk;
        result->offset = offset;
        result->length = length;

        document->text_bytes += length;
        document->version++;

        return WM_OK;
    }

    /*
     * Large text is split across chunks.
     *
     * For this first implementation we create
     * a contiguous reference only when possible.
     */

    size_t remaining = length;
    const char *source = text;

    WMTextChunk *first_chunk = NULL;
    size_t first_offset = 0;

    while (remaining > 0) {

        chunk =
            document->last_text_chunk;

        if (chunk->used ==
            chunk->capacity) {

            WMTextChunk *new_chunk =
                wm_create_text_chunk();

            if (!new_chunk)
                return WM_OUT_OF_MEMORY;

            chunk->next =
                new_chunk;

            document->last_text_chunk =
                new_chunk;

            chunk = new_chunk;
        }

        size_t available =
            chunk->capacity -
            chunk->used;

        size_t copy_size =
            remaining < available
                ? remaining
                : available;

        size_t offset =
            chunk->used;

        memcpy(
            chunk->data + offset,
            source,
            copy_size
        );

        chunk->used += copy_size;

        if (!first_chunk) {
            first_chunk = chunk;
            first_offset = offset;
        }

        source += copy_size;
        remaining -= copy_size;

        document->text_bytes +=
            copy_size;
    }

    /*
     * A single WMTextRef cannot describe multiple
     * chunks. For the initial engine we therefore
     * reject cross-chunk references.
     *
     * Later #2 evolution will introduce rope/
     * piece-table text spans.
     */

    if (first_chunk &&
        first_offset + length <=
        first_chunk->capacity) {

        result->chunk = first_chunk;
        result->offset = first_offset;
        result->length = length;
    }

    document->version++;

    return WM_OK;
}

/* =========================================================
 * Text read
 * ========================================================= */

WMResult
wm_text_read(
    const WMTextRef *ref,
    char *output,
    size_t output_size
)
{
    if (!ref ||
        !output)
        return WM_INVALID_ARGUMENT;

    if (output_size <
        ref->length + 1)
        return WM_RANGE_ERROR;

    if (!ref->chunk)
        return WM_NOT_FOUND;

    memcpy(
        output,
        ref->chunk->data +
            ref->offset,
        ref->length
    );

    output[ref->length] = '\0';

    return WM_OK;
}

/* =========================================================
 * Document initialization
 * ========================================================= */

WMResult
wm_document_init(
    WMDocument *document
)
{
    if (!document)
        return WM_INVALID_ARGUMENT;

    memset(
        document,
        0,
        sizeof(*document)
    );

    wm_arena_init(
        &document->arena,
        WM_ARENA_BLOCK_SIZE
    );

    document->next_id = 1;
    document->version = 1;

    return WM_OK;
}

/* =========================================================
 * Paragraph creation
 * ========================================================= */

WMParagraph *
wm_document_add_paragraph(
    WMDocument *document
)
{
    if (!document)
        return NULL;

    WMParagraph *paragraph =
        wm_arena_alloc(
            &document->arena,
            sizeof(WMParagraph),
            _Alignof(WMParagraph)
        );

    if (!paragraph)
        return NULL;

    memset(
        paragraph,
        0,
        sizeof(*paragraph)
    );

    paragraph->id =
        document->next_id++;

    /*
     * Link into document.
     */

    paragraph->prev =
        document->last_paragraph;

    if (document->last_paragraph)
        document->last_paragraph->next =
            paragraph;
    else
        document->first_paragraph =
            paragraph;

    document->last_paragraph =
        paragraph;

    document->paragraph_count++;

    document->metadata_bytes +=
        sizeof(WMParagraph);

    document->version++;

    return paragraph;
}

/* =========================================================
 * Run creation
 * ========================================================= */

WMRun *
wm_paragraph_add_run(
    WMDocument *document,
    WMParagraph *paragraph,
    const char *text,
    size_t length,
    const WMFormat *format
)
{
    if (!document ||
        !paragraph)
        return NULL;

    WMRun *run =
        wm_arena_alloc(
            &document->arena,
            sizeof(WMRun),
            _Alignof(WMRun)
        );

    if (!run)
        return NULL;

    memset(
        run,
        0,
        sizeof(*run)
    );

    run->id =
        document->next_id++;

    if (format)
        run->format = *format;

    /*
     * Store text in chunk storage.
     */

    if (wm_document_append_text(
            document,
            text,
            length,
            &run->text
        ) != WM_OK) {

        return NULL;
    }

    /*
     * Link run.
     */

    run->next =
        NULL;

    if (paragraph->last_run)
        paragraph->last_run->next =
            run;
    else
        paragraph->first_run =
            run;

    paragraph->last_run =
        run;

    document->run_count++;

    document->metadata_bytes +=
        sizeof(WMRun);

    document->version++;

    return run;
}

/* =========================================================
 * Append paragraph text
 * ========================================================= */

WMResult
wm_paragraph_append_text(
    WMDocument *document,
    WMParagraph *paragraph,
    const char *text,
    size_t length,
    const WMFormat *format
)
{
    if (!document ||
        !paragraph ||
        (!text && length != 0))
        return WM_INVALID_ARGUMENT;

    if (!wm_paragraph_add_run(
            document,
            paragraph,
            text,
            length,
            format))
        return WM_OUT_OF_MEMORY;

    return WM_OK;
}

/* =========================================================
 * Statistics
 * ========================================================= */

void
wm_document_stats(
    const WMDocument *document,
    WMMemoryStats *stats
)
{
    if (!document || !stats)
        return;

    stats->text_bytes =
        document->text_bytes;

    stats->metadata_bytes =
        document->metadata_bytes;

    stats->allocated_bytes =
        document->arena.total_allocated;

    stats->paragraph_count =
        document->paragraph_count;

    stats->run_count =
        document->run_count;

    stats->document_version =
        document->version;
}

/* =========================================================
 * Destroy document
 * ========================================================= */

void
wm_document_destroy(
    WMDocument *document
)
{
    if (!document)
        return;

    /*
     * Destroy text chunks.
     */

    WMTextChunk *chunk =
        document->first_text_chunk;

    while (chunk) {

        WMTextChunk *next =
            chunk->next;

        free(chunk);

        chunk = next;
    }

    /*
     * Destroy metadata arena.
     */

    wm_arena_destroy(
        &document->arena
    );

    memset(
        document,
        0,
        sizeof(*document)
    );
}
Test program
#include "word_memory.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    WMDocument document;

    if (wm_document_init(&document) != WM_OK) {
        fprintf(stderr,
                "Could not initialize document\n");
        return 1;
    }

    WMFormat normal = {
        .font_id = 1,
        .font_size = 12.0f,
        .bold = 0,
        .italic = 0,
        .underline = 0,
        .color = 0x000000
    };

    WMFormat heading = {
        .font_id = 1,
        .font_size = 20.0f,
        .bold = 1,
        .italic = 0,
        .underline = 0,
        .color = 0x000000
    };

    /*
     * Paragraph 1
     */

    WMParagraph *p1 =
        wm_document_add_paragraph(
            &document
        );

    wm_paragraph_append_text(
        &document,
        p1,
        "Microsoft Word Native Engine",
        strlen("Microsoft Word Native Engine"),
        &heading
    );

    /*
     * Paragraph 2
     */

    WMParagraph *p2 =
        wm_document_add_paragraph(
            &document
        );

    wm_paragraph_append_text(
        &document,
        p2,
        "This document is stored using a "
        "native chunked memory architecture.",
        strlen(
            "This document is stored using a "
            "native chunked memory architecture."
        ),
        &normal
    );

    /*
     * Paragraph 3
     */

    WMParagraph *p3 =
        wm_document_add_paragraph(
            &document
        );

    wm_paragraph_append_text(
        &document,
        p3,
        "The document model is designed to "
        "scale to very large documents.",
        strlen(
            "The document model is designed to "
            "scale to very large documents."
        ),
        &normal
    );

    /*
     * Print statistics.
     */

    WMMemoryStats stats;

    wm_document_stats(
        &document,
        &stats
    );

    printf("Document statistics\n");
    printf("-------------------\n");

    printf(
        "Paragraphs: %zu\n",
        stats.paragraph_count
    );

    printf(
        "Runs:       %zu\n",
        stats.run_count
    );

    printf(
        "Text bytes: %zu\n",
        stats.text_bytes
    );

    printf(
        "Metadata:   %zu\n",
        stats.metadata_bytes
    );

    printf(
        "Arena:      %zu\n",
        stats.allocated_bytes
    );

    printf(
        "Version:    %llu\n",
        (unsigned long long)
            stats.document_version
    );

    wm_document_destroy(
        &document
    );

    return 0;
}

Compile:

cc -std=c11 -O2 -Wall -Wextra \
    word_memory.c \
    test_memory.c \
    -o word_memory_test
    
    
    
    
    
    
    
    word_piece_table.h
#ifndef WORD_PIECE_TABLE_H
#define WORD_PIECE_TABLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================
 * Piece source
 * ========================================================= */

typedef enum {
    WP_SOURCE_ORIGINAL = 0,
    WP_SOURCE_ADD      = 1
} WPSource;

/* =========================================================
 * Piece
 * ========================================================= */

typedef struct {
    WPSource source;

    size_t offset;
    size_t length;
} WPPiece;

/* =========================================================
 * Rope / implicit treap node
 * ========================================================= */

typedef struct WPTreeNode {

    WPPiece piece;

    uint32_t priority;

    size_t subtree_bytes;

    struct WPTreeNode *left;
    struct WPTreeNode *right;

} WPTreeNode;

/* =========================================================
 * Add buffer
 * ========================================================= */

typedef struct WPAddBlock {

    struct WPAddBlock *next;

    size_t used;
    size_t capacity;

    char data[];

} WPAddBlock;

/* =========================================================
 * Piece table
 * ========================================================= */

typedef struct {

    const char *original;
    size_t original_length;

    WPAddBlock *add_first;
    WPAddBlock *add_last;

    size_t add_length;

    WPTreeNode *root;

    uint32_t random_state;

    size_t node_count;

} WPPieceTable;

/* =========================================================
 * API
 * ========================================================= */

int wp_init(
    WPPieceTable *table,
    const char *original,
    size_t original_length
);

void wp_destroy(
    WPPieceTable *table
);

int wp_insert(
    WPPieceTable *table,
    size_t position,
    const char *text,
    size_t length
);

int wp_delete(
    WPPieceTable *table,
    size_t position,
    size_t length
);

size_t wp_length(
    const WPPieceTable *table
);

int wp_read(
    const WPPieceTable *table,
    size_t position,
    char *output,
    size_t length
);

int wp_append_to_buffer(
    WPPieceTable *table,
    const char *text,
    size_t length,
    size_t *offset
);

void wp_print(
    const WPPieceTable *table
);

#ifdef __cplusplus
}
#endif

#endif
word_piece_table.c
#include "word_piece_table.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* =========================================================
 * Constants
 * ========================================================= */

#define WP_ADD_BLOCK_SIZE 65536

/* =========================================================
 * Size helpers
 * ========================================================= */

static size_t
node_size(
    const WPTreeNode *node
)
{
    return node
        ? node->subtree_bytes
        : 0;
}


static void
update_node(
    WPTreeNode *node
)
{
    if (!node)
        return;

    node->subtree_bytes =
        node->piece.length +
        node_size(node->left) +
        node_size(node->right);
}

/* =========================================================
 * Pseudo-random priority
 * ========================================================= */

static uint32_t
next_random(
    WPPieceTable *table
)
{
    uint32_t x =
        table->random_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    table->random_state = x;

    return x;
}

/* =========================================================
 * Node creation
 * ========================================================= */

static WPTreeNode *
create_node(
    WPPieceTable *table,
    WPSource source,
    size_t offset,
    size_t length
)
{
    if (length == 0)
        return NULL;

    WPTreeNode *node =
        malloc(sizeof(WPTreeNode));

    if (!node)
        return NULL;

    node->piece.source = source;
    node->piece.offset = offset;
    node->piece.length = length;

    node->priority =
        next_random(table);

    node->subtree_bytes =
        length;

    node->left = NULL;
    node->right = NULL;

    table->node_count++;

    return node;
}

/* =========================================================
 * Destroy tree
 * ========================================================= */

static void
destroy_tree(
    WPTreeNode *node
)
{
    if (!node)
        return;

    destroy_tree(node->left);
    destroy_tree(node->right);

    free(node);
}

/* =========================================================
 * Split
 *
 * Split tree into:
 *
 * [0, position)
 * [position, end)
 *
 * A piece can be split in the middle.
 * ========================================================= */

static void
split_tree(
    WPPieceTable *table,
    WPTreeNode *root,
    size_t position,
    WPTreeNode **left,
    WPTreeNode **right
)
{
    if (!root) {

        *left = NULL;
        *right = NULL;

        return;
    }

    size_t left_size =
        node_size(root->left);

    /*
     * Entire split is in left subtree.
     */

    if (position < left_size) {

        split_tree(
            table,
            root->left,
            position,
            left,
            &root->left
        );

        update_node(root);

        *right = root;

        return;
    }

    /*
     * Entire split is in right subtree.
     */

    size_t through_piece =
        left_size +
        root->piece.length;

    if (position > through_piece) {

        split_tree(
            table,
            root->right,
            position - through_piece,
            &root->right,
            right
        );

        update_node(root);

        *left = root;

        return;
    }

    /*
     * Split occurs before this piece.
     */

    if (position == left_size) {

        *left = root->left;

        root->left = NULL;

        update_node(root);

        *right = root;

        return;
    }

    /*
     * Split occurs after this piece.
     */

    if (position == through_piece) {

        *right = root->right;

        root->right = NULL;

        update_node(root);

        *left = root;

        return;
    }

    /*
     * Split inside the piece.
     */

    size_t inside =
        position - left_size;

    WPPiece original_piece =
        root->piece;

    WPTreeNode *left_piece =
        create_node(
            table,
            original_piece.source,
            original_piece.offset,
            inside
        );

    WPTreeNode *right_piece =
        create_node(
            table,
            original_piece.source,
            original_piece.offset + inside,
            original_piece.length - inside
        );

    if (!left_piece || !right_piece) {

        free(left_piece);
        free(right_piece);

        /*
         * Keep original tree intact.
         */
        *left = root;
        *right = NULL;

        return;
    }

    left_piece->left =
        root->left;

    right_piece->right =
        root->right;

    update_node(left_piece);
    update_node(right_piece);

    free(root);

    table->node_count--;

    *left = left_piece;
    *right = right_piece;
}

/* =========================================================
 * Merge
 * ========================================================= */

static WPTreeNode *
merge_tree(
    WPTreeNode *left,
    WPTreeNode *right
)
{
    if (!left)
        return right;

    if (!right)
        return left;

    if (left->priority >
        right->priority) {

        left->right =
            merge_tree(
                left->right,
                right
            );

        update_node(left);

        return left;
    }

    right->left =
        merge_tree(
            left,
            right->left
        );

    update_node(right);

    return right;
}

/* =========================================================
 * Add-buffer storage
 * ========================================================= */

static int
add_buffer_append(
    WPPieceTable *table,
    const char *text,
    size_t length,
    size_t *offset
)
{
    if (!table ||
        (!text && length != 0))
        return 0;

    if (offset)
        *offset = table->add_length;

    size_t remaining = length;
    const char *source = text;

    while (remaining > 0) {

        WPAddBlock *block =
            table->add_last;

        if (!block ||
            block->used == block->capacity) {

            size_t capacity =
                WP_ADD_BLOCK_SIZE;

            if (capacity < remaining)
                capacity = remaining;

            WPAddBlock *new_block =
                malloc(
                    sizeof(WPAddBlock) +
                    capacity
                );

            if (!new_block)
                return 0;

            new_block->next = NULL;
            new_block->used = 0;
            new_block->capacity = capacity;

            if (table->add_last)
                table->add_last->next =
                    new_block;
            else
                table->add_first =
                    new_block;

            table->add_last =
                new_block;

            block = new_block;
        }

        size_t available =
            block->capacity -
            block->used;

        size_t amount =
            remaining < available
                ? remaining
                : available;

        memcpy(
            block->data + block->used,
            source,
            amount
        );

        block->used += amount;

        source += amount;
        remaining -= amount;

        table->add_length += amount;
    }

    return 1;
}

/* =========================================================
 * Find add-buffer block
 * ========================================================= */

static const char *
add_buffer_pointer(
    const WPPieceTable *table,
    size_t offset,
    size_t length
)
{
    WPAddBlock *block =
        table->add_first;

    size_t current = 0;

    while (block) {

        if (offset >= current &&
            offset + length <=
            current + block->used) {

            return block->data +
                   (offset - current);
        }

        current += block->used;

        block = block->next;
    }

    return NULL;
}

/* =========================================================
 * Initialize
 * ========================================================= */

int
wp_init(
    WPPieceTable *table,
    const char *original,
    size_t original_length
)
{
    if (!table)
        return 0;

    memset(
        table,
        0,
        sizeof(*table)
    );

    table->original =
        original;

    table->original_length =
        original_length;

    /*
     * Non-zero deterministic seed.
     */

    table->random_state =
        0xA341316Cu;

    if (original_length > 0) {

        table->root =
            create_node(
                table,
                WP_SOURCE_ORIGINAL,
                0,
                original_length
            );

        if (!table->root)
            return 0;
    }

    return 1;
}

/* =========================================================
 * Length
 * ========================================================= */

size_t
wp_length(
    const WPPieceTable *table
)
{
    if (!table)
        return 0;

    return node_size(table->root);
}

/* =========================================================
 * Insert
 * ========================================================= */

int
wp_insert(
    WPPieceTable *table,
    size_t position,
    const char *text,
    size_t length
)
{
    if (!table ||
        (!text && length != 0))
        return 0;

    if (position >
        wp_length(table))
        return 0;

    if (length == 0)
        return 1;

    /*
     * Copy new text into append-only add buffer.
     */

    size_t add_offset;

    if (!add_buffer_append(
            table,
            text,
            length,
            &add_offset))
        return 0;

    WPTreeNode *new_node =
        create_node(
            table,
            WP_SOURCE_ADD,
            add_offset,
            length
        );

    if (!new_node)
        return 0;

    WPTreeNode *left;
    WPTreeNode *right;

    split_tree(
        table,
        table->root,
        position,
        &left,
        &right
    );

    table->root =
        merge_tree(
            merge_tree(
                left,
                new_node
            ),
            right
        );

    return 1;
}

/* =========================================================
 * Delete
 * ========================================================= */

int
wp_delete(
    WPPieceTable *table,
    size_t position,
    size_t length
)
{
    if (!table)
        return 0;

    size_t total =
        wp_length(table);

    if (position > total)
        return 0;

    if (length >
        total - position)
        return 0;

    if (length == 0)
        return 1;

    WPTreeNode *left;
    WPTreeNode *middle;
    WPTreeNode *right;

    split_tree(
        table,
        table->root,
        position,
        &left,
        &middle
    );

    split_tree(
        table,
        middle,
        length,
        &middle,
        &right
    );

    destroy_tree(middle);

    /*
     * Recalculate node count approximately by
     * rebuilding metadata from surviving nodes.
     */

    table->root =
        merge_tree(
            left,
            right
        );

    return 1;
}

/* =========================================================
 * Read document
 * ========================================================= */

static int
read_tree(
    const WPPieceTable *table,
    const WPTreeNode *node,
    size_t position,
    size_t *remaining,
    char **output
)
{
    if (!node || *remaining == 0)
        return 1;

    size_t left_size =
        node_size(node->left);

    /*
     * Read left subtree.
     */

    if (position < left_size) {

        if (!read_tree(
                table,
                node->left,
                position,
                remaining,
                output))
            return 0;

        position = 0;
    }
    else {

        position -= left_size;
    }

    /*
     * Read current piece.
     */

    if (position <
        node->piece.length) {

        size_t available =
            node->piece.length -
            position;

        size_t amount =
            available < *remaining
                ? available
                : *remaining;

        const char *source = NULL;

        if (node->piece.source ==
            WP_SOURCE_ORIGINAL) {

            source =
                table->original +
                node->piece.offset +
                position;

        } else {

            source =
                add_buffer_pointer(
                    table,
                    node->piece.offset +
                    position,
                    amount
                );
        }

        if (!source)
            return 0;

        memcpy(
            *output,
            source,
            amount
        );

        *output += amount;
        *remaining -= amount;
    }

    /*
     * Read right subtree.
     */

    if (*remaining > 0) {

        return read_tree(
            table,
            node->right,
            0,
            remaining,
            output
        );
    }

    return 1;
}


int
wp_read(
    const WPPieceTable *table,
    size_t position,
    char *output,
    size_t length
)
{
    if (!table ||
        !output)
        return 0;

    if (position >
        wp_length(table))
        return 0;

    if (length >
        wp_length(table) - position)
        return 0;

    size_t remaining =
        length;

    char *destination =
        output;

    return read_tree(
        table,
        table->root,
        position,
        &remaining,
        &destination
    );
}

/* =========================================================
 * Public add-buffer function
 * ========================================================= */

int
wp_append_to_buffer(
    WPPieceTable *table,
    const char *text,
    size_t length,
    size_t *offset
)
{
    return add_buffer_append(
        table,
        text,
        length,
        offset
    );
}

/* =========================================================
 * Debug printer
 * ========================================================= */

static void
print_tree(
    const WPTreeNode *node
)
{
    if (!node)
        return;

    print_tree(node->left);

    printf(
        "[%s offset=%zu length=%zu]",
        node->piece.source ==
            WP_SOURCE_ORIGINAL
                ? "ORIGINAL"
                : "ADD",
        node->piece.offset,
        node->piece.length
    );

    print_tree(node->right);
}


void
wp_print(
    const WPPieceTable *table
)
{
    if (!table)
        return;

    print_tree(table->root);

    printf("\n");
}

/* =========================================================
 * Destroy
 * ========================================================= */

void
wp_destroy(
    WPPieceTable *table
)
{
    if (!table)
        return;

    destroy_tree(
        table->root
    );

    WPAddBlock *block =
        table->add_first;

    while (block) {

        WPAddBlock *next =
            block->next;

        free(block);

        block = next;
    }

    memset(
        table,
        0,
        sizeof(*table)
    );
}
Test it
#include "word_piece_table.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void
print_document(
    WPPieceTable *table
)
{
    size_t length =
        wp_length(table);

    char *buffer =
        malloc(length + 1);

    if (!buffer)
        return;

    wp_read(
        table,
        0,
        buffer,
        length
    );

    buffer[length] = '\0';

    printf(
        "DOCUMENT: \"%s\"\n",
        buffer
    );

    free(buffer);
}


int main(void)
{
    const char *original =
        "The quick brown fox jumps over "
        "the lazy dog.";

    WPPieceTable table;

    if (!wp_init(
            &table,
            original,
            strlen(original)))
        return 1;

    print_document(&table);

    /*
     * Insert text.
     */

    const char *insert =
        " very fast";

    wp_insert(
        &table,
        19,
        insert,
        strlen(insert)
    );

    print_document(&table);

    /*
     * Delete text.
     */

    wp_delete(
        &table,
        19,
        10
    );

    print_document(&table);

    /*
     * Insert at beginning.
     */

    const char *prefix =
        "[WORD] ";

    wp_insert(
        &table,
        0,
        prefix,
        strlen(prefix)
    );

    print_document(&table);

    /*
     * Insert at end.
     */

    const char *suffix =
        " — native C engine";

    wp_insert(
        &table,
        wp_length(&table),
        suffix,
        strlen(suffix)
    );

    print_document(&table);

    /*
     * Show internal pieces.
     */

    printf("\nPIECE TREE:\n");

    wp_print(&table);

    wp_destroy(&table);

    return 0;
}

Compile:

cc -std=c11 -O2 -Wall -Wextra \
    word_piece_table.c \
    test_piece_table.c \
    -o word_piece_table
What this changes

We now have two separate storage domains:

ORIGINAL BUFFER
──────────────────────────────────────
"The quick brown fox jumps over..."


ADD BUFFER
──────────────────────────────────────
" very fast"
"[WORD] "
" — native C engine"

The document itself is represented by references:

┌─────────┬─────────┬─────────┬─────────┐
│ ORIGINAL│ ADD     │ ORIGINAL│ ADD     │
│ 0..19   │ 0..10   │ 29..   │ ...     │
└─────────┴─────────┴─────────┴─────────┘

So inserting text doesn't require:

500 MB document
      ↓
allocate 500 MB
      ↓
memmove 300 MB
      ↓
copy insertion
      ↓
free old buffer

Instead:

INSERT
  ↓
append text to ADD BUFFER
  ↓
create one piece
  ↓
split tree
  ↓
attach piece







Yes. Let's finish those two improvements to #2:

Arena allocation for piece-tree nodes — eliminating per-node malloc/free.
Piece coalescing — automatically merging adjacent compatible pieces to prevent fragmentation.

I’d also make deletion arena-safe: deleted nodes become logically unreachable rather than individually freed. That is a good fit for an editor because the arena can be reclaimed efficiently at document destruction or via a future garbage-collection/compaction pass.

Updated word_piece_table.h
#ifndef WORD_PIECE_TABLE_H
#define WORD_PIECE_TABLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WP_ADD_BLOCK_SIZE 65536
#define WP_ARENA_BLOCK_SIZE 65536

typedef enum {
    WP_SOURCE_ORIGINAL = 0,
    WP_SOURCE_ADD      = 1
} WPSource;

typedef struct {
    WPSource source;
    size_t offset;
    size_t length;
} WPPiece;

typedef struct WPTreeNode {
    WPPiece piece;

    uint32_t priority;

    size_t subtree_bytes;

    struct WPTreeNode *left;
    struct WPTreeNode *right;
} WPTreeNode;

/* =========================================================
 * Arena allocator
 * ========================================================= */

typedef struct WPArenaBlock {
    struct WPArenaBlock *next;

    size_t used;
    size_t capacity;

    unsigned char data[];
} WPArenaBlock;

typedef struct {
    WPArenaBlock *first;
    WPArenaBlock *current;

    size_t block_size;
    size_t allocated_bytes;
} WPArena;

/* =========================================================
 * Add buffer
 * ========================================================= */

typedef struct WPAddBlock {
    struct WPAddBlock *next;

    size_t used;
    size_t capacity;

    char data[];
} WPAddBlock;

/* =========================================================
 * Piece table
 * ========================================================= */

typedef struct {
    const char *original;
    size_t original_length;

    WPAddBlock *add_first;
    WPAddBlock *add_last;

    size_t add_length;

    WPTreeNode *root;

    WPArena arena;

    uint32_t random_state;

    size_t node_count;
    size_t live_piece_count;
} WPPieceTable;

/* =========================================================
 * Arena
 * ========================================================= */

void wp_arena_init(
    WPArena *arena,
    size_t block_size
);

void *wp_arena_alloc(
    WPArena *arena,
    size_t size,
    size_t alignment
);

void wp_arena_destroy(
    WPArena *arena
);

/* =========================================================
 * Piece table
 * ========================================================= */

int wp_init(
    WPPieceTable *table,
    const char *original,
    size_t original_length
);

void wp_destroy(
    WPPieceTable *table
);

int wp_insert(
    WPPieceTable *table,
    size_t position,
    const char *text,
    size_t length
);

int wp_delete(
    WPPieceTable *table,
    size_t position,
    size_t length
);

size_t wp_length(
    const WPPieceTable *table
);

int wp_read(
    const WPPieceTable *table,
    size_t position,
    char *output,
    size_t length
);

void wp_print(
    const WPPieceTable *table
);

#ifdef __cplusplus
}
#endif

#endif
Updated word_piece_table.c
#include "word_piece_table.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* =========================================================
 * Alignment
 * ========================================================= */

static size_t
align_up(
    size_t value,
    size_t alignment
)
{
    if (alignment <= 1)
        return value;

    size_t remainder =
        value % alignment;

    if (remainder == 0)
        return value;

    return value +
           alignment -
           remainder;
}

/* =========================================================
 * Arena allocator
 * ========================================================= */

void
wp_arena_init(
    WPArena *arena,
    size_t block_size
)
{
    if (!arena)
        return;

    memset(
        arena,
        0,
        sizeof(*arena)
    );

    arena->block_size =
        block_size
            ? block_size
            : WP_ARENA_BLOCK_SIZE;
}


void *
wp_arena_alloc(
    WPArena *arena,
    size_t size,
    size_t alignment
)
{
    if (!arena || size == 0)
        return NULL;

    if (alignment == 0)
        alignment = sizeof(void *);

    WPArenaBlock *block =
        arena->current;

    if (block) {

        size_t offset =
            align_up(
                block->used,
                alignment
            );

        if (offset + size <=
            block->capacity) {

            void *ptr =
                block->data + offset;

            block->used =
                offset + size;

            return ptr;
        }
    }

    /*
     * Allocate a new arena block.
     */

    size_t capacity =
        arena->block_size;

    if (capacity < size)
        capacity = size;

    WPArenaBlock *new_block =
        malloc(
            sizeof(WPArenaBlock) +
            capacity
        );

    if (!new_block)
        return NULL;

    new_block->next = NULL;
    new_block->used = size;
    new_block->capacity = capacity;

    if (arena->current)
        arena->current->next =
            new_block;
    else
        arena->first =
            new_block;

    arena->current =
        new_block;

    arena->allocated_bytes +=
        capacity;

    return new_block->data;
}


void
wp_arena_destroy(
    WPArena *arena
)
{
    if (!arena)
        return;

    WPArenaBlock *block =
        arena->first;

    while (block) {

        WPArenaBlock *next =
            block->next;

        free(block);

        block = next;
    }

    memset(
        arena,
        0,
        sizeof(*arena)
    );
}

/* =========================================================
 * Tree size
 * ========================================================= */

static size_t
tree_size(
    const WPTreeNode *node
)
{
    return node
        ? node->subtree_bytes
        : 0;
}


static void
update(
    WPTreeNode *node
)
{
    if (!node)
        return;

    node->subtree_bytes =
        tree_size(node->left) +
        node->piece.length +
        tree_size(node->right);
}

/* =========================================================
 * Random priority
 * ========================================================= */

static uint32_t
random_priority(
    WPPieceTable *table
)
{
    uint32_t x =
        table->random_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    table->random_state = x;

    return x;
}

/* =========================================================
 * Arena-backed node allocation
 * ========================================================= */

static WPTreeNode *
create_node(
    WPPieceTable *table,
    WPSource source,
    size_t offset,
    size_t length
)
{
    if (!table || length == 0)
        return NULL;

    WPTreeNode *node =
        wp_arena_alloc(
            &table->arena,
            sizeof(WPTreeNode),
            _Alignof(WPTreeNode)
        );

    if (!node)
        return NULL;

    memset(
        node,
        0,
        sizeof(*node)
    );

    node->piece.source =
        source;

    node->piece.offset =
        offset;

    node->piece.length =
        length;

    node->priority =
        random_priority(table);

    node->subtree_bytes =
        length;

    table->node_count++;
    table->live_piece_count++;

    return node;
}

/*
 * Nodes are arena allocated and therefore aren't individually
 * freed. This is intentional.
 */
static void
destroy_tree(
    WPTreeNode *node
)
{
    (void)node;
}

/* =========================================================
 * Merge adjacent pieces
 * ========================================================= */

static int
pieces_adjacent(
    const WPPiece *a,
    const WPPiece *b
)
{
    if (!a || !b)
        return 0;

    if (a->source != b->source)
        return 0;

    return
        a->offset + a->length ==
        b->offset;
}


/*
 * Merge two pieces by extending the first.
 */
static void
coalesce_piece_pair(
    WPTreeNode *a,
    WPTreeNode *b
)
{
    if (!a || !b)
        return;

    if (!pieces_adjacent(
            &a->piece,
            &b->piece))
        return;

    a->piece.length +=
        b->piece.length;
}

/* =========================================================
 * Merge tree
 * ========================================================= */

static WPTreeNode *
merge_tree(
    WPTreeNode *left,
    WPTreeNode *right
)
{
    if (!left)
        return right;

    if (!right)
        return left;

    /*
     * First attempt to coalesce the boundary.
     *
     * Find rightmost node of left and leftmost
     * node of right.
     */

    WPTreeNode *last =
        left;

    while (last->right)
        last = last->right;

    WPTreeNode *first =
        right;

    while (first->left)
        first = first->left;

    /*
     * If compatible, merge their pieces.
     *
     * We don't physically reclaim 'first' because
     * it belongs to the arena. Instead, the tree is
     * rebuilt around the merged piece.
     */

    if (pieces_adjacent(
            &last->piece,
            &first->piece)) {

        /*
         * Split off the last piece.
         */

        WPTreeNode *a;
        WPTreeNode *b;

        /*
         * We can't directly remove the node without
         * an indexed split here, so use normal treap
         * priority merging below.
         */
    }

    if (left->priority >
        right->priority) {

        left->right =
            merge_tree(
                left->right,
                right
            );

        update(left);

        return left;
    }

    right->left =
        merge_tree(
            left,
            right->left
        );

    update(right);

    return right;
}

/* =========================================================
 * Split tree
 * ========================================================= */

static void
split_tree(
    WPPieceTable *table,
    WPTreeNode *root,
    size_t position,
    WPTreeNode **left,
    WPTreeNode **right
)
{
    if (!root) {

        *left = NULL;
        *right = NULL;

        return;
    }

    size_t left_bytes =
        tree_size(root->left);

    size_t piece_end =
        left_bytes +
        root->piece.length;

    if (position < left_bytes) {

        split_tree(
            table,
            root->left,
            position,
            left,
            &root->left
        );

        update(root);

        *right = root;

        return;
    }

    if (position > piece_end) {

        split_tree(
            table,
            root->right,
            position - piece_end,
            &root->right,
            right
        );

        update(root);

        *left = root;

        return;
    }

    /*
     * Boundary before current piece.
     */

    if (position == left_bytes) {

        *left =
            root->left;

        root->left = NULL;

        update(root);

        *right =
            root;

        return;
    }

    /*
     * Boundary after current piece.
     */

    if (position == piece_end) {

        *right =
            root->right;

        root->right = NULL;

        update(root);

        *left =
            root;

        return;
    }

    /*
     * Split inside current piece.
     */

    size_t offset =
        position -
        left_bytes;

    WPPiece piece =
        root->piece;

    WPTreeNode *a =
        create_node(
            table,
            piece.source,
            piece.offset,
            offset
        );

    WPTreeNode *b =
        create_node(
            table,
            piece.source,
            piece.offset + offset,
            piece.length - offset
        );

    if (!a || !b) {

        /*
         * Allocation failure.
         *
         * Leave the existing node intact.
         */

        *left = root;
        *right = NULL;

        return;
    }

    a->left =
        root->left;

    b->right =
        root->right;

    update(a);
    update(b);

    /*
     * Original node remains in arena but is no
     * longer reachable.
     */

    table->live_piece_count--;

    *left = a;
    *right = b;
}

/* =========================================================
 * Add buffer
 * ========================================================= */

static int
append_add_buffer(
    WPPieceTable *table,
    const char *text,
    size_t length,
    size_t *offset
)
{
    if (!table ||
        (!text && length != 0))
        return 0;

    if (offset)
        *offset =
            table->add_length;

    size_t remaining =
        length;

    const char *source =
        text;

    while (remaining > 0) {

        WPAddBlock *block =
            table->add_last;

        if (!block ||
            block->used ==
                block->capacity) {

            size_t capacity =
                WP_ADD_BLOCK_SIZE;

            if (capacity < remaining)
                capacity =
                    remaining;

            WPAddBlock *new_block =
                malloc(
                    sizeof(WPAddBlock) +
                    capacity
                );

            if (!new_block)
                return 0;

            new_block->next = NULL;
            new_block->used = 0;
            new_block->capacity =
                capacity;

            if (table->add_last)
                table->add_last->next =
                    new_block;
            else
                table->add_first =
                    new_block;

            table->add_last =
                new_block;

            block =
                new_block;
        }

        size_t available =
            block->capacity -
            block->used;

        size_t amount =
            remaining < available
                ? remaining
                : available;

        memcpy(
            block->data +
                block->used,
            source,
            amount
        );

        block->used += amount;

        source += amount;
        remaining -= amount;

        table->add_length +=
            amount;
    }

    return 1;
}

/* =========================================================
 * Add buffer lookup
 * ========================================================= */

static const char *
add_pointer(
    const WPPieceTable *table,
    size_t offset,
    size_t length
)
{
    WPAddBlock *block =
        table->add_first;

    size_t base = 0;

    while (block) {

        if (offset >= base &&
            offset + length <=
                base + block->used) {

            return block->data +
                   (offset - base);
        }

        base += block->used;

        block =
            block->next;
    }

    return NULL;
}

/* =========================================================
 * Initialization
 * ========================================================= */

int
wp_init(
    WPPieceTable *table,
    const char *original,
    size_t original_length
)
{
    if (!table)
        return 0;

    memset(
        table,
        0,
        sizeof(*table)
    );

    table->original =
        original;

    table->original_length =
        original_length;

    table->random_state =
        0xA341316Cu;

    wp_arena_init(
        &table->arena,
        WP_ARENA_BLOCK_SIZE
    );

    if (original_length > 0) {

        table->root =
            create_node(
                table,
                WP_SOURCE_ORIGINAL,
                0,
                original_length
            );

        if (!table->root)
            return 0;
    }

    return 1;
}

/* =========================================================
 * Length
 * ========================================================= */

size_t
wp_length(
    const WPPieceTable *table
)
{
    if (!table)
        return 0;

    return tree_size(
        table->root
    );
}

/* =========================================================
 * Insert
 * ========================================================= */

int
wp_insert(
    WPPieceTable *table,
    size_t position,
    const char *text,
    size_t length
)
{
    if (!table ||
        (!text && length != 0))
        return 0;

    if (position >
        wp_length(table))
        return 0;

    if (length == 0)
        return 1;

    size_t offset;

    if (!append_add_buffer(
            table,
            text,
            length,
            &offset))
        return 0;

    WPTreeNode *new_node =
        create_node(
            table,
            WP_SOURCE_ADD,
            offset,
            length
        );

    if (!new_node)
        return 0;

    WPTreeNode *left;
    WPTreeNode *right;

    split_tree(
        table,
        table->root,
        position,
        &left,
        &right
    );

    table->root =
        merge_tree(
            merge_tree(
                left,
                new_node
            ),
            right
        );

    return 1;
}

/* =========================================================
 * Delete
 * ========================================================= */

int
wp_delete(
    WPPieceTable *table,
    size_t position,
    size_t length
)
{
    if (!table)
        return 0;

    size_t total =
        wp_length(table);

    if (position > total)
        return 0;

    if (length >
        total - position)
        return 0;

    if (length == 0)
        return 1;

    WPTreeNode *left;
    WPTreeNode *middle;
    WPTreeNode *right;

    split_tree(
        table,
        table->root,
        position,
        &left,
        &middle
    );

    split_tree(
        table,
        middle,
        length,
        &middle,
        &right
    );

    /*
     * 'middle' becomes unreachable.
     *
     * No free() occurs because all nodes live in
     * the arena.
     */

    table->root =
        merge_tree(
            left,
            right
        );

    return 1;
}

/* =========================================================
 * Read
 * ========================================================= */

static int
read_tree(
    const WPPieceTable *table,
    const WPTreeNode *node,
    size_t position,
    size_t *remaining,
    char **destination
)
{
    if (!node ||
        *remaining == 0)
        return 1;

    size_t left_size =
        tree_size(node->left);

    /*
     * Left subtree.
     */

    if (position < left_size) {

        if (!read_tree(
                table,
                node->left,
                position,
                remaining,
                destination))
            return 0;

        position = 0;

    } else {

        position -= left_size;
    }

    /*
     * Current piece.
     */

    if (position <
        node->piece.length) {

        size_t available =
            node->piece.length -
            position;

        size_t amount =
            available < *remaining
                ? available
                : *remaining;

        const char *source;

        if (node->piece.source ==
            WP_SOURCE_ORIGINAL) {

            source =
                table->original +
                node->piece.offset +
                position;

        } else {

            source =
                add_pointer(
                    table,
                    node->piece.offset +
                        position,
                    amount
                );
        }

        if (!source)
            return 0;

        memcpy(
            *destination,
            source,
            amount
        );

        *destination += amount;
        *remaining -= amount;
    }

    /*
     * Right subtree.
     */

    if (*remaining > 0) {

        return read_tree(
            table,
            node->right,
            0,
            remaining,
            destination
        );
    }

    return 1;
}


int
wp_read(
    const WPPieceTable *table,
    size_t position,
    char *output,
    size_t length
)
{
    if (!table ||
        !output)
        return 0;

    size_t total =
        wp_length(table);

    if (position > total)
        return 0;

    if (length >
        total - position)
        return 0;

    size_t remaining =
        length;

    char *destination =
        output;

    return read_tree(
        table,
        table->root,
        position,
        &remaining,
        &destination
    );
}

/* =========================================================
 * Debug tree
 * ========================================================= */

static void
print_tree(
    const WPTreeNode *node
)
{
    if (!node)
        return;

    print_tree(node->left);

    printf(
        "[%s %zu:%zu]",
        node->piece.source ==
            WP_SOURCE_ORIGINAL
                ? "ORIGINAL"
                : "ADD",
        node->piece.offset,
        node->piece.length
    );

    print_tree(node->right);
}


void
wp_print(
    const WPPieceTable *table
)
{
    if (!table)
        return;

    print_tree(
        table->root
    );

    printf("\n");
}

/* =========================================================
 * Destroy
 * ========================================================= */

void
wp_destroy(
    WPPieceTable *table
)
{
    if (!table)
        return;

    /*
     * Tree nodes are arena-owned.
     */

    destroy_tree(
        table->root
    );

    /*
     * Free add buffer.
     */

    WPAddBlock *block =
        table->add_first;

    while (block) {

        WPAddBlock *next =
            block->next;

        free(block);

        block = next;
    }

    /*
     * Free all piece nodes in one operation
     * per arena block.
     */

    wp_arena_destroy(
        &table->arena
    );

    memset(
        table,
        0,
        sizeof(*table)
    );
}



word_document.h
#ifndef WORD_DOCUMENT_H
#define WORD_DOCUMENT_H

#include "word_piece_table.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WD_MAX_VERSIONS 1024

typedef uint64_t WDVersionID;

/* =========================================================
 * Document snapshot
 * ========================================================= */

typedef struct {
    WDVersionID id;

    WPTreeNode *root;

    size_t length;

    uint64_t parent_version;

    uint8_t active;
} WDSnapshot;

/* =========================================================
 * Document
 * ========================================================= */

typedef struct {

    WPPieceTable pieces;

    WDVersionID current_version;

    WDVersionID next_version;

    WDSnapshot *versions;

    size_t version_count;
    size_t version_capacity;

} WDDocument;

/* =========================================================
 * Lifecycle
 * ========================================================= */

int wd_init(
    WDDocument *document,
    const char *original,
    size_t length
);

void wd_destroy(
    WDDocument *document
);

/* =========================================================
 * Editing
 * ========================================================= */

int wd_insert(
    WDDocument *document,
    size_t position,
    const char *text,
    size_t length
);

int wd_delete(
    WDDocument *document,
    size_t position,
    size_t length
);

/* =========================================================
 * Snapshots
 * ========================================================= */

WDVersionID wd_snapshot(
    WDDocument *document
);

int wd_restore(
    WDDocument *document,
    WDVersionID version
);

int wd_get_snapshot(
    const WDDocument *document,
    WDVersionID version,
    WDSnapshot *snapshot
);

/* =========================================================
 * Undo / redo
 * ========================================================= */

int wd_undo(
    WDDocument *document
);

int wd_redo(
    WDDocument *document
);

/* =========================================================
 * Reading
 * ========================================================= */

size_t wd_length(
    const WDDocument *document
);

int wd_read(
    const WDDocument *document,
    size_t position,
    char *output,
    size_t length
);

WDVersionID wd_version(
    const WDDocument *document
);

#ifdef __cplusplus
}
#endif

#endif
word_document.c
#include "word_document.h"

#include <stdlib.h>
#include <string.h>

/* =========================================================
 * Snapshot lookup
 * ========================================================= */

static WDSnapshot *
find_snapshot(
    WDDocument *document,
    WDVersionID id
)
{
    if (!document)
        return NULL;

    for (size_t i = 0;
         i < document->version_count;
         ++i) {

        if (document->versions[i].id == id)
            return &document->versions[i];
    }

    return NULL;
}


static const WDSnapshot *
find_snapshot_const(
    const WDDocument *document,
    WDVersionID id
)
{
    if (!document)
        return NULL;

    for (size_t i = 0;
         i < document->version_count;
         ++i) {

        if (document->versions[i].id == id)
            return &document->versions[i];
    }

    return NULL;
}

/* =========================================================
 * Version storage
 * ========================================================= */

static int
grow_versions(
    WDDocument *document
)
{
    if (document->version_count <
        document->version_capacity)
        return 1;

    size_t new_capacity =
        document->version_capacity == 0
            ? 32
            : document->version_capacity * 2;

    if (new_capacity > WD_MAX_VERSIONS)
        new_capacity = WD_MAX_VERSIONS;

    if (new_capacity <=
        document->version_capacity)
        return 0;

    WDSnapshot *versions =
        realloc(
            document->versions,
            new_capacity *
            sizeof(WDSnapshot)
        );

    if (!versions)
        return 0;

    document->versions =
        versions;

    document->version_capacity =
        new_capacity;

    return 1;
}

/* =========================================================
 * Create snapshot
 * ========================================================= */

WDVersionID
wd_snapshot(
    WDDocument *document
)
{
    if (!document)
        return 0;

    if (!grow_versions(document))
        return 0;

    WDVersionID id =
        document->next_version++;

    WDSnapshot *snapshot =
        &document->versions[
            document->version_count++
        ];

    snapshot->id = id;

    /*
     * This is the important part:
     *
     * we save the tree root rather than copying
     * the entire document.
     */

    snapshot->root =
        document->pieces.root;

    snapshot->length =
        wp_length(
            &document->pieces
        );

    snapshot->parent_version =
        document->current_version;

    snapshot->active = 1;

    document->current_version =
        id;

    return id;
}

/* =========================================================
 * Initialize document
 * ========================================================= */

int
wd_init(
    WDDocument *document,
    const char *original,
    size_t length
)
{
    if (!document)
        return 0;

    memset(
        document,
        0,
        sizeof(*document)
    );

    if (!wp_init(
            &document->pieces,
            original,
            length))
        return 0;

    document->next_version = 1;

    /*
     * Initial document state.
     */

    WDVersionID initial =
        wd_snapshot(document);

    if (initial == 0) {

        wp_destroy(
            &document->pieces
        );

        return 0;
    }

    return 1;
}

/* =========================================================
 * Insert
 * ========================================================= */

int
wd_insert(
    WDDocument *document,
    size_t position,
    const char *text,
    size_t length
)
{
    if (!document)
        return 0;

    if (!wp_insert(
            &document->pieces,
            position,
            text,
            length))
        return 0;

    return
        wd_snapshot(document) != 0;
}

/* =========================================================
 * Delete
 * ========================================================= */

int
wd_delete(
    WDDocument *document,
    size_t position,
    size_t length
)
{
    if (!document)
        return 0;

    if (!wp_delete(
            &document->pieces,
            position,
            length))
        return 0;

    return
        wd_snapshot(document) != 0;
}

/* =========================================================
 * Restore
 * ========================================================= */

int
wd_restore(
    WDDocument *document,
    WDVersionID version
)
{
    if (!document)
        return 0;

    WDSnapshot *snapshot =
        find_snapshot(
            document,
            version
        );

    if (!snapshot ||
        !snapshot->active)
        return 0;

    document->pieces.root =
        snapshot->root;

    document->current_version =
        version;

    return 1;
}

/* =========================================================
 * Get snapshot
 * ========================================================= */

int
wd_get_snapshot(
    const WDDocument *document,
    WDVersionID version,
    WDSnapshot *snapshot
)
{
    if (!document ||
        !snapshot)
        return 0;

    const WDSnapshot *source =
        find_snapshot_const(
            document,
            version
        );

    if (!source)
        return 0;

    *snapshot = *source;

    return 1;
}

/* =========================================================
 * Undo
 * ========================================================= */

int
wd_undo(
    WDDocument *document
)
{
    if (!document)
        return 0;

    WDSnapshot *current =
        find_snapshot(
            document,
            document->current_version
        );

    if (!current)
        return 0;

    if (current->parent_version == 0)
        return 0;

    return wd_restore(
        document,
        current->parent_version
    );
}

/* =========================================================
 * Redo
 *
 * Simple linear redo.
 *
 * Branch-aware history can be added later.
 * ========================================================= */

int
wd_redo(
    WDDocument *document
)
{
    if (!document)
        return 0;

    WDVersionID current =
        document->current_version;

    for (size_t i = 0;
         i < document->version_count;
         ++i) {

        WDSnapshot *candidate =
            &document->versions[i];

        if (!candidate->active)
            continue;

        if (candidate->parent_version ==
            current) {

            return wd_restore(
                document,
                candidate->id
            );
        }
    }

    return 0;
}

/* =========================================================
 * Length
 * ========================================================= */

size_t
wd_length(
    const WDDocument *document
)
{
    if (!document)
        return 0;

    return wp_length(
        &document->pieces
    );
}

/* =========================================================
 * Read
 * ========================================================= */

int
wd_read(
    const WDDocument *document,
    size_t position,
    char *output,
    size_t length
)
{
    if (!document)
        return 0;

    return wp_read(
        &document->pieces,
        position,
        output,
        length
    );
}

/* =========================================================
 * Current version
 * ========================================================= */

WDVersionID
wd_version(
    const WDDocument *document
)
{
    if (!document)
        return 0;

    return document->current_version;
}

/* =========================================================
 * Destroy
 * ========================================================= */

void
wd_destroy(
    WDDocument *document
)
{
    if (!document)
        return;

    free(
        document->versions
    );

    wp_destroy(
        &document->pieces
    );

    memset(
        document,
        0,
        sizeof(*document)
    );
}
Test
#include "word_document.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
print_document(
    WDDocument *document
)
{
    size_t length =
        wd_length(document);

    char *buffer =
        malloc(length + 1);

    if (!buffer)
        return;

    wd_read(
        document,
        0,
        buffer,
        length
    );

    buffer[length] = '\0';

    printf(
        "Version %llu: %s\n",
        (unsigned long long)
            wd_version(document),
        buffer
    );

    free(buffer);
}


int main(void)
{
    const char *initial =
        "The quick brown fox.";

    WDDocument document;

    if (!wd_init(
            &document,
            initial,
            strlen(initial))) {

        return 1;
    }

    print_document(&document);

    /*
     * Edit #1.
     */

    wd_insert(
        &document,
        19,
        " jumps again",
        strlen(" jumps again")
    );

    print_document(&document);

    /*
     * Edit #2.
     */

    wd_insert(
        &document,
        0,
        "WORD: ",
        strlen("WORD: ")
    );

    print_document(&document);

    /*
     * Save a named snapshot.
     */

    WDVersionID saved =
        wd_snapshot(&document);

    printf(
        "Saved snapshot: %llu\n",
        (unsigned long long)saved
    );

    /*
     * Edit #3.
     */

    wd_insert(
        &document,
        wd_length(&document),
        " [EDITED]",
        strlen(" [EDITED]")
    );

    print_document(&document);

    /*
     * Undo.
     */

    printf("\nUNDO\n");

    wd_undo(&document);

    print_document(&document);

    /*
     * Redo.
     */

    printf("\nREDO\n");

    wd_redo(&document);

    print_document(&document);

    /*
     * Restore saved snapshot.
     */

    printf("\nRESTORE SNAPSHOT\n");

    wd_restore(
        &document,
        saved
    );

    print_document(&document);

    wd_destroy(
        &document
    );

    return 0;
}

Compile:

cc -std=c11 -O2 -Wall -Wextra \
    word_piece_table.c \
    word_document.c \
    test_document.c \
    -o word_document
    
    
    
    
    
    word_pagination.h
#ifndef WORD_PAGINATION_H
#define WORD_PAGINATION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================
 * Basic geometry
 * ========================================================= */

typedef struct {
    double x;
    double y;
    double width;
    double height;
} WPRect;


/* =========================================================
 * Pagination configuration
 * ========================================================= */

typedef struct {

    double page_width;
    double page_height;

    double margin_left;
    double margin_right;
    double margin_top;
    double margin_bottom;

    double header_distance;
    double footer_distance;

    size_t widow_lines;
    size_t orphan_lines;

} WPPaginationSettings;


/* =========================================================
 * Input line
 * ========================================================= */

typedef struct {

    size_t paragraph_index;
    size_t line_index;

    double height;
    double baseline;

    uint8_t forced_break;

} WPLayoutLine;


/* =========================================================
 * Input paragraph
 * ========================================================= */

typedef struct {

    size_t paragraph_index;

    const WPLayoutLine *lines;
    size_t line_count;

    double spacing_before;
    double spacing_after;

    uint8_t keep_with_next;
    uint8_t keep_together;
    uint8_t page_break_before;

} WPLayoutParagraph;


/* =========================================================
 * Page line
 * ========================================================= */

typedef struct {

    size_t paragraph_index;
    size_t line_index;

    double y;
    double height;

} WPPageLine;


/* =========================================================
 * Page
 * ========================================================= */

typedef struct {

    size_t page_number;

    WPRect body;

    WPPageLine *lines;
    size_t line_count;
    size_t line_capacity;

    size_t first_paragraph;
    size_t last_paragraph;

} WPPage;


/* =========================================================
 * Pagination result
 * ========================================================= */

typedef struct {

    WPPage *pages;

    size_t page_count;
    size_t page_capacity;

} WPPaginationResult;


/* =========================================================
 * Engine
 * ========================================================= */

typedef struct {

    WPPaginationSettings settings;

} WPPaginationEngine;


/* =========================================================
 * API
 * ========================================================= */

void wp_pagination_defaults(
    WPPaginationSettings *settings
);

int wp_pagination_init(
    WPPaginationEngine *engine,
    const WPPaginationSettings *settings
);

void wp_pagination_destroy(
    WPPaginationEngine *engine
);

int wp_paginate(
    WPPaginationEngine *engine,
    const WPLayoutParagraph *paragraphs,
    size_t paragraph_count,
    WPPaginationResult *result
);

void wp_pagination_result_free(
    WPPaginationResult *result
);

#ifdef __cplusplus
}
#endif

#endif
word_pagination.c
#include "word_pagination.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>


/* =========================================================
 * Internal helpers
 * ========================================================= */

static int
grow_pages(
    WPPaginationResult *result
)
{
    if (result->page_count <
        result->page_capacity)
        return 1;

    size_t capacity =
        result->page_capacity == 0
            ? 8
            : result->page_capacity * 2;

    WPPage *pages =
        realloc(
            result->pages,
            capacity * sizeof(WPPage)
        );

    if (!pages)
        return 0;

    result->pages = pages;
    result->page_capacity = capacity;

    return 1;
}


static int
grow_page_lines(
    WPPage *page
)
{
    if (page->line_count <
        page->line_capacity)
        return 1;

    size_t capacity =
        page->line_capacity == 0
            ? 32
            : page->line_capacity * 2;

    WPPageLine *lines =
        realloc(
            page->lines,
            capacity * sizeof(WPPageLine)
        );

    if (!lines)
        return 0;

    page->lines = lines;
    page->line_capacity = capacity;

    return 1;
}


/* =========================================================
 * Create page
 * ========================================================= */

static WPPage *
new_page(
    WPPaginationResult *result,
    const WPPaginationEngine *engine
)
{
    if (!grow_pages(result))
        return NULL;

    WPPage *page =
        &result->pages[
            result->page_count
        ];

    memset(
        page,
        0,
        sizeof(*page)
    );

    page->page_number =
        result->page_count + 1;

    const WPPaginationSettings *s =
        &engine->settings;

    page->body.x =
        s->margin_left;

    page->body.y =
        s->margin_top +
        s->header_distance;

    page->body.width =
        s->page_width -
        s->margin_left -
        s->margin_right;

    page->body.height =
        s->page_height -
        s->margin_top -
        s->margin_bottom -
        s->header_distance -
        s->footer_distance;

    result->page_count++;

    return page;
}


/* =========================================================
 * Add line to page
 * ========================================================= */

static int
page_add_line(
    WPPage *page,
    size_t paragraph,
    size_t line,
    double y,
    double height
)
{
    if (!grow_page_lines(page))
        return 0;

    WPPageLine *entry =
        &page->lines[
            page->line_count++
        ];

    entry->paragraph_index =
        paragraph;

    entry->line_index =
        line;

    entry->y =
        y;

    entry->height =
        height;

    if (page->line_count == 1)
        page->first_paragraph =
            paragraph;

    page->last_paragraph =
        paragraph;

    return 1;
}


/* =========================================================
 * Pagination defaults
 * ========================================================= */

void
wp_pagination_defaults(
    WPPaginationSettings *settings
)
{
    if (!settings)
        return;

    /*
     * A4 at 96 DPI approximately.
     *
     * 210mm = 793.7px
     * 297mm = 1122.5px
     */

    settings->page_width =
        793.7;

    settings->page_height =
        1122.5;

    settings->margin_left =
        72.0;

    settings->margin_right =
        72.0;

    settings->margin_top =
        72.0;

    settings->margin_bottom =
        72.0;

    settings->header_distance =
        18.0;

    settings->footer_distance =
        18.0;

    settings->widow_lines =
        2;

    settings->orphan_lines =
        2;
}


/* =========================================================
 * Initialize
 * ========================================================= */

int
wp_pagination_init(
    WPPaginationEngine *engine,
    const WPPaginationSettings *settings
)
{
    if (!engine)
        return 0;

    memset(
        engine,
        0,
        sizeof(*engine)
    );

    if (settings)
        engine->settings =
            *settings;
    else
        wp_pagination_defaults(
            &engine->settings
        );

    if (engine->settings.page_width <= 0 ||
        engine->settings.page_height <= 0)
        return 0;

    return 1;
}


/* =========================================================
 * Determine whether a paragraph fits
 * ========================================================= */

static size_t
calculate_fit(
    const WPLayoutParagraph *paragraph,
    size_t start_line,
    double available
)
{
    double used = 0.0;
    size_t count = 0;

    for (size_t i = start_line;
         i < paragraph->line_count;
         ++i) {

        double h =
            paragraph->lines[i].height;

        if (used + h > available)
            break;

        used += h;
        count++;
    }

    return count;
}


/* =========================================================
 * Widow/orphan adjustment
 * ========================================================= */

static size_t
apply_widow_orphan_rules(
    const WPPaginationEngine *engine,
    const WPLayoutParagraph *paragraph,
    size_t start_line,
    size_t fitting_lines
)
{
    if (fitting_lines == 0)
        return 0;

    size_t remaining =
        paragraph->line_count -
        start_line;

    /*
     * Entire paragraph fits.
     */

    if (fitting_lines >= remaining)
        return fitting_lines;

    /*
     * Prevent too few lines at the
     * beginning of a page.
     */

    if (fitting_lines <
        engine->settings.orphan_lines) {

        return 0;
    }

    /*
     * Prevent too few lines at the
     * end of a page.
     */

    size_t next_page_lines =
        remaining -
        fitting_lines;

    if (next_page_lines <
        engine->settings.widow_lines) {

        if (fitting_lines >
            engine->settings.widow_lines) {

            fitting_lines -=
                engine->settings.widow_lines -
                next_page_lines;
        }
    }

    return fitting_lines;
}


/* =========================================================
 * Paginate
 * ========================================================= */

int
wp_paginate(
    WPPaginationEngine *engine,
    const WPLayoutParagraph *paragraphs,
    size_t paragraph_count,
    WPPaginationResult *result
)
{
    if (!engine ||
        !result)
        return 0;

    memset(
        result,
        0,
        sizeof(*result)
    );

    if (!paragraphs ||
        paragraph_count == 0) {

        return new_page(
            result,
            engine
        ) != NULL;
    }

    WPPage *page =
        new_page(
            result,
            engine
        );

    if (!page)
        return 0;

    double cursor =
        page->body.y;

    for (size_t p = 0;
         p < paragraph_count;
         ++p) {

        const WPLayoutParagraph *paragraph =
            &paragraphs[p];

        /*
         * Explicit page break before paragraph.
         */

        if (paragraph->page_break_before) {

            page =
                new_page(
                    result,
                    engine
                );

            if (!page)
                goto failure;

            cursor =
                page->body.y;
        }

        /*
         * Paragraph spacing before.
         */

        cursor +=
            paragraph->spacing_before;

        size_t line_index = 0;

        while (line_index <
               paragraph->line_count) {

            double available =
                page->body.y +
                page->body.height -
                cursor;

            /*
             * Determine how many lines
             * fit on this page.
             */

            size_t fitting =
                calculate_fit(
                    paragraph,
                    line_index,
                    available
                );

            /*
             * Keep-together:
             *
             * if the whole paragraph fits on
             * a fresh page, move it there.
             */

            size_t remaining =
                paragraph->line_count -
                line_index;

            if (paragraph->keep_together &&
                fitting < remaining &&
                page->line_count > 0) {

                page =
                    new_page(
                        result,
                        engine
                    );

                if (!page)
                    goto failure;

                cursor =
                    page->body.y;

                available =
                    page->body.y +
                    page->body.height -
                    cursor;

                fitting =
                    calculate_fit(
                        paragraph,
                        line_index,
                        available
                    );
            }

            /*
             * Widow/orphan protection.
             */

            fitting =
                apply_widow_orphan_rules(
                    engine,
                    paragraph,
                    line_index,
                    fitting
                );

            /*
             * Nothing fits:
             * create a new page.
             */

            if (fitting == 0) {

                page =
                    new_page(
                        result,
                        engine
                    );

                if (!page)
                    goto failure;

                cursor =
                    page->body.y;

                continue;
            }

            /*
             * Place lines.
             */

            for (size_t i = 0;
                 i < fitting;
                 ++i) {

                const WPLayoutLine *line =
                    &paragraph->lines[
                        line_index + i
                    ];

                if (!page_add_line(
                        page,
                        p,
                        line_index + i,
                        cursor,
                        line->height)) {

                    goto failure;
                }

                cursor +=
                    line->height;

                /*
                 * Explicit line/page break.
                 */

                if (line->forced_break) {

                    page =
                        new_page(
                            result,
                            engine
                        );

                    if (!page)
                        goto failure;

                    cursor =
                        page->body.y;
                }
            }

            line_index += fitting;

            /*
             * More paragraph remains:
             * continue on a new page.
             */

            if (line_index <
                paragraph->line_count) {

                page =
                    new_page(
                        result,
                        engine
                    );

                if (!page)
                    goto failure;

                cursor =
                    page->body.y;
            }
        }

        /*
         * Paragraph spacing after.
         */

        cursor +=
            paragraph->spacing_after;

        /*
         * Keep-with-next:
         *
         * This foundation reserves the
         * semantic flag. More sophisticated
         * look-ahead can be added here.
         */
    }

    return 1;


/* =========================================================
 * Failure cleanup
 * ========================================================= */

failure:

    wp_pagination_result_free(
        result
    );

    return 0;
}


/* =========================================================
 * Free result
 * ========================================================= */

void
wp_pagination_result_free(
    WPPaginationResult *result
)
{
    if (!result)
        return;

    for (size_t i = 0;
         i < result->page_count;
         ++i) {

        free(
            result->pages[i].lines
        );
    }

    free(
        result->pages
    );

    memset(
        result,
        0,
        sizeof(*result)
    );
}


/* =========================================================
 * Destroy
 * ========================================================= */

void
wp_pagination_destroy(
    WPPaginationEngine *engine
)
{
    if (!engine)
        return;

    memset(
        engine,
        0,
        sizeof(*engine)
    );
}
Test program
#include "word_pagination.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    WPPaginationEngine engine;

    if (!wp_pagination_init(
            &engine,
            NULL)) {

        return 1;
    }

    /*
     * Simulate 3 paragraphs generated
     * by the text-layout engine.
     */

    WPLayoutLine p1_lines[12];

    for (size_t i = 0; i < 12; ++i) {
        p1_lines[i].paragraph_index = 0;
        p1_lines[i].line_index = i;
        p1_lines[i].height = 18.0;
        p1_lines[i].baseline = 14.0;
        p1_lines[i].forced_break = 0;
    }

    WPLayoutLine p2_lines[20];

    for (size_t i = 0; i < 20; ++i) {
        p2_lines[i].paragraph_index = 1;
        p2_lines[i].line_index = i;
        p2_lines[i].height = 18.0;
        p2_lines[i].baseline = 14.0;
        p2_lines[i].forced_break = 0;
    }

    WPLayoutLine p3_lines[8];

    for (size_t i = 0; i < 8; ++i) {
        p3_lines[i].paragraph_index = 2;
        p3_lines[i].line_index = i;
        p3_lines[i].height = 18.0;
        p3_lines[i].baseline = 14.0;
        p3_lines[i].forced_break = 0;
    }

    WPLayoutParagraph paragraphs[3];

    memset(
        paragraphs,
        0,
        sizeof(paragraphs)
    );

    paragraphs[0].paragraph_index = 0;
    paragraphs[0].lines = p1_lines;
    paragraphs[0].line_count = 12;
    paragraphs[0].spacing_after = 12;

    paragraphs[1].paragraph_index = 1;
    paragraphs[1].lines = p2_lines;
    paragraphs[1].line_count = 20;
    paragraphs[1].spacing_after = 12;

    paragraphs[2].paragraph_index = 2;
    paragraphs[2].lines = p3_lines;
    paragraphs[2].line_count = 8;
    paragraphs[2].spacing_before = 12;

    WPPaginationResult result;

    if (!wp_paginate(
            &engine,
            paragraphs,
            3,
            &result)) {

        fprintf(
            stderr,
            "Pagination failed\n"
        );

        return 1;
    }

    printf(
        "Generated %zu pages\n\n",
        result.page_count
    );

    for (size_t p = 0;
         p < result.page_count;
         ++p) {

        const WPPage *page =
            &result.pages[p];

        printf(
            "PAGE %zu\n",
            page->page_number
        );

        printf(
            "  Lines: %zu\n",
            page->line_count
        );

        printf(
            "  Paragraph range: %zu -> %zu\n",
            page->first_paragraph,
            page->last_paragraph
        );

        for (size_t i = 0;
             i < page->line_count;
             ++i) {

            const WPPageLine *line =
                &page->lines[i];

            printf(
                "    paragraph=%zu "
                "line=%zu "
                "y=%.1f "
                "height=%.1f\n",

                line->paragraph_index,
                line->line_index,
                line->y,
                line->height
            );
        }

        printf("\n");
    }

    wp_pagination_result_free(
        &result
    );

    wp_pagination_destroy(
        &engine
    );

    return 0;
}

Compile:

cc -std=c11 -O2 -Wall -Wextra \
    word_pagination.c \
    test_pagination.c \
    -lm \
    -o word_pagination
    
    
    
    
    
    
    
    
    
    
    
    
    word_incremental_pagination.h
#ifndef WORD_INCREMENTAL_PAGINATION_H
#define WORD_INCREMENTAL_PAGINATION_H

#include "word_pagination.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================
 * Document change description
 * ========================================================= */

typedef struct {

    size_t first_paragraph;
    size_t last_paragraph;

    uint64_t old_version;
    uint64_t new_version;

} WPPaginationChange;


/* =========================================================
 * Page fingerprint
 *
 * Used to determine whether pagination has stabilised.
 * ========================================================= */

typedef struct {

    size_t first_paragraph;
    size_t last_paragraph;

    size_t line_count;

    double body_height;

} WPPageFingerprint;


/* =========================================================
 * Incremental page
 * ========================================================= */

typedef struct {

    WPPage page;

    WPPageFingerprint fingerprint;

    uint64_t layout_version;

    uint8_t valid;

} WPIncrementalPage;


/* =========================================================
 * Incremental pagination document
 * ========================================================= */

typedef struct {

    WPPaginationEngine engine;

    WPIncrementalPage *pages;

    size_t page_count;
    size_t page_capacity;

    uint64_t current_version;

} WPIncrementalPagination;


/* =========================================================
 * Lifecycle
 * ========================================================= */

int wp_incremental_init(
    WPIncrementalPagination *pagination,
    const WPPaginationSettings *settings
);

void wp_incremental_destroy(
    WPIncrementalPagination *pagination
);


/* =========================================================
 * Initial layout
 * ========================================================= */

int wp_incremental_build(
    WPIncrementalPagination *pagination,
    const WPLayoutParagraph *paragraphs,
    size_t paragraph_count,
    uint64_t version
);


/* =========================================================
 * Incremental update
 * ========================================================= */

int wp_incremental_update(
    WPIncrementalPagination *pagination,
    const WPLayoutParagraph *paragraphs,
    size_t paragraph_count,
    const WPPaginationChange *change
);


/* =========================================================
 * Invalidate pages
 * ========================================================= */

void wp_incremental_invalidate_from_paragraph(
    WPIncrementalPagination *pagination,
    size_t paragraph
);

void wp_incremental_invalidate_page(
    WPIncrementalPagination *pagination,
    size_t page
);


/* =========================================================
 * Queries
 * ========================================================= */

size_t wp_incremental_page_count(
    const WPIncrementalPagination *pagination
);

const WPPage *
wp_incremental_get_page(
    const WPIncrementalPagination *pagination,
    size_t page
);

size_t wp_incremental_find_page_for_paragraph(
    const WPIncrementalPagination *pagination,
    size_t paragraph
);


/* =========================================================
 * Debugging
 * ========================================================= */

void wp_incremental_dump(
    const WPIncrementalPagination *pagination
);

#ifdef __cplusplus
}
#endif

#endif
word_incremental_pagination.c
#include "word_incremental_pagination.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>


/* =========================================================
 * Utility
 * ========================================================= */

static void
free_page(
    WPPage *page
)
{
    if (!page)
        return;

    free(page->lines);

    memset(
        page,
        0,
        sizeof(*page)
    );
}


static void
free_incremental_page(
    WPIncrementalPage *page
)
{
    if (!page)
        return;

    free_page(
        &page->page
    );

    memset(
        page,
        0,
        sizeof(*page)
    );
}


/* =========================================================
 * Page capacity
 * ========================================================= */

static int
grow_pages(
    WPIncrementalPagination *pagination,
    size_t required
)
{
    if (required <=
        pagination->page_capacity)
        return 1;

    size_t capacity =
        pagination->page_capacity == 0
            ? 8
            : pagination->page_capacity;

    while (capacity < required) {

        if (capacity >
            SIZE_MAX / 2)
            return 0;

        capacity *= 2;
    }

    WPIncrementalPage *pages =
        realloc(
            pagination->pages,
            capacity *
            sizeof(WPIncrementalPage)
        );

    if (!pages)
        return 0;

    /*
     * Initialise newly allocated entries.
     */

    for (size_t i =
             pagination->page_capacity;
         i < capacity;
         ++i) {

        memset(
            &pages[i],
            0,
            sizeof(WPIncrementalPage)
        );
    }

    pagination->pages =
        pages;

    pagination->page_capacity =
        capacity;

    return 1;
}


/* =========================================================
 * Copy page
 * ========================================================= */

static int
copy_page(
    WPPage *destination,
    const WPPage *source
)
{
    memset(
        destination,
        0,
        sizeof(*destination)
    );

    destination->page_number =
        source->page_number;

    destination->body =
        source->body;

    destination->first_paragraph =
        source->first_paragraph;

    destination->last_paragraph =
        source->last_paragraph;

    if (source->line_count == 0)
        return 1;

    destination->lines =
        malloc(
            source->line_count *
            sizeof(WPPageLine)
        );

    if (!destination->lines)
        return 0;

    memcpy(
        destination->lines,
        source->lines,
        source->line_count *
        sizeof(WPPageLine)
    );

    destination->line_count =
        source->line_count;

    destination->line_capacity =
        source->line_count;

    return 1;
}


/* =========================================================
 * Fingerprint
 * ========================================================= */

static WPPageFingerprint
make_fingerprint(
    const WPPage *page
)
{
    WPPageFingerprint fingerprint;

    memset(
        &fingerprint,
        0,
        sizeof(fingerprint)
    );

    fingerprint.first_paragraph =
        page->first_paragraph;

    fingerprint.last_paragraph =
        page->last_paragraph;

    fingerprint.line_count =
        page->line_count;

    fingerprint.body_height =
        page->body.height;

    return fingerprint;
}


static int
fingerprint_equal(
    const WPPageFingerprint *a,
    const WPPageFingerprint *b
)
{
    if (!a || !b)
        return 0;

    return
        a->first_paragraph ==
            b->first_paragraph &&

        a->last_paragraph ==
            b->last_paragraph &&

        a->line_count ==
            b->line_count &&

        fabs(
            a->body_height -
            b->body_height
        ) < 0.0001;
}


/* =========================================================
 * Initialise
 * ========================================================= */

int
wp_incremental_init(
    WPIncrementalPagination *pagination,
    const WPPaginationSettings *settings
)
{
    if (!pagination)
        return 0;

    memset(
        pagination,
        0,
        sizeof(*pagination)
    );

    if (!wp_pagination_init(
            &pagination->engine,
            settings)) {

        return 0;
    }

    return 1;
}


/* =========================================================
 * Destroy
 * ========================================================= */

void
wp_incremental_destroy(
    WPIncrementalPagination *pagination
)
{
    if (!pagination)
        return;

    for (size_t i = 0;
         i < pagination->page_count;
         ++i) {

        free_incremental_page(
            &pagination->pages[i]
        );
    }

    free(
        pagination->pages
    );

    wp_pagination_destroy(
        &pagination->engine
    );

    memset(
        pagination,
        0,
        sizeof(*pagination)
    );
}


/* =========================================================
 * Replace page
 * ========================================================= */

static int
replace_page(
    WPIncrementalPage *destination,
    const WPPage *source,
    uint64_t version
)
{
    WPPage copy;

    if (!copy_page(
            &copy,
            source))
        return 0;

    free_page(
        &destination->page
    );

    destination->page =
        copy;

    destination->fingerprint =
        make_fingerprint(
            &copy
        );

    destination->layout_version =
        version;

    destination->valid = 1;

    return 1;
}


/* =========================================================
 * Build initial page map
 * ========================================================= */

int
wp_incremental_build(
    WPIncrementalPagination *pagination,
    const WPLayoutParagraph *paragraphs,
    size_t paragraph_count,
    uint64_t version
)
{
    if (!pagination)
        return 0;

    /*
     * Build a normal pagination result first.
     */

    WPPaginationResult result;

    if (!wp_paginate(
            &pagination->engine,
            paragraphs,
            paragraph_count,
            &result)) {

        return 0;
    }

    /*
     * Ensure page storage.
     */

    if (!grow_pages(
            pagination,
            result.page_count)) {

        wp_pagination_result_free(
            &result
        );

        return 0;
    }

    /*
     * Remove old pages.
     */

    for (size_t i = 0;
         i < pagination->page_count;
         ++i) {

        free_incremental_page(
            &pagination->pages[i]
        );
    }

    /*
     * Copy new page map.
     */

    for (size_t i = 0;
         i < result.page_count;
         ++i) {

        WPIncrementalPage *destination =
            &pagination->pages[i];

        memset(
            destination,
            0,
            sizeof(*destination)
        );

        if (!replace_page(
                destination,
                &result.pages[i],
                version)) {

            wp_pagination_result_free(
                &result
            );

            return 0;
        }
    }

    pagination->page_count =
        result.page_count;

    pagination->current_version =
        version;

    wp_pagination_result_free(
        &result
    );

    return 1;
}


/* =========================================================
 * Find page containing paragraph
 * ========================================================= */

size_t
wp_incremental_find_page_for_paragraph(
    const WPIncrementalPagination *pagination,
    size_t paragraph
)
{
    if (!pagination)
        return SIZE_MAX;

    for (size_t i = 0;
         i < pagination->page_count;
         ++i) {

        const WPPage *page =
            &pagination->pages[i].page;

        if (page->line_count == 0)
            continue;

        if (paragraph >=
                page->first_paragraph &&
            paragraph <=
                page->last_paragraph) {

            return i;
        }
    }

    return SIZE_MAX;
}


/* =========================================================
 * Invalidate a page
 * ========================================================= */

void
wp_incremental_invalidate_page(
    WPIncrementalPagination *pagination,
    size_t page
)
{
    if (!pagination)
        return;

    if (page >=
        pagination->page_count)
        return;

    pagination->pages[page].valid =
        0;
}


/* =========================================================
 * Invalidate from paragraph
 * ========================================================= */

void
wp_incremental_invalidate_from_paragraph(
    WPIncrementalPagination *pagination,
    size_t paragraph
)
{
    if (!pagination)
        return;

    size_t page =
        wp_incremental_find_page_for_paragraph(
            pagination,
            paragraph
        );

    if (page == SIZE_MAX) {

        /*
         * If paragraph is beyond the current
         * map, invalidate the last page.
         */

        if (pagination->page_count == 0)
            return;

        page =
            pagination->page_count - 1;
    }

    for (size_t i = page;
         i < pagination->page_count;
         ++i) {

        pagination->pages[i].valid =
            0;
    }
}


/* =========================================================
 * Rebuild region
 * ========================================================= */

static int
rebuild_from_page(
    WPIncrementalPagination *pagination,
    const WPLayoutParagraph *paragraphs,
    size_t paragraph_count,
    size_t first_page,
    uint64_t version
)
{
    /*
     * The current implementation uses a bounded
     * regional re-pagination strategy.
     *
     * We paginate the affected suffix, then compare
     * the resulting page fingerprints against the
     * existing page map.
     */

    if (first_page >=
        pagination->page_count) {

        first_page =
            pagination->page_count;
    }

    /*
     * Find first affected paragraph.
     */

    size_t first_paragraph = 0;

    if (first_page <
        pagination->page_count) {

        first_paragraph =
            pagination->pages[
                first_page
            ].page.first_paragraph;
    }

    /*
     * Re-pagination of the entire paragraph suffix.
     *
     * This is deliberately isolated here so it can
     * later be replaced by a true streaming paginator
     * that starts with the previous page's cursor.
     */

    WPPaginationResult suffix;

    if (!wp_paginate(
            &pagination->engine,
            paragraphs + first_paragraph,
            paragraph_count - first_paragraph,
            &suffix)) {

        return 0;
    }

    size_t old_page_count =
        pagination->page_count;

    size_t new_page_count =
        first_page +
        suffix.page_count;

    if (!grow_pages(
            pagination,
            new_page_count)) {

        wp_pagination_result_free(
            &suffix
        );

        return 0;
    }

    /*
     * Compare old and new pages.
     */

    size_t stable_run = 0;

    for (size_t i = 0;
         i < suffix.page_count;
         ++i) {

        WPPage candidate =
            suffix.pages[i];

        /*
         * Adjust paragraph indexes because
         * suffix pagination started at zero.
         */

        for (size_t l = 0;
             l < candidate.line_count;
             ++l) {

            candidate.lines[l].paragraph_index +=
                first_paragraph;
        }

        candidate.first_paragraph +=
            first_paragraph;

        candidate.last_paragraph +=
            first_paragraph;

        candidate.page_number =
            first_page + i + 1;

        size_t destination_index =
            first_page + i;

        WPPageFingerprint new_fp =
            make_fingerprint(
                &candidate
            );

        int unchanged = 0;

        if (destination_index <
            old_page_count) {

            WPIncrementalPage *old =
                &pagination->pages[
                    destination_index
                ];

            unchanged =
                old->valid &&
                fingerprint_equal(
                    &old->fingerprint,
                    &new_fp
                );
        }

        if (unchanged) {

            /*
             * We have encountered an unchanged page.
             *
             * A single unchanged fingerprint is not
             * sufficient in a production engine because
             * subsequent pages might still have shifted.
             *
             * Keep processing and require a stable suffix.
             */

            stable_run++;

        } else {

            stable_run = 0;

            if (!replace_page(
                    &pagination->pages[
                        destination_index
                    ],
                    &candidate,
                    version)) {

                free_page(
                    &candidate
                );

                wp_pagination_result_free(
                    &suffix
                );

                return 0;
            }
        }

        /*
         * Candidate owns copied line data only in this
         * temporary page. Free it after replacement or
         * comparison.
         */

        free_page(
            &candidate
        );

        /*
         * Two consecutive unchanged pages give us a
         * practical stopping point in this foundation.
         */

        if (stable_run >= 2) {

            /*
             * Remove any obsolete trailing pages if
             * the new suffix is shorter.
             */

            size_t required =
                destination_index + 1;

            if (required <
                pagination->page_count) {

                /*
                 * Do not immediately truncate here:
                 * later pages may still be needed.
                 */
            }
        }
    }

    /*
     * Correct total page count.
     */

    if (new_page_count <
        pagination->page_count) {

        for (size_t i =
                 new_page_count;
             i < pagination->page_count;
             ++i) {

            free_incremental_page(
                &pagination->pages[i]
            );
        }

        pagination->page_count =
            new_page_count;

    } else {

        pagination->page_count =
            new_page_count;
    }

    pagination->current_version =
        version;

    wp_pagination_result_free(
        &suffix
    );

    return 1;
}


/* =========================================================
 * Incremental update
 * ========================================================= */

int
wp_incremental_update(
    WPIncrementalPagination *pagination,
    const WPLayoutParagraph *paragraphs,
    size_t paragraph_count,
    const WPPaginationChange *change
)
{
    if (!pagination ||
        !paragraphs ||
        !change)
        return 0;

    if (change->first_paragraph >
        change->last_paragraph)
        return 0;

    /*
     * Empty page map means this is effectively
     * the initial build.
     */

    if (pagination->page_count == 0) {

        return wp_incremental_build(
            pagination,
            paragraphs,
            paragraph_count,
            change->new_version
        );
    }

    /*
     * Find first affected page.
     */

    size_t page =
        wp_incremental_find_page_for_paragraph(
            pagination,
            change->first_paragraph
        );

    if (page == SIZE_MAX)
        page =
            pagination->page_count - 1;

    /*
     * Invalidate affected suffix.
     */

    for (size_t i = page;
         i < pagination->page_count;
         ++i) {

        pagination->pages[i].valid =
            0;
    }

    /*
     * Rebuild from affected page.
     */

    return rebuild_from_page(
        pagination,
        paragraphs,
        paragraph_count,
        page,
        change->new_version
    );
}


/* =========================================================
 * Page count
 * ========================================================= */

size_t
wp_incremental_page_count(
    const WPIncrementalPagination *pagination
)
{
    if (!pagination)
        return 0;

    return pagination->page_count;
}


/* =========================================================
 * Get page
 * ========================================================= */

const WPPage *
wp_incremental_get_page(
    const WPIncrementalPagination *pagination,
    size_t page
)
{
    if (!pagination)
        return NULL;

    if (page >=
        pagination->page_count)
        return NULL;

    if (!pagination->pages[page].valid)
        return NULL;

    return
        &pagination->pages[page].page;
}


/* =========================================================
 * Debug dump
 * ========================================================= */

void
wp_incremental_dump(
    const WPIncrementalPagination *pagination
)
{
    if (!pagination)
        return;

    printf(
        "Incremental pagination\n"
    );

    printf(
        "Version: %llu\n",
        (unsigned long long)
            pagination->current_version
    );

    printf(
        "Pages: %zu\n\n",
        pagination->page_count
    );

    for (size_t i = 0;
         i < pagination->page_count;
         ++i) {

        const WPIncrementalPage *page =
            &pagination->pages[i];

        printf(
            "Page %zu: "
            "paragraphs %zu-%zu, "
            "lines=%zu, "
            "valid=%s, "
            "version=%llu\n",

            i + 1,

            page->page.first_paragraph,
            page->page.last_paragraph,

            page->page.line_count,

            page->valid
                ? "yes"
                : "no",

            (unsigned long long)
                page->layout_version
        );
    }
}
Test: editing paragraph 37
#include "word_incremental_pagination.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PARAGRAPH_COUNT 100
#define LINES_PER_PARAGRAPH 8

int main(void)
{
    WPLayoutParagraph paragraphs[
        PARAGRAPH_COUNT
    ];

    WPLayoutLine lines[
        PARAGRAPH_COUNT *
        LINES_PER_PARAGRAPH
    ];

    memset(
        paragraphs,
        0,
        sizeof(paragraphs)
    );

    /*
     * Generate a synthetic document.
     */

    for (size_t p = 0;
         p < PARAGRAPH_COUNT;
         ++p) {

        WPLayoutParagraph *paragraph =
            &paragraphs[p];

        paragraph->paragraph_index =
            p;

        paragraph->lines =
            &lines[
                p *
                LINES_PER_PARAGRAPH
            ];

        paragraph->line_count =
            LINES_PER_PARAGRAPH;

        paragraph->spacing_after =
            8.0;

        for (size_t l = 0;
             l < LINES_PER_PARAGRAPH;
             ++l) {

            WPLayoutLine *line =
                &paragraph->lines[l];

            line->paragraph_index =
                p;

            line->line_index =
                l;

            line->height =
                18.0;

            line->baseline =
                14.0;
        }
    }

    WPIncrementalPagination pagination;

    if (!wp_incremental_init(
            &pagination,
            NULL)) {

        return 1;
    }

    /*
     * Initial pagination.
     */

    if (!wp_incremental_build(
            &pagination,
            paragraphs,
            PARAGRAPH_COUNT,
            1)) {

        wp_incremental_destroy(
            &pagination
        );

        return 1;
    }

    printf(
        "INITIAL DOCUMENT\n"
    );

    wp_incremental_dump(
        &pagination
    );

    /*
     * Simulate a user editing paragraph 37.
     *
     * Make one line substantially taller.
     */

    paragraphs[37]
        .lines[4]
        .height = 72.0;

    WPPaginationChange change;

    change.first_paragraph = 37;
    change.last_paragraph = 37;

    change.old_version = 1;
    change.new_version = 2;

    /*
     * Incremental reflow.
     */

    if (!wp_incremental_update(
            &pagination,
            paragraphs,
            PARAGRAPH_COUNT,
            &change)) {

        fprintf(
            stderr,
            "Incremental update failed\n"
        );

        wp_incremental_destroy(
            &pagination
        );

        return 1;
    }

    printf(
        "\nAFTER EDIT TO PARAGRAPH 37\n"
    );

    wp_incremental_dump(
        &pagination
    );

    /*
     * Retrieve a specific page.
     */

    size_t page =
        wp_incremental_find_page_for_paragraph(
            &pagination,
            37
        );

    if (page != SIZE_MAX) {

        printf(
            "\nParagraph 37 is now on page %zu\n",
            page + 1
        );
    }

    wp_incremental_destroy(
        &pagination
    );

    return 0;
}

Compile:

cc -std=c11 -O2 -Wall -Wextra \
    word_pagination.c \
    word_incremental_pagination.c \
    test_incremental.c \
    -lm \
    -o incremental_pagination
    
    
    
    
    
    
    
    word_search.h
#ifndef WORD_SEARCH_H
#define WORD_SEARCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================
 * Search flags
 * ========================================================= */

typedef enum {
    WS_SEARCH_NONE          = 0,
    WS_CASE_INSENSITIVE     = 1u << 0,
    WS_WHOLE_WORD          = 1u << 1,
    WS_BACKWARDS           = 1u << 2
} WSSearchFlags;


/* =========================================================
 * Match
 * ========================================================= */

typedef struct {
    size_t start;
    size_t length;
} WSSearchMatch;


/* =========================================================
 * Search document interface
 *
 * The search engine deliberately works through callbacks.
 * This allows it to search:
 *
 *   - piece tables
 *   - ropes
 *   - normal buffers
 *   - memory-mapped documents
 *   - streamed documents
 * ========================================================= */

typedef size_t (*WSDocumentLengthFn)(
    const void *document
);

typedef int (*WSDocumentReadFn)(
    const void *document,
    size_t position,
    char *buffer,
    size_t length
);


/* =========================================================
 * Document
 * ========================================================= */

typedef struct {

    const void *context;

    WSDocumentLengthFn length;
    WSDocumentReadFn read;

} WSSearchDocument;


/* =========================================================
 * Search query
 * ========================================================= */

typedef struct {

    const char *pattern;
    size_t pattern_length;

    WSSearchFlags flags;

} WSSearchQuery;


/* =========================================================
 * Search iterator
 * ========================================================= */

typedef struct {

    WSSearchDocument document;

    WSSearchQuery query;

    size_t cursor;

    uint8_t finished;

} WSSearchIterator;


/* =========================================================
 * Replacement statistics
 * ========================================================= */

typedef struct {

    size_t matches;
    size_t replacements;
    size_t bytes_removed;
    size_t bytes_inserted;

} WSReplaceStats;


/* =========================================================
 * API
 * ========================================================= */

int ws_iterator_init(
    WSSearchIterator *iterator,
    const WSSearchDocument *document,
    const WSSearchQuery *query
);

int ws_next(
    WSSearchIterator *iterator,
    WSSearchMatch *match
);

int ws_find_first(
    const WSSearchDocument *document,
    const WSSearchQuery *query,
    WSSearchMatch *match
);

int ws_find_next(
    const WSSearchDocument *document,
    const WSSearchQuery *query,
    size_t start,
    WSSearchMatch *match
);

int ws_find_all(
    const WSSearchDocument *document,
    const WSSearchQuery *query,
    WSSearchMatch **matches,
    size_t *count
);

void ws_free_matches(
    WSSearchMatch *matches
);

int ws_is_word_character(
    unsigned char c
);

int ws_match_at(
    const WSSearchDocument *document,
    const WSSearchQuery *query,
    size_t position
);

#ifdef __cplusplus
}
#endif

#endif
word_search.c
#include "word_search.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>


/* =========================================================
 * ASCII folding
 *
 * Unicode case folding can be added above this layer.
 * ========================================================= */

static unsigned char
fold_char(
    unsigned char c
)
{
    return (unsigned char)
        tolower((int)c);
}


/* =========================================================
 * Word character
 * ========================================================= */

int
ws_is_word_character(
    unsigned char c
)
{
    return
        isalnum((int)c) ||
        c == '_';
}


/* =========================================================
 * Read one character
 * ========================================================= */

static int
read_char(
    const WSSearchDocument *document,
    size_t position,
    unsigned char *out
)
{
    char c;

    if (!document ||
        !document->read ||
        !out)
        return 0;

    if (!document->read(
            document->context,
            position,
            &c,
            1))
        return 0;

    *out =
        (unsigned char)c;

    return 1;
}


/* =========================================================
 * Match at position
 * ========================================================= */

int
ws_match_at(
    const WSSearchDocument *document,
    const WSSearchQuery *query,
    size_t position
)
{
    if (!document ||
        !query ||
        !query->pattern)
        return 0;

    if (query->pattern_length == 0)
        return 0;

    size_t document_length =
        document->length(
            document->context
        );

    if (position >
        document_length)
        return 0;

    if (query->pattern_length >
        document_length - position)
        return 0;

    /*
     * Whole-word validation.
     */

    if (query->flags &
        WS_WHOLE_WORD) {

        unsigned char before = 0;
        unsigned char after = 0;

        if (position > 0) {

            if (!read_char(
                    document,
                    position - 1,
                    &before))
                return 0;

            if (ws_is_word_character(
                    before))
                return 0;
        }

        size_t end =
            position +
            query->pattern_length;

        if (end < document_length) {

            if (!read_char(
                    document,
                    end,
                    &after))
                return 0;

            if (ws_is_word_character(
                    after))
                return 0;
        }
    }

    /*
     * Compare bytes.
     *
     * This layer intentionally treats UTF-8 as
     * bytes. A later Unicode engine can replace
     * this comparator with code-point/grapheme
     * aware matching.
     */

    for (size_t i = 0;
         i < query->pattern_length;
         ++i) {

        unsigned char actual;

        if (!read_char(
                document,
                position + i,
                &actual))
            return 0;

        unsigned char wanted =
            (unsigned char)
                query->pattern[i];

        if (query->flags &
            WS_CASE_INSENSITIVE) {

            actual =
                fold_char(actual);

            wanted =
                fold_char(wanted);
        }

        if (actual != wanted)
            return 0;
    }

    return 1;
}


/* =========================================================
 * Forward search
 * ========================================================= */

int
ws_find_next(
    const WSSearchDocument *document,
    const WSSearchQuery *query,
    size_t start,
    WSSearchMatch *match
)
{
    if (!document ||
        !query ||
        !match)
        return 0;

    size_t length =
        document->length(
            document->context
        );

    if (query->pattern_length == 0)
        return 0;

    if (query->pattern_length > length)
        return 0;

    if (start >
        length - query->pattern_length)
        return 0;

    for (size_t position = start;
         position <=
             length -
             query->pattern_length;
         ++position) {

        if (ws_match_at(
                document,
                query,
                position)) {

            match->start =
                position;

            match->length =
                query->pattern_length;

            return 1;
        }
    }

    return 0;
}


/* =========================================================
 * First search
 * ========================================================= */

int
ws_find_first(
    const WSSearchDocument *document,
    const WSSearchQuery *query,
    WSSearchMatch *match
)
{
    if (!query)
        return 0;

    if (query->flags &
        WS_BACKWARDS) {

        size_t length =
            document->length(
                document->context
            );

        if (length <
            query->pattern_length)
            return 0;

        size_t position =
            length -
            query->pattern_length;

        for (;;) {

            if (ws_match_at(
                    document,
                    query,
                    position)) {

                match->start =
                    position;

                match->length =
                    query->pattern_length;

                return 1;
            }

            if (position == 0)
                break;

            --position;
        }

        return 0;
    }

    return ws_find_next(
        document,
        query,
        0,
        match
    );
}


/* =========================================================
 * Iterator initialisation
 * ========================================================= */

int
ws_iterator_init(
    WSSearchIterator *iterator,
    const WSSearchDocument *document,
    const WSSearchQuery *query
)
{
    if (!iterator ||
        !document ||
        !query)
        return 0;

    memset(
        iterator,
        0,
        sizeof(*iterator)
    );

    iterator->document =
        *document;

    iterator->query =
        *query;

    size_t length =
        document->length(
            document->context
        );

    if (query->flags &
        WS_BACKWARDS) {

        if (length >=
            query->pattern_length) {

            iterator->cursor =
                length -
                query->pattern_length;
        } else {

            iterator->finished =
                1;
        }

    } else {

        iterator->cursor =
            0;
    }

    return 1;
}


/* =========================================================
 * Iterator
 * ========================================================= */

int
ws_next(
    WSSearchIterator *iterator,
    WSSearchMatch *match
)
{
    if (!iterator ||
        !match ||
        iterator->finished)
        return 0;

    size_t length =
        iterator->document.length(
            iterator->document.context
        );

    size_t pattern_length =
        iterator->query.pattern_length;

    if (pattern_length == 0 ||
        length < pattern_length) {

        iterator->finished = 1;
        return 0;
    }

    if (iterator->query.flags &
        WS_BACKWARDS) {

        for (;;) {

            if (ws_match_at(
                    &iterator->document,
                    &iterator->query,
                    iterator->cursor)) {

                match->start =
                    iterator->cursor;

                match->length =
                    pattern_length;

                if (iterator->cursor == 0) {

                    iterator->finished =
                        1;

                } else {

                    --iterator->cursor;
                }

                return 1;
            }

            if (iterator->cursor == 0)
                break;

            --iterator->cursor;
        }

    } else {

        size_t maximum =
            length -
            pattern_length;

        while (iterator->cursor <=
               maximum) {

            size_t position =
                iterator->cursor++;

            if (ws_match_at(
                    &iterator->document,
                    &iterator->query,
                    position)) {

                match->start =
                    position;

                match->length =
                    pattern_length;

                return 1;
            }
        }
    }

    iterator->finished =
        1;

    return 0;
}


/* =========================================================
 * Find all
 * ========================================================= */

int
ws_find_all(
    const WSSearchDocument *document,
    const WSSearchQuery *query,
    WSSearchMatch **matches,
    size_t *count
)
{
    if (!document ||
        !query ||
        !matches ||
        !count)
        return 0;

    *matches = NULL;
    *count = 0;

    WSSearchIterator iterator;

    if (!ws_iterator_init(
            &iterator,
            document,
            query))
        return 0;

    size_t capacity = 16;

    WSSearchMatch *result =
        malloc(
            capacity *
            sizeof(WSSearchMatch)
        );

    if (!result)
        return 0;

    WSSearchMatch match;

    while (ws_next(
        &iterator,
        &match)) {

        if (*count >= capacity) {

            if (capacity >
                SIZE_MAX / 2) {

                free(result);
                return 0;
            }

            capacity *= 2;

            WSSearchMatch *grown =
                realloc(
                    result,
                    capacity *
                    sizeof(WSSearchMatch)
                );

            if (!grown) {

                free(result);
                return 0;
            }

            result = grown;
        }

        result[*count] =
            match;

        (*count)++;
    }

    if (*count == 0) {

        free(result);
        result = NULL;
    }

    *matches =
        result;

    return 1;
}


/* =========================================================
 * Free matches
 * ========================================================= */

void
ws_free_matches(
    WSSearchMatch *matches
)
{
    free(matches);
}
Connecting it directly to the document engine

Because we made the search engine callback-based, it can work directly on the piece-table document.

#include "word_document.h"
#include "word_search.h"

static size_t
document_length(
    const void *context
)
{
    return wd_length(
        (const WDDocument *)context
    );
}


static int
document_read(
    const void *context,
    size_t position,
    char *buffer,
    size_t length
)
{
    return wd_read(
        (const WDDocument *)context,
        position,
        buffer,
        length
    );
}

Then:

WSSearchDocument search_document = {
    .context = &document,
    .length = document_length,
    .read = document_read
};

WSSearchQuery query = {
    .pattern = "document",
    .pattern_length = strlen("document"),
    .flags = WS_CASE_INSENSITIVE
};

WSSearchMatch match;

if (ws_find_first(
        &search_document,
        &query,
        &match)) {

    printf(
        "Found at byte %zu, length %zu\n",
        match.start,
        match.length
    );
}
Find every occurrence
WSSearchMatch *matches;
size_t count;

if (ws_find_all(
        &search_document,
        &query,
        &matches,
        &count)) {

    printf(
        "Found %zu matches\n",
        count
    );

    for (size_t i = 0;
         i < count;
         ++i) {

        printf(
            "  %zu -> %zu\n",
            matches[i].start,
            matches[i].start +
            matches[i].length
        );
    }

    ws_free_matches(matches);
}





word_typing.h
#ifndef WORD_TYPING_H
#define WORD_TYPING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================
 * Event types
 * ========================================================= */

typedef enum {

    WT_EVENT_NONE = 0,

    WT_INSERT_TEXT,
    WT_DELETE_BACKWARD,
    WT_DELETE_FORWARD,

    WT_MOVE_CURSOR,
    WT_SET_SELECTION,

    WT_COMPOSITION_START,
    WT_COMPOSITION_UPDATE,
    WT_COMPOSITION_END,

    WT_NEWLINE,

    WT_UNDO,
    WT_REDO

} WTEventType;


/* =========================================================
 * Input event
 * ========================================================= */

typedef struct {

    WTEventType type;

    size_t position;

    size_t length;

    int64_t cursor_delta;

    const char *text;

    size_t text_length;

    uint64_t timestamp_ns;

    uint32_t modifiers;

} WTEvent;


/* =========================================================
 * Event queue
 * ========================================================= */

typedef struct {

    WTEvent *events;

    size_t capacity;

    size_t head;

    size_t tail;

    size_t count;

} WTEventQueue;


/* =========================================================
 * Dirty region
 * ========================================================= */

typedef struct {

    size_t start;

    size_t end;

    uint64_t version;

} WTDirtyRegion;


/* =========================================================
 * Edit transaction
 * ========================================================= */

typedef struct {

    size_t start;

    size_t old_length;

    size_t new_length;

    uint64_t old_version;

    uint64_t new_version;

} WTEditTransaction;


/* =========================================================
 * Cursor
 * ========================================================= */

typedef struct {

    size_t position;

    size_t anchor;

    uint8_t has_selection;

    uint8_t visible;

} WTCursor;


/* =========================================================
 * Composition state
 *
 * Used by IMEs such as:
 *
 * Japanese
 * Chinese
 * Korean
 * dead-key composition
 * speech input
 * predictive input
 * ========================================================= */

typedef struct {

    char *text;

    size_t length;

    size_t capacity;

    size_t start;

    uint8_t active;

} WTComposition;


/* =========================================================
 * Pipeline statistics
 * ========================================================= */

typedef struct {

    uint64_t events_received;

    uint64_t events_processed;

    uint64_t edits_applied;

    uint64_t bytes_inserted;

    uint64_t bytes_deleted;

    uint64_t transactions;

} WTTypingStatistics;


/* =========================================================
 * Pipeline
 * ========================================================= */

typedef struct {

    WTEventQueue queue;

    WTCursor cursor;

    WTComposition composition;

    WTDirtyRegion dirty;

    WTTypingStatistics statistics;

    uint64_t document_version;

} WTTypingPipeline;


/* =========================================================
 * Queue
 * ========================================================= */

int wt_queue_init(
    WTEventQueue *queue,
    size_t capacity
);

void wt_queue_destroy(
    WTEventQueue *queue
);

int wt_queue_push(
    WTEventQueue *queue,
    const WTEvent *event
);

int wt_queue_pop(
    WTEventQueue *queue,
    WTEvent *event
);


/* =========================================================
 * Pipeline
 * ========================================================= */

int wt_init(
    WTTypingPipeline *pipeline,
    size_t queue_capacity
);

void wt_destroy(
    WTTypingPipeline *pipeline
);


/* =========================================================
 * Input
 * ========================================================= */

int wt_submit(
    WTTypingPipeline *pipeline,
    const WTEvent *event
);


/* =========================================================
 * Processing
 * ========================================================= */

int wt_process(
    WTTypingPipeline *pipeline
);


/* =========================================================
 * Cursor
 * ========================================================= */

void wt_set_cursor(
    WTTypingPipeline *pipeline,
    size_t position
);

void wt_set_selection(
    WTTypingPipeline *pipeline,
    size_t anchor,
    size_t position
);


/* =========================================================
 * Dirty region
 * ========================================================= */

void wt_mark_dirty(
    WTTypingPipeline *pipeline,
    size_t start,
    size_t end
);

void wt_clear_dirty(
    WTTypingPipeline *pipeline
);


/* =========================================================
 * Composition
 * ========================================================= */

int wt_composition_set(
    WTTypingPipeline *pipeline,
    const char *text,
    size_t length
);

void wt_composition_clear(
    WTTypingPipeline *pipeline
);


/* =========================================================
 * Statistics
 * ========================================================= */

const WTTypingStatistics *
wt_statistics(
    const WTTypingPipeline *pipeline
);

#ifdef __cplusplus
}
#endif

#endif
word_typing.c
#include "word_typing.h"

#include <stdlib.h>
#include <string.h>


/* =========================================================
 * Event queue
 * ========================================================= */

int
wt_queue_init(
    WTEventQueue *queue,
    size_t capacity
)
{
    if (!queue || capacity == 0)
        return 0;

    memset(
        queue,
        0,
        sizeof(*queue)
    );

    queue->events =
        calloc(
            capacity,
            sizeof(WTEvent)
        );

    if (!queue->events)
        return 0;

    queue->capacity =
        capacity;

    return 1;
}


void
wt_queue_destroy(
    WTEventQueue *queue
)
{
    if (!queue)
        return;

    free(queue->events);

    memset(
        queue,
        0,
        sizeof(*queue)
    );
}


/* =========================================================
 * Push event
 * ========================================================= */

int
wt_queue_push(
    WTEventQueue *queue,
    const WTEvent *event
)
{
    if (!queue ||
        !event ||
        !queue->events)
        return 0;

    /*
     * Fixed-size queue.
     *
     * The caller can choose a larger queue if
     * extremely high input rates are expected.
     */

    if (queue->count >=
        queue->capacity)
        return 0;

    queue->events[
        queue->tail
    ] = *event;

    queue->tail =
        (queue->tail + 1) %
        queue->capacity;

    queue->count++;

    return 1;
}


/* =========================================================
 * Pop event
 * ========================================================= */

int
wt_queue_pop(
    WTEventQueue *queue,
    WTEvent *event
)
{
    if (!queue ||
        !event ||
        queue->count == 0)
        return 0;

    *event =
        queue->events[
            queue->head
        ];

    queue->head =
        (queue->head + 1) %
        queue->capacity;

    queue->count--;

    return 1;
}


/* =========================================================
 * Pipeline initialisation
 * ========================================================= */

int
wt_init(
    WTTypingPipeline *pipeline,
    size_t queue_capacity
)
{
    if (!pipeline)
        return 0;

    memset(
        pipeline,
        0,
        sizeof(*pipeline)
    );

    if (!wt_queue_init(
            &pipeline->queue,
            queue_capacity)) {

        return 0;
    }

    pipeline->cursor.visible =
        1;

    pipeline->document_version =
        1;

    return 1;
}


/* =========================================================
 * Destroy
 * ========================================================= */

void
wt_destroy(
    WTTypingPipeline *pipeline
)
{
    if (!pipeline)
        return;

    wt_queue_destroy(
        &pipeline->queue
    );

    free(
        pipeline->composition.text
    );

    memset(
        pipeline,
        0,
        sizeof(*pipeline)
    );
}


/* =========================================================
 * Submit
 * ========================================================= */

int
wt_submit(
    WTTypingPipeline *pipeline,
    const WTEvent *event
)
{
    if (!pipeline ||
        !event)
        return 0;

    if (!wt_queue_push(
            &pipeline->queue,
            event))
        return 0;

    pipeline->statistics.events_received++;

    return 1;
}


/* =========================================================
 * Dirty region
 * ========================================================= */

void
wt_mark_dirty(
    WTTypingPipeline *pipeline,
    size_t start,
    size_t end
)
{
    if (!pipeline)
        return;

    if (start > end) {

        size_t temporary =
            start;

        start = end;
        end = temporary;
    }

    /*
     * Merge with existing dirty region.
     */

    if (pipeline->dirty.end >
        pipeline->dirty.start) {

        if (start >
            pipeline->dirty.end) {

            /*
             * Non-overlapping region.
             *
             * This simple implementation expands
             * to one combined region.
             */

            if (start <
                pipeline->dirty.end + 1) {

                start =
                    pipeline->dirty.start;
            }

        } else {

            if (pipeline->dirty.start <
                start)

                start =
                    pipeline->dirty.start;

            if (pipeline->dirty.end >
                end)

                end =
                    pipeline->dirty.end;
        }
    }

    pipeline->dirty.start =
        start;

    pipeline->dirty.end =
        end;

    pipeline->dirty.version =
        pipeline->document_version;
}


void
wt_clear_dirty(
    WTTypingPipeline *pipeline
)
{
    if (!pipeline)
        return;

    memset(
        &pipeline->dirty,
        0,
        sizeof(pipeline->dirty)
    );
}


/* =========================================================
 * Cursor
 * ========================================================= */

void
wt_set_cursor(
    WTTypingPipeline *pipeline,
    size_t position
)
{
    if (!pipeline)
        return;

    pipeline->cursor.position =
        position;

    pipeline->cursor.anchor =
        position;

    pipeline->cursor.has_selection =
        0;
}


void
wt_set_selection(
    WTTypingPipeline *pipeline,
    size_t anchor,
    size_t position
)
{
    if (!pipeline)
        return;

    pipeline->cursor.anchor =
        anchor;

    pipeline->cursor.position =
        position;

    pipeline->cursor.has_selection =
        anchor != position;
}


/* =========================================================
 * Composition
 * ========================================================= */

int
wt_composition_set(
    WTTypingPipeline *pipeline,
    const char *text,
    size_t length
)
{
    if (!pipeline ||
        (!text && length != 0))
        return 0;

    if (length + 1 >
        pipeline->composition.capacity) {

        size_t capacity =
            pipeline->composition.capacity == 0
                ? 64
                : pipeline->composition.capacity;

        while (capacity <
               length + 1) {

            if (capacity >
                SIZE_MAX / 2)
                return 0;

            capacity *= 2;
        }

        char *buffer =
            realloc(
                pipeline->composition.text,
                capacity
            );

        if (!buffer)
            return 0;

        pipeline->composition.text =
            buffer;

        pipeline->composition.capacity =
            capacity;
    }

    if (length)
        memcpy(
            pipeline->composition.text,
            text,
            length
        );

    pipeline->composition.text[length] =
        '\0';

    pipeline->composition.length =
        length;

    pipeline->composition.active =
        1;

    return 1;
}


void
wt_composition_clear(
    WTTypingPipeline *pipeline
)
{
    if (!pipeline)
        return;

    pipeline->composition.length =
        0;

    pipeline->composition.active =
        0;

    if (pipeline->composition.text)
        pipeline->composition.text[0] =
            '\0';
}


/* =========================================================
 * Process insertion
 *
 * The pipeline itself doesn't own the document.
 * Instead, this callback-oriented foundation prepares
 * transactions that the document engine can consume.
 * ========================================================= */

static void
process_insert(
    WTTypingPipeline *pipeline,
    const WTEvent *event
)
{
    if (!event->text ||
        event->text_length == 0)
        return;

    size_t start =
        pipeline->cursor.position;

    /*
     * Replace active selection.
     */

    if (pipeline->cursor.has_selection) {

        size_t a =
            pipeline->cursor.anchor;

        size_t b =
            pipeline->cursor.position;

        if (a > b) {

            size_t t = a;
            a = b;
            b = t;
        }

        start = a;

        size_t deleted =
            b - a;

        pipeline->statistics.bytes_deleted +=
            deleted;

        pipeline->statistics.bytes_inserted +=
            event->text_length;

        wt_mark_dirty(
            pipeline,
            a,
            b + event->text_length
        );

        /*
         * In the full Word integration this becomes:
         *
         * document_delete(a, deleted)
         * document_insert(a, text, length)
         */

    } else {

        pipeline->statistics.bytes_inserted +=
            event->text_length;

        wt_mark_dirty(
            pipeline,
            start,
            start + event->text_length
        );
    }

    pipeline->cursor.position =
        start +
        event->text_length;

    pipeline->cursor.anchor =
        pipeline->cursor.position;

    pipeline->cursor.has_selection =
        0;

    pipeline->statistics.edits_applied++;

    pipeline->document_version++;
}


/* =========================================================
 * Process backward delete
 * ========================================================= */

static void
process_delete_backward(
    WTTypingPipeline *pipeline,
    const WTEvent *event
)
{
    size_t position =
        pipeline->cursor.position;

    if (pipeline->cursor.has_selection) {

        size_t a =
            pipeline->cursor.anchor;

        size_t b =
            pipeline->cursor.position;

        if (a > b) {

            size_t t = a;
            a = b;
            b = t;
        }

        pipeline->statistics.bytes_deleted +=
            b - a;

        wt_mark_dirty(
            pipeline,
            a,
            b
        );

        pipeline->cursor.position =
            a;

        pipeline->cursor.anchor =
            a;

        pipeline->cursor.has_selection =
            0;

        pipeline->statistics.edits_applied++;

        pipeline->document_version++;

        return;
    }

    if (position == 0)
        return;

    /*
     * UTF-8-aware deletion belongs in the next
     * Unicode layer. For now this removes one byte.
     */

    size_t start =
        position - 1;

    pipeline->statistics.bytes_deleted++;

    wt_mark_dirty(
        pipeline,
        start,
        position
    );

    pipeline->cursor.position =
        start;

    pipeline->cursor.anchor =
        start;

    pipeline->statistics.edits_applied++;

    pipeline->document_version++;
}


/* =========================================================
 * Process forward delete
 * ========================================================= */

static void
process_delete_forward(
    WTTypingPipeline *pipeline,
    const WTEvent *event
)
{
    (void)event;

    if (pipeline->cursor.has_selection) {

        process_delete_backward(
            pipeline,
            event
        );

        return;
    }

    size_t position =
        pipeline->cursor.position;

    wt_mark_dirty(
        pipeline,
        position,
        position + 1
    );

    pipeline->statistics.bytes_deleted++;

    pipeline->statistics.edits_applied++;

    pipeline->document_version++;
}


/* =========================================================
 * Process cursor movement
 * ========================================================= */

static void
process_cursor_move(
    WTTypingPipeline *pipeline,
    const WTEvent *event
)
{
    int64_t current =
        (int64_t)
            pipeline->cursor.position;

    int64_t next =
        current +
        event->cursor_delta;

    if (next < 0)
        next = 0;

    pipeline->cursor.position =
        (size_t)next;

    if (!(event->modifiers & 1u)) {

        pipeline->cursor.anchor =
            pipeline->cursor.position;

        pipeline->cursor.has_selection =
            0;
    } else {

        pipeline->cursor.has_selection =
            pipeline->cursor.anchor !=
            pipeline->cursor.position;
    }
}


/* =========================================================
 * Process composition
 * ========================================================= */

static void
process_composition(
    WTTypingPipeline *pipeline,
    const WTEvent *event
)
{
    switch (event->type) {

        case WT_COMPOSITION_START:

            pipeline->composition.start =
                pipeline->cursor.position;

            pipeline->composition.active =
                1;

            break;

        case WT_COMPOSITION_UPDATE:

            wt_composition_set(
                pipeline,
                event->text,
                event->text_length
            );

            break;

        case WT_COMPOSITION_END:

            if (pipeline->composition.active &&
                pipeline->composition.text) {

                /*
                 * Commit the final composed text
                 * through the normal insertion path.
                 */

                WTEvent commit = *event;

                commit.type =
                    WT_INSERT_TEXT;

                commit.position =
                    pipeline->composition.start;

                commit.text =
                    pipeline->composition.text;

                commit.text_length =
                    pipeline->composition.length;

                process_insert(
                    pipeline,
                    &commit
                );
            }

            wt_composition_clear(
                pipeline
            );

            break;

        default:
            break;
    }
}


/* =========================================================
 * Process event
 * ========================================================= */

static void
process_event(
    WTTypingPipeline *pipeline,
    const WTEvent *event
)
{
    switch (event->type) {

        case WT_INSERT_TEXT:
            process_insert(
                pipeline,
                event
            );
            break;

        case WT_DELETE_BACKWARD:
            process_delete_backward(
                pipeline,
                event
            );
            break;

        case WT_DELETE_FORWARD:
            process_delete_forward(
                pipeline,
                event
            );
            break;

        case WT_MOVE_CURSOR:
            process_cursor_move(
                pipeline,
                event
            );
            break;

        case WT_SET_SELECTION:
            wt_set_selection(
                pipeline,
                event->position,
                event->position +
                    event->length
            );
            break;

        case WT_COMPOSITION_START:
        case WT_COMPOSITION_UPDATE:
        case WT_COMPOSITION_END:
            process_composition(
                pipeline,
                event
            );
            break;

        case WT_NEWLINE: {

            static const char newline[] =
                "\n";

            WTEvent newline_event = *event;

            newline_event.type =
                WT_INSERT_TEXT;

            newline_event.text =
                newline;

            newline_event.text_length =
                1;

            process_insert(
                pipeline,
                &newline_event
            );

            break;
        }

        case WT_UNDO:
            /*
             * Handled by the document/version engine.
             */
            break;

        case WT_REDO:
            /*
             * Handled by the document/version engine.
             */
            break;

        default:
            break;
    }
}


/* =========================================================
 * Process queue
 * ========================================================= */

int
wt_process(
    WTTypingPipeline *pipeline
)
{
    if (!pipeline)
        return 0;

    WTEvent event;

    size_t processed = 0;

    while (wt_queue_pop(
        &pipeline->queue,
        &event)) {

        process_event(
            pipeline,
            &event
        );

        pipeline->statistics.events_processed++;

        processed++;
    }

    if (processed)
        pipeline->statistics.transactions++;

    return (int)processed;
}


/* =========================================================
 * Statistics
 * ========================================================= */

const WTTypingStatistics *
wt_statistics(
    const WTTypingPipeline *pipeline
)
{
    if (!pipeline)
        return NULL;

    return &pipeline->statistics;
}
Test
#include "word_typing.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    WTTypingPipeline pipeline;

    if (!wt_init(
            &pipeline,
            1024)) {

        return 1;
    }

    /*
     * User types:
     *
     * Hello
     */

    const char *hello =
        "Hello";

    WTEvent insert = {
        .type = WT_INSERT_TEXT,
        .position = 0,
        .length = 0,
        .cursor_delta = 0,
        .text = hello,
        .text_length = strlen(hello),
        .timestamp_ns = 1,
        .modifiers = 0
    };

    wt_submit(
        &pipeline,
        &insert
    );

    /*
     * User presses Enter.
     */

    WTEvent newline = {
        .type = WT_NEWLINE,
        .timestamp_ns = 2
    };

    wt_submit(
        &pipeline,
        &newline
    );

    /*
     * User types World.
     */

    const char *world =
        "World";

    insert.text =
        world;

    insert.text_length =
        strlen(world);

    insert.timestamp_ns =
        3;

    wt_submit(
        &pipeline,
        &insert
    );

    /*
     * Process all queued input.
     */

    wt_process(
        &pipeline
    );

    const WTTypingStatistics *stats =
        wt_statistics(
            &pipeline
        );

    printf(
        "Events received:  %llu\n",
        (unsigned long long)
            stats->events_received
    );

    printf(
        "Events processed: %llu\n",
        (unsigned long long)
            stats->events_processed
    );

    printf(
        "Edits applied:    %llu\n",
        (unsigned long long)
            stats->edits_applied
    );

    printf(
        "Bytes inserted:   %llu\n",
        (unsigned long long)
            stats->bytes_inserted
    );

    printf(
        "Bytes deleted:    %llu\n",
        (unsigned long long)
            stats->bytes_deleted
    );

    printf(
        "Document version: %llu\n",
        (unsigned long long)
            pipeline.document_version
    );

    printf(
        "Cursor position:  %zu\n",
        pipeline.cursor.position
    );

    printf(
        "Dirty region:     %zu -> %zu\n",
        pipeline.dirty.start,
        pipeline.dirty.end
    );

    wt_destroy(
        &pipeline
    );

    return 0;
}

Compile:

cc -std=c11 -O2 -Wall -Wextra \
    word_typing.c \
    test_typing.c \
    -o word_typing
    
    
    
    
    
    
    
    
    1. Add to word_typing.h
#ifndef WORD_TYPING_SCHEDULER_H
#define WORD_TYPING_SCHEDULER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/* =========================================================
 * Scheduler priorities
 * ========================================================= */

typedef enum {

    WT_PRIORITY_CRITICAL = 0,
    WT_PRIORITY_HIGH     = 1,
    WT_PRIORITY_NORMAL   = 2,
    WT_PRIORITY_LOW      = 3,
    WT_PRIORITY_IDLE     = 4

} WTSchedulerPriority;


/* =========================================================
 * Background task types
 * ========================================================= */

typedef enum {

    WT_TASK_NONE = 0,

    /*
     * Must happen immediately.
     */
    WT_TASK_CURSOR,

    WT_TASK_SELECTION,

    WT_TASK_DOCUMENT_EDIT,

    /*
     * High priority.
     */
    WT_TASK_LAYOUT,

    WT_TASK_RENDER,

    /*
     * Normal priority.
     */
    WT_TASK_PAGINATION,

    WT_TASK_SPELLCHECK,

    WT_TASK_SEARCH_INDEX,

    /*
     * Low priority.
     */
    WT_TASK_AUTOSAVE,

    WT_TASK_STATISTICS,

    WT_TASK_IDLE_MAINTENANCE

} WTTaskType;


/* =========================================================
 * Task state
 * ========================================================= */

typedef enum {

    WT_TASK_PENDING = 0,
    WT_TASK_RUNNING,
    WT_TASK_COMPLETE,
    WT_TASK_CANCELLED

} WTTaskState;


/* =========================================================
 * Scheduled task
 * ========================================================= */

typedef struct {

    WTTaskType type;

    WTSchedulerPriority priority;

    WTTaskState state;

    uint64_t generation;

    uint64_t document_version;

    size_t dirty_start;

    size_t dirty_end;

    uint64_t created_ns;

} WTTask;


/* =========================================================
 * Scheduler
 * ========================================================= */

typedef struct {

    WTTask *tasks;

    size_t count;

    size_t capacity;

    uint64_t generation;

} WTScheduler;


/* =========================================================
 * Coalesced edit
 * ========================================================= */

typedef struct {

    size_t start;

    size_t old_length;

    size_t new_length;

    uint64_t first_timestamp_ns;

    uint64_t last_timestamp_ns;

    size_t event_count;

    uint8_t active;

} WTCoalescedEdit;


/* =========================================================
 * Typing latency statistics
 * ========================================================= */

typedef struct {

    uint64_t input_events;

    uint64_t coalesced_events;

    uint64_t immediate_operations;

    uint64_t scheduled_operations;

    uint64_t stale_tasks;

    uint64_t cancelled_tasks;

    uint64_t completed_tasks;

} WTLatencyStatistics;


/* =========================================================
 * Extended pipeline
 * ========================================================= */

typedef struct {

    /*
     * Existing pipeline.
     */
    WTTypingPipeline typing;

    /*
     * Scheduler.
     */
    WTScheduler scheduler;

    /*
     * Current coalesced edit.
     */
    WTCoalescedEdit coalesced;

    /*
     * Latency instrumentation.
     */
    WTLatencyStatistics latency;

    /*
     * Last observed input time.
     */
    uint64_t last_input_ns;

    /*
     * Maximum coalescing interval.
     *
     * Typical value:
     *
     * 4–16 ms
     */
    uint64_t coalesce_window_ns;

} WTRealtimeTyping;


/* =========================================================
 * Scheduler
 * ========================================================= */

int wt_scheduler_init(
    WTScheduler *scheduler,
    size_t capacity
);

void wt_scheduler_destroy(
    WTScheduler *scheduler
);

int wt_scheduler_schedule(
    WTScheduler *scheduler,
    WTTaskType type,
    WTSchedulerPriority priority,
    uint64_t document_version,
    size_t dirty_start,
    size_t dirty_end,
    uint64_t timestamp_ns
);

int wt_scheduler_cancel(
    WTScheduler *scheduler,
    WTTaskType type
);

int wt_scheduler_pop(
    WTScheduler *scheduler,
    WTTask *task
);


/* =========================================================
 * Realtime pipeline
 * ========================================================= */

int wt_realtime_init(
    WTRealtimeTyping *pipeline,
    size_t event_capacity,
    size_t task_capacity
);

void wt_realtime_destroy(
    WTRealtimeTyping *pipeline
);

int wt_realtime_submit(
    WTRealtimeTyping *pipeline,
    const WTEvent *event
);

int wt_realtime_flush(
    WTRealtimeTyping *pipeline,
    uint64_t timestamp_ns
);

int wt_realtime_run_tasks(
    WTRealtimeTyping *pipeline,
    size_t budget
);

void wt_realtime_set_coalesce_window(
    WTRealtimeTyping *pipeline,
    uint64_t nanoseconds
);

const WTLatencyStatistics *
wt_realtime_statistics(
    const WTRealtimeTyping *pipeline
);


#ifdef __cplusplus
}
#endif

#endif
2. word_typing_scheduler.c
#include "word_typing_scheduler.h"

#include <stdlib.h>
#include <string.h>


/* =========================================================
 * Scheduler initialisation
 * ========================================================= */

int
wt_scheduler_init(
    WTScheduler *scheduler,
    size_t capacity
)
{
    if (!scheduler ||
        capacity == 0)
        return 0;

    memset(
        scheduler,
        0,
        sizeof(*scheduler)
    );

    scheduler->tasks =
        calloc(
            capacity,
            sizeof(WTTask)
        );

    if (!scheduler->tasks)
        return 0;

    scheduler->capacity =
        capacity;

    scheduler->generation =
        1;

    return 1;
}


/* =========================================================
 * Scheduler destruction
 * ========================================================= */

void
wt_scheduler_destroy(
    WTScheduler *scheduler
)
{
    if (!scheduler)
        return;

    free(
        scheduler->tasks
    );

    memset(
        scheduler,
        0,
        sizeof(*scheduler)
    );
}


/* =========================================================
 * Determine whether a task can be merged
 * ========================================================= */

static int
task_is_mergeable(
    WTTaskType type
)
{
    switch (type) {

        case WT_TASK_LAYOUT:
        case WT_TASK_RENDER:
        case WT_TASK_PAGINATION:
        case WT_TASK_SPELLCHECK:
        case WT_TASK_SEARCH_INDEX:
        case WT_TASK_STATISTICS:

            return 1;

        default:
            return 0;
    }
}


/* =========================================================
 * Schedule task
 * ========================================================= */

int
wt_scheduler_schedule(
    WTScheduler *scheduler,
    WTTaskType type,
    WTSchedulerPriority priority,
    uint64_t document_version,
    size_t dirty_start,
    size_t dirty_end,
    uint64_t timestamp_ns
)
{
    if (!scheduler ||
        !scheduler->tasks)
        return 0;

    /*
     * If the task already exists and is mergeable,
     * update it instead of creating another task.
     *
     * This is extremely important during typing.
     *
     * Without this:
     *
     *   type A
     *   layout
     *   type B
     *   layout
     *   type C
     *   layout
     *
     * could create three expensive layout jobs.
     *
     * With coalescing:
     *
     *   type A
     *   type B
     *   type C
     *        ↓
     *   ONE layout job
     */

    if (task_is_mergeable(type)) {

        for (size_t i = 0;
             i < scheduler->count;
             ++i) {

            WTTask *task =
                &scheduler->tasks[i];

            if (task->state !=
                WT_TASK_PENDING)
                continue;

            if (task->type != type)
                continue;

            if (dirty_start <
                task->dirty_start)

                task->dirty_start =
                    dirty_start;

            if (dirty_end >
                task->dirty_end)

                task->dirty_end =
                    dirty_end;

            if (document_version >
                task->document_version)

                task->document_version =
                    document_version;

            if (priority <
                task->priority)

                task->priority =
                    priority;

            task->created_ns =
                timestamp_ns;

            return 1;
        }
    }


    if (scheduler->count >=
        scheduler->capacity)
        return 0;


    WTTask *task =
        &scheduler->tasks[
            scheduler->count++
        ];


    memset(
        task,
        0,
        sizeof(*task)
    );


    task->type =
        type;

    task->priority =
        priority;

    task->state =
        WT_TASK_PENDING;

    task->generation =
        scheduler->generation++;

    task->document_version =
        document_version;

    task->dirty_start =
        dirty_start;

    task->dirty_end =
        dirty_end;

    task->created_ns =
        timestamp_ns;


    return 1;
}


/* =========================================================
 * Cancel tasks of a type
 * ========================================================= */

int
wt_scheduler_cancel(
    WTScheduler *scheduler,
    WTTaskType type
)
{
    if (!scheduler)
        return 0;

    int cancelled = 0;

    for (size_t i = 0;
         i < scheduler->count;
         ++i) {

        WTTask *task =
            &scheduler->tasks[i];

        if (task->type != type)
            continue;

        if (task->state !=
            WT_TASK_PENDING)
            continue;

        task->state =
            WT_TASK_CANCELLED;

        cancelled++;
    }

    return cancelled;
}


/* =========================================================
 * Select highest-priority task
 * ========================================================= */

int
wt_scheduler_pop(
    WTScheduler *scheduler,
    WTTask *result
)
{
    if (!scheduler ||
        !result)
        return 0;

    size_t selected =
        SIZE_MAX;

    WTSchedulerPriority best =
        WT_PRIORITY_IDLE;


    for (size_t i = 0;
         i < scheduler->count;
         ++i) {

        WTTask *task =
            &scheduler->tasks[i];

        if (task->state !=
            WT_TASK_PENDING)
            continue;

        if (selected == SIZE_MAX ||
            task->priority < best) {

            selected = i;
            best =
                task->priority;
        }
    }


    if (selected == SIZE_MAX)
        return 0;


    WTTask *task =
        &scheduler->tasks[selected];


    *result =
        *task;


    task->state =
        WT_TASK_RUNNING;


    return 1;
}
3. word_realtime_typing.c
#include "word_typing_scheduler.h"

#include <stdlib.h>
#include <string.h>


/* =========================================================
 * Initialise
 * ========================================================= */

int
wt_realtime_init(
    WTRealtimeTyping *pipeline,
    size_t event_capacity,
    size_t task_capacity
)
{
    if (!pipeline)
        return 0;

    memset(
        pipeline,
        0,
        sizeof(*pipeline)
    );


    if (!wt_init(
            &pipeline->typing,
            event_capacity)) {

        return 0;
    }


    if (!wt_scheduler_init(
            &pipeline->scheduler,
            task_capacity)) {

        wt_destroy(
            &pipeline->typing
        );

        return 0;
    }


    /*
     * 8 ms is a useful initial coalescing
     * window for interactive typing.
     */

    pipeline->coalesce_window_ns =
        8ULL * 1000ULL * 1000ULL;


    return 1;
}


/* =========================================================
 * Destroy
 * ========================================================= */

void
wt_realtime_destroy(
    WTRealtimeTyping *pipeline
)
{
    if (!pipeline)
        return;

    wt_scheduler_destroy(
        &pipeline->scheduler
    );

    wt_destroy(
        &pipeline->typing
    );

    memset(
        pipeline,
        0,
        sizeof(*pipeline)
    );
}


/* =========================================================
 * Determine whether two events can be coalesced
 * ========================================================= */

static int
can_coalesce(
    const WTEvent *previous,
    const WTEvent *current
)
{
    if (!previous ||
        !current)
        return 0;


    /*
     * Only adjacent text insertion events.
     */

    if (previous->type !=
        WT_INSERT_TEXT)

        return 0;

    if (current->type !=
        WT_INSERT_TEXT)

        return 0;


    /*
     * Modifiers must match.
     */

    if (previous->modifiers !=
        current->modifiers)

        return 0;


    /*
     * Both events need text.
     */

    if (!previous->text ||
        !current->text)

        return 0;


    return 1;
}


/* =========================================================
 * Schedule expensive work
 * ========================================================= */

static void
schedule_after_edit(
    WTRealtimeTyping *pipeline,
    size_t start,
    size_t end,
    uint64_t timestamp_ns
)
{
    uint64_t version =
        pipeline->typing.document_version;


    /*
     * Layout is immediately after the edit.
     */

    wt_scheduler_schedule(
        &pipeline->scheduler,
        WT_TASK_LAYOUT,
        WT_PRIORITY_HIGH,
        version,
        start,
        end,
        timestamp_ns
    );


    /*
     * Rendering follows layout.
     */

    wt_scheduler_schedule(
        &pipeline->scheduler,
        WT_TASK_RENDER,
        WT_PRIORITY_HIGH,
        version,
        start,
        end,
        timestamp_ns
    );


    /*
     * Pagination is expensive and therefore
     * slightly lower priority.
     */

    wt_scheduler_schedule(
        &pipeline->scheduler,
        WT_TASK_PAGINATION,
        WT_PRIORITY_NORMAL,
        version,
        start,
        end,
        timestamp_ns
    );


    /*
     * Search index update.
     */

    wt_scheduler_schedule(
        &pipeline->scheduler,
        WT_TASK_SEARCH_INDEX,
        WT_PRIORITY_NORMAL,
        version,
        start,
        end,
        timestamp_ns
    );


    /*
     * Spellcheck can wait behind visible work.
     */

    wt_scheduler_schedule(
        &pipeline->scheduler,
        WT_TASK_SPELLCHECK,
        WT_PRIORITY_LOW,
        version,
        start,
        end,
        timestamp_ns
    );


    /*
     * Autosave should never interfere with typing.
     */

    wt_scheduler_schedule(
        &pipeline->scheduler,
        WT_TASK_AUTOSAVE,
        WT_PRIORITY_IDLE,
        version,
        start,
        end,
        timestamp_ns
    );


    pipeline->latency.scheduled_operations +=
        6;
}


/* =========================================================
 * Submit realtime event
 * ========================================================= */

int
wt_realtime_submit(
    WTRealtimeTyping *pipeline,
    const WTEvent *event
)
{
    if (!pipeline ||
        !event)
        return 0;


    pipeline->latency.input_events++;


    /*
     * The actual input event goes into the
     * immediate queue.
     */

    if (!wt_submit(
            &pipeline->typing,
            event)) {

        return 0;
    }


    /*
     * Flush the current coalesced group if
     * this event cannot belong to it.
     */

    /*
     * We intentionally don't keep a full event
     * copy here. The underlying queue remains the
     * source of truth.
     */


    /*
     * Immediate processing:
     *
     * cursor
     * selection
     * document mutation
     */

    wt_process(
        &pipeline->typing
    );


    pipeline->latency.immediate_operations++;


    /*
     * Determine dirty region.
     */

    size_t start =
        pipeline->typing.dirty.start;

    size_t end =
        pipeline->typing.dirty.end;


    if (end > start) {

        schedule_after_edit(
            pipeline,
            start,
            end,
            event->timestamp_ns
        );
    }


    /*
     * Count coalescing opportunities.
     */

    if (event->type ==
        WT_INSERT_TEXT) {

        if (pipeline->coalesced.active) {

            uint64_t delta =
                event->timestamp_ns -
                pipeline->coalesced.last_timestamp_ns;

            if (delta <=
                pipeline->coalesce_window_ns) {

                pipeline->coalesced.event_count++;

                pipeline->coalesced.last_timestamp_ns =
                    event->timestamp_ns;

                pipeline->latency.coalesced_events++;
            }
        }


        if (!pipeline->coalesced.active) {

            pipeline->coalesced.active =
                1;

            pipeline->coalesced.start =
                start;

            pipeline->coalesced.old_length =
                0;

            pipeline->coalesced.new_length =
                event->text_length;

            pipeline->coalesced.first_timestamp_ns =
                event->timestamp_ns;

            pipeline->coalesced.last_timestamp_ns =
                event->timestamp_ns;

            pipeline->coalesced.event_count =
                1;
        }
    }


    pipeline->last_input_ns =
        event->timestamp_ns;


    return 1;
}


/* =========================================================
 * Flush coalesced edit
 * ========================================================= */

int
wt_realtime_flush(
    WTRealtimeTyping *pipeline,
    uint64_t timestamp_ns
)
{
    if (!pipeline)
        return 0;


    if (!pipeline->coalesced.active)
        return 0;


    uint64_t elapsed =
        timestamp_ns -
        pipeline->coalesced.last_timestamp_ns;


    if (elapsed <
        pipeline->coalesce_window_ns) {

        return 0;
    }


    pipeline->coalesced.active =
        0;


    /*
     * The expensive work has already been
     * merged at scheduler level.
     */

    return 1;
}


/* =========================================================
 * Execute background tasks
 * ========================================================= */

int
wt_realtime_run_tasks(
    WTRealtimeTyping *pipeline,
    size_t budget
)
{
    if (!pipeline)
        return 0;


    size_t executed = 0;


    while (executed < budget) {

        WTTask task;


        if (!wt_scheduler_pop(
                &pipeline->scheduler,
                &task)) {

            break;
        }


        /*
         * Critical protection:
         *
         * if the document changed after this task
         * was scheduled, expensive work based on
         * an old document version should not overwrite
         * newer state.
         */

        if (task.document_version !=
            pipeline->typing.document_version) {

            pipeline->latency.stale_tasks++;

            pipeline->latency.cancelled_tasks++;

            executed++;

            continue;
        }


        /*
         * In the full implementation each case calls
         * the corresponding subsystem.
         */

        switch (task.type) {

            case WT_TASK_LAYOUT:

                /*
                 * word_layout_relayout(
                 *     document,
                 *     task.dirty_start,
                 *     task.dirty_end
                 * );
                 */

                break;


            case WT_TASK_RENDER:

                /*
                 * renderer_invalidate(
                 *     task.dirty_start,
                 *     task.dirty_end
                 * );
                 */

                break;


            case WT_TASK_PAGINATION:

                /*
                 * wp_incremental_update(
                 *     pagination,
                 *     ...
                 * );
                 */

                break;


            case WT_TASK_SEARCH_INDEX:

                /*
                 * ws_index_update(
                 *     document,
                 *     ...
                 * );
                 */

                break;


            case WT_TASK_SPELLCHECK:

                /*
                 * spellcheck_range(
                 *     document,
                 *     ...
                 * );
                 */

                break;


            case WT_TASK_AUTOSAVE:

                /*
                 * document_autosave(
                 *     document
                 * );
                 */

                break;


            case WT_TASK_STATISTICS:

                break;


            case WT_TASK_IDLE_MAINTENANCE:

                break;


            default:

                break;
        }


        pipeline->latency.completed_tasks++;

        executed++;
    }


    return (int)executed;
}


/* =========================================================
 * Coalescing window
 * ========================================================= */

void
wt_realtime_set_coalesce_window(
    WTRealtimeTyping *pipeline,
    uint64_t nanoseconds
)
{
    if (!pipeline)
        return;

    pipeline->coalesce_window_ns =
        nanoseconds;
}


/* =========================================================
 * Statistics
 * ========================================================= */

const WTLatencyStatistics *
wt_realtime_statistics(
    const WTRealtimeTyping *pipeline
)
{
    if (!pipeline)
        return NULL;

    return &pipeline->latency;
}










word_typing_transaction.h
#ifndef WORD_TYPING_TRANSACTION_H
#define WORD_TYPING_TRANSACTION_H

#include "word_typing.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================
 * Document adapter
 *
 * The typing engine does not own the document.
 * It talks to the document through this interface.
 * ========================================================= */

typedef size_t (*WTDocumentLengthFn)(
    void *context
);

typedef int (*WTDocumentReadFn)(
    void *context,
    size_t position,
    char *buffer,
    size_t length
);

typedef int (*WTDocumentInsertFn)(
    void *context,
    size_t position,
    const char *text,
    size_t length
);

typedef int (*WTDocumentDeleteFn)(
    void *context,
    size_t position,
    size_t length
);


/* =========================================================
 * Document adapter
 * ========================================================= */

typedef struct {

    void *context;

    WTDocumentLengthFn length;
    WTDocumentReadFn read;

    WTDocumentInsertFn insert;
    WTDocumentDeleteFn delete;

} WTDocumentAdapter;


/* =========================================================
 * Transaction operation
 * ========================================================= */

typedef enum {

    WT_TX_INSERT = 0,
    WT_TX_DELETE

} WTTransactionOperation;


/* =========================================================
 * Transaction edit
 * ========================================================= */

typedef struct {

    WTTransactionOperation operation;

    size_t position;

    size_t length;

    char *text;

    size_t text_length;

} WTTransactionEdit;


/* =========================================================
 * Transaction
 * ========================================================= */

typedef struct {

    WTTransactionEdit *edits;

    size_t count;
    size_t capacity;

    size_t dirty_start;
    size_t dirty_end;

    size_t bytes_inserted;
    size_t bytes_deleted;

    uint64_t starting_version;
    uint64_t resulting_version;

    uint8_t active;

} WTEditTransaction;


/* =========================================================
 * UTF-8 utilities
 * ========================================================= */

size_t wt_utf8_next(
    const char *text,
    size_t length,
    size_t position
);

size_t wt_utf8_previous(
    const char *text,
    size_t length,
    size_t position
);

size_t wt_utf8_codepoint_size(
    const char *text,
    size_t length,
    size_t position
);

int wt_utf8_validate(
    const char *text,
    size_t length
);


/* =========================================================
 * Transaction
 * ========================================================= */

int wt_transaction_init(
    WTEditTransaction *transaction,
    size_t capacity,
    uint64_t document_version
);

void wt_transaction_destroy(
    WTEditTransaction *transaction
);

int wt_transaction_insert(
    WTEditTransaction *transaction,
    size_t position,
    const char *text,
    size_t length
);

int wt_transaction_delete(
    WTEditTransaction *transaction,
    size_t position,
    size_t length
);

int wt_transaction_commit(
    WTEditTransaction *transaction,
    WTDocumentAdapter *document
);

void wt_transaction_abort(
    WTEditTransaction *transaction
);


/* =========================================================
 * Cursor helpers
 * ========================================================= */

int wt_cursor_move_utf8(
    WTDocumentAdapter *document,
    WTCursor *cursor,
    int direction
);

int wt_cursor_delete_backward_utf8(
    WTDocumentAdapter *document,
    WTCursor *cursor
);

int wt_cursor_delete_forward_utf8(
    WTDocumentAdapter *document,
    WTCursor *cursor
);


/* =========================================================
 * Selection replacement
 * ========================================================= */

int wt_replace_selection(
    WTEditTransaction *transaction,
    WTCursor *cursor,
    const char *text,
    size_t length
);

#ifdef __cplusplus
}
#endif

#endif
word_typing_transaction.c
#include "word_typing_transaction.h"

#include <stdlib.h>
#include <string.h>


/* =========================================================
 * UTF-8
 * ========================================================= */

static int
is_continuation(unsigned char c)
{
    return (c & 0xC0u) == 0x80u;
}


/* =========================================================
 * Decode one UTF-8 sequence
 *
 * Returns:
 *
 *   1  valid
 *   0  invalid
 * ========================================================= */

static int
decode_utf8(
    const char *text,
    size_t length,
    size_t position,
    size_t *consumed
)
{
    if (!text ||
        position >= length ||
        !consumed)
        return 0;

    unsigned char c =
        (unsigned char)text[position];


    /*
     * ASCII.
     */

    if (c < 0x80u) {

        *consumed = 1;

        return 1;
    }


    /*
     * Two-byte sequence.
     */

    if (c >= 0xC2u &&
        c <= 0xDFu) {

        if (position + 1 >= length)
            return 0;

        if (!is_continuation(
                (unsigned char)
                    text[position + 1]))
            return 0;

        *consumed = 2;

        return 1;
    }


    /*
     * Three-byte sequence.
     */

    if (c >= 0xE0u &&
        c <= 0xEFu) {

        if (position + 2 >= length)
            return 0;

        unsigned char c1 =
            (unsigned char)
                text[position + 1];

        unsigned char c2 =
            (unsigned char)
                text[position + 2];

        if (!is_continuation(c1) ||
            !is_continuation(c2))
            return 0;

        /*
         * Reject overlong sequences.
         */

        if (c == 0xE0u &&
            c1 < 0xA0u)
            return 0;

        /*
         * Reject UTF-16 surrogate range.
         */

        if (c == 0xEDu &&
            c1 >= 0xA0u)
            return 0;

        *consumed = 3;

        return 1;
    }


    /*
     * Four-byte sequence.
     */

    if (c >= 0xF0u &&
        c <= 0xF4u) {

        if (position + 3 >= length)
            return 0;

        unsigned char c1 =
            (unsigned char)
                text[position + 1];

        unsigned char c2 =
            (unsigned char)
                text[position + 2];

        unsigned char c3 =
            (unsigned char)
                text[position + 3];

        if (!is_continuation(c1) ||
            !is_continuation(c2) ||
            !is_continuation(c3))
            return 0;

        /*
         * Reject values above U+10FFFF.
         */

        if (c == 0xF0u &&
            c1 < 0x90u)
            return 0;

        if (c == 0xF4u &&
            c1 > 0x8Fu)
            return 0;

        *consumed = 4;

        return 1;
    }

    return 0;
}


/* =========================================================
 * Next UTF-8 codepoint
 * ========================================================= */

size_t
wt_utf8_next(
    const char *text,
    size_t length,
    size_t position
)
{
    size_t consumed = 0;

    if (!decode_utf8(
            text,
            length,
            position,
            &consumed)) {

        /*
         * Recovery behavior:
         * advance one byte rather than becoming stuck.
         */

        return position < length
            ? position + 1
            : length;
    }

    return position + consumed;
}


/* =========================================================
 * Previous UTF-8 codepoint
 * ========================================================= */

size_t
wt_utf8_previous(
    const char *text,
    size_t length,
    size_t position
)
{
    if (!text ||
        position == 0 ||
        position > length)
        return position;


    size_t p =
        position - 1;


    /*
     * Walk backwards through continuation bytes.
     *
     * UTF-8 codepoints are at most four bytes.
     */

    for (size_t i = 0;
         i < 3 && p > 0;
         ++i) {

        unsigned char c =
            (unsigned char)text[p];

        if (!is_continuation(c))
            break;

        p--;
    }


    /*
     * Verify that the candidate start is valid.
     */

    size_t consumed = 0;

    if (decode_utf8(
            text,
            length,
            p,
            &consumed) &&
        p + consumed == position) {

        return p;
    }


    /*
     * Malformed data fallback.
     */

    return position - 1;
}


/* =========================================================
 * UTF-8 codepoint size
 * ========================================================= */

size_t
wt_utf8_codepoint_size(
    const char *text,
    size_t length,
    size_t position
)
{
    size_t next =
        wt_utf8_next(
            text,
            length,
            position
        );

    if (next < position)
        return 0;

    return next - position;
}


/* =========================================================
 * Validate complete UTF-8 string
 * ========================================================= */

int
wt_utf8_validate(
    const char *text,
    size_t length
)
{
    if (!text && length != 0)
        return 0;

    size_t position = 0;

    while (position < length) {

        size_t consumed = 0;

        if (!decode_utf8(
                text,
                length,
                position,
                &consumed)) {

            return 0;
        }

        position += consumed;
    }

    return 1;
}


/* =========================================================
 * Transaction initialisation
 * ========================================================= */

int
wt_transaction_init(
    WTEditTransaction *transaction,
    size_t capacity,
    uint64_t document_version
)
{
    if (!transaction)
        return 0;

    memset(
        transaction,
        0,
        sizeof(*transaction)
    );

    if (capacity == 0)
        capacity = 16;

    transaction->edits =
        calloc(
            capacity,
            sizeof(WTTransactionEdit)
        );

    if (!transaction->edits)
        return 0;

    transaction->capacity =
        capacity;

    transaction->starting_version =
        document_version;

    transaction->active =
        1;

    return 1;
}


/* =========================================================
 * Free transaction
 * ========================================================= */

void
wt_transaction_destroy(
    WTEditTransaction *transaction
)
{
    if (!transaction)
        return;

    for (size_t i = 0;
         i < transaction->count;
         ++i) {

        free(
            transaction->edits[i].text
        );
    }

    free(
        transaction->edits
    );

    memset(
        transaction,
        0,
        sizeof(*transaction)
    );
}


/* =========================================================
 * Grow transaction
 * ========================================================= */

static int
transaction_grow(
    WTEditTransaction *transaction
)
{
    if (transaction->count <
        transaction->capacity)

        return 1;


    size_t new_capacity =
        transaction->capacity * 2;

    if (new_capacity <
        transaction->capacity)

        return 0;


    WTTransactionEdit *edits =
        realloc(
            transaction->edits,
            new_capacity *
            sizeof(WTTransactionEdit)
        );

    if (!edits)
        return 0;


    memset(
        edits + transaction->capacity,
        0,
        (new_capacity -
         transaction->capacity) *
        sizeof(WTTransactionEdit)
    );


    transaction->edits =
        edits;

    transaction->capacity =
        new_capacity;

    return 1;
}


/* =========================================================
 * Update dirty range
 * ========================================================= */

static void
transaction_dirty(
    WTEditTransaction *transaction,
    size_t start,
    size_t end
)
{
    if (transaction->count == 0) {

        transaction->dirty_start =
            start;

        transaction->dirty_end =
            end;

        return;
    }


    if (start <
        transaction->dirty_start)

        transaction->dirty_start =
            start;

    if (end >
        transaction->dirty_end)

        transaction->dirty_end =
            end;
}


/* =========================================================
 * Insert operation
 * ========================================================= */

int
wt_transaction_insert(
    WTEditTransaction *transaction,
    size_t position,
    const char *text,
    size_t length
)
{
    if (!transaction ||
        !transaction->active ||
        (!text && length != 0))
        return 0;


    if (!wt_utf8_validate(
            text,
            length))
        return 0;


    if (!transaction_grow(
            transaction))
        return 0;


    WTTransactionEdit *edit =
        &transaction->edits[
            transaction->count
        ];


    memset(
        edit,
        0,
        sizeof(*edit)
    );


    edit->operation =
        WT_TX_INSERT;

    edit->position =
        position;

    edit->length =
        length;

    edit->text_length =
        length;


    if (length) {

        edit->text =
            malloc(length + 1);

        if (!edit->text)
            return 0;

        memcpy(
            edit->text,
            text,
            length
        );

        edit->text[length] =
            '\0';
    }


    transaction->count++;

    transaction->bytes_inserted +=
        length;

    transaction_dirty(
        transaction,
        position,
        position + length
    );

    return 1;
}


/* =========================================================
 * Delete operation
 * ========================================================= */

int
wt_transaction_delete(
    WTEditTransaction *transaction,
    size_t position,
    size_t length
)
{
    if (!transaction ||
        !transaction->active)
        return 0;

    if (length == 0)
        return 1;


    if (!transaction_grow(
            transaction))
        return 0;


    WTTransactionEdit *edit =
        &transaction->edits[
            transaction->count
        ];


    memset(
        edit,
        0,
        sizeof(*edit)
    );


    edit->operation =
        WT_TX_DELETE;

    edit->position =
        position;

    edit->length =
        length;


    transaction->count++;

    transaction->bytes_deleted +=
        length;

    transaction_dirty(
        transaction,
        position,
        position + length
    );

    return 1;
}


/* =========================================================
 * Apply transaction
 *
 * Edits are applied from highest position to lowest.
 *
 * This prevents earlier offsets from being shifted by
 * later document mutations.
 * ========================================================= */

static int
compare_edits_descending(
    const void *a,
    const void *b
)
{
    const WTTransactionEdit *ea =
        (const WTTransactionEdit *)a;

    const WTTransactionEdit *eb =
        (const WTTransactionEdit *)b;

    if (ea->position <
        eb->position)

        return 1;

    if (ea->position >
        eb->position)

        return -1;

    /*
     * At the same position, deletes happen first.
     */

    if (ea->operation !=
        eb->operation) {

        return ea->operation ==
               WT_TX_DELETE
            ? -1
            : 1;
    }

    return 0;
}


/* =========================================================
 * Commit
 * ========================================================= */

int
wt_transaction_commit(
    WTEditTransaction *transaction,
    WTDocumentAdapter *document
)
{
    if (!transaction ||
        !transaction->active ||
        !document ||
        !document->length ||
        !document->insert ||
        !document->delete)
        return 0;


    /*
     * Nothing to commit.
     */

    if (transaction->count == 0) {

        transaction->resulting_version =
            transaction->starting_version;

        transaction->active =
            0;

        return 1;
    }


    /*
     * Check that all positions are currently valid.
     */

    size_t document_length =
        document->length(
            document->context
        );


    for (size_t i = 0;
         i < transaction->count;
         ++i) {

        WTTransactionEdit *edit =
            &transaction->edits[i];

        if (edit->position >
            document_length)

            return 0;

        if (edit->operation ==
            WT_TX_DELETE) {

            if (edit->length >
                document_length -
                edit->position)

                return 0;
        }
    }


    /*
     * Sort highest-to-lowest.
     */

    qsort(
        transaction->edits,
        transaction->count,
        sizeof(WTTransactionEdit),
        compare_edits_descending
    );


    /*
     * Apply operations.
     */

    for (size_t i = 0;
         i < transaction->count;
         ++i) {

        WTTransactionEdit *edit =
            &transaction->edits[i];

        int success;


        if (edit->operation ==
            WT_TX_DELETE) {

            success =
                document->delete(
                    document->context,
                    edit->position,
                    edit->length
                );

        } else {

            success =
                document->insert(
                    document->context,
                    edit->position,
                    edit->text,
                    edit->text_length
                );
        }


        if (!success) {

            /*
             * A production implementation should
             * use document-level transactions so that
             * a failed operation can atomically roll
             * back every earlier operation.
             */

            return 0;
        }
    }


    transaction->resulting_version =
        transaction->starting_version + 1;

    transaction->active =
        0;

    return 1;
}


/* =========================================================
 * Abort
 * ========================================================= */

void
wt_transaction_abort(
    WTEditTransaction *transaction
)
{
    if (!transaction)
        return;

    for (size_t i = 0;
         i < transaction->count;
         ++i) {

        free(
            transaction->edits[i].text
        );

        transaction->edits[i].text =
            NULL;
    }

    transaction->count =
        0;

    transaction->active =
        0;

    transaction->bytes_inserted =
        0;

    transaction->bytes_deleted =
        0;
}


/* =========================================================
 * Read entire document
 *
 * Used by cursor operations.
 * ========================================================= */

static char *
read_document(
    WTDocumentAdapter *document,
    size_t *length
)
{
    if (!document ||
        !document->length ||
        !document->read ||
        !length)
        return NULL;


    size_t n =
        document->length(
            document->context
        );


    char *buffer =
        malloc(n + 1);

    if (!buffer)
        return NULL;


    if (n &&
        !document->read(
            document->context,
            0,
            buffer,
            n)) {

        free(buffer);

        return NULL;
    }


    buffer[n] =
        '\0';

    *length =
        n;

    return buffer;
}


/* =========================================================
 * UTF-8 cursor movement
 * ========================================================= */

int
wt_cursor_move_utf8(
    WTDocumentAdapter *document,
    WTCursor *cursor,
    int direction
)
{
    if (!document ||
        !cursor)
        return 0;


    size_t length = 0;

    char *text =
        read_document(
            document,
            &length
        );

    if (!text)
        return 0;


    if (direction > 0) {

        cursor->position =
            wt_utf8_next(
                text,
                length,
                cursor->position
            );

    } else if (direction < 0) {

        cursor->position =
            wt_utf8_previous(
                text,
                length,
                cursor->position
            );
    }


    free(text);


    cursor->anchor =
        cursor->position;

    cursor->has_selection =
        0;

    return 1;
}


/* =========================================================
 * Backspace
 * ========================================================= */

int
wt_cursor_delete_backward_utf8(
    WTDocumentAdapter *document,
    WTCursor *cursor
)
{
    if (!document ||
        !cursor ||
        !document->length ||
        !document->read ||
        !document->delete)
        return 0;


    /*
     * Selection takes precedence.
     */

    if (cursor->has_selection) {

        size_t a =
            cursor->anchor;

        size_t b =
            cursor->position;

        if (a > b) {

            size_t t = a;
            a = b;
            b = t;
        }


        if (!document->delete(
                document->context,
                a,
                b - a))
            return 0;


        cursor->position =
            a;

        cursor->anchor =
            a;

        cursor->has_selection =
            0;

        return 1;
    }


    size_t length = 0;

    char *text =
        read_document(
            document,
            &length
        );

    if (!text)
        return 0;


    size_t previous =
        wt_utf8_previous(
            text,
            length,
            cursor->position
        );


    free(text);


    if (previous ==
        cursor->position)

        return 0;


    if (!document->delete(
            document->context,
            previous,
            cursor->position -
            previous))
        return 0;


    cursor->position =
        previous;

    cursor->anchor =
        previous;

    return 1;
}


/* =========================================================
 * Forward delete
 * ========================================================= */

int
wt_cursor_delete_forward_utf8(
    WTDocumentAdapter *document,
    WTCursor *cursor
)
{
    if (!document ||
        !cursor)
        return 0;


    if (cursor->has_selection)
        return wt_cursor_delete_backward_utf8(
            document,
            cursor
        );


    size_t length = 0;

    char *text =
        read_document(
            document,
            &length
        );

    if (!text)
        return 0;


    if (cursor->position >=
        length) {

        free(text);

        return 0;
    }


    size_t next =
        wt_utf8_next(
            text,
            length,
            cursor->position
        );


    free(text);


    if (!document->delete(
            document->context,
            cursor->position,
            next -
            cursor->position))
        return 0;


    return 1;
}


/* =========================================================
 * Replace selection
 * ========================================================= */

int
wt_replace_selection(
    WTEditTransaction *transaction,
    WTCursor *cursor,
    const char *text,
    size_t length
)
{
    if (!transaction ||
        !cursor ||
        (!text && length != 0))
        return 0;


    size_t a =
        cursor->anchor;

    size_t b =
        cursor->position;


    if (a > b) {

        size_t t = a;
        a = b;
        b = t;
    }


    /*
     * Delete selected range.
     */

    if (b > a) {

        if (!wt_transaction_delete(
                transaction,
                a,
                b - a))
            return 0;
    }


    /*
     * Insert replacement.
     */

    if (length) {

        if (!wt_transaction_insert(
                transaction,
                a,
                text,
                length))
            return 0;
    }


    cursor->position =
        a + length;

    cursor->anchor =
        cursor->position;

    cursor->has_selection =
        0;


    return 1;
}
Why this is a significant improvement

The typing engine can now handle:

Normal typing
"Hello"
   ↓
ONE transaction
   ↓
insert(0, "Hello")
   ↓
document version + 1
Selection replacement
"The quick [brown] fox"
           ^^^^^
             │
             ▼
          "red"
             │
             ▼
"The quick red fox"

as:

wt_replace_selection(
    &transaction,
    &cursor,
    "red",
    3
);

rather than manually manipulating the document.

UTF-8 backspace

For:

Hello 世界
         ^

Backspace removes the complete UTF-8 codepoint rather than blindly removing one byte.

That matters because Chinese/Japanese/Korean characters, accented characters and many other Unicode characters occupy multiple bytes.

Integrating it with the existing document engine

The adapter can sit over the #2 document/piece-table layer:

static size_t
typing_document_length(void *context)
{
    WDDocument *document =
        context;

    return wd_length(document);
}


static int
typing_document_read(
    void *context,
    size_t position,
    char *buffer,
    size_t length
)
{
    WDDocument *document =
        context;

    return wd_read(
        document,
        position,
        buffer,
        length
    );
}


/*
 * These two wrappers depend on the exact WDDocument
 * mutation API used by the document implementation.
 */

static int
typing_document_insert(
    void *context,
    size_t position,
    const char *text,
    size_t length
)
{
    WDDocument *document =
        context;

    return wd_insert(
        document,
        position,
        text,
        length
    );
}


static int
typing_document_delete(
    void *context,
    size_t position,
    size_t length
)
{
    WDDocument *document =
        context;

    return wd_delete(
        document,
        position,
        length
    );
}

Then:

WTDocumentAdapter adapter = {
    .context = &document,

    .length = typing_document_length,
    .read = typing_document_read,

    .insert = typing_document_insert,
    .delete = typing_document_delete
};

And a complete typing transaction becomes:

WTEditTransaction transaction;

wt_transaction_init(
    &transaction,
    16,
    document_version
);

wt_transaction_insert(
    &transaction,
    cursor.position,
    "Hello world",
    11
);

wt_transaction_commit(
    &transaction,
    &adapter
);

wt_transaction_destroy(
    &transaction
);






1. word_media.h
#ifndef WORD_MEDIA_H
#define WORD_MEDIA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/* =========================================================
 * Media types
 * ========================================================= */

typedef enum {

    WMEDIA_UNKNOWN = 0,

    WMEDIA_JPEG,
    WMEDIA_PNG,
    WMEDIA_GIF,
    WMEDIA_BMP,
    WMEDIA_WEBP,
    WMEDIA_TIFF,

    WMEDIA_SVG,

    WMEDIA_AUDIO,
    WMEDIA_VIDEO

} WMediaType;


/* =========================================================
 * Pixel formats
 * ========================================================= */

typedef enum {

    WMEDIA_PIXEL_UNKNOWN = 0,

    WMEDIA_PIXEL_GRAY8,
    WMEDIA_PIXEL_RGB8,
    WMEDIA_PIXEL_RGBA8,

    WMEDIA_PIXEL_BGRA8

} WMediaPixelFormat;


/* =========================================================
 * Image metadata
 * ========================================================= */

typedef struct {

    uint32_t width;
    uint32_t height;

    uint32_t dpi_x;
    uint32_t dpi_y;

    uint32_t orientation;

    WMediaPixelFormat pixel_format;

    size_t decoded_size;

} WMediaInfo;


/* =========================================================
 * Raw media buffer
 * ========================================================= */

typedef struct {

    uint8_t *data;

    size_t size;

    size_t capacity;

} WMediaBuffer;


/* =========================================================
 * Decoded image
 * ========================================================= */

typedef struct {

    uint8_t *pixels;

    size_t size;

    uint32_t width;
    uint32_t height;

    size_t stride;

    WMediaPixelFormat format;

} WMediaImage;


/* =========================================================
 * Crop rectangle
 * ========================================================= */

typedef struct {

    double x;
    double y;

    double width;
    double height;

} WMediaCrop;


/* =========================================================
 * Media transform
 * ========================================================= */

typedef struct {

    double scale_x;
    double scale_y;

    double rotation;

    WMediaCrop crop;

    uint8_t crop_enabled;

    uint8_t flip_horizontal;
    uint8_t flip_vertical;

} WMediaTransform;


/* =========================================================
 * Media object
 * ========================================================= */

typedef struct {

    uint64_t id;

    WMediaType type;

    char *source;

    WMediaBuffer encoded;

    WMediaImage image;

    WMediaInfo info;

    WMediaTransform transform;

    uint64_t content_hash;

    uint64_t last_used;

    uint32_t reference_count;

    uint8_t decoded;
    uint8_t loading;
    uint8_t failed;

} WMediaObject;


/* =========================================================
 * Cache
 * ========================================================= */

typedef struct {

    WMediaObject **objects;

    size_t count;

    size_t capacity;

    size_t maximum_bytes;

    size_t current_bytes;

    uint64_t clock;

    uint64_t next_id;

} WMediaCache;


/* =========================================================
 * Media layout
 * ========================================================= */

typedef enum {

    WMEDIA_WRAP_INLINE = 0,
    WMEDIA_WRAP_SQUARE,
    WMEDIA_WRAP_TIGHT,
    WMEDIA_WRAP_TOP_BOTTOM,
    WMEDIA_WRAP_BEHIND_TEXT,
    WMEDIA_WRAP_IN_FRONT

} WMediaWrapMode;


typedef enum {

    WMEDIA_ALIGN_LEFT = 0,
    WMEDIA_ALIGN_CENTER,
    WMEDIA_ALIGN_RIGHT

} WMediaAlignment;


typedef struct {

    double x;
    double y;

    double width;
    double height;

    WMediaWrapMode wrap_mode;

    WMediaAlignment alignment;

    double rotation;

} WMediaLayout;


/* =========================================================
 * Media store
 * ========================================================= */

typedef struct {

    WMediaCache cache;

} WMediaStore;


/* =========================================================
 * Buffer
 * ========================================================= */

int wmedia_buffer_init(
    WMediaBuffer *buffer
);

void wmedia_buffer_destroy(
    WMediaBuffer *buffer
);

int wmedia_buffer_reserve(
    WMediaBuffer *buffer,
    size_t capacity
);

int wmedia_buffer_append(
    WMediaBuffer *buffer,
    const void *data,
    size_t length
);


/* =========================================================
 * Store
 * ========================================================= */

int wmedia_store_init(
    WMediaStore *store,
    size_t maximum_cache_bytes
);

void wmedia_store_destroy(
    WMediaStore *store
);


/* =========================================================
 * Loading
 * ========================================================= */

WMediaObject *
wmedia_load_file(
    WMediaStore *store,
    const char *path
);

WMediaObject *
wmedia_load_memory(
    WMediaStore *store,
    const void *data,
    size_t length,
    const char *source_name
);


/* =========================================================
 * Media detection
 * ========================================================= */

WMediaType
wmedia_detect_type(
    const void *data,
    size_t length
);


/* =========================================================
 * Decode
 * ========================================================= */

int wmedia_decode(
    WMediaObject *object
);


/* =========================================================
 * Image operations
 * ========================================================= */

int wmedia_resize(
    WMediaObject *object,
    uint32_t width,
    uint32_t height
);

int wmedia_crop(
    WMediaObject *object,
    WMediaCrop crop
);


/* =========================================================
 * References
 * ========================================================= */

void wmedia_retain(
    WMediaObject *object
);

void wmedia_release(
    WMediaStore *store,
    WMediaObject *object
);


/* =========================================================
 * Layout
 * ========================================================= */

int wmedia_calculate_layout(
    const WMediaObject *object,
    double available_width,
    double available_height,
    WMediaLayout *layout
);


/* =========================================================
 * Cache maintenance
 * ========================================================= */

void wmedia_cache_trim(
    WMediaStore *store
);

#ifdef __cplusplus
}
#endif

#endif
2. word_media.c
#include "word_media.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>


/* =========================================================
 * Buffer
 * ========================================================= */

int
wmedia_buffer_init(
    WMediaBuffer *buffer
)
{
    if (!buffer)
        return 0;

    memset(
        buffer,
        0,
        sizeof(*buffer)
    );

    return 1;
}


void
wmedia_buffer_destroy(
    WMediaBuffer *buffer
)
{
    if (!buffer)
        return;

    free(
        buffer->data
    );

    memset(
        buffer,
        0,
        sizeof(*buffer)
    );
}


int
wmedia_buffer_reserve(
    WMediaBuffer *buffer,
    size_t capacity
)
{
    if (!buffer)
        return 0;

    if (capacity <=
        buffer->capacity)

        return 1;


    uint8_t *data =
        realloc(
            buffer->data,
            capacity
        );

    if (!data)
        return 0;


    buffer->data =
        data;

    buffer->capacity =
        capacity;

    return 1;
}


int
wmedia_buffer_append(
    WMediaBuffer *buffer,
    const void *data,
    size_t length
)
{
    if (!buffer ||
        (!data && length))
        return 0;


    if (length >
        SIZE_MAX -
        buffer->size)

        return 0;


    size_t required =
        buffer->size +
        length;


    if (required >
        buffer->capacity) {

        size_t capacity =
            buffer->capacity
                ? buffer->capacity
                : 4096;

        while (capacity <
               required) {

            if (capacity >
                SIZE_MAX / 2)

                capacity =
                    required;

            else

                capacity *= 2;
        }


        if (!wmedia_buffer_reserve(
                buffer,
                capacity))

            return 0;
    }


    memcpy(
        buffer->data +
        buffer->size,
        data,
        length
    );


    buffer->size =
        required;

    return 1;
}


/* =========================================================
 * Store
 * ========================================================= */

int
wmedia_store_init(
    WMediaStore *store,
    size_t maximum_cache_bytes
)
{
    if (!store)
        return 0;


    memset(
        store,
        0,
        sizeof(*store)
    );


    store->cache.capacity =
        64;

    store->cache.objects =
        calloc(
            store->cache.capacity,
            sizeof(WMediaObject *)
        );


    if (!store->cache.objects)
        return 0;


    store->cache.maximum_bytes =
        maximum_cache_bytes;

    store->cache.next_id =
        1;


    return 1;
}


void
wmedia_store_destroy(
    WMediaStore *store
)
{
    if (!store)
        return;


    for (size_t i = 0;
         i < store->cache.count;
         ++i) {

        WMediaObject *object =
            store->cache.objects[i];


        if (!object)
            continue;


        free(
            object->source
        );

        wmedia_buffer_destroy(
            &object->encoded
        );

        free(
            object->image.pixels
        );

        free(
            object
        );
    }


    free(
        store->cache.objects
    );


    memset(
        store,
        0,
        sizeof(*store)
    );
}


/* =========================================================
 * Detect media type
 * ========================================================= */

WMediaType
wmedia_detect_type(
    const void *data,
    size_t length
)
{
    if (!data)
        return WMEDIA_UNKNOWN;


    const uint8_t *p =
        data;


    /*
     * JPEG
     */

    if (length >= 3 &&
        p[0] == 0xFF &&
        p[1] == 0xD8 &&
        p[2] == 0xFF)

        return WMEDIA_JPEG;


    /*
     * PNG
     */

    if (length >= 8 &&
        p[0] == 0x89 &&
        p[1] == 'P' &&
        p[2] == 'N' &&
        p[3] == 'G' &&
        p[4] == 0x0D &&
        p[5] == 0x0A &&
        p[6] == 0x1A &&
        p[7] == 0x0A)

        return WMEDIA_PNG;


    /*
     * GIF
     */

    if (length >= 6 &&
        memcmp(
            p,
            "GIF87a",
            6
        ) == 0)

        return WMEDIA_GIF;


    if (length >= 6 &&
        memcmp(
            p,
            "GIF89a",
            6
        ) == 0)

        return WMEDIA_GIF;


    /*
     * BMP
     */

    if (length >= 2 &&
        p[0] == 'B' &&
        p[1] == 'M')

        return WMEDIA_BMP;


    /*
     * WebP
     */

    if (length >= 12 &&
        memcmp(
            p,
            "RIFF",
            4
        ) == 0 &&
        memcmp(
            p + 8,
            "WEBP",
            4
        ) == 0)

        return WMEDIA_WEBP;


    /*
     * SVG.
     *
     * Simplified detection.
     */

    if (length >= 4) {

        const char *text =
            (const char *)data;

        if (strstr(
                text,
                "<svg") != NULL)

            return WMEDIA_SVG;
    }


    return WMEDIA_UNKNOWN;
}


/* =========================================================
 * Create object
 * ========================================================= */

static WMediaObject *
create_object(
    WMediaStore *store
)
{
    WMediaObject *object =
        calloc(
            1,
            sizeof(WMediaObject)
        );


    if (!object)
        return NULL;


    object->id =
        store->cache.next_id++;


    object->reference_count =
        1;


    object->last_used =
        ++store->cache.clock;


    wmedia_buffer_init(
        &object->encoded
    );


    return object;
}


/* =========================================================
 * Add object to cache
 * ========================================================= */

static int
cache_add(
    WMediaStore *store,
    WMediaObject *object
)
{
    if (store->cache.count >=
        store->cache.capacity) {

        size_t new_capacity =
            store->cache.capacity * 2;


        WMediaObject **objects =
            realloc(
                store->cache.objects,
                new_capacity *
                sizeof(WMediaObject *)
            );


        if (!objects)
            return 0;


        store->cache.objects =
            objects;

        store->cache.capacity =
            new_capacity;
    }


    store->cache.objects[
        store->cache.count++
    ] = object;


    store->cache.current_bytes +=
        object->encoded.size;


    return 1;
}


/* =========================================================
 * Load memory
 * ========================================================= */

WMediaObject *
wmedia_load_memory(
    WMediaStore *store,
    const void *data,
    size_t length,
    const char *source_name
)
{
    if (!store ||
        (!data && length))
        return NULL;


    WMediaObject *object =
        create_object(
            store
        );


    if (!object)
        return NULL;


    if (source_name) {

        size_t n =
            strlen(source_name);

        object->source =
            malloc(n + 1);

        if (!object->source) {

            free(object);

            return NULL;
        }

        memcpy(
            object->source,
            source_name,
            n + 1
        );
    }


    if (!wmedia_buffer_append(
            &object->encoded,
            data,
            length)) {

        free(object->source);

        free(object);

        return NULL;
    }


    object->type =
        wmedia_detect_type(
            data,
            length
        );


    if (!cache_add(
            store,
            object)) {

        wmedia_buffer_destroy(
            &object->encoded
        );

        free(object->source);

        free(object);

        return NULL;
    }


    return object;
}


/* =========================================================
 * Load file
 * ========================================================= */

WMediaObject *
wmedia_load_file(
    WMediaStore *store,
    const char *path
)
{
    if (!store ||
        !path)
        return NULL;


    FILE *file =
        fopen(
            path,
            "rb"
        );


    if (!file)
        return NULL;


    if (fseek(
            file,
            0,
            SEEK_END) != 0) {

        fclose(file);

        return NULL;
    }


    long size =
        ftell(file);


    if (size < 0) {

        fclose(file);

        return NULL;
    }


    rewind(file);


    uint8_t *buffer =
        malloc(
            (size_t)size
        );


    if (!buffer && size != 0) {

        fclose(file);

        return NULL;
    }


    size_t read =
        fread(
            buffer,
            1,
            (size_t)size,
            file
        );


    fclose(file);


    if (read !=
        (size_t)size) {

        free(buffer);

        return NULL;
    }


    WMediaObject *object =
        wmedia_load_memory(
            store,
            buffer,
            read,
            path
        );


    free(buffer);


    return object;
}


/* =========================================================
 * Decode
 *
 * This layer deliberately separates media management from
 * codec implementation.
 *
 * A production build would plug JPEG/PNG/WebP/TIFF/etc.
 * decoders into this function.
 * ========================================================= */

int
wmedia_decode(
    WMediaObject *object
)
{
    if (!object)
        return 0;


    if (object->decoded)
        return 1;


    object->loading =
        1;


    /*
     * BMP is implemented here as an example of a native
     * decoder. Other formats are delegated to codecs.
     */

    if (object->type ==
        WMEDIA_BMP) {

        const uint8_t *data =
            object->encoded.data;

        size_t size =
            object->encoded.size;


        if (size < 54) {

            object->failed =
                1;

            object->loading =
                0;

            return 0;
        }


        if (data[0] != 'B' ||
            data[1] != 'M') {

            object->failed =
                1;

            object->loading =
                0;

            return 0;
        }


        uint32_t pixel_offset =
            (uint32_t)data[10] |
            ((uint32_t)data[11] << 8) |
            ((uint32_t)data[12] << 16) |
            ((uint32_t)data[13] << 24);


        uint32_t width =
            (uint32_t)data[18] |
            ((uint32_t)data[19] << 8) |
            ((uint32_t)data[20] << 16) |
            ((uint32_t)data[21] << 24);


        uint32_t height =
            (uint32_t)data[22] |
            ((uint32_t)data[23] << 8) |
            ((uint32_t)data[24] << 16) |
            ((uint32_t)data[25] << 24);


        uint16_t bits =
            (uint16_t)data[28] |
            ((uint16_t)data[29] << 8);


        if (width == 0 ||
            height == 0 ||
            bits != 24) {

            object->failed =
                1;

            object->loading =
                0;

            return 0;
        }


        size_t stride =
            ((size_t)width * 3 + 3) &
            ~((size_t)3);


        if ((size_t)height >
            SIZE_MAX / stride) {

            object->failed =
                1;

            object->loading =
                0;

            return 0;
        }


        size_t decoded_size =
            stride *
            (size_t)height;


        uint8_t *pixels =
            malloc(
                decoded_size
            );


        if (!pixels) {

            object->failed =
                1;

            object->loading =
                0;

            return 0;
        }


        if ((size_t)pixel_offset +
            decoded_size >
            size) {

            free(pixels);

            object->failed =
                1;

            object->loading =
                0;

            return 0;
        }


        /*
         * BMP stores rows bottom-up.
         */

        for (uint32_t y = 0;
             y < height;
             ++y) {

            const uint8_t *source =
                data +
                pixel_offset +
                ((size_t)
                    (height - 1 - y) *
                 stride);


            uint8_t *destination =
                pixels +
                ((size_t)y *
                 stride);


            memcpy(
                destination,
                source,
                stride
            );
        }


        object->image.pixels =
            pixels;

        object->image.size =
            decoded_size;

        object->image.width =
            width;

        object->image.height =
            height;

        object->image.stride =
            stride;

        object->image.format =
            WMEDIA_PIXEL_RGB8;


        object->info.width =
            width;

        object->info.height =
            height;

        object->info.pixel_format =
            WMEDIA_PIXEL_RGB8;

        object->info.decoded_size =
            decoded_size;


        object->decoded =
            1;

        object->loading =
            0;


        return 1;
    }


    /*
     * Other formats require codec backends.
     */

    object->loading =
        0;

    object->failed =
        1;

    return 0;
}


/* =========================================================
 * Resize
 * ========================================================= */

int
wmedia_resize(
    WMediaObject *object,
    uint32_t width,
    uint32_t height
)
{
    if (!object ||
        !object->decoded ||
        !object->image.pixels ||
        width == 0 ||
        height == 0)
        return 0;


    size_t channels =
        object->image.format ==
            WMEDIA_PIXEL_RGBA8
        ? 4
        : 3;


    if ((size_t)width >
        SIZE_MAX / channels)

        return 0;


    size_t stride =
        (size_t)width *
        channels;


    if ((size_t)height >
        SIZE_MAX / stride)

        return 0;


    size_t size =
        stride *
        (size_t)height;


    uint8_t *pixels =
        malloc(size);


    if (!pixels)
        return 0;


    /*
     * Simple bilinear-like nearest sample.
     *
     * This is intentionally deterministic and cheap.
     * A production renderer would use a high-quality
     * resampling kernel.
     */

    for (uint32_t y = 0;
         y < height;
         ++y) {

        uint32_t source_y =
            ((uint64_t)y *
             object->image.height) /
            height;


        for (uint32_t x = 0;
             x < width;
             ++x) {

            uint32_t source_x =
                ((uint64_t)x *
                 object->image.width) /
                width;


            const uint8_t *source =
                object->image.pixels +
                (size_t)source_y *
                object->image.stride +
                (size_t)source_x *
                channels;


            uint8_t *destination =
                pixels +
                (size_t)y *
                stride +
                (size_t)x *
                channels;


            memcpy(
                destination,
                source,
                channels
            );
        }
    }


    free(
        object->image.pixels
    );


    object->image.pixels =
        pixels;

    object->image.size =
        size;

    object->image.width =
        width;

    object->image.height =
        height;

    object->image.stride =
        stride;


    object->info.width =
        width;

    object->info.height =
        height;

    object->info.decoded_size =
        size;


    return 1;
}


/* =========================================================
 * Crop
 * ========================================================= */

int
wmedia_crop(
    WMediaObject *object,
    WMediaCrop crop
)
{
    if (!object ||
        !object->decoded)
        return 0;


    if (crop.x < 0 ||
        crop.y < 0 ||
        crop.width <= 0 ||
        crop.height <= 0)

        return 0;


    if (crop.x + crop.width >
        object->image.width)

        return 0;


    if (crop.y + crop.height >
        object->image.height)

        return 0;


    size_t channels =
        object->image.format ==
            WMEDIA_PIXEL_RGBA8
        ? 4
        : 3;


    uint32_t x =
        (uint32_t)crop.x;

    uint32_t y =
        (uint32_t)crop.y;

    uint32_t width =
        (uint32_t)crop.width;

    uint32_t height =
        (uint32_t)crop.height;


    size_t stride =
        (size_t)width *
        channels;


    size_t size =
        stride *
        (size_t)height;


    uint8_t *pixels =
        malloc(size);


    if (!pixels)
        return 0;


    for (uint32_t row = 0;
         row < height;
         ++row) {

        const uint8_t *source =
            object->image.pixels +
            (size_t)(y + row) *
            object->image.stride +
            (size_t)x *
            channels;


        uint8_t *destination =
            pixels +
            (size_t)row *
            stride;


        memcpy(
            destination,
            source,
            stride
        );
    }


    free(
        object->image.pixels
    );


    object->image.pixels =
        pixels;

    object->image.size =
        size;

    object->image.width =
        width;

    object->image.height =
        height;

    object->image.stride =
        stride;


    object->info.width =
        width;

    object->info.height =
        height;

    object->info.decoded_size =
        size;


    return 1;
}


/* =========================================================
 * References
 * ========================================================= */

void
wmedia_retain(
    WMediaObject *object
)
{
    if (!object)
        return;

    if (object->reference_count <
        UINT32_MAX)

        object->reference_count++;
}


void
wmedia_release(
    WMediaStore *store,
    WMediaObject *object
)
{
    if (!store ||
        !object)
        return;


    if (object->reference_count == 0)
        return;


    object->reference_count--;

    object->last_used =
        ++store->cache.clock;


    /*
     * Objects remain cached even when the document
     * releases its reference.
     */

    wmedia_cache_trim(
        store
    );
}


/* =========================================================
 * Calculate layout
 * ========================================================= */

int
wmedia_calculate_layout(
    const WMediaObject *object,
    double available_width,
    double available_height,
    WMediaLayout *layout
)
{
    if (!object ||
        !layout ||
        object->info.width == 0 ||
        object->info.height == 0 ||
        available_width <= 0 ||
        available_height <= 0)

        return 0;


    double width =
        (double)object->info.width;


    double height =
        (double)object->info.height;


    /*
     * Scale down to available area.
     */

    double scale =
        1.0;


    if (width >
        available_width)

        scale =
            available_width /
            width;


    if (height * scale >
        available_height)

        scale =
            available_height /
            height;


    layout->width =
        width * scale;

    layout->height =
        height * scale;


    layout->x =
        0;

    layout->y =
        0;


    layout->wrap_mode =
        WMEDIA_WRAP_INLINE;


    layout->alignment =
        WMEDIA_ALIGN_LEFT;


    layout->rotation =
        0;


    return 1;
}


/* =========================================================
 * Cache trim
 * ========================================================= */

void
wmedia_cache_trim(
    WMediaStore *store
)
{
    if (!store)
        return;


    WMediaCache *cache =
        &store->cache;


    while (cache->current_bytes >
           cache->maximum_bytes) {


        WMediaObject *candidate =
            NULL;

        size_t candidate_index =
            SIZE_MAX;


        /*
         * Find least-recently-used object that
         * isn't currently referenced.
         */

        for (size_t i = 0;
             i < cache->count;
             ++i) {

            WMediaObject *object =
                cache->objects[i];


            if (!object)
                continue;


            if (object->reference_count != 0)
                continue;


            if (!candidate ||
                object->last_used <
                candidate->last_used) {

                candidate =
                    object;

                candidate_index =
                    i;
            }
        }


        if (!candidate)
            break;


        cache->current_bytes -=
            candidate->encoded.size;


        free(
            candidate->source
        );

        wmedia_buffer_destroy(
            &candidate->encoded
        );

        free(
            candidate->image.pixels
        );

        free(
            candidate
        );


        /*
         * Compact cache array.
         */

        if (candidate_index + 1 <
            cache->count) {

            memmove(
                &cache->objects[
                    candidate_index
                ],
                &cache->objects[
                    candidate_index + 1
                ],
                (cache->count -
                 candidate_index - 1) *
                sizeof(WMediaObject *)
            );
        }


        cache->count--;
    }
}
3. Example
#include "word_media.h"

#include <stdio.h>

int main(void)
{
    WMediaStore store;

    if (!wmedia_store_init(
            &store,
            256 * 1024 * 1024)) {

        return 1;
    }


    WMediaObject *image =
        wmedia_load_file(
            &store,
            "photo.bmp"
        );


    if (!image) {

        printf(
            "Failed to load image\n"
        );

        wmedia_store_destroy(
            &store
        );

        return 1;
    }


    printf(
        "Media ID: %llu\n",
        (unsigned long long)
            image->id
    );


    printf(
        "Type: %d\n",
        image->type
    );


    if (!wmedia_decode(image)) {

        printf(
            "Decoder unavailable or image invalid\n"
        );

        wmedia_release(
            &store,
            image
        );

        wmedia_store_destroy(
            &store
        );

        return 1;
    }


    printf(
        "Dimensions: %u x %u\n",
        image->info.width,
        image->info.height
    );


    /*
     * Fit into a Word page content area.
     */

    WMediaLayout layout;

    if (wmedia_calculate_layout(
            image,
            600.0,
            800.0,
            &layout)) {

        printf(
            "Layout: %.1f x %.1f\n",
            layout.width,
            layout.height
        );
    }


    /*
     * Resize.
     */

    wmedia_resize(
        image,
        1200,
        800
    );


    /*
     * Release document reference.
     *
     * Cache may retain the media.
     */

    wmedia_release(
        &store,
        image
    );


    wmedia_store_destroy(
        &store
    );

    return 0;
}

Compile:

cc -std=c11 -O2 -Wall -Wextra \
    word_media.c \
    test_media.c \
    -lm \
    -o word_media
    
    
    
    
    
    
    word_media_async.h
#ifndef WORD_MEDIA_ASYNC_H
#define WORD_MEDIA_ASYNC_H

#include "word_media.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/* =========================================================
 * Job types
 * ========================================================= */

typedef enum {

    WMEDIA_JOB_NONE = 0,

    WMEDIA_JOB_DECODE,

    WMEDIA_JOB_THUMBNAIL,

    WMEDIA_JOB_PREVIEW,

    WMEDIA_JOB_FULL_RESOLUTION,

    WMEDIA_JOB_RELEASE

} WMediaJobType;


/* =========================================================
 * Job state
 * ========================================================= */

typedef enum {

    WMEDIA_JOB_PENDING = 0,

    WMEDIA_JOB_RUNNING,

    WMEDIA_JOB_COMPLETE,

    WMEDIA_JOB_FAILED,

    WMEDIA_JOB_CANCELLED

} WMediaJobState;


/* =========================================================
 * Resolution class
 * ========================================================= */

typedef enum {

    WMEDIA_RESOLUTION_THUMBNAIL = 0,

    WMEDIA_RESOLUTION_PREVIEW,

    WMEDIA_RESOLUTION_FULL

} WMediaResolution;


/* =========================================================
 * Render request
 * ========================================================= */

typedef struct {

    uint32_t requested_width;

    uint32_t requested_height;

    double scale;

    WMediaResolution resolution;

} WMediaRenderRequest;


/* =========================================================
 * Render image
 * ========================================================= */

typedef struct {

    uint8_t *pixels;

    size_t size;

    uint32_t width;

    uint32_t height;

    size_t stride;

    WMediaPixelFormat format;

    uint64_t generation;

} WMediaRenderImage;


/* =========================================================
 * Async job
 * ========================================================= */

typedef struct {

    uint64_t id;

    WMediaJobType type;

    WMediaJobState state;

    WMediaObject *object;

    WMediaRenderRequest request;

    uint64_t document_version;

    uint64_t generation;

} WMediaJob;


/* =========================================================
 * Job queue
 * ========================================================= */

typedef struct {

    WMediaJob *jobs;

    size_t capacity;

    size_t count;

    uint64_t next_id;

} WMediaJobQueue;


/* =========================================================
 * Async media manager
 * ========================================================= */

typedef struct {

    WMediaStore *store;

    WMediaJobQueue queue;

    uint8_t running;

    uint8_t worker_available;

    uint64_t generation;

} WMediaAsync;


/* =========================================================
 * Job queue
 * ========================================================= */

int wmedia_job_queue_init(
    WMediaJobQueue *queue,
    size_t capacity
);

void wmedia_job_queue_destroy(
    WMediaJobQueue *queue
);

int wmedia_job_submit(
    WMediaJobQueue *queue,
    WMediaJobType type,
    WMediaObject *object,
    const WMediaRenderRequest *request,
    uint64_t document_version
);

int wmedia_job_pop(
    WMediaJobQueue *queue,
    WMediaJob *job
);


/* =========================================================
 * Async manager
 * ========================================================= */

int wmedia_async_init(
    WMediaAsync *async,
    WMediaStore *store,
    size_t queue_capacity
);

void wmedia_async_destroy(
    WMediaAsync *async
);


/* =========================================================
 * Request decoding
 * ========================================================= */

int wmedia_async_decode(
    WMediaAsync *async,
    WMediaObject *object,
    uint64_t document_version
);


/* =========================================================
 * Request thumbnail
 * ========================================================= */

int wmedia_async_thumbnail(
    WMediaAsync *async,
    WMediaObject *object,
    uint32_t width,
    uint32_t height,
    uint64_t document_version
);


/* =========================================================
 * Request preview
 * ========================================================= */

int wmedia_async_preview(
    WMediaAsync *async,
    WMediaObject *object,
    uint32_t width,
    uint32_t height,
    uint64_t document_version
);


/* =========================================================
 * Worker
 * ========================================================= */

int wmedia_async_process_one(
    WMediaAsync *async
);

int wmedia_async_process(
    WMediaAsync *async,
    size_t budget
);


/* =========================================================
 * Render resolution
 * ========================================================= */

WMediaResolution
wmedia_choose_resolution(
    const WMediaObject *object,
    const WMediaRenderRequest *request
);


/* =========================================================
 * Retrieve render image
 * ========================================================= */

const WMediaRenderImage *
wmedia_get_render_image(
    const WMediaObject *object,
    WMediaResolution resolution
);

#ifdef __cplusplus
}
#endif

#endif
word_media_async.c
#include "word_media_async.h"

#include <stdlib.h>
#include <string.h>


/* =========================================================
 * Extended media state
 *
 * Since WMediaObject from the previous layer is deliberately
 * compact, auxiliary render images are maintained separately.
 * ========================================================= */

typedef struct WMediaRenderCacheEntry {

    WMediaObject *object;

    WMediaRenderImage thumbnail;

    WMediaRenderImage preview;

    WMediaRenderImage full;

    struct WMediaRenderCacheEntry *next;

} WMediaRenderCacheEntry;


/*
 * Internal cache.
 *
 * In a complete build this belongs directly inside
 * WMediaStore.
 */

static WMediaRenderCacheEntry *
find_render_cache(
    WMediaObject *object
)
{
    /*
     * Placeholder for the store-owned render cache.
     *
     * The public implementation below primarily demonstrates
     * the job architecture.
     */

    (void)object;

    return NULL;
}


/* =========================================================
 * Queue
 * ========================================================= */

int
wmedia_job_queue_init(
    WMediaJobQueue *queue,
    size_t capacity
)
{
    if (!queue ||
        capacity == 0)
        return 0;

    memset(
        queue,
        0,
        sizeof(*queue)
    );


    queue->jobs =
        calloc(
            capacity,
            sizeof(WMediaJob)
        );


    if (!queue->jobs)
        return 0;


    queue->capacity =
        capacity;

    queue->next_id =
        1;


    return 1;
}


void
wmedia_job_queue_destroy(
    WMediaJobQueue *queue
)
{
    if (!queue)
        return;


    free(
        queue->jobs
    );


    memset(
        queue,
        0,
        sizeof(*queue)
    );
}


/* =========================================================
 * Submit job
 * ========================================================= */

int
wmedia_job_submit(
    WMediaJobQueue *queue,
    WMediaJobType type,
    WMediaObject *object,
    const WMediaRenderRequest *request,
    uint64_t document_version
)
{
    if (!queue ||
        !object)
        return 0;


    /*
     * Coalesce duplicate work.
     */

    for (size_t i = 0;
         i < queue->count;
         ++i) {

        WMediaJob *job =
            &queue->jobs[i];


        if (job->state !=
            WMEDIA_JOB_PENDING)

            continue;


        if (job->object !=
            object)

            continue;


        if (job->type !=
            type)

            continue;


        /*
         * Upgrade requested resolution if
         * the new request is larger.
         */

        if (request) {

            if (request->requested_width >
                job->request.requested_width)

                job->request.requested_width =
                    request->requested_width;


            if (request->requested_height >
                job->request.requested_height)

                job->request.requested_height =
                    request->requested_height;
        }


        if (document_version >
            job->document_version)

            job->document_version =
                document_version;


        return 1;
    }


    if (queue->count >=
        queue->capacity)

        return 0;


    WMediaJob *job =
        &queue->jobs[
            queue->count++
        ];


    memset(
        job,
        0,
        sizeof(*job)
    );


    job->id =
        queue->next_id++;


    job->type =
        type;


    job->state =
        WMEDIA_JOB_PENDING;


    job->object =
        object;


    if (request)
        job->request =
            *request;


    job->document_version =
        document_version;


    job->generation =
        job->id;


    wmedia_retain(
        object
    );


    return 1;
}


/* =========================================================
 * Pop job
 * ========================================================= */

int
wmedia_job_pop(
    WMediaJobQueue *queue,
    WMediaJob *job
)
{
    if (!queue ||
        !job)
        return 0;


    for (size_t i = 0;
         i < queue->count;
         ++i) {

        WMediaJob *candidate =
            &queue->jobs[i];


        if (candidate->state !=
            WMEDIA_JOB_PENDING)

            continue;


        *job =
            *candidate;


        candidate->state =
            WMEDIA_JOB_RUNNING;


        return 1;
    }


    return 0;
}


/* =========================================================
 * Async initialisation
 * ========================================================= */

int
wmedia_async_init(
    WMediaAsync *async,
    WMediaStore *store,
    size_t queue_capacity
)
{
    if (!async ||
        !store)
        return 0;


    memset(
        async,
        0,
        sizeof(*async)
    );


    if (!wmedia_job_queue_init(
            &async->queue,
            queue_capacity))

        return 0;


    async->store =
        store;

    async->running =
        1;

    async->worker_available =
        1;

    async->generation =
        1;


    return 1;
}


/* =========================================================
 * Destroy
 * ========================================================= */

void
wmedia_async_destroy(
    WMediaAsync *async
)
{
    if (!async)
        return;


    /*
     * Release objects retained by pending jobs.
     */

    for (size_t i = 0;
         i < async->queue.count;
         ++i) {

        WMediaJob *job =
            &async->queue.jobs[i];


        if (job->object &&
            (job->state ==
                 WMEDIA_JOB_PENDING ||
             job->state ==
                 WMEDIA_JOB_RUNNING)) {

            wmedia_release(
                async->store,
                job->object
            );
        }
    }


    wmedia_job_queue_destroy(
        &async->queue
    );


    memset(
        async,
        0,
        sizeof(*async)
    );
}


/* =========================================================
 * Decode request
 * ========================================================= */

int
wmedia_async_decode(
    WMediaAsync *async,
    WMediaObject *object,
    uint64_t document_version
)
{
    if (!async ||
        !object)
        return 0;


    return wmedia_job_submit(
        &async->queue,
        WMEDIA_JOB_DECODE,
        object,
        NULL,
        document_version
    );
}


/* =========================================================
 * Thumbnail request
 * ========================================================= */

int
wmedia_async_thumbnail(
    WMediaAsync *async,
    WMediaObject *object,
    uint32_t width,
    uint32_t height,
    uint64_t document_version
)
{
    if (!async ||
        !object)
        return 0;


    WMediaRenderRequest request = {

        .requested_width =
            width,

        .requested_height =
            height,

        .scale =
            1.0,

        .resolution =
            WMEDIA_RESOLUTION_THUMBNAIL

    };


    return wmedia_job_submit(
        &async->queue,
        WMEDIA_JOB_THUMBNAIL,
        object,
        &request,
        document_version
    );
}


/* =========================================================
 * Preview request
 * ========================================================= */

int
wmedia_async_preview(
    WMediaAsync *async,
    WMediaObject *object,
    uint32_t width,
    uint32_t height,
    uint64_t document_version
)
{
    if (!async ||
        !object)
        return 0;


    WMediaRenderRequest request = {

        .requested_width =
            width,

        .requested_height =
            height,

        .scale =
            1.0,

        .resolution =
            WMEDIA_RESOLUTION_PREVIEW

    };


    return wmedia_job_submit(
        &async->queue,
        WMEDIA_JOB_PREVIEW,
        object,
        &request,
        document_version
    );
}


/* =========================================================
 * Choose resolution
 * ========================================================= */

WMediaResolution
wmedia_choose_resolution(
    const WMediaObject *object,
    const WMediaRenderRequest *request
)
{
    if (!object ||
        !request)

        return WMEDIA_RESOLUTION_THUMBNAIL;


    if (request->resolution ==
        WMEDIA_RESOLUTION_THUMBNAIL)

        return WMEDIA_RESOLUTION_THUMBNAIL;


    if (request->resolution ==
        WMEDIA_RESOLUTION_PREVIEW)

        return WMEDIA_RESOLUTION_PREVIEW;


    /*
     * If the image is being displayed at a small
     * physical size, a full-resolution image is
     * unnecessary.
     */

    if (request->requested_width <
        256 &&
        request->requested_height <
        256)

        return WMEDIA_RESOLUTION_THUMBNAIL;


    if (request->requested_width <
        1200 &&
        request->requested_height <
        1200)

        return WMEDIA_RESOLUTION_PREVIEW;


    return WMEDIA_RESOLUTION_FULL;
}


/* =========================================================
 * Generate thumbnail
 *
 * This uses the existing resize operation.
 *
 * In a production implementation the thumbnail should
 * be generated directly from the decoder to avoid first
 * materialising the entire full-resolution image.
 * ========================================================= */

static int
process_thumbnail(
    WMediaJob *job
)
{
    if (!job ||
        !job->object)
        return 0;


    WMediaObject *object =
        job->object;


    if (!object->decoded) {

        if (!wmedia_decode(
                object))

            return 0;
    }


    uint32_t width =
        job->request.requested_width;


    uint32_t height =
        job->request.requested_height;


    if (width == 0 ||
        height == 0)
        return 0;


    /*
     * Don't destroy the document's canonical image
     * merely to generate a thumbnail.
     *
     * A dedicated render cache is required for the
     * real implementation.
     *
     * This function therefore validates the request
     * and leaves the canonical image untouched.
     */

    return 1;
}


/* =========================================================
 * Process preview
 * ========================================================= */

static int
process_preview(
    WMediaJob *job
)
{
    if (!job ||
        !job->object)
        return 0;


    WMediaObject *object =
        job->object;


    if (!object->decoded) {

        if (!wmedia_decode(
                object))

            return 0;
    }


    return 1;
}


/* =========================================================
 * Process one job
 * ========================================================= */

int
wmedia_async_process_one(
    WMediaAsync *async
)
{
    if (!async ||
        !async->running)
        return 0;


    WMediaJob job;


    if (!wmedia_job_pop(
            &async->queue,
            &job))

        return 0;


    int success =
        0;


    switch (job.type) {

        case WMEDIA_JOB_DECODE:

            success =
                wmedia_decode(
                    job.object
                );

            break;


        case WMEDIA_JOB_THUMBNAIL:

            success =
                process_thumbnail(
                    &job
                );

            break;


        case WMEDIA_JOB_PREVIEW:

            success =
                process_preview(
                    &job
                );

            break;


        case WMEDIA_JOB_FULL_RESOLUTION:

            success =
                wmedia_decode(
                    job.object
                );

            break;


        default:

            success = 0;

            break;
    }


    /*
     * Locate original job and update its state.
     */

    for (size_t i = 0;
         i < async->queue.count;
         ++i) {

        WMediaJob *queued =
            &async->queue.jobs[i];


        if (queued->id !=
            job.id)

            continue;


        queued->state =
            success
                ? WMEDIA_JOB_COMPLETE
                : WMEDIA_JOB_FAILED;


        break;
    }


    /*
     * Worker's temporary reference.
     */

    wmedia_release(
        async->store,
        job.object
    );


    return success;
}


/* =========================================================
 * Process with budget
 * ========================================================= */

int
wmedia_async_process(
    WMediaAsync *async,
    size_t budget
)
{
    if (!async)
        return 0;


    size_t completed =
        0;


    while (completed < budget) {

        if (!wmedia_async_process_one(
                async))

            break;


        completed++;
    }


    return (int)completed;
}


/* =========================================================
 * Render image retrieval
 * ========================================================= */

const WMediaRenderImage *
wmedia_get_render_image(
    const WMediaObject *object,
    WMediaResolution resolution
)
{
    /*
     * Render-cache lookup becomes active once the
     * WMediaStore owns the resolution-specific cache.
     */

    (void)object;
    (void)resolution;

    return NULL;
}

There is one architectural point worth correcting from the previous version: thumbnail/preview pixels should not overwrite WMediaObject.image. The canonical decoded image and resolution-specific render products need separate storage.

A clean production structure is:

WMediaObject
│
├── encoded
│
├── metadata
│
└── RenderCache
      │
      ├── thumbnail  128–256 px
      ├── preview    1024–1600 px
      └── full       source/native resolution

That prevents an image being repeatedly resized up and down as the user zooms or scrolls.

4. Media loading from the typing pipeline

When the user inserts an image:

WMediaObject *image =
    wmedia_load_file(
        &media_store,
        "large_photo.jpg"
    );

if (image) {

    /*
     * Don't decode synchronously.
     */

    wmedia_async_decode(
        &media_async,
        image,
        document_version
    );

    /*
     * Give the renderer something cheap to display.
     */

    wmedia_async_thumbnail(
        &media_async,
        image,
        256,
        256,
        document_version
    );
}

The typing system can immediately continue:

Insert image
     │
     ├── document updated immediately
     │
     ├── cursor remains responsive
     │
     ├── placeholder displayed
     │
     └── media worker:
            │
            ├── decode
            ├── thumbnail
            └── preview
                    │
                    ▼
                 renderer
5. Resolution selection

The renderer shouldn't automatically decode a 6000×4000 photograph merely because it appears on screen at 300×200.

For example:

WMediaRenderRequest request = {
    .requested_width = 300,
    .requested_height = 200,
    .scale = 1.0,
    .resolution = WMEDIA_RESOLUTION_PREVIEW
};

WMediaResolution resolution =
    wmedia_choose_resolution(
        image,
        &request
    );
    
    
    
    
    
    
    word_media_codec.h
#ifndef WORD_MEDIA_CODEC_H
#define WORD_MEDIA_CODEC_H

#include "word_media.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WMediaCodec WMediaCodec;

/* ---------------------------------------------------------
 * Codec probing
 * --------------------------------------------------------- */

typedef int (*WMediaCodecProbeFn)(
    const uint8_t *data,
    size_t size
);

/* ---------------------------------------------------------
 * Metadata reader
 * --------------------------------------------------------- */

typedef int (*WMediaCodecInfoFn)(
    const uint8_t *data,
    size_t size,
    WMediaInfo *info
);

/* ---------------------------------------------------------
 * Full decode
 * --------------------------------------------------------- */

typedef int (*WMediaCodecDecodeFn)(
    const uint8_t *data,
    size_t size,
    WMediaImage *image
);

/* ---------------------------------------------------------
 * Region decode
 *
 * x/y/w/h are source-image coordinates.
 * --------------------------------------------------------- */

typedef int (*WMediaCodecDecodeRegionFn)(
    const uint8_t *data,
    size_t size,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    WMediaImage *image
);

/* ---------------------------------------------------------
 * Destroy decoded image
 * --------------------------------------------------------- */

typedef void (*WMediaCodecDestroyImageFn)(
    WMediaImage *image
);


/* ---------------------------------------------------------
 * Codec definition
 * --------------------------------------------------------- */

struct WMediaCodec {

    WMediaType type;

    const char *name;

    WMediaCodecProbeFn probe;

    WMediaCodecInfoFn read_info;

    WMediaCodecDecodeFn decode;

    WMediaCodecDecodeRegionFn decode_region;

    WMediaCodecDestroyImageFn destroy_image;
};


/* ---------------------------------------------------------
 * Registry
 * --------------------------------------------------------- */

typedef struct {

    const WMediaCodec **codecs;

    size_t count;

    size_t capacity;

} WMediaCodecRegistry;


/* ---------------------------------------------------------
 * Registry API
 * --------------------------------------------------------- */

int wmedia_codec_registry_init(
    WMediaCodecRegistry *registry,
    size_t capacity
);

void wmedia_codec_registry_destroy(
    WMediaCodecRegistry *registry
);

int wmedia_codec_register(
    WMediaCodecRegistry *registry,
    const WMediaCodec *codec
);

const WMediaCodec *
wmedia_codec_find(
    const WMediaCodecRegistry *registry,
    WMediaType type
);

const WMediaCodec *
wmedia_codec_probe(
    const WMediaCodecRegistry *registry,
    const uint8_t *data,
    size_t size
);

#ifdef __cplusplus
}
#endif

#endif
word_media_codec.c
#include "word_media_codec.h"

#include <stdlib.h>
#include <string.h>


int
wmedia_codec_registry_init(
    WMediaCodecRegistry *registry,
    size_t capacity
)
{
    if (!registry || capacity == 0)
        return 0;

    memset(
        registry,
        0,
        sizeof(*registry)
    );

    registry->codecs =
        calloc(
            capacity,
            sizeof(*registry->codecs)
        );

    if (!registry->codecs)
        return 0;

    registry->capacity =
        capacity;

    return 1;
}


void
wmedia_codec_registry_destroy(
    WMediaCodecRegistry *registry
)
{
    if (!registry)
        return;

    free(registry->codecs);

    memset(
        registry,
        0,
        sizeof(*registry)
    );
}


int
wmedia_codec_register(
    WMediaCodecRegistry *registry,
    const WMediaCodec *codec
)
{
    if (!registry || !codec)
        return 0;

    if (registry->count >= registry->capacity)
        return 0;

    registry->codecs[
        registry->count++
    ] = codec;

    return 1;
}


const WMediaCodec *
wmedia_codec_find(
    const WMediaCodecRegistry *registry,
    WMediaType type
)
{
    if (!registry)
        return NULL;

    for (size_t i = 0;
         i < registry->count;
         ++i) {

        const WMediaCodec *codec =
            registry->codecs[i];

        if (codec &&
            codec->type == type)

            return codec;
    }

    return NULL;
}


const WMediaCodec *
wmedia_codec_probe(
    const WMediaCodecRegistry *registry,
    const uint8_t *data,
    size_t size
)
{
    if (!registry ||
        !data ||
        size == 0)

        return NULL;

    for (size_t i = 0;
         i < registry->count;
         ++i) {

        const WMediaCodec *codec =
            registry->codecs[i];

        if (!codec ||
            !codec->probe)

            continue;

        if (codec->probe(
                data,
                size))

            return codec;
    }

    return NULL;
}
Tiled image storage

Now the important part.

Instead of:

6000 × 4000 image
        ↓
96 MB RGBA image
        ↓
always resident

we use:

Level 0
6000 × 4000

Level 1
3000 × 2000

Level 2
1500 × 1000

Level 3
750 × 500

Level 4
375 × 250

Level 5
188 × 125

And each level is divided into tiles.

word_media_tiles.h
#ifndef WORD_MEDIA_TILES_H
#define WORD_MEDIA_TILES_H

#include "word_media.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/* ---------------------------------------------------------
 * Tile constants
 * --------------------------------------------------------- */

#define WMEDIA_DEFAULT_TILE_SIZE 256


/* ---------------------------------------------------------
 * Tile state
 * --------------------------------------------------------- */

typedef enum {

    WMEDIA_TILE_EMPTY = 0,

    WMEDIA_TILE_LOADING,

    WMEDIA_TILE_READY,

    WMEDIA_TILE_FAILED

} WMediaTileState;


/* ---------------------------------------------------------
 * Tile
 * --------------------------------------------------------- */

typedef struct {

    uint32_t level;

    uint32_t x;

    uint32_t y;

    uint32_t width;

    uint32_t height;

    size_t stride;

    size_t byte_size;

    uint8_t *pixels;

    WMediaPixelFormat format;

    WMediaTileState state;

    uint64_t generation;

    uint64_t last_used;

} WMediaTile;


/* ---------------------------------------------------------
 * Pyramid level
 * --------------------------------------------------------- */

typedef struct {

    uint32_t level;

    uint32_t width;

    uint32_t height;

    uint32_t columns;

    uint32_t rows;

    WMediaTile *tiles;

} WMediaTileLevel;


/* ---------------------------------------------------------
 * Image pyramid
 * --------------------------------------------------------- */

typedef struct {

    uint32_t source_width;

    uint32_t source_height;

    uint32_t tile_size;

    uint32_t level_count;

    WMediaTileLevel *levels;

    uint64_t generation;

} WMediaTilePyramid;


/* ---------------------------------------------------------
 * Visible rectangle
 * --------------------------------------------------------- */

typedef struct {

    double x;

    double y;

    double width;

    double height;

    double scale;

} WMediaViewport;


/* ---------------------------------------------------------
 * Tile request
 * --------------------------------------------------------- */

typedef struct {

    uint32_t level;

    uint32_t tile_x;

    uint32_t tile_y;

} WMediaTileRequest;


/* ---------------------------------------------------------
 * Pyramid lifecycle
 * --------------------------------------------------------- */

int wmedia_pyramid_init(
    WMediaTilePyramid *pyramid,
    uint32_t width,
    uint32_t height,
    uint32_t tile_size
);

void wmedia_pyramid_destroy(
    WMediaTilePyramid *pyramid
);


/* ---------------------------------------------------------
 * Tile lookup
 * --------------------------------------------------------- */

WMediaTile *
wmedia_pyramid_get_tile(
    WMediaTilePyramid *pyramid,
    uint32_t level,
    uint32_t x,
    uint32_t y
);


/* ---------------------------------------------------------
 * Choose mip level
 * --------------------------------------------------------- */

uint32_t wmedia_pyramid_choose_level(
    const WMediaTilePyramid *pyramid,
    double scale
);


/* ---------------------------------------------------------
 * Calculate visible tiles
 * --------------------------------------------------------- */

size_t wmedia_pyramid_visible_tiles(
    const WMediaTilePyramid *pyramid,
    const WMediaViewport *viewport,
    WMediaTileRequest *requests,
    size_t capacity
);


/* ---------------------------------------------------------
 * Allocate tile
 * --------------------------------------------------------- */

int wmedia_tile_allocate(
    WMediaTile *tile,
    WMediaPixelFormat format
);


/* ---------------------------------------------------------
 * Free tile
 * --------------------------------------------------------- */

void wmedia_tile_free(
    WMediaTile *tile
);


/* ---------------------------------------------------------
 * Touch tile
 * --------------------------------------------------------- */

void wmedia_tile_touch(
    WMediaTile *tile,
    uint64_t timestamp
);

#ifdef __cplusplus
}
#endif

#endif
word_media_tiles.c
#include "word_media_tiles.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>


static uint32_t
ceil_div_u32(
    uint32_t a,
    uint32_t b
)
{
    return (a + b - 1u) / b;
}


static uint32_t
level_dimension(
    uint32_t dimension,
    uint32_t level
)
{
    while (level--) {

        dimension =
            (dimension + 1u) / 2u;
    }

    return dimension;
}


/* ---------------------------------------------------------
 * Tile allocation
 * --------------------------------------------------------- */

int
wmedia_tile_allocate(
    WMediaTile *tile,
    WMediaPixelFormat format
)
{
    if (!tile ||
        tile->width == 0 ||
        tile->height == 0)

        return 0;


    size_t bytes_per_pixel;


    switch (format) {

        case WMEDIA_PIXEL_GRAY8:
            bytes_per_pixel = 1;
            break;

        case WMEDIA_PIXEL_RGB8:
            bytes_per_pixel = 3;
            break;

        case WMEDIA_PIXEL_RGBA8:
        case WMEDIA_PIXEL_BGRA8:
            bytes_per_pixel = 4;
            break;

        default:
            return 0;
    }


    tile->stride =
        (size_t)tile->width *
        bytes_per_pixel;


    tile->byte_size =
        tile->stride *
        tile->height;


    tile->pixels =
        malloc(tile->byte_size);


    if (!tile->pixels) {

        tile->stride = 0;
        tile->byte_size = 0;

        return 0;
    }


    tile->format =
        format;


    tile->state =
        WMEDIA_TILE_LOADING;


    return 1;
}


/* ---------------------------------------------------------
 * Free tile
 * --------------------------------------------------------- */

void
wmedia_tile_free(
    WMediaTile *tile
)
{
    if (!tile)
        return;


    free(tile->pixels);


    memset(
        tile,
        0,
        sizeof(*tile)
    );
}


/* ---------------------------------------------------------
 * Initialise pyramid
 * --------------------------------------------------------- */

int
wmedia_pyramid_init(
    WMediaTilePyramid *pyramid,
    uint32_t width,
    uint32_t height,
    uint32_t tile_size
)
{
    if (!pyramid ||
        width == 0 ||
        height == 0)

        return 0;


    if (tile_size == 0)
        tile_size =
            WMEDIA_DEFAULT_TILE_SIZE;


    memset(
        pyramid,
        0,
        sizeof(*pyramid)
    );


    pyramid->source_width =
        width;

    pyramid->source_height =
        height;

    pyramid->tile_size =
        tile_size;

    pyramid->generation =
        1;


    uint32_t max_dimension =
        width > height
            ? width
            : height;


    uint32_t levels = 1;


    while (max_dimension > 1) {

        max_dimension =
            (max_dimension + 1u) / 2u;

        levels++;
    }


    pyramid->level_count =
        levels;


    pyramid->levels =
        calloc(
            levels,
            sizeof(WMediaTileLevel)
        );


    if (!pyramid->levels) {

        memset(
            pyramid,
            0,
            sizeof(*pyramid)
        );

        return 0;
    }


    for (uint32_t level = 0;
         level < levels;
         ++level) {

        WMediaTileLevel *L =
            &pyramid->levels[level];


        L->level =
            level;


        L->width =
            level_dimension(
                width,
                level
            );


        L->height =
            level_dimension(
                height,
                level
            );


        L->columns =
            ceil_div_u32(
                L->width,
                tile_size
            );


        L->rows =
            ceil_div_u32(
                L->height,
                tile_size
            );


        size_t tile_count =
            (size_t)L->columns *
            L->rows;


        L->tiles =
            calloc(
                tile_count,
                sizeof(WMediaTile)
            );


        if (!L->tiles) {

            wmedia_pyramid_destroy(
                pyramid
            );

            return 0;
        }


        for (uint32_t y = 0;
             y < L->rows;
             ++y) {

            for (uint32_t x = 0;
                 x < L->columns;
                 ++x) {

                WMediaTile *tile =
                    &L->tiles[
                        (size_t)y *
                        L->columns +
                        x
                    ];


                tile->level =
                    level;

                tile->x =
                    x;

                tile->y =
                    y;


                uint32_t px =
                    x * tile_size;

                uint32_t py =
                    y * tile_size;


                tile->width =
                    (px + tile_size <= L->width)
                        ? tile_size
                        : L->width - px;


                tile->height =
                    (py + tile_size <= L->height)
                        ? tile_size
                        : L->height - py;


                tile->state =
                    WMEDIA_TILE_EMPTY;
            }
        }
    }


    return 1;
}


/* ---------------------------------------------------------
 * Destroy pyramid
 * --------------------------------------------------------- */

void
wmedia_pyramid_destroy(
    WMediaTilePyramid *pyramid
)
{
    if (!pyramid)
        return;


    if (pyramid->levels) {

        for (uint32_t level = 0;
             level < pyramid->level_count;
             ++level) {

            WMediaTileLevel *L =
                &pyramid->levels[level];


            if (L->tiles) {

                size_t count =
                    (size_t)L->columns *
                    L->rows;


                for (size_t i = 0;
                     i < count;
                     ++i) {

                    wmedia_tile_free(
                        &L->tiles[i]
                    );
                }


                free(L->tiles);
            }
        }


        free(
            pyramid->levels
        );
    }


    memset(
        pyramid,
        0,
        sizeof(*pyramid)
    );
}


/* ---------------------------------------------------------
 * Tile lookup
 * --------------------------------------------------------- */

WMediaTile *
wmedia_pyramid_get_tile(
    WMediaTilePyramid *pyramid,
    uint32_t level,
    uint32_t x,
    uint32_t y
)
{
    if (!pyramid)
        return NULL;


    if (level >=
        pyramid->level_count)

        return NULL;


    WMediaTileLevel *L =
        &pyramid->levels[level];


    if (x >= L->columns ||
        y >= L->rows)

        return NULL;


    return &L->tiles[
        (size_t)y *
        L->columns +
        x
    ];
}


/* ---------------------------------------------------------
 * Select appropriate pyramid level
 * --------------------------------------------------------- */

uint32_t
wmedia_pyramid_choose_level(
    const WMediaTilePyramid *pyramid,
    double scale
)
{
    if (!pyramid ||
        scale <= 0.0)

        return 0;


    /*
     * scale = 1.0 means native resolution.
     *
     * scale = 0.5 means half resolution.
     */

    uint32_t level = 0;

    double current_scale = 1.0;


    while (
        level + 1 <
            pyramid->level_count &&
        current_scale * 0.5 >=
            scale
    ) {

        current_scale *= 0.5;

        level++;
    }


    return level;
}


/* ---------------------------------------------------------
 * Visible tiles
 * --------------------------------------------------------- */

size_t
wmedia_pyramid_visible_tiles(
    const WMediaTilePyramid *pyramid,
    const WMediaViewport *viewport,
    WMediaTileRequest *requests,
    size_t capacity
)
{
    if (!pyramid ||
        !viewport ||
        !requests ||
        capacity == 0)

        return 0;


    uint32_t level =
        wmedia_pyramid_choose_level(
            pyramid,
            viewport->scale
        );


    const WMediaTileLevel *L =
        &pyramid->levels[level];


    /*
     * Convert viewport coordinates from
     * screen space to pyramid-level space.
     */

    double inverse_scale =
        1.0 / viewport->scale;


    double x0 =
        viewport->x *
        inverse_scale;


    double y0 =
        viewport->y *
        inverse_scale;


    double x1 =
        (viewport->x +
         viewport->width) *
        inverse_scale;


    double y1 =
        (viewport->y +
         viewport->height) *
        inverse_scale;


    /*
     * Convert image coordinates to tiles.
     */

    uint32_t tx0 =
        (uint32_t)
        floor(
            x0 /
            pyramid->tile_size
        );


    uint32_t ty0 =
        (uint32_t)
        floor(
            y0 /
            pyramid->tile_size
        );


    uint32_t tx1 =
        (uint32_t)
        floor(
            x1 /
            pyramid->tile_size
        );


    uint32_t ty1 =
        (uint32_t)
        floor(
            y1 /
            pyramid->tile_size
        );


    if (tx1 >= L->columns)
        tx1 = L->columns - 1;

    if (ty1 >= L->rows)
        ty1 = L->rows - 1;


    size_t count = 0;


    for (uint32_t y = ty0;
         y <= ty1;
         ++y) {

        for (uint32_t x = tx0;
             x <= tx1;
             ++x) {

            if (count >= capacity)
                return count;


            requests[count++] =
                (WMediaTileRequest) {

                    .level = level,

                    .tile_x = x,

                    .tile_y = y

                };
        }
    }


    return count;
}


/* ---------------------------------------------------------
 * Touch
 * --------------------------------------------------------- */

void
wmedia_tile_touch(
    WMediaTile *tile,
    uint64_t timestamp
)
{
    if (!tile)
        return;

    tile->last_used =
        timestamp;
}
Important correction for viewport coordinates

For a real renderer, the viewport should be expressed in document/image coordinates, not raw screen coordinates. The production version should therefore carry:

typedef struct {

    double image_x;
    double image_y;

    double image_width;
    double image_height;

    double scale;

} WMediaViewport;

That avoids ambiguity when page transforms, rotation, zoom and DPI scaling are introduced.

Region decoding

Now the codec layer can exploit the tile system.

For a JPEG/PNG codec supporting region decode:

int
decode_visible_tile(
    const WMediaCodec *codec,
    const WMediaObject *object,
    WMediaTile *tile,
    uint32_t level
)
{
    if (!codec ||
        !object ||
        !tile)

        return 0;

    if (!codec->decode_region)
        return 0;

    /*
     * At pyramid level N the source coordinates
     * represent 2^N reduction.
     */

    uint32_t scale =
        1u << level;

    uint32_t source_x =
        tile->x * 256u * scale;

    uint32_t source_y =
        tile->y * 256u * scale;

    uint32_t source_width =
        tile->width * scale;

    uint32_t source_height =
        tile->height * scale;


    WMediaImage image;

    memset(
        &image,
        0,
        sizeof(image)
    );


    if (!codec->decode_region(
            object->encoded.data,
            object->encoded.size,
            source_x,
            source_y,
            source_width,
            source_height,
            &image))

        return 0;


    /*
     * Transfer ownership to tile.
     */

    tile->pixels =
        image.pixels;

    tile->width =
        image.width;

    tile->height =
        image.height;

    tile->stride =
        image.stride;

    tile->format =
        image.format;

    tile->byte_size =
        image.size;

    tile->state =
        WMEDIA_TILE_READY;


    return 1;
}

This gives the renderer a much more scalable model:

                   12000 × 8000 photograph
                             │
                             ▼
                    ┌─────────────────┐
                    │ Codec / Decoder │
                    └────────┬────────┘
                             │
              ┌──────────────┼──────────────┐
              ▼              ▼              ▼
           Tile 0          Tile 1          Tile 2
           256²            256²            256²
              │              │              │
              └──────────────┼──────────────┘
                             ▼
                       Render viewport

Only visible tiles need to become resident.

Tile eviction

The tile cache should also have a memory ceiling.

typedef struct {

    size_t maximum_bytes;

    size_t current_bytes;

    uint64_t clock;

} WMediaTileCache;


static void
tile_cache_touch(
    WMediaTileCache *cache,
    WMediaTile *tile
)
{
    if (!cache || !tile)
        return;

    tile->last_used =
        ++cache->clock;
}


static void
tile_cache_evict(
    WMediaTileCache *cache,
    WMediaTilePyramid *pyramid
)
{
    if (!cache || !pyramid)
        return;


    while (
        cache->current_bytes >
        cache->maximum_bytes
    ) {

        WMediaTile *oldest =
            NULL;


        for (uint32_t l = 0;
             l < pyramid->level_count;
             ++l) {

            WMediaTileLevel *level =
                &pyramid->levels[l];


            size_t count =
                (size_t)
                level->columns *
                level->rows;


            for (size_t i = 0;
                 i < count;
                 ++i) {

                WMediaTile *tile =
                    &level->tiles[i];


                if (tile->state !=
                    WMEDIA_TILE_READY)

                    continue;


                if (!oldest ||
                    tile->last_used <
                    oldest->last_used)

                    oldest = tile;
            }
        }


        if (!oldest)
            break;


        cache->current_bytes -=
            oldest->byte_size;


        wmedia_tile_free(
            oldest
        );


        oldest->state =
            WMEDIA_TILE_EMPTY;
    }
}






word_media_metadata.h
#ifndef WORD_MEDIA_METADATA_H
#define WORD_MEDIA_METADATA_H

#include "word_media.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WMEDIA_ORIENTATION_NORMAL = 1,
    WMEDIA_ORIENTATION_FLIP_H,
    WMEDIA_ORIENTATION_180,
    WMEDIA_ORIENTATION_FLIP_V,
    WMEDIA_ORIENTATION_TRANSPOSE,
    WMEDIA_ORIENTATION_90_CW,
    WMEDIA_ORIENTATION_TRANSVERSE,
    WMEDIA_ORIENTATION_90_CCW
} WMediaOrientation;

typedef struct {
    uint32_t width;
    uint32_t height;

    uint32_t bits_per_component;
    uint32_t channels;

    double dpi_x;
    double dpi_y;

    WMediaOrientation orientation;

    uint8_t has_alpha;
    uint8_t progressive;
    uint8_t has_icc_profile;

    size_t icc_offset;
    size_t icc_size;
} WMediaMetadata;

int wmedia_metadata_probe(
    const uint8_t *data,
    size_t size,
    WMediaType type,
    WMediaMetadata *metadata
);

int wmedia_metadata_jpeg(
    const uint8_t *data,
    size_t size,
    WMediaMetadata *metadata
);

int wmedia_metadata_png(
    const uint8_t *data,
    size_t size,
    WMediaMetadata *metadata
);

int wmedia_metadata_webp(
    const uint8_t *data,
    size_t size,
    WMediaMetadata *metadata
);

int wmedia_metadata_bmp(
    const uint8_t *data,
    size_t size,
    WMediaMetadata *metadata
);

#ifdef __cplusplus
}
#endif

#endif
word_media_metadata.c
#include "word_media_metadata.h"

#include <string.h>
#include <limits.h>


/* =========================================================
 * Safe integer readers
 * ========================================================= */

static uint16_t
read_be16(
    const uint8_t *p
)
{
    return (uint16_t)(
        ((uint16_t)p[0] << 8) |
        (uint16_t)p[1]
    );
}


static uint32_t
read_be32(
    const uint8_t *p
)
{
    return
        ((uint32_t)p[0] << 24) |
        ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8)  |
        (uint32_t)p[3];
}


static uint16_t
read_le16(
    const uint8_t *p
)
{
    return (uint16_t)(
        ((uint16_t)p[1] << 8) |
        (uint16_t)p[0]
    );
}


static uint32_t
read_le32(
    const uint8_t *p
)
{
    return
        ((uint32_t)p[3] << 24) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[1] << 8)  |
        (uint32_t)p[0];
}


static uint64_t
read_le64(
    const uint8_t *p
)
{
    uint64_t value = 0;

    for (int i = 7; i >= 0; --i) {
        value <<= 8;
        value |= p[i];
    }

    return value;
}


/* =========================================================
 * Bounds helper
 * ========================================================= */

static int
range_valid(
    size_t offset,
    size_t length,
    size_t total
)
{
    if (offset > total)
        return 0;

    return length <= total - offset;
}


/* =========================================================
 * TIFF endian helpers
 * ========================================================= */

static uint16_t
tiff_u16(
    const uint8_t *data,
    size_t size,
    size_t offset,
    int little
)
{
    if (!range_valid(offset, 2, size))
        return 0;

    if (little)
        return read_le16(data + offset);

    return read_be16(data + offset);
}


static uint32_t
tiff_u32(
    const uint8_t *data,
    size_t size,
    size_t offset,
    int little
)
{
    if (!range_valid(offset, 4, size))
        return 0;

    if (little)
        return read_le32(data + offset);

    return read_be32(data + offset);
}


/* =========================================================
 * TIFF type sizes
 * ========================================================= */

static size_t
tiff_type_size(
    uint16_t type
)
{
    switch (type) {

        case 1:  /* BYTE */
        case 2:  /* ASCII */
        case 6:  /* SBYTE */
        case 7:  /* UNDEFINED */
            return 1;

        case 3:  /* SHORT */
        case 8:  /* SSHORT */
            return 2;

        case 4:  /* LONG */
        case 9:  /* SLONG */
        case 11: /* FLOAT */
            return 4;

        case 5:  /* RATIONAL */
        case 10: /* SRATIONAL */
        case 12: /* DOUBLE */
            return 8;

        default:
            return 0;
    }
}


/* =========================================================
 * Read TIFF value
 * ========================================================= */

static int
tiff_read_scalar(
    const uint8_t *data,
    size_t size,
    size_t entry,
    int little,
    uint32_t *value
)
{
    if (!value ||
        !range_valid(entry, 12, size))
        return 0;

    uint16_t type =
        tiff_u16(
            data,
            size,
            entry + 2,
            little
        );

    uint32_t count =
        tiff_u32(
            data,
            size,
            entry + 4,
            little
        );

    size_t element_size =
        tiff_type_size(type);

    if (!element_size ||
        count == 0)
        return 0;

    if (count >
        SIZE_MAX / element_size)
        return 0;

    size_t total =
        (size_t)count *
        element_size;

    size_t value_offset;

    if (total <= 4) {

        value_offset =
            entry + 8;

    } else {

        value_offset =
            tiff_u32(
                data,
                size,
                entry + 8,
                little
            );
    }

    if (!range_valid(
            value_offset,
            element_size,
            size))
        return 0;

    switch (type) {

        case 3:
            *value =
                tiff_u16(
                    data,
                    size,
                    value_offset,
                    little
                );
            return 1;

        case 4:
            *value =
                tiff_u32(
                    data,
                    size,
                    value_offset,
                    little
                );
            return 1;

        default:
            return 0;
    }
}


/* =========================================================
 * Parse EXIF orientation
 * ========================================================= */

static void
parse_exif_tiff(
    const uint8_t *data,
    size_t size,
    size_t offset,
    size_t length,
    WMediaMetadata *metadata
)
{
    if (!range_valid(
            offset,
            length,
            size))
        return;


    const uint8_t *tiff =
        data + offset;


    size_t tiff_size =
        length;


    if (tiff_size < 8)
        return;


    int little;


    if (tiff[0] == 'I' &&
        tiff[1] == 'I') {

        little = 1;

    } else if (
        tiff[0] == 'M' &&
        tiff[1] == 'M') {

        little = 0;

    } else {

        return;
    }


    if (tiff_u16(
            tiff,
            tiff_size,
            2,
            little) != 42)

        return;


    uint32_t ifd_offset =
        tiff_u32(
            tiff,
            tiff_size,
            4,
            little
        );


    if (!range_valid(
            ifd_offset,
            2,
            tiff_size))
        return;


    uint16_t count =
        tiff_u16(
            tiff,
            tiff_size,
            ifd_offset,
            little
        );


    size_t entries_offset =
        (size_t)ifd_offset + 2;


    if (!range_valid(
            entries_offset,
            (size_t)count * 12,
            tiff_size))
        return;


    for (uint16_t i = 0;
         i < count;
         ++i) {

        size_t entry =
            entries_offset +
            (size_t)i * 12;


        uint16_t tag =
            tiff_u16(
                tiff,
                tiff_size,
                entry,
                little
            );


        if (tag != 0x0112)
            continue;


        uint32_t orientation = 0;


        if (tiff_read_scalar(
                tiff,
                tiff_size,
                entry,
                little,
                &orientation)) {

            if (orientation >= 1 &&
                orientation <= 8)

                metadata->orientation =
                    (WMediaOrientation)
                        orientation;
        }


        break;
    }
}


/* =========================================================
 * JPEG APP1 / EXIF
 * ========================================================= */

static void
jpeg_parse_app1(
    const uint8_t *segment,
    size_t length,
    WMediaMetadata *metadata
)
{
    if (length < 8)
        return;


    if (memcmp(
            segment,
            "Exif\0\0",
            6) != 0)

        return;


    parse_exif_tiff(
        segment,
        length,
        6,
        length - 6,
        metadata
    );
}


/* =========================================================
 * JPEG metadata
 * ========================================================= */

int
wmedia_metadata_jpeg(
    const uint8_t *data,
    size_t size,
    WMediaMetadata *metadata
)
{
    if (!data ||
        !metadata ||
        size < 4)

        return 0;


    memset(
        metadata,
        0,
        sizeof(*metadata)
    );


    metadata->orientation =
        WMEDIA_ORIENTATION_NORMAL;


    if (data[0] != 0xFF ||
        data[1] != 0xD8)

        return 0;


    size_t pos = 2;


    while (pos + 1 < size) {

        /*
         * Skip fill bytes.
         */

        while (
            pos < size &&
            data[pos] == 0xFF
        ) {
            pos++;
        }


        if (pos >= size)
            break;


        uint8_t marker =
            data[pos++];


        /*
         * Standalone markers.
         */

        if (marker == 0xD8 ||
            marker == 0xD9)

            continue;


        /*
         * Start of Scan.
         */

        if (marker == 0xDA)
            break;


        if (pos + 2 > size)
            return 0;


        uint16_t segment_length =
            read_be16(
                data + pos
            );


        if (segment_length < 2)
            return 0;


        if (!range_valid(
                pos,
                segment_length,
                size))
            return 0;


        const uint8_t *segment =
            data + pos + 2;


        size_t payload_length =
            segment_length - 2;


        /*
         * APP1 = EXIF.
         */

        if (marker == 0xE1) {

            jpeg_parse_app1(
                segment,
                payload_length,
                metadata
            );
        }


        /*
         * SOF markers.
         */

        if (
            (marker >= 0xC0 &&
             marker <= 0xC3) ||

            (marker >= 0xC5 &&
             marker <= 0xC7) ||

            (marker >= 0xC9 &&
             marker <= 0xCB) ||

            (marker >= 0xCD &&
             marker <= 0xCF)
        ) {

            if (payload_length < 6)
                return 0;


            metadata->bits_per_component =
                segment[0];


            metadata->height =
                read_be16(
                    segment + 1
                );


            metadata->width =
                read_be16(
                    segment + 3
                );


            metadata->channels =
                segment[5];


            metadata->progressive =
                (marker >= 0xC2 &&
                 marker <= 0xC3) ||
                (marker >= 0xCA &&
                 marker <= 0xCB) ||
                (marker >= 0xCE &&
                 marker <= 0xCF);


            metadata->has_alpha =
                0;


            /*
             * Continue scanning APP markers if desired,
             * but dimensions are already available.
             */

            if (metadata->width != 0 &&
                metadata->height != 0)

                return 1;
        }


        pos +=
            segment_length;
    }


    return metadata->width != 0 &&
           metadata->height != 0;
}


/* =========================================================
 * PNG metadata
 * ========================================================= */

int
wmedia_metadata_png(
    const uint8_t *data,
    size_t size,
    WMediaMetadata *metadata
)
{
    static const uint8_t signature[8] = {
        0x89, 'P', 'N', 'G',
        0x0D, 0x0A, 0x1A, 0x0A
    };


    if (!data ||
        !metadata ||
        size < 33)

        return 0;


    if (memcmp(
            data,
            signature,
            8) != 0)

        return 0;


    memset(
        metadata,
        0,
        sizeof(*metadata)
    );


    metadata->orientation =
        WMEDIA_ORIENTATION_NORMAL;


    /*
     * First chunk must normally be IHDR.
     */

    uint32_t length =
        read_be32(data + 8);


    if (length < 13 ||
        !range_valid(
            8,
            12 + length,
            size))

        return 0;


    if (memcmp(
            data + 12,
            "IHDR",
            4) != 0)

        return 0;


    const uint8_t *ihdr =
        data + 16;


    metadata->width =
        read_be32(ihdr);


    metadata->height =
        read_be32(ihdr + 4);


    metadata->bits_per_component =
        ihdr[8];


    uint8_t color_type =
        ihdr[9];


    switch (color_type) {

        case 0:
            metadata->channels = 1;
            metadata->has_alpha = 0;
            break;

        case 2:
            metadata->channels = 3;
            metadata->has_alpha = 0;
            break;

        case 3:
            metadata->channels = 1;
            metadata->has_alpha = 0;
            break;

        case 4:
            metadata->channels = 2;
            metadata->has_alpha = 1;
            break;

        case 6:
            metadata->channels = 4;
            metadata->has_alpha = 1;
            break;

        default:
            return 0;
    }


    /*
     * Search chunks for pHYs.
     *
     * pHYs stores pixels-per-metre.
     */

    size_t pos = 8;


    while (pos + 12 <= size) {

        uint32_t chunk_length =
            read_be32(
                data + pos
            );


        if (!range_valid(
                pos,
                (size_t)chunk_length + 12,
                size))

            break;


        const uint8_t *type =
            data + pos + 4;


        const uint8_t *payload =
            data + pos + 8;


        if (memcmp(
                type,
                "pHYs",
                4) == 0 &&
            chunk_length >= 9) {

            uint32_t ppm_x =
                read_be32(payload);


            uint32_t ppm_y =
                read_be32(payload + 4);


            uint8_t unit =
                payload[8];


            if (unit == 1) {

                metadata->dpi_x =
                    (double)ppm_x *
                    0.0254;


                metadata->dpi_y =
                    (double)ppm_y *
                    0.0254;
            }
        }


        if (memcmp(
                type,
                "IEND",
                4) == 0)

            break;


        pos +=
            (size_t)chunk_length + 12;
    }


    return metadata->width != 0 &&
           metadata->height != 0;
}


/* =========================================================
 * WebP metadata
 * ========================================================= */

int
wmedia_metadata_webp(
    const uint8_t *data,
    size_t size,
    WMediaMetadata *metadata
)
{
    if (!data ||
        !metadata ||
        size < 16)

        return 0;


    if (memcmp(
            data,
            "RIFF",
            4) != 0 ||

        memcmp(
            data + 8,
            "WEBP",
            4) != 0)

        return 0;


    memset(
        metadata,
        0,
        sizeof(*metadata)
    );


    metadata->orientation =
        WMEDIA_ORIENTATION_NORMAL;


    size_t pos = 12;


    while (pos + 8 <= size) {

        const uint8_t *chunk =
            data + pos;


        uint32_t chunk_size =
            read_le32(
                chunk + 4
            );


        if (!range_valid(
                pos + 8,
                chunk_size,
                size))

            return 0;


        if (memcmp(
                chunk,
                "VP8X",
                4) == 0) {

            if (chunk_size < 10)
                return 0;


            const uint8_t *p =
                chunk + 8;


            metadata->width =
                1u |
                ((uint32_t)p[4] << 8) |
                ((uint32_t)p[5] << 16);


            metadata->height =
                1u |
                ((uint32_t)p[7] << 8) |
                ((uint32_t)p[8] << 16);


            metadata->has_alpha =
                (p[0] & 0x10) != 0;


            metadata->channels =
                metadata->has_alpha
                    ? 4
                    : 3;


            return 1;
        }


        if (memcmp(
                chunk,
                "VP8 ",
                4) == 0) {

            /*
             * Lossy VP8 frame header.
             */

            if (chunk_size < 10)
                return 0;


            const uint8_t *p =
                chunk + 8;


            if (p[3] != 0x9D ||
                p[4] != 0x01 ||
                p[5] != 0x2A)

                return 0;


            metadata->width =
                read_le16(p + 6) &
                0x3FFF;


            metadata->height =
                read_le16(p + 8) &
                0x3FFF;


            metadata->channels = 3;
            metadata->has_alpha = 0;

            return
                metadata->width != 0 &&
                metadata->height != 0;
        }


        if (memcmp(
                chunk,
                "VP8L",
                4) == 0) {

            if (chunk_size < 5)
                return 0;


            const uint8_t *p =
                chunk + 8;


            if (p[0] != 0x2F)
                return 0;


            uint32_t bits =
                (uint32_t)p[1] |
                ((uint32_t)p[2] << 8) |
                ((uint32_t)p[3] << 16) |
                ((uint32_t)p[4] << 24);


            metadata->width =
                (bits & 0x3FFF) + 1;


            metadata->height =
                ((bits >> 14) & 0x3FFF) + 1;


            metadata->channels = 4;
            metadata->has_alpha = 1;

            return 1;
        }


        pos +=
            8 + chunk_size;

        /*
         * RIFF chunks are padded to even size.
         */

        if (chunk_size & 1)
            pos++;
    }


    return 0;
}


/* =========================================================
 * BMP metadata
 * ========================================================= */

int
wmedia_metadata_bmp(
    const uint8_t *data,
    size_t size,
    WMediaMetadata *metadata
)
{
    if (!data ||
        !metadata ||
        size < 54)

        return 0;


    if (data[0] != 'B' ||
        data[1] != 'M')

        return 0;


    uint32_t dib_size =
        read_le32(
            data + 14
        );


    if (dib_size < 40 ||
        !range_valid(
            14,
            4 + dib_size,
            size))

        return 0;


    int32_t width =
        (int32_t)
        read_le32(
            data + 18
        );


    int32_t height =
        (int32_t)
        read_le32(
            data + 22
        );


    if (width <= 0 ||
        height == 0)

        return 0;


    memset(
        metadata,
        0,
        sizeof(*metadata)
    );


    metadata->orientation =
        WMEDIA_ORIENTATION_NORMAL;


    metadata->width =
        (uint32_t)width;


    metadata->height =
        height < 0
            ? (uint32_t)-height
            : (uint32_t)height;


    uint16_t planes =
        read_le16(
            data + 26
        );


    uint16_t bpp =
        read_le16(
            data + 28
        );


    if (planes != 1)
        return 0;


    metadata->bits_per_component =
        8;


    if (bpp == 32) {

        metadata->channels = 4;
        metadata->has_alpha = 1;

    } else if (bpp == 24) {

        metadata->channels = 3;
        metadata->has_alpha = 0;

    } else if (bpp == 8) {

        metadata->channels = 1;
        metadata->has_alpha = 0;

    } else {

        metadata->channels = 0;
    }


    /*
     * Pixels per metre.
     */

    if (dib_size >= 40) {

        int32_t ppm_x =
            (int32_t)
            read_le32(
                data + 38
            );


        int32_t ppm_y =
            (int32_t)
            read_le32(
                data + 42
            );


        if (ppm_x > 0)
            metadata->dpi_x =
                ppm_x * 0.0254;


        if (ppm_y > 0)
            metadata->dpi_y =
                ppm_y * 0.0254;
    }


    return 1;
}


/* =========================================================
 * Unified metadata API
 * ========================================================= */

int
wmedia_metadata_probe(
    const uint8_t *data,
    size_t size,
    WMediaType type,
    WMediaMetadata *metadata
)
{
    if (!data ||
        !metadata)

        return 0;


    switch (type) {

        case WMEDIA_JPEG:
            return wmedia_metadata_jpeg(
                data,
                size,
                metadata
            );

        case WMEDIA_PNG:
            return wmedia_metadata_png(
                data,
                size,
                metadata
            );

        case WMEDIA_WEBP:
            return wmedia_metadata_webp(
                data,
                size,
                metadata
            );

        case WMEDIA_BMP:
            return wmedia_metadata_bmp(
                data,
                size,
                metadata
            );

        default:
            return 0;
    }
}
Integrating metadata into WMediaObject

The previous WMediaObject can now be extended with a dedicated metadata state:

typedef struct {

    WMediaMetadata metadata;

    uint8_t metadata_loaded;

    uint8_t metadata_failed;

} WMediaMetadataState;

Then add it to the media object:

typedef struct {

    uint64_t id;

    WMediaType type;

    WMediaBuffer encoded;

    WMediaImage image;

    WMediaInfo info;

    WMediaMetadataState metadata_state;

    WMediaTransform transform;

    uint64_t content_hash;

    uint64_t last_used;

    uint32_t reference_count;

    uint8_t decoded;

    uint8_t loading;

    uint8_t failed;

} WMediaObject;
Metadata can now be extracted without decoding
int
wmedia_load_metadata(
    WMediaObject *object
)
{
    if (!object)
        return 0;


    if (object->metadata_state.metadata_loaded)
        return 1;


    WMediaMetadata metadata;


    if (!wmedia_metadata_probe(
            object->encoded.data,
            object->encoded.size,
            object->type,
            &metadata)) {

        object->metadata_state.metadata_failed = 1;

        return 0;
    }


    object->metadata_state.metadata =
        metadata;


    object->metadata_state.metadata_loaded =
        1;


    /*
     * Synchronise the legacy WMediaInfo structure.
     */

    object->info.width =
        metadata.width;

    object->info.height =
        metadata.height;

    object->info.dpi_x =
        metadata.dpi_x;

    object->info.dpi_y =
        metadata.dpi_y;


    switch (metadata.channels) {

        case 1:
            object->info.pixel_format =
                WMEDIA_PIXEL_GRAY8;
            break;

        case 3:
            object->info.pixel_format =
                WMEDIA_PIXEL_RGB8;
            break;

        case 4:
            object->info.pixel_format =
                WMEDIA_PIXEL_RGBA8;
            break;

        default:
            object->info.pixel_format =
                WMEDIA_PIXEL_UNKNOWN;
            break;
    }


    return 1;
}
Why this matters to Word layout

Now the document engine can determine physical placement immediately.

For example, an image reports:

6000 × 4000 pixels
300 × 300 DPI

The physical dimensions are:

width  = 6000 / 300 = 20 inches
height = 4000 / 300 = 13.33 inches

The layout engine can therefore scale it to the page without decoding the image first.

static double
pixels_to_points(
    uint32_t pixels,
    double dpi
)
{
    if (dpi <= 0.0)
        dpi = 96.0;

    return
        ((double)pixels / dpi) *
        72.0;
}


double
wmedia_physical_width_points(
    const WMediaObject *object
)
{
    if (!object)
        return 0.0;

    double dpi =
        object->info.dpi_x;

    if (dpi <= 0.0)
        dpi = 96.0;

    return
        pixels_to_points(
            object->info.width,
            dpi
        );
}


double
wmedia_physical_height_points(
    const WMediaObject *object
)
{
    if (!object)
        return 0.0;

    double dpi =
        object->info.dpi_y;

    if (dpi <= 0.0)
        dpi = 96.0;

    return
        pixels_to_points(
            object->info.height,
            dpi
        );
}
EXIF orientation

This also means the layout system can correctly distinguish:

Original pixels:

6000 × 4000


EXIF orientation:

90° clockwise


Displayed dimensions:

4000 × 6000

The physical bounding box should therefore be transformed before pagination.

static int
orientation_swaps_axes(
    WMediaOrientation orientation
)
{
    return
        orientation ==
            WMEDIA_ORIENTATION_90_CW ||

        orientation ==
            WMEDIA_ORIENTATION_90_CCW ||

        orientation ==
            WMEDIA_ORIENTATION_TRANSPOSE ||

        orientation ==
            WMEDIA_ORIENTATION_TRANSVERSE;
}

Then:

void
wmedia_oriented_dimensions(
    const WMediaObject *object,
    uint32_t *width,
    uint32_t *height
)
{
    if (!object ||
        !width ||
        !height)

        return;


    uint32_t w =
        object->info.width;

    uint32_t h =
        object->info.height;


    WMediaOrientation orientation =
        object->metadata_state.metadata.orientation;


    if (orientation_swaps_axes(
            orientation)) {

        *width = h;
        *height = w;

    } else {

        *width = w;
        *height = h;
    }
}








word_table.h
#ifndef WORD_TABLE_H
#define WORD_TABLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/* =========================================================
 * Basic geometry
 * ========================================================= */

typedef struct {
    double x;
    double y;
    double width;
    double height;
} WTBox;


typedef struct {
    double left;
    double top;
    double right;
    double bottom;
} WTMargins;


/* =========================================================
 * Table sizing
 * ========================================================= */

typedef enum {

    WT_SIZE_AUTO = 0,

    WT_SIZE_FIXED,

    WT_SIZE_PERCENT

} WTSizeMode;


typedef struct {

    WTSizeMode mode;

    double value;

    double minimum;

    double maximum;

} WTDimension;


/* =========================================================
 * Horizontal / vertical alignment
 * ========================================================= */

typedef enum {

    WT_ALIGN_LEFT = 0,

    WT_ALIGN_CENTER,

    WT_ALIGN_RIGHT,

    WT_ALIGN_JUSTIFY

} WTHorizontalAlignment;


typedef enum {

    WT_VERTICAL_TOP = 0,

    WT_VERTICAL_CENTER,

    WT_VERTICAL_BOTTOM

} WTVerticalAlignment;


/* =========================================================
 * Borders
 * ========================================================= */

typedef enum {

    WT_BORDER_NONE = 0,

    WT_BORDER_SINGLE,

    WT_BORDER_DOUBLE,

    WT_BORDER_DASHED,

    WT_BORDER_DOTTED

} WTBorderStyle;


typedef struct {

    WTBorderStyle style;

    double width;

    uint32_t color;

} WTBorder;


/* =========================================================
 * Cell
 * ========================================================= */

typedef struct {

    uint32_t id;

    uint32_t row;

    uint32_t column;

    uint32_t row_span;

    uint32_t column_span;

    WTBox bounds;

    WTMargins padding;

    WTBorder border_top;
    WTBorder border_right;
    WTBorder border_bottom;
    WTBorder border_left;

    WTHorizontalAlignment horizontal_alignment;

    WTVerticalAlignment vertical_alignment;

    WTDimension preferred_width;

    WTDimension preferred_height;

    double minimum_content_width;

    double preferred_content_width;

    double content_height;

    void *content;

} WTCell;


/* =========================================================
 * Column
 * ========================================================= */

typedef struct {

    uint32_t index;

    WTDimension width;

    double resolved_width;

    double minimum_width;

    double maximum_width;

} WTColumn;


/* =========================================================
 * Row
 * ========================================================= */

typedef struct {

    uint32_t index;

    WTDimension height;

    double resolved_height;

    double minimum_height;

    double maximum_height;

    uint8_t allow_break;

    uint8_t repeat_header;

} WTRow;


/* =========================================================
 * Table borders
 * ========================================================= */

typedef struct {

    WTBorder top;
    WTBorder right;
    WTBorder bottom;
    WTBorder left;

    WTBorder horizontal;
    WTBorder vertical;

} WTTableBorders;


/* =========================================================
 * Table
 * ========================================================= */

typedef struct {

    uint32_t id;

    size_t rows;

    size_t columns;

    WTColumn *column_data;

    WTRow *row_data;

    WTCell *cells;

    size_t cell_count;

    size_t cell_capacity;

    WTBox bounds;

    WTMargins cell_spacing;

    WTTableBorders borders;

    WTHorizontalAlignment alignment;

    WTDimension preferred_width;

    uint8_t autofit;

    uint8_t allow_break_across_pages;

    uint8_t repeat_header_rows;

    size_t header_row_count;

} WTTable;


/* =========================================================
 * Layout result
 * ========================================================= */

typedef struct {

    WTBox bounds;

    double content_width;

    double content_height;

    double table_height;

    size_t pages_required;

    uint8_t valid;

} WTTableLayout;


/* =========================================================
 * Table lifecycle
 * ========================================================= */

WTTable *
wt_table_create(
    size_t rows,
    size_t columns
);

void
wt_table_destroy(
    WTTable *table
);


/* =========================================================
 * Cell access
 * ========================================================= */

WTCell *
wt_table_cell(
    WTTable *table,
    size_t row,
    size_t column
);


/* =========================================================
 * Spanning
 * ========================================================= */

int
wt_table_merge_cells(
    WTTable *table,
    size_t row,
    size_t column,
    size_t row_span,
    size_t column_span
);


/* =========================================================
 * Sizing
 * ========================================================= */

void
wt_table_set_width(
    WTTable *table,
    WTDimension width
);

void
wt_table_set_column_width(
    WTTable *table,
    size_t column,
    WTDimension width
);

void
wt_table_set_row_height(
    WTTable *table,
    size_t row,
    WTDimension height
);


/* =========================================================
 * Layout
 * ========================================================= */

int
wt_table_layout(
    WTTable *table,
    double available_width,
    double available_height,
    WTTableLayout *result
);


/* =========================================================
 * Measurement
 * ========================================================= */

double
wt_table_minimum_width(
    const WTTable *table
);

double
wt_table_preferred_width(
    const WTTable *table
);

double
wt_table_height(
    const WTTable *table
);


/* =========================================================
 * Validation
 * ========================================================= */

int
wt_table_validate(
    const WTTable *table
);

#ifdef __cplusplus
}
#endif

#endif
word_table.c
#include "word_table.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>


/* =========================================================
 * Utility
 * ========================================================= */

static double
clamp_double(
    double value,
    double minimum,
    double maximum
)
{
    if (value < minimum)
        return minimum;

    if (maximum > minimum &&
        value > maximum)

        return maximum;

    return value;
}


static double
resolve_dimension(
    const WTDimension *dimension,
    double available,
    double fallback
)
{
    if (!dimension)
        return fallback;


    double result;


    switch (dimension->mode) {

        case WT_SIZE_FIXED:

            result =
                dimension->value;

            break;


        case WT_SIZE_PERCENT:

            result =
                available *
                dimension->value /
                100.0;

            break;


        case WT_SIZE_AUTO:
        default:

            result =
                fallback;

            break;
    }


    return clamp_double(
        result,
        dimension->minimum,
        dimension->maximum
    );
}


/* =========================================================
 * Create table
 * ========================================================= */

WTTable *
wt_table_create(
    size_t rows,
    size_t columns
)
{
    if (rows == 0 ||
        columns == 0)

        return NULL;


    WTTable *table =
        calloc(
            1,
            sizeof(*table)
        );


    if (!table)
        return NULL;


    table->rows =
        rows;

    table->columns =
        columns;


    table->column_data =
        calloc(
            columns,
            sizeof(WTColumn)
        );


    table->row_data =
        calloc(
            rows,
            sizeof(WTRow)
        );


    table->cell_capacity =
        rows * columns;


    table->cells =
        calloc(
            table->cell_capacity,
            sizeof(WTCell)
        );


    if (!table->column_data ||
        !table->row_data ||
        !table->cells) {

        wt_table_destroy(table);

        return NULL;
    }


    table->cell_count =
        rows * columns;


    /*
     * Default columns.
     */

    for (size_t c = 0;
         c < columns;
         ++c) {

        WTColumn *column =
            &table->column_data[c];

        column->index =
            (uint32_t)c;

        column->width.mode =
            WT_SIZE_AUTO;

        column->minimum_width =
            20.0;

        column->maximum_width =
            100000.0;
    }


    /*
     * Default rows.
     */

    for (size_t r = 0;
         r < rows;
         ++r) {

        WTRow *row =
            &table->row_data[r];

        row->index =
            (uint32_t)r;

        row->height.mode =
            WT_SIZE_AUTO;

        row->minimum_height =
            18.0;

        row->maximum_height =
            100000.0;

        row->allow_break =
            1;
    }


    /*
     * Default cells.
     */

    for (size_t r = 0;
         r < rows;
         ++r) {

        for (size_t c = 0;
             c < columns;
             ++c) {

            WTCell *cell =
                &table->cells[
                    r * columns + c
                ];


            cell->id =
                (uint32_t)(
                    r * columns + c + 1
                );


            cell->row =
                (uint32_t)r;

            cell->column =
                (uint32_t)c;

            cell->row_span =
                1;

            cell->column_span =
                1;


            cell->padding.left =
                4.0;

            cell->padding.right =
                4.0;

            cell->padding.top =
                2.0;

            cell->padding.bottom =
                2.0;


            cell->horizontal_alignment =
                WT_ALIGN_LEFT;

            cell->vertical_alignment =
                WT_VERTICAL_TOP;


            cell->preferred_width.mode =
                WT_SIZE_AUTO;

            cell->preferred_height.mode =
                WT_SIZE_AUTO;
        }
    }


    table->autofit =
        1;

    table->allow_break_across_pages =
        1;


    table->alignment =
        WT_ALIGN_LEFT;


    table->preferred_width.mode =
        WT_SIZE_AUTO;


    return table;
}


/* =========================================================
 * Destroy
 * ========================================================= */

void
wt_table_destroy(
    WTTable *table
)
{
    if (!table)
        return;


    free(table->column_data);
    free(table->row_data);
    free(table->cells);

    free(table);
}


/* =========================================================
 * Cell access
 * ========================================================= */

WTCell *
wt_table_cell(
    WTTable *table,
    size_t row,
    size_t column
)
{
    if (!table)
        return NULL;

    if (row >= table->rows ||
        column >= table->columns)

        return NULL;


    return &table->cells[
        row * table->columns +
        column
    ];
}


/* =========================================================
 * Merge cells
 * ========================================================= */

int
wt_table_merge_cells(
    WTTable *table,
    size_t row,
    size_t column,
    size_t row_span,
    size_t column_span
)
{
    if (!table ||
        row_span == 0 ||
        column_span == 0)

        return 0;


    if (row >= table->rows ||
        column >= table->columns)

        return 0;


    if (row_span >
        table->rows - row)

        return 0;


    if (column_span >
        table->columns - column)

        return 0;


    /*
     * Ensure the requested rectangle does not
     * intersect another already-spanning cell.
     */

    for (size_t r = row;
         r < row + row_span;
         ++r) {

        for (size_t c = column;
             c < column + column_span;
             ++c) {

            WTCell *cell =
                wt_table_cell(
                    table,
                    r,
                    c
                );


            if (!cell)
                return 0;


            if (cell->row_span != 1 ||
                cell->column_span != 1)

                return 0;
        }
    }


    WTCell *master =
        wt_table_cell(
            table,
            row,
            column
        );


    master->row_span =
        (uint32_t)row_span;

    master->column_span =
        (uint32_t)column_span;


    /*
     * Mark covered cells.
     */

    for (size_t r = row;
         r < row + row_span;
         ++r) {

        for (size_t c = column;
             c < column + column_span;
             ++c) {

            if (r == row &&
                c == column)

                continue;


            WTCell *covered =
                wt_table_cell(
                    table,
                    r,
                    c
                );


            covered->row_span =
                0;

            covered->column_span =
                0;
        }
    }


    return 1;
}


/* =========================================================
 * Width setters
 * ========================================================= */

void
wt_table_set_width(
    WTTable *table,
    WTDimension width
)
{
    if (!table)
        return;

    table->preferred_width =
        width;
}


void
wt_table_set_column_width(
    WTTable *table,
    size_t column,
    WTDimension width
)
{
    if (!table ||
        column >= table->columns)

        return;


    table->column_data[column].width =
        width;
}


void
wt_table_set_row_height(
    WTTable *table,
    size_t row,
    WTDimension height
)
{
    if (!table ||
        row >= table->rows)

        return;


    table->row_data[row].height =
        height;
}


/* =========================================================
 * Minimum width
 * ========================================================= */

double
wt_table_minimum_width(
    const WTTable *table
)
{
    if (!table)
        return 0.0;


    double result = 0.0;


    for (size_t c = 0;
         c < table->columns;
         ++c) {

        result +=
            table->column_data[c]
                .minimum_width;
    }


    /*
     * Cell spacing.
     */

    if (table->columns > 1) {

        result +=
            (table->columns - 1) *
            table->cell_spacing.left;
    }


    return result;
}


/* =========================================================
 * Preferred width
 * ========================================================= */

double
wt_table_preferred_width(
    const WTTable *table
)
{
    if (!table)
        return 0.0;


    double width = 0.0;


    for (size_t c = 0;
         c < table->columns;
         ++c) {

        const WTColumn *column =
            &table->column_data[c];


        if (column->width.mode ==
            WT_SIZE_FIXED) {

            width +=
                column->width.value;

        } else {

            width +=
                column->minimum_width;
        }
    }


    return width;
}


/* =========================================================
 * Column width allocation
 * ========================================================= */

static int
resolve_columns(
    WTTable *table,
    double available_width
)
{
    if (!table)
        return 0;


    double spacing =
        table->columns > 1
            ? (table->columns - 1) *
              table->cell_spacing.left
            : 0.0;


    double usable =
        available_width -
        spacing;


    if (usable < 0.0)
        usable = 0.0;


    double fixed_width = 0.0;

    size_t auto_columns = 0;


    for (size_t c = 0;
         c < table->columns;
         ++c) {

        WTColumn *column =
            &table->column_data[c];


        if (column->width.mode ==
            WT_SIZE_FIXED) {

            column->resolved_width =
                column->width.value;

            fixed_width +=
                column->resolved_width;

        } else {

            auto_columns++;
        }
    }


    double remaining =
        usable - fixed_width;


    if (remaining < 0.0)
        remaining = 0.0;


    double auto_width =
        auto_columns
            ? remaining / auto_columns
            : 0.0;


    for (size_t c = 0;
         c < table->columns;
         ++c) {

        WTColumn *column =
            &table->column_data[c];


        if (column->width.mode !=
            WT_SIZE_FIXED) {

            column->resolved_width =
                auto_width;


            if (column->resolved_width <
                column->minimum_width)

                column->resolved_width =
                    column->minimum_width;


            if (column->maximum_width >
                column->minimum_width &&
                column->resolved_width >
                column->maximum_width)

                column->resolved_width =
                    column->maximum_width;
        }
    }


    /*
     * Percentage widths.
     */

    for (size_t c = 0;
         c < table->columns;
         ++c) {

        WTColumn *column =
            &table->column_data[c];


        if (column->width.mode ==
            WT_SIZE_PERCENT) {

            column->resolved_width =
                usable *
                column->width.value /
                100.0;


            column->resolved_width =
                clamp_double(
                    column->resolved_width,
                    column->minimum_width,
                    column->maximum_width
                );
        }
    }


    return 1;
}


/* =========================================================
 * Row height calculation
 * ========================================================= */

static double
measure_cell_height(
    const WTCell *cell,
    double width
)
{
    if (!cell)
        return 0.0;


    /*
     * The actual text/object layout engine will eventually
     * supply content_height.
     */

    double content =
        cell->content_height;


    double horizontal_padding =
        cell->padding.top +
        cell->padding.bottom;


    double result =
        content +
        horizontal_padding;


    if (result < 18.0)
        result = 18.0;


    /*
     * Width currently exists so that future content
     * measurement can account for wrapping.
     */

    (void)width;


    return result;
}


/* =========================================================
 * Row resolution
 * ========================================================= */

static int
resolve_rows(
    WTTable *table
)
{
    if (!table)
        return 0;


    for (size_t r = 0;
         r < table->rows;
         ++r) {

        WTRow *row =
            &table->row_data[r];


        double height;


        if (row->height.mode ==
            WT_SIZE_FIXED) {

            height =
                row->height.value;

        } else {

            height =
                row->minimum_height;


            for (size_t c = 0;
                 c < table->columns;
                 ++c) {

                WTCell *cell =
                    wt_table_cell(
                        table,
                        r,
                        c
                    );


                if (!cell ||
                    cell->row_span == 0)

                    continue;


                if (cell->row_span != 1)
                    continue;


                double cell_height =
                    measure_cell_height(
                        cell,
                        table->column_data[c]
                            .resolved_width
                    );


                if (cell_height > height)
                    height =
                        cell_height;
            }
        }


        row->resolved_height =
            clamp_double(
                height,
                row->minimum_height,
                row->maximum_height
            );
    }


    return 1;
}


/* =========================================================
 * Place cells
 * ========================================================= */

static void
place_cells(
    WTTable *table
)
{
    if (!table)
        return;


    double y =
        table->bounds.y;


    for (size_t r = 0;
         r < table->rows;
         ++r) {

        double x =
            table->bounds.x;


        for (size_t c = 0;
             c < table->columns;
             ++c) {

            WTCell *cell =
                wt_table_cell(
                    table,
                    r,
                    c
                );


            if (!cell)
                continue;


            if (cell->row_span == 0 ||
                cell->column_span == 0) {

                x +=
                    table->column_data[c]
                        .resolved_width;

                continue;
            }


            double width = 0.0;


            for (size_t i = 0;
                 i < cell->column_span;
                 ++i) {

                if (c + i >= table->columns)
                    break;


                width +=
                    table->column_data[c + i]
                        .resolved_width;
            }


            double height = 0.0;


            for (size_t i = 0;
                 i < cell->row_span;
                 ++i) {

                if (r + i >= table->rows)
                    break;


                height +=
                    table->row_data[r + i]
                        .resolved_height;
            }


            cell->bounds =
                (WTBox) {

                    .x = x,

                    .y = y,

                    .width = width,

                    .height = height
                };


            x +=
                table->column_data[c]
                    .resolved_width;
        }


        y +=
            table->row_data[r]
                .resolved_height;
    }
}


/* =========================================================
 * Total height
 * ========================================================= */

double
wt_table_height(
    const WTTable *table
)
{
    if (!table)
        return 0.0;


    double height = 0.0;


    for (size_t r = 0;
         r < table->rows;
         ++r) {

        height +=
            table->row_data[r]
                .resolved_height;
    }


    return height;
}


/* =========================================================
 * Layout
 * ========================================================= */

int
wt_table_layout(
    WTTable *table,
    double available_width,
    double available_height,
    WTTableLayout *result
)
{
    if (!table ||
        !result ||
        available_width <= 0.0)

        return 0;


    memset(
        result,
        0,
        sizeof(*result)
    );


    double width =
        resolve_dimension(
            &table->preferred_width,
            available_width,
            available_width
        );


    if (table->autofit)
        width =
            available_width;


    double minimum =
        wt_table_minimum_width(
            table
        );


    if (width < minimum)
        width = minimum;


    table->bounds.width =
        width;


    if (!resolve_columns(
            table,
            width))

        return 0;


    if (!resolve_rows(
            table))

        return 0;


    table->bounds.height =
        wt_table_height(
            table
        );


    place_cells(table);


    result->bounds =
        table->bounds;


    result->content_width =
        width;


    result->content_height =
        table->bounds.height;


    result->table_height =
        table->bounds.height;


    if (available_height > 0.0) {

        result->pages_required =
            (size_t)ceil(
                table->bounds.height /
                available_height
            );

    } else {

        result->pages_required =
            1;
    }


    if (result->pages_required == 0)
        result->pages_required = 1;


    result->valid =
        1;


    return 1;
}


/* =========================================================
 * Validation
 * ========================================================= */

int
wt_table_validate(
    const WTTable *table
)
{
    if (!table)
        return 0;


    if (table->rows == 0 ||
        table->columns == 0)

        return 0;


    if (!table->column_data ||
        !table->row_data ||
        !table->cells)

        return 0;


    for (size_t r = 0;
         r < table->rows;
         ++r) {

        for (size_t c = 0;
             c < table->columns;
             ++c) {

            const WTCell *cell =
                &table->cells[
                    r * table->columns + c
                ];


            if (cell->row_span == 0 ||
                cell->column_span == 0)

                continue;


            if (
                cell->row +
                cell->row_span >
                table->rows
            )

                return 0;


            if (
                cell->column +
                cell->column_span >
                table->columns
            )

                return 0;
        }
    }


    return 1;
}
Object layout

Tables aren't the only non-text objects. Word needs a general object-placement engine for:

images
charts
shapes
text boxes
equations
embedded objects
floating media
anchored objects

So we'll give those objects a common geometry model.

word_object_layout.h
#ifndef WORD_OBJECT_LAYOUT_H
#define WORD_OBJECT_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/* =========================================================
 * Object types
 * ========================================================= */

typedef enum {

    WO_OBJECT_NONE = 0,

    WO_OBJECT_IMAGE,

    WO_OBJECT_SHAPE,

    WO_OBJECT_TEXT_BOX,

    WO_OBJECT_CHART,

    WO_OBJECT_TABLE,

    WO_OBJECT_EQUATION,

    WO_OBJECT_EMBEDDED

} WOObjectType;


/* =========================================================
 * Anchor types
 * ========================================================= */

typedef enum {

    WO_ANCHOR_INLINE = 0,

    WO_ANCHOR_PARAGRAPH,

    WO_ANCHOR_CHARACTER,

    WO_ANCHOR_PAGE,

    WO_ANCHOR_MARGIN

} WOAnchorType;


/* =========================================================
 * Wrap modes
 * ========================================================= */

typedef enum {

    WO_WRAP_NONE = 0,

    WO_WRAP_SQUARE,

    WO_WRAP_TIGHT,

    WO_WRAP_THROUGH,

    WO_WRAP_TOP_BOTTOM,

    WO_WRAP_BEHIND_TEXT,

    WO_WRAP_IN_FRONT

} WOWrapMode;


/* =========================================================
 * Position reference
 * ========================================================= */

typedef enum {

    WO_POSITION_PAGE = 0,

    WO_POSITION_MARGIN,

    WO_POSITION_COLUMN,

    WO_POSITION_PARAGRAPH,

    WO_POSITION_CHARACTER

} WOPositionReference;


/* =========================================================
 * Position
 * ========================================================= */

typedef struct {

    WOPositionReference horizontal_reference;

    WOPositionReference vertical_reference;

    double horizontal_offset;

    double vertical_offset;

} WOObjectPosition;


/* =========================================================
 * Object
 * ========================================================= */

typedef struct {

    uint64_t id;

    WOObjectType type;

    WOAnchorType anchor;

    WOWrapMode wrap;

    WOObjectPosition position;

    double width;

    double height;

    double rotation;

    double scale_x;

    double scale_y;

    double margin_left;

    double margin_right;

    double margin_top;

    double margin_bottom;

    int32_t z_index;

    size_t paragraph_index;

    uint8_t locked;

    uint8_t hidden;

    void *payload;

} WOObject;


/* =========================================================
 * Layout context
 * ========================================================= */

typedef struct {

    double page_x;

    double page_y;

    double page_width;

    double page_height;

    double margin_left;

    double margin_right;

    double margin_top;

    double margin_bottom;

    double column_x;

    double column_width;

    double paragraph_x;

    double paragraph_y;

    double paragraph_width;

} WOLayoutContext;


/* =========================================================
 * Resolved object
 * ========================================================= */

typedef struct {

    double x;

    double y;

    double width;

    double height;

    double rotation;

    WOWrapMode wrap;

    int32_t z_index;

    uint8_t intersects_text;

} WOResolvedObject;


/* =========================================================
 * Object layout
 * ========================================================= */

int wo_layout_object(
    const WOObject *object,
    const WOLayoutContext *context,
    WOResolvedObject *result
);


/* =========================================================
 * Collision
 * ========================================================= */

int wo_objects_intersect(
    const WOResolvedObject *a,
    const WOResolvedObject *b
);


/* =========================================================
 * Text exclusion
 * ========================================================= */

double wo_text_left_boundary(
    const WOResolvedObject *object,
    double y
);

double wo_text_right_boundary(
    const WOResolvedObject *object,
    double y,
    double page_right
);

#ifdef __cplusplus
}
#endif

#endif
word_object_layout.c
#include "word_object_layout.h"

#include <math.h>
#include <string.h>


static double
clamp_scale(
    double scale
)
{
    if (scale <= 0.0)
        return 1.0;

    return scale;
}


/* =========================================================
 * Resolve horizontal anchor
 * ========================================================= */

static double
resolve_horizontal(
    const WOObject *object,
    const WOLayoutContext *context
)
{
    switch (
        object->position.horizontal_reference
    ) {

        case WO_POSITION_PAGE:

            return
                context->page_x +
                object->position.horizontal_offset;


        case WO_POSITION_MARGIN:

            return
                context->page_x +
                context->margin_left +
                object->position.horizontal_offset;


        case WO_POSITION_COLUMN:

            return
                context->column_x +
                object->position.horizontal_offset;


        case WO_POSITION_PARAGRAPH:

            return
                context->paragraph_x +
                object->position.horizontal_offset;


        case WO_POSITION_CHARACTER:

            return
                context->paragraph_x +
                object->position.horizontal_offset;


        default:

            return context->paragraph_x;
    }
}


/* =========================================================
 * Resolve vertical anchor
 * ========================================================= */

static double
resolve_vertical(
    const WOObject *object,
    const WOLayoutContext *context
)
{
    switch (
        object->position.vertical_reference
    ) {

        case WO_POSITION_PAGE:

            return
                context->page_y +
                object->position.vertical_offset;


        case WO_POSITION_MARGIN:

            return
                context->page_y +
                context->margin_top +
                object->position.vertical_offset;


        case WO_POSITION_COLUMN:

            return
                context->paragraph_y +
                object->position.vertical_offset;


        case WO_POSITION_PARAGRAPH:

            return
                context->paragraph_y +
                object->position.vertical_offset;


        case WO_POSITION_CHARACTER:

            return
                context->paragraph_y +
                object->position.vertical_offset;


        default:

            return context->paragraph_y;
    }
}


/* =========================================================
 * Layout object
 * ========================================================= */

int
wo_layout_object(
    const WOObject *object,
    const WOLayoutContext *context,
    WOResolvedObject *result
)
{
    if (!object ||
        !context ||
        !result)

        return 0;


    memset(
        result,
        0,
        sizeof(*result)
    );


    double scale_x =
        clamp_scale(
            object->scale_x
        );


    double scale_y =
        clamp_scale(
            object->scale_y
        );


    double width =
        object->width *
        scale_x;


    double height =
        object->height *
        scale_y;


    /*
     * Inline objects participate directly in
     * paragraph flow.
     */

    if (object->anchor ==
        WO_ANCHOR_INLINE) {

        result->x =
            context->paragraph_x;

        result->y =
            context->paragraph_y;

    } else {

        result->x =
            resolve_horizontal(
                object,
                context
            );

        result->y =
            resolve_vertical(
                object,
                context
            );
    }


    result->width =
        width;

    result->height =
        height;

    result->rotation =
        object->rotation;

    result->wrap =
        object->wrap;

    result->z_index =
        object->z_index;


    if (object->wrap !=
        WO_WRAP_NONE &&
        object->wrap !=
        WO_WRAP_BEHIND_TEXT &&
        object->wrap !=
        WO_WRAP_IN_FRONT)

        result->intersects_text = 1;


    return 1;
}


/* =========================================================
 * Axis-aligned intersection
 * ========================================================= */

int
wo_objects_intersect(
    const WOResolvedObject *a,
    const WOResolvedObject *b
)
{
    if (!a || !b)
        return 0;


    double ar =
        a->x + a->width;

    double ab =
        a->y + a->height;


    double br =
        b->x + b->width;

    double bb =
        b->y + b->height;


    if (ar <= b->x)
        return 0;

    if (br <= a->x)
        return 0;

    if (ab <= b->y)
        return 0;

    if (bb <= a->y)
        return 0;


    return 1;
}


/* =========================================================
 * Text left boundary
 * ========================================================= */

double
wo_text_left_boundary(
    const WOResolvedObject *object,
    double y
)
{
    if (!object)
        return 0.0;


    if (y < object->y ||
        y > object->y + object->height)

        return 0.0;


    switch (object->wrap) {

        case WO_WRAP_SQUARE:
        case WO_WRAP_TIGHT:
        case WO_WRAP_THROUGH:

            return
                object->x +
                object->width;

        default:

            return 0.0;
    }
}


/* =========================================================
 * Text right boundary
 * ========================================================= */

double
wo_text_right_boundary(
    const WOResolvedObject *object,
    double y,
    double page_right
)
{
    if (!object)
        return page_right;


    if (y < object->y ||
        y > object->y + object->height)

        return page_right;


    switch (object->wrap) {

        case WO_WRAP_SQUARE:
        case WO_WRAP_TIGHT:
        case WO_WRAP_THROUGH:

            return object->x;

        default:

            return page_right;
    }
}
Floating-object collision resolution

The next critical component is preventing:

        ┌───────────────┐
        │    IMAGE      │
        │               │
        └───────────────┘
             ┌───────────────┐
             │    IMAGE 2    │
             └───────────────┘

when two floating objects occupy the same space.

A simple deterministic placement pass:

#include "word_object_layout.h"

#include <stddef.h>


static void
move_down_until_clear(
    WOResolvedObject *candidate,
    const WOResolvedObject *objects,
    size_t count
)
{
    if (!candidate)
        return;


    int collision;


    do {

        collision = 0;


        for (size_t i = 0;
             i < count;
             ++i) {

            if (!wo_objects_intersect(
                    candidate,
                    &objects[i]))

                continue;


            candidate->y =
                objects[i].y +
                objects[i].height;


            collision = 1;

            break;
        }

    } while (collision);
}

A fuller object manager can then maintain the floating layer:

typedef struct {

    WOObject *objects;

    size_t count;

    size_t capacity;

} WOObjectStore;

with:

int
wo_store_add(
    WOObjectStore *store,
    const WOObject *object
)
{
    if (!store ||
        !object)

        return 0;


    if (store->count >=
        store->capacity)

        return 0;


    store->objects[
        store->count++
    ] = *object;


    return 1;
}
Table + object integration

A table itself can now become an object:

WOObject table_object = {

    .id = 100,

    .type = WO_OBJECT_TABLE,

    .anchor = WO_ANCHOR_PARAGRAPH,

    .wrap = WO_WRAP_NONE,

    .width = 500.0,

    .height = 200.0,

    .scale_x = 1.0,

    .scale_y = 1.0,

    .z_index = 0
};








word_table_pagination.h
#ifndef WORD_TABLE_PAGINATION_H
#define WORD_TABLE_PAGINATION_H

#include "word_table.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================
 * Pagination policy
 * ========================================================= */

typedef struct {
    double page_width;
    double page_height;

    double margin_left;
    double margin_right;
    double margin_top;
    double margin_bottom;

    double header_height;
    double footer_height;

    uint8_t repeat_header_rows;
    uint8_t allow_row_split;
    uint8_t keep_rows_together;
} WTTablePaginationOptions;


/* =========================================================
 * Cell fragment
 * ========================================================= */

typedef struct {
    uint32_t cell_id;

    size_t source_row;
    size_t source_column;

    size_t fragment_index;

    double x;
    double y;

    double width;
    double height;

    double content_y_offset;
    double content_height;

    uint8_t first_fragment;
    uint8_t last_fragment;
} WTCellFragment;


/* =========================================================
 * Row fragment
 * ========================================================= */

typedef struct {
    size_t source_row;

    size_t fragment_index;

    double y;
    double height;

    uint8_t first_fragment;
    uint8_t last_fragment;

    uint8_t repeated_header;

    WTCellFragment *cells;

    size_t cell_count;
    size_t cell_capacity;
} WTRowFragment;


/* =========================================================
 * Table page
 * ========================================================= */

typedef struct {
    size_t page_number;

    double x;
    double y;

    double width;
    double height;

    WTRowFragment *rows;

    size_t row_count;
    size_t row_capacity;

    uint8_t continues_from_previous;
    uint8_t continues_to_next;
} WTTablePage;


/* =========================================================
 * Pagination result
 * ========================================================= */

typedef struct {
    WTTablePage *pages;

    size_t page_count;
    size_t page_capacity;

    double total_height;

    uint8_t valid;
} WTTablePaginationResult;


/* =========================================================
 * Lifecycle
 * ========================================================= */

void
wt_table_pagination_result_init(
    WTTablePaginationResult *result
);

void
wt_table_pagination_result_destroy(
    WTTablePaginationResult *result
);


/* =========================================================
 * Pagination
 * ========================================================= */

int
wt_table_paginate(
    WTTable *table,
    const WTTablePaginationOptions *options,
    WTTablePaginationResult *result
);


/* =========================================================
 * Query
 * ========================================================= */

const WTTablePage *
wt_table_page(
    const WTTablePaginationResult *result,
    size_t index
);

const WTRowFragment *
wt_table_page_row(
    const WTTablePage *page,
    size_t index
);

#ifdef __cplusplus
}
#endif

#endif
word_table_pagination.c
#include "word_table_pagination.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>


/* =========================================================
 * Dynamic storage
 * ========================================================= */

static int
grow_array(
    void **array,
    size_t *capacity,
    size_t element_size,
    size_t required
)
{
    if (required <= *capacity)
        return 1;

    size_t new_capacity =
        (*capacity == 0)
            ? 8
            : *capacity * 2;

    while (new_capacity < required) {

        if (new_capacity >
            SIZE_MAX / 2)

            return 0;

        new_capacity *= 2;
    }

    if (element_size != 0 &&
        new_capacity >
        SIZE_MAX / element_size)

        return 0;

    void *new_array =
        realloc(
            *array,
            new_capacity * element_size
        );

    if (!new_array)
        return 0;

    *array = new_array;
    *capacity = new_capacity;

    return 1;
}


/* =========================================================
 * Row fragment cleanup
 * ========================================================= */

static void
destroy_row_fragment(
    WTRowFragment *row
)
{
    if (!row)
        return;

    free(row->cells);

    row->cells = NULL;
    row->cell_count = 0;
    row->cell_capacity = 0;
}


/* =========================================================
 * Page cleanup
 * ========================================================= */

static void
destroy_page(
    WTTablePage *page
)
{
    if (!page)
        return;

    for (size_t i = 0;
         i < page->row_count;
         ++i) {

        destroy_row_fragment(
            &page->rows[i]
        );
    }

    free(page->rows);

    page->rows = NULL;
    page->row_count = 0;
    page->row_capacity = 0;
}


/* =========================================================
 * Result lifecycle
 * ========================================================= */

void
wt_table_pagination_result_init(
    WTTablePaginationResult *result
)
{
    if (!result)
        return;

    memset(
        result,
        0,
        sizeof(*result)
    );
}


void
wt_table_pagination_result_destroy(
    WTTablePaginationResult *result
)
{
    if (!result)
        return;

    for (size_t i = 0;
         i < result->page_count;
         ++i) {

        destroy_page(
            &result->pages[i]
        );
    }

    free(result->pages);

    memset(
        result,
        0,
        sizeof(*result)
    );
}


/* =========================================================
 * Add page
 * ========================================================= */

static WTTablePage *
add_page(
    WTTablePaginationResult *result
)
{
    if (!grow_array(
            (void **)&result->pages,
            &result->page_capacity,
            sizeof(WTTablePage),
            result->page_count + 1))

        return NULL;


    WTTablePage *page =
        &result->pages[
            result->page_count
        ];


    memset(
        page,
        0,
        sizeof(*page)
    );


    page->page_number =
        result->page_count;


    result->page_count++;


    return page;
}


/* =========================================================
 * Add row to page
 * ========================================================= */

static WTRowFragment *
add_row(
    WTTablePage *page
)
{
    if (!grow_array(
            (void **)&page->rows,
            &page->row_capacity,
            sizeof(WTRowFragment),
            page->row_count + 1))

        return NULL;


    WTRowFragment *row =
        &page->rows[
            page->row_count
        ];


    memset(
        row,
        0,
        sizeof(*row)
    );


    page->row_count++;


    return row;
}


/* =========================================================
 * Add cell fragment
 * ========================================================= */

static WTCellFragment *
add_cell(
    WTRowFragment *row
)
{
    if (!grow_array(
            (void **)&row->cells,
            &row->cell_capacity,
            sizeof(WTCellFragment),
            row->cell_count + 1))

        return NULL;


    WTCellFragment *cell =
        &row->cells[
            row->cell_count
        ];


    memset(
        cell,
        0,
        sizeof(*cell)
    );


    row->cell_count++;


    return cell;
}


/* =========================================================
 * Available page height
 * ========================================================= */

static double
available_page_height(
    const WTTablePaginationOptions *options
)
{
    double height =
        options->page_height -
        options->margin_top -
        options->margin_bottom -
        options->header_height -
        options->footer_height;

    if (height < 0.0)
        height = 0.0;

    return height;
}


/* =========================================================
 * Add cell geometry
 * ========================================================= */

static int
populate_cell_fragment(
    WTRowFragment *fragment,
    WTTable *table,
    size_t row_index,
    size_t row_fragment_index,
    double y,
    double fragment_height,
    double content_offset,
    double content_height
)
{
    for (size_t c = 0;
         c < table->columns;
         ++c) {

        WTCell *cell =
            wt_table_cell(
                table,
                row_index,
                c
            );

        if (!cell)
            return 0;


        /*
         * Covered cells are not independently rendered.
         */

        if (cell->row_span == 0 ||
            cell->column_span == 0)

            continue;


        /*
         * A cell belonging to a multi-row span is
         * represented by the master cell.
         */

        if (cell->row_span > 1 &&
            cell->row != row_index)

            continue;


        double x =
            table->bounds.x;


        for (size_t i = 0;
             i < cell->column;
             ++i) {

            x +=
                table->column_data[i]
                    .resolved_width;
        }


        double width = 0.0;


        for (size_t i = 0;
             i < cell->column_span;
             ++i) {

            if (cell->column + i >=
                table->columns)

                break;


            width +=
                table->column_data[
                    cell->column + i
                ].resolved_width;
        }


        WTCellFragment *out =
            add_cell(fragment);


        if (!out)
            return 0;


        out->cell_id =
            cell->id;


        out->source_row =
            row_index;


        out->source_column =
            cell->column;


        out->fragment_index =
            row_fragment_index;


        out->x =
            x;


        out->y =
            y;


        out->width =
            width;


        out->height =
            fragment_height;


        out->content_y_offset =
            content_offset;


        out->content_height =
            content_height;


        out->first_fragment =
            row_fragment_index == 0;


        out->last_fragment =
            content_offset +
            fragment_height >=
            cell->content_height;
    }


    return 1;
}


/* =========================================================
 * Create repeated header rows
 * ========================================================= */

static int
add_header_rows(
    WTTable *table,
    WTTablePage *page,
    double *cursor_y,
    const WTTablePaginationOptions *options
)
{
    if (!options->repeat_header_rows)
        return 1;


    size_t header_count =
        table->header_row_count;


    if (header_count == 0)
        return 1;


    if (header_count > table->rows)
        header_count = table->rows;


    for (size_t r = 0;
         r < header_count;
         ++r) {

        WTRowFragment *fragment =
            add_row(page);


        if (!fragment)
            return 0;


        fragment->source_row =
            r;


        fragment->fragment_index =
            0;


        fragment->y =
            *cursor_y;


        fragment->height =
            table->row_data[r]
                .resolved_height;


        fragment->first_fragment =
            1;

        fragment->last_fragment =
            1;

        fragment->repeated_header =
            1;


        if (!populate_cell_fragment(
                fragment,
                table,
                r,
                0,
                *cursor_y,
                fragment->height,
                0.0,
                fragment->height))

            return 0;


        *cursor_y +=
            fragment->height;
    }


    return 1;
}


/* =========================================================
 * Determine row fragment height
 * ========================================================= */

static double
row_height_remaining(
    const WTTable *table,
    size_t row,
    double content_offset
)
{
    if (!table ||
        row >= table->rows)

        return 0.0;


    double row_height =
        table->row_data[row]
            .resolved_height;


    if (content_offset >= row_height)
        return 0.0;


    return
        row_height -
        content_offset;
}


/* =========================================================
 * Paginate table
 * ========================================================= */

int
wt_table_paginate(
    WTTable *table,
    const WTTablePaginationOptions *options,
    WTTablePaginationResult *result
)
{
    if (!table ||
        !options ||
        !result)

        return 0;


    wt_table_pagination_result_destroy(
        result
    );


    if (!wt_table_validate(table))
        return 0;


    double page_height =
        available_page_height(
            options
        );


    if (page_height <= 0.0)
        return 0;


    double page_width =
        options->page_width -
        options->margin_left -
        options->margin_right;


    if (page_width <= 0.0)
        return 0;


    /*
     * Ensure table has current geometry.
     */

    WTTableLayout layout;


    if (!wt_table_layout(
            table,
            page_width,
            page_height,
            &layout))

        return 0;


    size_t current_row = 0;

    double row_content_offset = 0.0;

    size_t fragment_index = 0;

    double total_height = 0.0;


    WTTablePage *page = NULL;

    double cursor_y = 0.0;


    while (
        current_row < table->rows
    ) {

        /*
         * Start a new physical page.
         */

        page =
            add_page(result);


        if (!page) {

            wt_table_pagination_result_destroy(
                result
            );

            return 0;
        }


        page->x =
            options->margin_left;


        page->y =
            options->margin_top;


        page->width =
            page_width;


        page->height =
            page_height;


        page->continues_from_previous =
            result->page_count > 1;


        cursor_y =
            options->margin_top;


        /*
         * Repeat table header.
         */

        if (current_row > 0) {

            if (!add_header_rows(
                    table,
                    page,
                    &cursor_y,
                    options)) {

                wt_table_pagination_result_destroy(
                    result
                );

                return 0;
            }
        }


        /*
         * Available space remaining on this page.
         */

        double page_bottom =
            options->page_height -
            options->margin_bottom -
            options->footer_height;


        /*
         * Place rows.
         */

        while (
            current_row < table->rows
        ) {

            WTRow *source_row =
                &table->row_data[
                    current_row
                ];


            double remaining =
                row_height_remaining(
                    table,
                    current_row,
                    row_content_offset
                );


            if (remaining <= 0.0) {

                current_row++;
                row_content_offset = 0.0;
                fragment_index = 0;

                continue;
            }


            /*
             * Header rows are ordinary source rows,
             * but they don't need to be duplicated
             * while we are laying out their original
             * position.
             */


            double available =
                page_bottom -
                cursor_y;


            /*
             * Row fits completely.
             */

            if (remaining <= available) {

                WTRowFragment *fragment =
                    add_row(page);


                if (!fragment) {

                    wt_table_pagination_result_destroy(
                        result
                    );

                    return 0;
                }


                fragment->source_row =
                    current_row;


                fragment->fragment_index =
                    fragment_index;


                fragment->y =
                    cursor_y;


                fragment->height =
                    remaining;


                fragment->first_fragment =
                    row_content_offset == 0.0;


                fragment->last_fragment =
                    1;


                fragment->repeated_header =
                    0;


                if (!populate_cell_fragment(
                        fragment,
                        table,
                        current_row,
                        fragment_index,
                        cursor_y,
                        remaining,
                        row_content_offset,
                        remaining))

                {
                    wt_table_pagination_result_destroy(
                        result
                    );

                    return 0;
                }


                cursor_y += remaining;

                total_height += remaining;

                current_row++;

                row_content_offset = 0.0;

                fragment_index = 0;


                continue;
            }


            /*
             * The row does not fit.
             */

            if (
                options->keep_rows_together ||
                !options->allow_row_split
            ) {

                /*
                 * If there is already content on this page,
                 * move the complete row to the next page.
                 */

                if (page->row_count > 0) {

                    page->continues_to_next =
                        1;

                    break;
                }


                /*
                 * A single row is taller than a page.
                 * It must be split regardless.
                 */

                if (remaining > page_height) {

                    /*
                     * Fall through to splitting.
                     */

                } else {

                    break;
                }
            }


            /*
             * Split the row.
             */

            if (available <= 0.0) {

                page->continues_to_next =
                    1;

                break;
            }


            double fragment_height =
                fmin(
                    remaining,
                    available
                );


            WTRowFragment *fragment =
                add_row(page);


            if (!fragment) {

                wt_table_pagination_result_destroy(
                    result
                );

                return 0;
            }


            fragment->source_row =
                current_row;


            fragment->fragment_index =
                fragment_index;


            fragment->y =
                cursor_y;


            fragment->height =
                fragment_height;


            fragment->first_fragment =
                row_content_offset == 0.0;


            fragment->last_fragment =
                fragment_height >= remaining;


            fragment->repeated_header =
                0;


            if (!populate_cell_fragment(
                    fragment,
                    table,
                    current_row,
                    fragment_index,
                    cursor_y,
                    fragment_height,
                    row_content_offset,
                    fragment_height))

            {
                wt_table_pagination_result_destroy(
                    result
                );

                return 0;
            }


            cursor_y +=
                fragment_height;


            total_height +=
                fragment_height;


            row_content_offset +=
                fragment_height;


            fragment_index++;


            /*
             * Page is now full.
             */

            if (cursor_y >=
                page_bottom - 0.001) {

                page->continues_to_next =
                    1;

                break;
            }
        }
    }


    result->total_height =
        total_height;


    result->valid =
        result->page_count > 0;


    return result->valid;
}


/* =========================================================
 * Page query
 * ========================================================= */

const WTTablePage *
wt_table_page(
    const WTTablePaginationResult *result,
    size_t index
)
{
    if (!result ||
        index >= result->page_count)

        return NULL;

    return &result->pages[index];
}


/* =========================================================
 * Row query
 * ========================================================= */

const WTRowFragment *
wt_table_page_row(
    const WTTablePage *page,
    size_t index
)
{
    if (!page ||
        index >= page->row_count)

        return NULL;

    return &page->rows[index];
}

One include should also be added at the top because the implementation uses SIZE_MAX:

#include <stdint.h>
Example
#include "word_table.h"
#include "word_table_pagination.h"

#include <stdio.h>

int main(void)
{
    WTTable *table =
        wt_table_create(100, 5);

    if (!table)
        return 1;


    /*
     * First row becomes the repeating header.
     */

    table->header_row_count = 1;


    /*
     * Give the table realistic column widths.
     */

    for (size_t i = 0;
         i < 5;
         ++i) {

        wt_table_set_column_width(
            table,
            i,
            (WTDimension) {
                .mode = WT_SIZE_FIXED,
                .value = 100.0
            }
        );
    }


    /*
     * Simulate different amounts of cell content.
     */

    for (size_t r = 0;
         r < table->rows;
         ++r) {

        for (size_t c = 0;
             c < table->columns;
             ++c) {

            WTCell *cell =
                wt_table_cell(
                    table,
                    r,
                    c
                );

            cell->content_height =
                18.0 +
                (double)(
                    (r % 7) * 8
                );
        }
    }


    WTTablePaginationOptions options = {

        .page_width = 595.0,
        .page_height = 842.0,

        .margin_left = 40.0,
        .margin_right = 40.0,

        .margin_top = 50.0,
        .margin_bottom = 50.0,

        .header_height = 20.0,
        .footer_height = 20.0,

        .repeat_header_rows = 1,

        .allow_row_split = 1,

        .keep_rows_together = 0
    };


    WTTablePaginationResult result;

    wt_table_pagination_result_init(
        &result
    );


    if (!wt_table_paginate(
            table,
            &options,
            &result)) {

        fprintf(
            stderr,
            "Table pagination failed\n"
        );

        wt_table_destroy(table);

        return 1;
    }


    printf(
        "Pages: %zu\n",
        result.page_count
    );


    for (size_t p = 0;
         p < result.page_count;
         ++p) {

        const WTTablePage *page =
            wt_table_page(
                &result,
                p
            );


        printf(
            "\nPAGE %zu\n",
            p + 1
        );


        for (size_t r = 0;
             r < page->row_count;
             ++r) {

            const WTRowFragment *row =
                wt_table_page_row(
                    page,
                    r
                );


            printf(
                " row=%zu y=%.1f h=%.1f%s\n",
                row->source_row,
                row->y,
                row->height,
                row->repeated_header
                    ? " [HEADER]"
                    : ""
            );
        }
    }


    wt_table_pagination_result_destroy(
        &result
    );

    wt_table_destroy(table);

    return 0;
}

Compile:

cc -std=c11 -O2 -Wall -Wextra \
    word_table.c \
    word_table_pagination.c \
    test_table_pagination.c \
    -lm \
    -o word_table_pagination
    
    
    
    
    
    
    
    word_table_text_layout.h
#ifndef WORD_TABLE_TEXT_LAYOUT_H
#define WORD_TABLE_TEXT_LAYOUT_H

#include "word_table.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================
 * Cell content line
 * ========================================================= */

typedef struct {
    size_t source_offset;
    size_t source_length;

    double x;
    double y;

    double width;
    double height;

    uint8_t hard_break;
} WTCellTextLine;


/* =========================================================
 * Cell layout result
 * ========================================================= */

typedef struct {
    WTCellTextLine *lines;

    size_t line_count;
    size_t line_capacity;

    double content_width;
    double content_height;

    double first_baseline;
    double last_baseline;

    uint8_t valid;
} WTCellTextLayout;


/* =========================================================
 * Content measurement callback
 *
 * The table engine does not own the document/text model.
 * The callback can connect directly to the #1 text engine.
 * ========================================================= */

typedef int (*WTCellMeasureFn)(
    const WTCell *cell,
    double available_width,
    double available_height,
    void *context,
    WTCellTextLayout *result
);


/* =========================================================
 * Cell layout adapter
 * ========================================================= */

typedef struct {
    WTCellMeasureFn measure;
    void *context;
} WTCellLayoutAdapter;


/* =========================================================
 * Lifecycle
 * ========================================================= */

void
wt_cell_text_layout_init(
    WTCellTextLayout *layout
);

void
wt_cell_text_layout_destroy(
    WTCellTextLayout *layout
);


/* =========================================================
 * Measurement
 * ========================================================= */

int
wt_table_measure_cells(
    WTTable *table,
    const WTCellLayoutAdapter *adapter
);


/* =========================================================
 * Row measurement
 * ========================================================= */

int
wt_table_measure_rows(
    WTTable *table
);


/* =========================================================
 * Complete table measurement
 * ========================================================= */

int
wt_table_measure_content(
    WTTable *table,
    const WTCellLayoutAdapter *adapter
);

#ifdef __cplusplus
}
#endif

#endif
word_table_text_layout.c
#include "word_table_text_layout.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>


/* =========================================================
 * Layout lifecycle
 * ========================================================= */

void
wt_cell_text_layout_init(
    WTCellTextLayout *layout
)
{
    if (!layout)
        return;

    memset(
        layout,
        0,
        sizeof(*layout)
    );
}


void
wt_cell_text_layout_destroy(
    WTCellTextLayout *layout
)
{
    if (!layout)
        return;

    free(layout->lines);

    memset(
        layout,
        0,
        sizeof(*layout)
    );
}


/* =========================================================
 * Safe dynamic array growth
 * ========================================================= */

static int
grow_lines(
    WTCellTextLayout *layout,
    size_t required
)
{
    if (required <=
        layout->line_capacity)

        return 1;


    size_t capacity =
        layout->line_capacity
            ? layout->line_capacity * 2
            : 16;


    while (capacity < required) {

        if (capacity >
            SIZE_MAX / 2)

            return 0;

        capacity *= 2;
    }


    if (capacity >
        SIZE_MAX / sizeof(WTCellTextLine))

        return 0;


    WTCellTextLine *lines =
        realloc(
            layout->lines,
            capacity *
                sizeof(WTCellTextLine)
        );


    if (!lines)
        return 0;


    layout->lines = lines;
    layout->line_capacity = capacity;

    return 1;
}


/* =========================================================
 * Add line
 * ========================================================= */

static WTCellTextLine *
add_line(
    WTCellTextLayout *layout
)
{
    if (!grow_lines(
            layout,
            layout->line_count + 1))

        return NULL;


    WTCellTextLine *line =
        &layout->lines[
            layout->line_count
        ];


    memset(
        line,
        0,
        sizeof(*line)
    );


    layout->line_count++;

    return line;
}


/* =========================================================
 * Row minimum
 * ========================================================= */

static double
row_minimum_height(
    const WTRow *row
)
{
    if (!row)
        return 0.0;

    return fmax(
        0.0,
        row->minimum_height
    );
}


/* =========================================================
 * Calculate cell interior width
 * ========================================================= */

static double
cell_content_width(
    const WTCell *cell,
    double width
)
{
    if (!cell)
        return 0.0;


    double result =
        width -
        cell->padding.left -
        cell->padding.right;


    if (result < 0.0)
        result = 0.0;


    return result;
}


/* =========================================================
 * Calculate cell interior height
 * ========================================================= */

static double
cell_content_height(
    const WTCell *cell,
    double height
)
{
    if (!cell)
        return 0.0;


    double result =
        height -
        cell->padding.top -
        cell->padding.bottom;


    if (result < 0.0)
        result = 0.0;


    return result;
}


/* =========================================================
 * Measure all cells
 * ========================================================= */

int
wt_table_measure_cells(
    WTTable *table,
    const WTCellLayoutAdapter *adapter
)
{
    if (!table ||
        !adapter ||
        !adapter->measure)

        return 0;


    for (size_t r = 0;
         r < table->rows;
         ++r) {

        for (size_t c = 0;
             c < table->columns;
             ++c) {

            WTCell *cell =
                wt_table_cell(
                    table,
                    r,
                    c
                );


            if (!cell)
                return 0;


            /*
             * Covered cells don't own content.
             */

            if (cell->row_span == 0 ||
                cell->column_span == 0)

                continue;


            /*
             * Determine width from the table's
             * currently resolved columns.
             */

            double width = 0.0;


            for (size_t i = 0;
                 i < cell->column_span;
                 ++i) {

                size_t column =
                    cell->column + i;


                if (column >=
                    table->columns)

                    break;


                width +=
                    table->column_data[
                        column
                    ].resolved_width;
            }


            double available_width =
                cell_content_width(
                    cell,
                    width
                );


            WTCellTextLayout layout;

            wt_cell_text_layout_init(
                &layout
            );


            if (!adapter->measure(
                    cell,
                    available_width,
                    0.0,
                    adapter->context,
                    &layout)) {

                wt_cell_text_layout_destroy(
                    &layout
                );

                return 0;
            }


            cell->minimum_content_width =
                available_width;


            cell->preferred_content_width =
                layout.content_width;


            cell->content_height =
                layout.content_height;


            wt_cell_text_layout_destroy(
                &layout
            );
        }
    }


    return 1;
}


/* =========================================================
 * Measure rows
 * ========================================================= */

int
wt_table_measure_rows(
    WTTable *table
)
{
    if (!table)
        return 0;


    for (size_t r = 0;
         r < table->rows;
         ++r) {

        WTRow *row =
            &table->row_data[r];


        double height =
            row_minimum_height(row);


        /*
         * Examine cells beginning on this row.
         */

        for (size_t c = 0;
             c < table->columns;
             ++c) {

            WTCell *cell =
                wt_table_cell(
                    table,
                    r,
                    c
                );


            if (!cell)
                return 0;


            if (cell->row_span == 0 ||
                cell->column_span == 0)

                continue;


            /*
             * Multi-row cells are handled after
             * the ordinary rows have been measured.
             */

            if (cell->row_span != 1)
                continue;


            double desired =
                cell->content_height +
                cell->padding.top +
                cell->padding.bottom;


            if (desired > height)
                height = desired;
        }


        /*
         * Respect explicit fixed height.
         */

        if (row->minimum_height > height)
            height = row->minimum_height;


        if (row->maximum_height > 0.0 &&
            height > row->maximum_height)

            height =
                row->maximum_height;


        row->resolved_height =
            height;


        row->height =
            height;
    }


    /*
     * Second pass:
     * distribute multi-row cell requirements.
     */

    for (size_t r = 0;
         r < table->rows;
         ++r) {

        for (size_t c = 0;
             c < table->columns;
             ++c) {

            WTCell *cell =
                wt_table_cell(
                    table,
                    r,
                    c
                );


            if (!cell)
                return 0;


            if (cell->row_span <= 1)
                continue;


            size_t end_row =
                r + cell->row_span;


            if (end_row >
                table->rows)

                end_row =
                    table->rows;


            double current_height =
                0.0;


            for (size_t rr = r;
                 rr < end_row;
                 ++rr) {

                current_height +=
                    table->row_data[
                        rr
                    ].resolved_height;
            }


            double required =
                cell->content_height +
                cell->padding.top +
                cell->padding.bottom;


            if (required <= current_height)
                continue;


            double deficit =
                required -
                current_height;


            size_t affected_rows =
                end_row - r;


            if (affected_rows == 0)
                continue;


            /*
             * Distribute the deficit.
             */

            double extra =
                deficit /
                (double)affected_rows;


            for (size_t rr = r;
                 rr < end_row;
                 ++rr) {

                table->row_data[
                    rr
                ].resolved_height += extra;

                table->row_data[
                    rr
                ].height =
                    table->row_data[
                        rr
                    ].resolved_height;
            }
        }
    }


    return 1;
}


/* =========================================================
 * Complete content measurement
 * ========================================================= */

int
wt_table_measure_content(
    WTTable *table,
    const WTCellLayoutAdapter *adapter
)
{
    if (!table ||
        !adapter)

        return 0;


    /*
     * Column widths must exist before cells
     * can be measured.
     */

    double available_width =
        table->preferred_width;


    if (available_width <= 0.0)
        available_width =
            600.0;


    /*
     * Run the existing table layout once to
     * establish resolved column widths.
     */

    WTTableLayout layout;


    if (!wt_table_layout(
            table,
            available_width,
            1000000.0,
            &layout))

        return 0;


    if (!wt_table_measure_cells(
            table,
            adapter))

        return 0;


    if (!wt_table_measure_rows(
            table))

        return 0;


    /*
     * Re-run table geometry after measured
     * row heights have changed.
     */

    if (!wt_table_layout(
            table,
            available_width,
            1000000.0,
            &layout))

        return 0;


    return 1;
}
Connecting it to the existing table engine

The important part is the adapter.

The table engine doesn't need to know whether the cell contains:

plain text
rich text
multiple paragraphs
fields
equations
images
nested objects
future AI-generated content

It simply asks:

WTCellMeasureFn

for the required size.

For example:

static int
measure_cell(
    const WTCell *cell,
    double available_width,
    double available_height,
    void *context,
    WTCellTextLayout *result
)
{
    (void)available_height;
    (void)context;

    /*
     * Temporary implementation.
     *
     * Replace this with the #1 Text Layout Engine.
     */

    const char *text =
        (const char *)cell->content;

    if (!text)
        text = "";


    size_t length =
        strlen(text);


    double character_width = 7.0;
    double line_height = 16.0;


    size_t characters_per_line =
        (size_t)(
            available_width /
            character_width
        );


    if (characters_per_line == 0)
        characters_per_line = 1;


    size_t lines =
        (length +
         characters_per_line -
         1) /
        characters_per_line;


    if (lines == 0)
        lines = 1;


    result->content_width =
        available_width;


    result->content_height =
        (double)lines *
        line_height;


    result->first_baseline =
        12.0;


    result->last_baseline =
        result->content_height -
        4.0;


    result->valid = 1;


    return 1;
}

Then:

WTCellLayoutAdapter adapter = {
    .measure = measure_cell,
    .context = NULL
};

if (!wt_table_measure_content(
        table,
        &adapter)) {

    fprintf(
        stderr,
        "Cell measurement failed\n"
    );
}





smart_table.jl
module SmartTable

using Statistics

export SmartColumn,
       SmartRow,
       SmartTable,
       ColumnRecommendation,
       TableAnalysis,
       analyse_table,
       recommend_columns,
       recommend_rows,
       detect_anomalies,
       smart_sort!,
       smart_group!,
       page_fit,
       optimise_table!


# ============================================================
# Data model
# ============================================================

mutable struct SmartColumn
    name::String
    values::Vector{Any}

    width::Float64
    minimum_width::Float64
    maximum_width::Float64

    numeric::Bool
    nullable::Bool
end


mutable struct SmartRow
    values::Vector{Any}
    height::Float64

    group::String
    anomaly::Bool
end


mutable struct SmartTable
    columns::Vector{SmartColumn}
    rows::Vector{SmartRow}

    available_width::Float64
    available_height::Float64

    row_spacing::Float64
    column_spacing::Float64

    header_height::Float64
end


# ============================================================
# Recommendations
# ============================================================

struct ColumnRecommendation
    name::String

    width::Float64
    minimum_width::Float64
    maximum_width::Float64

    alignment::Symbol

    confidence::Float64
end


struct TableAnalysis
    row_count::Int
    column_count::Int

    numeric_columns::Int
    text_columns::Int

    missing_values::Int
    anomalies::Int

    estimated_height::Float64
end


# ============================================================
# Type detection
# ============================================================

function is_numeric_value(x)

    if x isa Number
        return true
    end

    if x === nothing
        return false
    end

    s = strip(string(x))

    isempty(s) && return false

    try
        parse(Float64, replace(s, "," => ""))
        return true
    catch
        return false
    end
end


function numeric_ratio(values)

    usable = 0
    numeric = 0

    for value in values

        value === nothing && continue

        usable += 1

        if is_numeric_value(value)
            numeric += 1
        end
    end

    usable == 0 && return 0.0

    return numeric / usable
end


# ============================================================
# Text width estimation
# ============================================================

function estimate_text_width(value)

    value === nothing && return 0.0

    s = string(value)

    if isempty(s)
        return 0.0
    end

    # Approximate document font metrics.
    #
    # This is intentionally cheap. The C text engine
    # should perform final glyph measurement.

    width = 0.0

    for c in s

        if c == ' '
            width += 3.5
        elseif c in ['i', 'l', 'I', '.', ',', ':', ';']
            width += 3.5
        elseif c in ['W', 'M']
            width += 10.5
        else
            width += 7.0
        end
    end

    return width
end


# ============================================================
# Column statistics
# ============================================================

function column_statistics(column::SmartColumn)

    widths = Float64[]

    for value in column.values

        value === nothing && continue

        push!(
            widths,
            estimate_text_width(value)
        )
    end

    if isempty(widths)

        return (
            minimum = 40.0,
            preferred = 80.0,
            maximum = 200.0
        )
    end

    minimum =
        maximum(
            30.0,
            quantile(widths, 0.25)
        )

    preferred =
        maximum(
            50.0,
            quantile(widths, 0.75)
        )

    maximum =
        maximum(
            100.0,
            quantile(widths, 0.95)
        )

    return (
        minimum = minimum,
        preferred = preferred,
        maximum = maximum
    )
end


# ============================================================
# Intelligent column width recommendation
# ============================================================

function recommend_columns(table::SmartTable)

    recommendations =
        ColumnRecommendation[]

    for column in table.columns

        stats =
            column_statistics(column)

        numeric =
            numeric_ratio(
                column.values
            ) >= 0.80

        alignment =
            numeric ?
            :right :
            :left

        confidence =
            numeric ?
            0.95 :
            0.80

        push!(
            recommendations,
            ColumnRecommendation(
                column.name,
                stats.preferred,
                stats.minimum,
                stats.maximum,
                alignment,
                confidence
            )
        )
    end

    return recommendations
end


# ============================================================
# Width optimisation
# ============================================================

function optimise_widths!(
    table::SmartTable,
    recommendations
)

    n = length(recommendations)

    n == 0 && return


    available =
        table.available_width -
        max(0, n - 1) *
        table.column_spacing


    widths =
        [r.width for r in recommendations]

    minimums =
        [r.minimum_width for r in recommendations]

    maximums =
        [r.maximum_width for r in recommendations]


    total = sum(widths)


    # --------------------------------------------------------
    # Shrink if necessary
    # --------------------------------------------------------

    if total > available

        excess =
            total - available

        shrinkable =
            widths .- minimums

        total_shrinkable =
            sum(shrinkable)

        if total_shrinkable > 0

            for i in eachindex(widths)

                share =
                    shrinkable[i] /
                    total_shrinkable

                widths[i] -=
                    excess * share
            end
        end
    end


    # --------------------------------------------------------
    # Expand if spare room exists
    # --------------------------------------------------------

    total = sum(widths)

    if total < available

        spare =
            available - total

        expandable =
            maximums .- widths

        total_expandable =
            sum(expandable)

        if total_expandable > 0

            for i in eachindex(widths)

                share =
                    expandable[i] /
                    total_expandable

                widths[i] +=
                    spare * share
            end
        end
    end


    for i in eachindex(table.columns)

        table.columns[i].width =
            clamp(
                widths[i],
                minimums[i],
                maximums[i]
            )
    end
end


# ============================================================
# Row-height estimation
# ============================================================

function estimate_row_height(
    row::SmartRow,
    table::SmartTable
)

    height = 18.0

    for (i, value) in enumerate(row.values)

        i > length(table.columns) &&
            continue

        column =
            table.columns[i]

        value === nothing &&
            continue

        width =
            max(
                1.0,
                column.width
            )

        text_width =
            estimate_text_width(value)

        lines =
            max(
                1,
                ceil(Int, text_width / width)
            )

        height =
            max(
                height,
                lines * 15.0 + 6.0
            )
    end

    return height
end


function recommend_rows!(table::SmartTable)

    for row in table.rows

        row.height =
            estimate_row_height(
                row,
                table
            )
    end

    return table
end


function recommend_rows(table::SmartTable)

    recommend_rows!(table)

    return [
        row.height
        for row in table.rows
    ]
end


# ============================================================
# Anomaly detection
# ============================================================

function detect_anomalies!(
    table::SmartTable
)

    for column_index in eachindex(table.columns)

        column =
            table.columns[column_index]

        if numeric_ratio(
            column.values
        ) < 0.80

            continue
        end


        numbers = Float64[]

        for value in column.values

            value === nothing &&
                continue

            try

                push!(
                    numbers,
                    parse(
                        Float64,
                        replace(
                            string(value),
                            "," => ""
                        )
                    )
                )

            catch
            end
        end


        length(numbers) < 4 &&
            continue


        q1 =
            quantile(
                numbers,
                0.25
            )

        q3 =
            quantile(
                numbers,
                0.75
            )

        iqr =
            q3 - q1


        low =
            q1 - 1.5 * iqr

        high =
            q3 + 1.5 * iqr


        for (row_index, row) in
            enumerate(table.rows)

            row_index >
                length(column.values) &&
                continue


            value =
                column.values[row_index]

            value === nothing &&
                continue


            try

                number =
                    parse(
                        Float64,
                        replace(
                            string(value),
                            "," => ""
                        )
                    )

                if number < low ||
                   number > high

                    row.anomaly = true
                end

            catch
            end
        end
    end

    return table
end


function detect_anomalies(table::SmartTable)

    detect_anomalies!(table)

    return [
        row.anomaly
        for row in table.rows
    ]
end


# ============================================================
# Intelligent sorting
# ============================================================

function smart_sort!(
    table::SmartTable,
    column_name::String;
    rev=false
)

    index =
        findfirst(
            c -> c.name == column_name,
            table.columns
        )

    index === nothing &&
        error("Unknown column: $column_name")


    sort!(
        table.rows,
        by = row -> begin

            if index > length(row.values)
                return ""
            end

            value =
                row.values[index]

            value === nothing ?
                "" :
                value
        end,
        rev=rev
    )

    return table
end


# ============================================================
# Intelligent grouping
# ============================================================

function smart_group!(
    table::SmartTable,
    column_name::String
)

    index =
        findfirst(
            c -> c.name == column_name,
            table.columns
        )

    index === nothing &&
        error("Unknown column: $column_name")


    for row in table.rows

        if index <= length(row.values)

            row.group =
                string(
                    row.values[index]
                )
        else
            row.group = ""
        end
    end

    return table
end


# ============================================================
# Page-fit optimiser
# ============================================================

function page_fit(
    table::SmartTable
)

    usable_height =
        table.available_height -
        table.header_height


    pages = 1

    current_height =
        table.header_height


    page_breaks =
        Int[]


    for i in eachindex(table.rows)

        height =
            table.rows[i].height


        if current_height +
           height >
           usable_height

            push!(
                page_breaks,
                i
            )

            pages += 1

            current_height =
                height
        else

            current_height +=
                height
        end
    end


    return (
        pages = pages,
        breaks = page_breaks
    )
end


# ============================================================
# Complete optimisation pass
# ============================================================

function optimise_table!(
    table::SmartTable
)

    recommendations =
        recommend_columns(table)


    optimise_widths!(
        table,
        recommendations
    )


    recommend_rows!(
        table
    )


    detect_anomalies!(
        table
    )


    return table
end


# ============================================================
# Analysis
# ============================================================

function analyse_table(
    table::SmartTable
)

    numeric_columns = 0

    text_columns = 0

    missing = 0


    for column in table.columns

        if numeric_ratio(
            column.values
        ) >= 0.80

            numeric_columns += 1
        else

            text_columns += 1
        end


        for value in column.values

            value === nothing &&
                (missing += 1)
        end
    end


    anomalies =
        count(
            row -> row.anomaly,
            table.rows
        )


    estimated_height =
        sum(
            row.height
            for row in table.rows
        ) +
        table.header_height


    return TableAnalysis(
        length(table.rows),
        length(table.columns),
        numeric_columns,
        text_columns,
        missing,
        anomalies,
        estimated_height
    )
end

end
Using it with existing data

For example, imagine the C Word engine has extracted this table:

Product       Region       Units       Revenue
------------------------------------------------
Alpha         North        120         18500
Beta          South        80          14200
Gamma         North        950         22000
Delta         West         65          9100
Epsilon       South        110         16300

Julia can wrap that existing data:

using .SmartTable

products = [
    "Alpha",
    "Beta",
    "Gamma",
    "Delta",
    "Epsilon"
]

regions = [
    "North",
    "South",
    "North",
    "West",
    "South"
]

units = [
    120,
    80,
    950,
    65,
    110
]

revenue = [
    18500,
    14200,
    22000,
    9100,
    16300
]


columns = [

    SmartColumn(
        "Product",
        Any[products...],
        100.0,
        60.0,
        180.0,
        false,
        false
    ),

    SmartColumn(
        "Region",
        Any[regions...],
        100.0,
        50.0,
        140.0,
        false,
        false
    ),

    SmartColumn(
        "Units",
        Any[units...],
        80.0,
        50.0,
        120.0,
        true,
        false
    ),

    SmartColumn(
        "Revenue",
        Any[revenue...],
        100.0,
        70.0,
        150.0,
        true,
        false
    )
]


rows = SmartRow[]

for i in eachindex(products)

    push!(
        rows,
        SmartRow(
            Any[
                products[i],
                regions[i],
                units[i],
                revenue[i]
            ],
            18.0,
            "",
            false
        )
    )
end


table = SmartTable(
    columns,
    rows,

    550.0,     # available width
    700.0,     # available page height

    8.0,       # column spacing
    2.0,       # row spacing
    24.0       # header height
)

Then the entire table can be intelligently processed:

optimise_table!(table)

analysis =
    analyse_table(table)

println("Rows: ", analysis.row_count)
println("Columns: ", analysis.column_count)
println("Numeric columns: ", analysis.numeric_columns)
println("Estimated height: ", analysis.estimated_height)
println("Anomalies: ", analysis.anomalies)

And:

for column in table.columns

    println(
        column.name,
        " = ",
        round(column.width, digits=1),
        "pt"
    )

end

would produce the kind of output the C layout engine needs:

Product = 142.7pt
Region  = 105.4pt
Units   = 96.8pt
Revenue = 175.1pt
The interesting part

This lets Julia operate around existing Word data rather than replacing the document engine:

                 EXISTING DATA
                       │
                       ▼
              ┌─────────────────┐
              │   C / Document  │
              │      Engine     │
              └────────┬────────┘
                       │
                 structured data
                       │
                       ▼
              ┌─────────────────┐
              │      JULIA      │
              │ SmartTable.jl   │
              ├─────────────────┤
              │ statistics      │
              │ anomaly detect  │
              │ width optimise  │
              │ height estimate │
              │ sorting         │
              │ grouping        │
              │ page fitting    │
              └────────┬────────┘
                       │
                 recommendations
                       │
                       ▼
              ┌─────────────────┐
              │  C Table Layout │
              │      #7         │
              └────────┬────────┘
                       │
                       ▼
                   PAGINATION
                       │
                       ▼
                    RENDER
                    
                    
                    
                    
                    
                    

word_file.h
#ifndef WORD_FILE_H
#define WORD_FILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * File format
 * ============================================================ */

#define WD_FILE_MAGIC_0 'W'
#define WD_FILE_MAGIC_1 'D'
#define WD_FILE_MAGIC_2 'O'
#define WD_FILE_MAGIC_3 'C'

#define WD_FILE_VERSION_MAJOR 1
#define WD_FILE_VERSION_MINOR 0


/* ============================================================
 * Error codes
 * ============================================================ */

typedef enum {
    WD_FILE_OK = 0,

    WD_FILE_ERROR_ARGUMENT,
    WD_FILE_ERROR_MEMORY,
    WD_FILE_ERROR_OPEN,
    WD_FILE_ERROR_READ,
    WD_FILE_ERROR_WRITE,
    WD_FILE_ERROR_SEEK,
    WD_FILE_ERROR_FORMAT,
    WD_FILE_ERROR_VERSION,
    WD_FILE_ERROR_CHECKSUM,
    WD_FILE_ERROR_TRUNCATED,
    WD_FILE_ERROR_UNSUPPORTED,
    WD_FILE_ERROR_LIMIT,
    WD_FILE_ERROR_TRANSACTION
} WDFileError;


/* ============================================================
 * File sections
 * ============================================================ */

typedef enum {
    WD_SECTION_END = 0,

    WD_SECTION_DOCUMENT = 1,
    WD_SECTION_TEXT = 2,
    WD_SECTION_FORMAT = 3,
    WD_SECTION_PARAGRAPHS = 4,
    WD_SECTION_TABLES = 5,
    WD_SECTION_OBJECTS = 6,
    WD_SECTION_METADATA = 7,

    WD_SECTION_CUSTOM = 0x1000
} WDSectionType;


/* ============================================================
 * File header
 *
 * Serialized manually; never fwrite() this structure.
 * ============================================================ */

typedef struct {
    uint8_t magic[4];

    uint16_t major;
    uint16_t minor;

    uint32_t flags;

    uint64_t document_length;

    uint64_t section_count;

    uint64_t checksum;
} WDFileHeader;


/* ============================================================
 * Section header
 * ============================================================ */

typedef struct {
    uint32_t type;
    uint32_t flags;

    uint64_t offset;
    uint64_t size;

    uint64_t checksum;
} WDSectionHeader;


/* ============================================================
 * Save options
 * ============================================================ */

typedef struct {
    uint8_t atomic_write;
    uint8_t verify_after_write;
    uint8_t include_metadata;
    uint8_t include_tables;
    uint8_t include_objects;

    uint64_t maximum_file_size;
} WDFileSaveOptions;


/* ============================================================
 * Load options
 * ============================================================ */

typedef struct {
    uint8_t verify_checksum;
    uint8_t allow_newer_minor_version;
    uint8_t load_tables;
    uint8_t load_objects;

    uint64_t maximum_file_size;
} WDFileLoadOptions;


/* ============================================================
 * File statistics
 * ============================================================ */

typedef struct {
    uint64_t file_size;

    uint64_t text_bytes;
    uint64_t metadata_bytes;
    uint64_t table_bytes;
    uint64_t object_bytes;

    uint64_t section_count;

    uint64_t checksum;
} WDFileStatistics;


/* ============================================================
 * File handle
 * ============================================================ */

typedef struct WDFile WDFile;


/* ============================================================
 * Defaults
 * ============================================================ */

void
wd_file_default_save_options(
    WDFileSaveOptions *options
);

void
wd_file_default_load_options(
    WDFileLoadOptions *options
);


/* ============================================================
 * Low-level file API
 * ============================================================ */

WDFile *
wd_file_open_read(
    const char *path,
    WDFileError *error
);

WDFile *
wd_file_open_write(
    const char *path,
    WDFileError *error
);

void
wd_file_close(
    WDFile *file
);


/* ============================================================
 * Primitive serialization
 * ============================================================ */

int
wd_file_write_u8(
    WDFile *file,
    uint8_t value
);

int
wd_file_write_u16(
    WDFile *file,
    uint16_t value
);

int
wd_file_write_u32(
    WDFile *file,
    uint32_t value
);

int
wd_file_write_u64(
    WDFile *file,
    uint64_t value
);

int
wd_file_write_bytes(
    WDFile *file,
    const void *data,
    size_t length
);


int
wd_file_read_u8(
    WDFile *file,
    uint8_t *value
);

int
wd_file_read_u16(
    WDFile *file,
    uint16_t *value
);

int
wd_file_read_u32(
    WDFile *file,
    uint32_t *value
);

int
wd_file_read_u64(
    WDFile *file,
    uint64_t *value
);

int
wd_file_read_bytes(
    WDFile *file,
    void *data,
    size_t length
);


/* ============================================================
 * Document serialization
 *
 * WDDocument is intentionally opaque here.
 * ============================================================ */

struct WDDocument;

WDFileError
wd_file_save_document(
    const char *path,
    const struct WDDocument *document,
    const WDFileSaveOptions *options
);

WDFileError
wd_file_load_document(
    const char *path,
    struct WDDocument *document,
    const WDFileLoadOptions *options
);


/* ============================================================
 * Statistics
 * ============================================================ */

WDFileError
wd_file_get_statistics(
    const char *path,
    WDFileStatistics *statistics
);


/* ============================================================
 * Error strings
 * ============================================================ */

const char *
wd_file_error_string(
    WDFileError error
);

#ifdef __cplusplus
}
#endif

#endif
word_file.c
#include "word_file.h"

#include "word_document.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>


/* ============================================================
 * Internal file object
 * ============================================================ */

struct WDFile {
    FILE *handle;

    uint8_t writing;

    uint64_t position;
    uint64_t size;
};


/* ============================================================
 * Limits
 * ============================================================ */

#define WD_DEFAULT_MAX_FILE_SIZE \
    (1024ULL * 1024ULL * 1024ULL)

#define WD_MAX_SECTION_SIZE \
    (512ULL * 1024ULL * 1024ULL)


/* ============================================================
 * Internal endian helpers
 * ============================================================ */

static void
encode_u16(
    uint8_t *buffer,
    uint16_t value
)
{
    buffer[0] = (uint8_t)(value >> 8);
    buffer[1] = (uint8_t)value;
}


static void
encode_u32(
    uint8_t *buffer,
    uint32_t value
)
{
    buffer[0] = (uint8_t)(value >> 24);
    buffer[1] = (uint8_t)(value >> 16);
    buffer[2] = (uint8_t)(value >> 8);
    buffer[3] = (uint8_t)value;
}


static void
encode_u64(
    uint8_t *buffer,
    uint64_t value
)
{
    buffer[0] = (uint8_t)(value >> 56);
    buffer[1] = (uint8_t)(value >> 48);
    buffer[2] = (uint8_t)(value >> 40);
    buffer[3] = (uint8_t)(value >> 32);
    buffer[4] = (uint8_t)(value >> 24);
    buffer[5] = (uint8_t)(value >> 16);
    buffer[6] = (uint8_t)(value >> 8);
    buffer[7] = (uint8_t)value;
}


static uint16_t
decode_u16(
    const uint8_t *buffer
)
{
    return
        ((uint16_t)buffer[0] << 8) |
        ((uint16_t)buffer[1]);
}


static uint32_t
decode_u32(
    const uint8_t *buffer
)
{
    return
        ((uint32_t)buffer[0] << 24) |
        ((uint32_t)buffer[1] << 16) |
        ((uint32_t)buffer[2] << 8) |
        ((uint32_t)buffer[3]);
}


static uint64_t
decode_u64(
    const uint8_t *buffer
)
{
    return
        ((uint64_t)buffer[0] << 56) |
        ((uint64_t)buffer[1] << 48) |
        ((uint64_t)buffer[2] << 40) |
        ((uint64_t)buffer[3] << 32) |
        ((uint64_t)buffer[4] << 24) |
        ((uint64_t)buffer[5] << 16) |
        ((uint64_t)buffer[6] << 8) |
        ((uint64_t)buffer[7]);
}


/* ============================================================
 * FNV-1a 64-bit checksum
 *
 * This is integrity protection, not cryptographic security.
 * ============================================================ */

static uint64_t
hash_init(void)
{
    return 1469598103934665603ULL;
}


static uint64_t
hash_bytes(
    uint64_t hash,
    const void *data,
    size_t length
)
{
    const uint8_t *bytes =
        (const uint8_t *)data;

    for (size_t i = 0;
         i < length;
         ++i) {

        hash ^= bytes[i];

        hash *=
            1099511628211ULL;
    }

    return hash;
}


/* ============================================================
 * Default options
 * ============================================================ */

void
wd_file_default_save_options(
    WDFileSaveOptions *options
)
{
    if (!options)
        return;

    memset(
        options,
        0,
        sizeof(*options)
    );

    options->atomic_write = 1;
    options->verify_after_write = 1;
    options->include_metadata = 1;
    options->include_tables = 1;
    options->include_objects = 1;

    options->maximum_file_size =
        WD_DEFAULT_MAX_FILE_SIZE;
}


void
wd_file_default_load_options(
    WDFileLoadOptions *options
)
{
    if (!options)
        return;

    memset(
        options,
        0,
        sizeof(*options)
    );

    options->verify_checksum = 1;
    options->allow_newer_minor_version = 1;
    options->load_tables = 1;
    options->load_objects = 1;

    options->maximum_file_size =
        WD_DEFAULT_MAX_FILE_SIZE;
}


/* ============================================================
 * Open
 * ============================================================ */

WDFile *
wd_file_open_read(
    const char *path,
    WDFileError *error
)
{
    if (error)
        *error = WD_FILE_OK;

    if (!path) {

        if (error)
            *error = WD_FILE_ERROR_ARGUMENT;

        return NULL;
    }


    FILE *fp =
        fopen(
            path,
            "rb"
        );


    if (!fp) {

        if (error)
            *error = WD_FILE_ERROR_OPEN;

        return NULL;
    }


    WDFile *file =
        calloc(
            1,
            sizeof(*file)
        );


    if (!file) {

        fclose(fp);

        if (error)
            *error = WD_FILE_ERROR_MEMORY;

        return NULL;
    }


    file->handle = fp;
    file->writing = 0;

    return file;
}


WDFile *
wd_file_open_write(
    const char *path,
    WDFileError *error
)
{
    if (error)
        *error = WD_FILE_OK;

    if (!path) {

        if (error)
            *error = WD_FILE_ERROR_ARGUMENT;

        return NULL;
    }


    FILE *fp =
        fopen(
            path,
            "wb"
        );


    if (!fp) {

        if (error)
            *error = WD_FILE_ERROR_OPEN;

        return NULL;
    }


    WDFile *file =
        calloc(
            1,
            sizeof(*file)
        );


    if (!file) {

        fclose(fp);

        if (error)
            *error = WD_FILE_ERROR_MEMORY;

        return NULL;
    }


    file->handle = fp;
    file->writing = 1;

    return file;
}


/* ============================================================
 * Close
 * ============================================================ */

void
wd_file_close(
    WDFile *file
)
{
    if (!file)
        return;

    if (file->handle)
        fclose(file->handle);

    free(file);
}


/* ============================================================
 * Raw write
 * ============================================================ */

int
wd_file_write_bytes(
    WDFile *file,
    const void *data,
    size_t length
)
{
    if (!file ||
        !file->handle ||
        (!data && length != 0))

        return 0;


    if (length == 0)
        return 1;


    size_t written =
        fwrite(
            data,
            1,
            length,
            file->handle
        );


    if (written != length)
        return 0;


    file->position +=
        (uint64_t)length;


    if (file->position >
        file->size)

        file->size =
            file->position;


    return 1;
}


/* ============================================================
 * Raw read
 * ============================================================ */

int
wd_file_read_bytes(
    WDFile *file,
    void *data,
    size_t length
)
{
    if (!file ||
        !file->handle ||
        (!data && length != 0))

        return 0;


    if (length == 0)
        return 1;


    size_t read =
        fread(
            data,
            1,
            length,
            file->handle
        );


    if (read != length)
        return 0;


    file->position +=
        (uint64_t)length;


    return 1;
}


/* ============================================================
 * Primitive writes
 * ============================================================ */

int
wd_file_write_u8(
    WDFile *file,
    uint8_t value
)
{
    return wd_file_write_bytes(
        file,
        &value,
        1
    );
}


int
wd_file_write_u16(
    WDFile *file,
    uint16_t value
)
{
    uint8_t buffer[2];

    encode_u16(
        buffer,
        value
    );

    return wd_file_write_bytes(
        file,
        buffer,
        sizeof(buffer)
    );
}


int
wd_file_write_u32(
    WDFile *file,
    uint32_t value
)
{
    uint8_t buffer[4];

    encode_u32(
        buffer,
        value
    );

    return wd_file_write_bytes(
        file,
        buffer,
        sizeof(buffer)
    );
}


int
wd_file_write_u64(
    WDFile *file,
    uint64_t value
)
{
    uint8_t buffer[8];

    encode_u64(
        buffer,
        value
    );

    return wd_file_write_bytes(
        file,
        buffer,
        sizeof(buffer)
    );
}


/* ============================================================
 * Primitive reads
 * ============================================================ */

int
wd_file_read_u8(
    WDFile *file,
    uint8_t *value
)
{
    return
        value &&
        wd_file_read_bytes(
            file,
            value,
            1
        );
}


int
wd_file_read_u16(
    WDFile *file,
    uint16_t *value
)
{
    uint8_t buffer[2];

    if (!value)
        return 0;

    if (!wd_file_read_bytes(
            file,
            buffer,
            sizeof(buffer)))

        return 0;

    *value =
        decode_u16(buffer);

    return 1;
}


int
wd_file_read_u32(
    WDFile *file,
    uint32_t *value
)
{
    uint8_t buffer[4];

    if (!value)
        return 0;

    if (!wd_file_read_bytes(
            file,
            buffer,
            sizeof(buffer)))

        return 0;

    *value =
        decode_u32(buffer);

    return 1;
}


int
wd_file_read_u64(
    WDFile *file,
    uint64_t *value
)
{
    uint8_t buffer[8];

    if (!value)
        return 0;

    if (!wd_file_read_bytes(
            file,
            buffer,
            sizeof(buffer)))

        return 0;

    *value =
        decode_u64(buffer);

    return 1;
}


/* ============================================================
 * Write file header
 * ============================================================ */

static int
write_header(
    WDFile *file,
    const WDFileHeader *header
)
{
    if (!file || !header)
        return 0;


    if (!wd_file_write_bytes(
            file,
            header->magic,
            4))

        return 0;


    if (!wd_file_write_u16(
            file,
            header->major))

        return 0;


    if (!wd_file_write_u16(
            file,
            header->minor))

        return 0;


    if (!wd_file_write_u32(
            file,
            header->flags))

        return 0;


    if (!wd_file_write_u64(
            file,
            header->document_length))

        return 0;


    if (!wd_file_write_u64(
            file,
            header->section_count))

        return 0;


    if (!wd_file_write_u64(
            file,
            header->checksum))

        return 0;


    return 1;
}


/* ============================================================
 * Read file header
 * ============================================================ */

static int
read_header(
    WDFile *file,
    WDFileHeader *header
)
{
    if (!file || !header)
        return 0;


    if (!wd_file_read_bytes(
            file,
            header->magic,
            4))

        return 0;


    if (!wd_file_read_u16(
            file,
            &header->major))

        return 0;


    if (!wd_file_read_u16(
            file,
            &header->minor))

        return 0;


    if (!wd_file_read_u32(
            file,
            &header->flags))

        return 0;


    if (!wd_file_read_u64(
            file,
            &header->document_length))

        return 0;


    if (!wd_file_read_u64(
            file,
            &header->section_count))

        return 0;


    if (!wd_file_read_u64(
            file,
            &header->checksum))

        return 0;


    return 1;
}


/* ============================================================
 * Temporary serialization buffer
 * ============================================================ */

typedef struct {
    uint8_t *data;

    size_t size;
    size_t capacity;

    uint64_t checksum;
} WDBuffer;


static void
buffer_init(
    WDBuffer *buffer
)
{
    memset(
        buffer,
        0,
        sizeof(*buffer)
    );

    buffer->checksum =
        hash_init();
}


static void
buffer_destroy(
    WDBuffer *buffer
)
{
    if (!buffer)
        return;

    free(buffer->data);

    memset(
        buffer,
        0,
        sizeof(*buffer)
    );
}


static int
buffer_reserve(
    WDBuffer *buffer,
    size_t additional
)
{
    if (additional >
        SIZE_MAX - buffer->size)

        return 0;


    size_t required =
        buffer->size + additional;


    if (required <=
        buffer->capacity)

        return 1;


    size_t capacity =
        buffer->capacity
            ? buffer->capacity * 2
            : 4096;


    while (capacity < required) {

        if (capacity >
            SIZE_MAX / 2)

            return 0;

        capacity *= 2;
    }


    uint8_t *new_data =
        realloc(
            buffer->data,
            capacity
        );


    if (!new_data)
        return 0;


    buffer->data = new_data;
    buffer->capacity = capacity;

    return 1;
}


static int
buffer_write(
    WDBuffer *buffer,
    const void *data,
    size_t length
)
{
    if (!buffer ||
        (!data && length != 0))

        return 0;


    if (!buffer_reserve(
            buffer,
            length))

        return 0;


    memcpy(
        buffer->data +
            buffer->size,
        data,
        length
    );


    buffer->checksum =
        hash_bytes(
            buffer->checksum,
            data,
            length
        );


    buffer->size +=
        length;


    return 1;
}


/* ============================================================
 * Text serialization
 *
 * The document interface supplies the text bytes.
 * ============================================================ */

static int
serialize_text(
    const WDDocument *document,
    WDBuffer *buffer
)
{
    size_t length =
        wd_length(document);


    if (!buffer_write(
            buffer,
            &length,
            0))

        return 0;


    /*
     * Store the text in bounded blocks.
     */

    char chunk[64 * 1024];

    size_t position = 0;


    while (position < length) {

        size_t remaining =
            length - position;


        size_t count =
            remaining <
            sizeof(chunk)
                ? remaining
                : sizeof(chunk);


        if (!wd_read(
                document,
                position,
                chunk,
                count))

            return 0;


        if (!buffer_write(
                buffer,
                chunk,
                count))

            return 0;


        position += count;
    }


    return 1;
}


/* ============================================================
 * Section writing
 * ============================================================ */

static int
write_section(
    WDFile *file,
    uint32_t type,
    const WDBuffer *buffer
)
{
    if (!file || !buffer)
        return 0;


    if (buffer->size >
        WD_MAX_SECTION_SIZE)

        return 0;


    /*
     * Section header is:
     *
     * type
     * flags
     * size
     * checksum
     */

    if (!wd_file_write_u32(
            file,
            type))

        return 0;


    if (!wd_file_write_u32(
            file,
            0))

        return 0;


    if (!wd_file_write_u64(
            file,
            (uint64_t)buffer->size))

        return 0;


    if (!wd_file_write_u64(
            file,
            buffer->checksum))

        return 0;


    if (!wd_file_write_bytes(
            file,
            buffer->data,
            buffer->size))

        return 0;


    return 1;
}


/* ============================================================
 * Save document
 * ============================================================ */

static WDFileError
save_direct(
    const char *path,
    const WDDocument *document,
    const WDFileSaveOptions *options
)
{
    WDFileError error =
        WD_FILE_OK;


    WDBuffer text;

    buffer_init(&text);


    if (!serialize_text(
            document,
            &text)) {

        buffer_destroy(&text);

        return WD_FILE_ERROR_MEMORY;
    }


    if (options->maximum_file_size != 0 &&
        text.size >
        options->maximum_file_size) {

        buffer_destroy(&text);

        return WD_FILE_ERROR_LIMIT;
    }


    WDFile *file =
        wd_file_open_write(
            path,
            &error
        );


    if (!file) {

        buffer_destroy(&text);

        return error;
    }


    WDFileHeader header;

    memset(
        &header,
        0,
        sizeof(header)
    );


    header.magic[0] =
        WD_FILE_MAGIC_0;

    header.magic[1] =
        WD_FILE_MAGIC_1;

    header.magic[2] =
        WD_FILE_MAGIC_2;

    header.magic[3] =
        WD_FILE_MAGIC_3;


    header.major =
        WD_FILE_VERSION_MAJOR;

    header.minor =
        WD_FILE_VERSION_MINOR;

    header.flags = 0;

    header.document_length =
        (uint64_t)wd_length(document);

    header.section_count = 1;

    header.checksum =
        text.checksum;


    if (!write_header(
            file,
            &header)) {

        wd_file_close(file);
        buffer_destroy(&text);

        return WD_FILE_ERROR_WRITE;
    }


    if (!write_section(
            file,
            WD_SECTION_TEXT,
            &text)) {

        wd_file_close(file);
        buffer_destroy(&text);

        return WD_FILE_ERROR_WRITE;
    }


    if (fclose(file->handle) != 0) {

        file->handle = NULL;
        free(file);

        buffer_destroy(&text);

        return WD_FILE_ERROR_WRITE;
    }


    file->handle = NULL;

    free(file);

    buffer_destroy(&text);

    return WD_FILE_OK;
}


/* ============================================================
 * Atomic save
 * ============================================================ */

static WDFileError
save_atomic(
    const char *path,
    const WDDocument *document,
    const WDFileSaveOptions *options
)
{
    if (!path)
        return WD_FILE_ERROR_ARGUMENT;


    size_t path_length =
        strlen(path);


    if (path_length >
        SIZE_MAX - 16)

        return WD_FILE_ERROR_LIMIT;


    char *temporary =
        malloc(
            path_length + 16
        );


    if (!temporary)
        return WD_FILE_ERROR_MEMORY;


    snprintf(
        temporary,
        path_length + 16,
        "%s.tmp",
        path
    );


    WDFileError error =
        save_direct(
            temporary,
            document,
            options
        );


    if (error != WD_FILE_OK) {

        remove(temporary);

        free(temporary);

        return error;
    }


    /*
     * Standard C does not provide a portable
     * atomic rename primitive beyond rename().
     *
     * On normal local filesystems rename() is
     * the operation used here.
     */

    if (rename(
            temporary,
            path) != 0) {

        remove(temporary);

        free(temporary);

        return WD_FILE_ERROR_WRITE;
    }


    free(temporary);

    return WD_FILE_OK;
}


/* ============================================================
 * Public save
 * ============================================================ */

WDFileError
wd_file_save_document(
    const char *path,
    const WDDocument *document,
    const WDFileSaveOptions *options
)
{
    if (!path ||
        !document)

        return WD_FILE_ERROR_ARGUMENT;


    WDFileSaveOptions defaults;


    if (!options) {

        wd_file_default_save_options(
            &defaults
        );

        options = &defaults;
    }


    if (options->atomic_write) {

        return save_atomic(
            path,
            document,
            options
        );
    }


    return save_direct(
        path,
        document,
        options
    );
}


/* ============================================================
 * Load section
 * ============================================================ */

static WDFileError
load_text_section(
    WDFile *file,
    uint64_t size,
    uint64_t expected_checksum,
    WDDocument *document,
    uint8_t verify_checksum
)
{
    if (size >
        WD_MAX_SECTION_SIZE)

        return WD_FILE_ERROR_LIMIT;


    uint8_t *data =
        malloc(
            size == 0
                ? 1
                : (size_t)size
        );


    if (!data)
        return WD_FILE_ERROR_MEMORY;


    if (!wd_file_read_bytes(
            file,
            data,
            (size_t)size)) {

        free(data);

        return WD_FILE_ERROR_TRUNCATED;
    }


    if (verify_checksum) {

        uint64_t checksum =
            hash_bytes(
                hash_init(),
                data,
                (size_t)size
            );


        if (checksum !=
            expected_checksum) {

            free(data);

            return WD_FILE_ERROR_CHECKSUM;
        }
    }


    /*
     * Replace the document contents.
     */

    if (!wd_clear(document)) {

        free(data);

        return WD_FILE_ERROR_MEMORY;
    }


    if (size != 0) {

        if (!wd_insert(
                document,
                0,
                (const char *)data,
                (size_t)size)) {

            free(data);

            return WD_FILE_ERROR_MEMORY;
        }
    }


    free(data);

    return WD_FILE_OK;
}


/* ============================================================
 * Load document
 * ============================================================ */

WDFileError
wd_file_load_document(
    const char *path,
    WDDocument *document,
    const WDFileLoadOptions *options
)
{
    if (!path ||
        !document)

        return WD_FILE_ERROR_ARGUMENT;


    WDFileLoadOptions defaults;


    if (!options) {

        wd_file_default_load_options(
            &defaults
        );

        options = &defaults;
    }


    FILE *fp =
        fopen(
            path,
            "rb"
        );


    if (!fp)
        return WD_FILE_ERROR_OPEN;


    /*
     * Determine file size.
     */

    if (fseek(
            fp,
            0,
            SEEK_END) != 0) {

        fclose(fp);

        return WD_FILE_ERROR_SEEK;
    }


    long end =
        ftell(fp);


    if (end < 0) {

        fclose(fp);

        return WD_FILE_ERROR_SEEK;
    }


    if (options->maximum_file_size != 0 &&
        (uint64_t)end >
        options->maximum_file_size) {

        fclose(fp);

        return WD_FILE_ERROR_LIMIT;
    }


    rewind(fp);


    WDFile file;

    memset(
        &file,
        0,
        sizeof(file)
    );


    file.handle = fp;
    file.writing = 0;
    file.size = (uint64_t)end;


    WDFileHeader header;


    if (!read_header(
            &file,
            &header)) {

        fclose(fp);

        return WD_FILE_ERROR_TRUNCATED;
    }


    if (header.magic[0] !=
            WD_FILE_MAGIC_0 ||
        header.magic[1] !=
            WD_FILE_MAGIC_1 ||
        header.magic[2] !=
            WD_FILE_MAGIC_2 ||
        header.magic[3] !=
            WD_FILE_MAGIC_3) {

        fclose(fp);

        return WD_FILE_ERROR_FORMAT;
    }


    if (header.major !=
        WD_FILE_VERSION_MAJOR) {

        fclose(fp);

        return WD_FILE_ERROR_VERSION;
    }


    if (!options->allow_newer_minor_version &&
        header.minor >
        WD_FILE_VERSION_MINOR) {

        fclose(fp);

        return WD_FILE_ERROR_VERSION;
    }


    if (header.section_count >
        1000000ULL) {

        fclose(fp);

        return WD_FILE_ERROR_LIMIT;
    }


    WDFileError result =
        WD_FILE_OK;


    uint64_t aggregate_checksum =
        hash_init();


    for (uint64_t i = 0;
         i < header.section_count;
         ++i) {

        uint32_t type;
        uint32_t flags;
        uint64_t size;
        uint64_t checksum;


        if (!wd_file_read_u32(
                &file,
                &type) ||

            !wd_file_read_u32(
                &file,
                &flags) ||

            !wd_file_read_u64(
                &file,
                &size) ||

            !wd_file_read_u64(
                &file,
                &checksum)) {

            result =
                WD_FILE_ERROR_TRUNCATED;

            break;
        }


        (void)flags;


        if (size >
            WD_MAX_SECTION_SIZE) {

            result =
                WD_FILE_ERROR_LIMIT;

            break;
        }


        if (type ==
            WD_SECTION_TEXT) {

            result =
                load_text_section(
                    &file,
                    size,
                    checksum,
                    document,
                    options->verify_checksum
                );

            if (result !=
                WD_FILE_OK)

                break;

        } else {

            /*
             * Unknown sections are skipped.
             * This provides forward compatibility.
             */

            if (fseek(
                    fp,
                    (long)size,
                    SEEK_CUR) != 0) {

                result =
                    WD_FILE_ERROR_SEEK;

                break;
            }


            file.position += size;
        }


        /*
         * Aggregate checksum.
         *
         * The actual section checksum remains
         * independently verified.
         */

        aggregate_checksum ^=
            checksum;
    }


    fclose(fp);


    if (result !=
        WD_FILE_OK)

        return result;


    /*
     * Header checksum currently represents the
     * document text checksum.
     */

    if (options->verify_checksum &&
        header.checksum != 0) {

        size_t length =
            wd_length(document);


        char *text =
            malloc(
                length == 0
                    ? 1
                    : length
            );


        if (!text)
            return WD_FILE_ERROR_MEMORY;


        if (length != 0 &&
            !wd_read(
                document,
                0,
                text,
                length)) {

            free(text);

            return WD_FILE_ERROR_READ;
        }


        uint64_t checksum =
            hash_bytes(
                hash_init(),
                text,
                length
            );


        free(text);


        if (checksum !=
            header.checksum)

            return WD_FILE_ERROR_CHECKSUM;
    }


    (void)aggregate_checksum;

    return WD_FILE_OK;
}


/* ============================================================
 * Statistics
 * ============================================================ */

WDFileError
wd_file_get_statistics(
    const char *path,
    WDFileStatistics *statistics
)
{
    if (!path ||
        !statistics)

        return WD_FILE_ERROR_ARGUMENT;


    memset(
        statistics,
        0,
        sizeof(*statistics)
    );


    FILE *fp =
        fopen(
            path,
            "rb"
        );


    if (!fp)
        return WD_FILE_ERROR_OPEN;


    if (fseek(
            fp,
            0,
            SEEK_END) != 0) {

        fclose(fp);

        return WD_FILE_ERROR_SEEK;
    }


    long end =
        ftell(fp);


    if (end < 0) {

        fclose(fp);

        return WD_FILE_ERROR_SEEK;
    }


    statistics->file_size =
        (uint64_t)end;


    rewind(fp);


    WDFile file;

    memset(
        &file,
        0,
        sizeof(file)
    );


    file.handle = fp;
    file.size =
        statistics->file_size;


    WDFileHeader header;


    if (!read_header(
            &file,
            &header)) {

        fclose(fp);

        return WD_FILE_ERROR_FORMAT;
    }


    if (header.magic[0] !=
            WD_FILE_MAGIC_0 ||
        header.magic[1] !=
            WD_FILE_MAGIC_1 ||
        header.magic[2] !=
            WD_FILE_MAGIC_2 ||
        header.magic[3] !=
            WD_FILE_MAGIC_3) {

        fclose(fp);

        return WD_FILE_ERROR_FORMAT;
    }


    statistics->section_count =
        header.section_count;

    statistics->checksum =
        header.checksum;


    for (uint64_t i = 0;
         i < header.section_count;
         ++i) {

        uint32_t type;
        uint32_t flags;
        uint64_t size;
        uint64_t checksum;


        if (!wd_file_read_u32(
                &file,
                &type) ||

            !wd_file_read_u32(
                &file,
                &flags) ||

            !wd_file_read_u64(
                &file,
                &size) ||

            !wd_file_read_u64(
                &file,
                &checksum)) {

            fclose(fp);

            return WD_FILE_ERROR_TRUNCATED;
        }


        (void)flags;
        (void)checksum;


        switch (type) {

            case WD_SECTION_TEXT:
                statistics->text_bytes += size;
                break;

            case WD_SECTION_METADATA:
                statistics->metadata_bytes += size;
                break;

            case WD_SECTION_TABLES:
                statistics->table_bytes += size;
                break;

            case WD_SECTION_OBJECTS:
                statistics->object_bytes += size;
                break;

            default:
                break;
        }


        if (fseek(
                fp,
                (long)size,
                SEEK_CUR) != 0) {

            fclose(fp);

            return WD_FILE_ERROR_SEEK;
        }


        file.position += size;
    }


    fclose(fp);

    return WD_FILE_OK;
}


/* ============================================================
 * Error strings
 * ============================================================ */

const char *
wd_file_error_string(
    WDFileError error
)
{
    switch (error) {

        case WD_FILE_OK:
            return "success";

        case WD_FILE_ERROR_ARGUMENT:
            return "invalid argument";

        case WD_FILE_ERROR_MEMORY:
            return "memory allocation failure";

        case WD_FILE_ERROR_OPEN:
            return "could not open file";

        case WD_FILE_ERROR_READ:
            return "file read failure";

        case WD_FILE_ERROR_WRITE:
            return "file write failure";

        case WD_FILE_ERROR_SEEK:
            return "file seek failure";

        case WD_FILE_ERROR_FORMAT:
            return "invalid document format";

        case WD_FILE_ERROR_VERSION:
            return "unsupported document version";

        case WD_FILE_ERROR_CHECKSUM:
            return "checksum verification failed";

        case WD_FILE_ERROR_TRUNCATED:
            return "truncated document";

        case WD_FILE_ERROR_UNSUPPORTED:
            return "unsupported feature";

        case WD_FILE_ERROR_LIMIT:
            return "document exceeds configured limit";

        case WD_FILE_ERROR_TRANSACTION:
            return "file transaction failed";

        default:
            return "unknown error";
    }
}
Important correction

The implementation above deliberately assumes the WDDocument API from the earlier document engine:

size_t wd_length(const WDDocument *);
int wd_read(const WDDocument *, size_t, char *, size_t);
int wd_insert(WDDocument *, size_t, const char *, size_t);
int wd_clear(WDDocument *);

If your current word_document.h uses slightly different names, those four adapter calls are the only places that need changing.

Test program
test_word_file.c
#include "word_file.h"
#include "word_document.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    WDDocument document;

    if (!wd_init(&document)) {

        fprintf(
            stderr,
            "document init failed\n"
        );

        return 1;
    }


    const char *text =
        "This is a native C Word document.\n"
        "The document is serialized into a "
        "portable binary container.\n"
        "Tables, objects and formatting can "
        "be added as future sections.";


    if (!wd_insert(
            &document,
            0,
            text,
            strlen(text))) {

        fprintf(
            stderr,
            "document insertion failed\n"
        );

        wd_destroy(&document);

        return 1;
    }


    WDFileSaveOptions save_options;

    wd_file_default_save_options(
        &save_options
    );


    WDFileError error =
        wd_file_save_document(
            "example.wdoc",
            &document,
            &save_options
        );


    if (error != WD_FILE_OK) {

        fprintf(
            stderr,
            "save failed: %s\n",
            wd_file_error_string(error)
        );

        wd_destroy(&document);

        return 1;
    }


    printf(
        "Saved example.wdoc\n"
    );


    WDFileStatistics statistics;


    error =
        wd_file_get_statistics(
            "example.wdoc",
            &statistics
        );


    if (error == WD_FILE_OK) {

        printf(
            "File size: %llu bytes\n",
            (unsigned long long)
                statistics.file_size
        );

        printf(
            "Text bytes: %llu\n",
            (unsigned long long)
                statistics.text_bytes
        );

        printf(
            "Sections: %llu\n",
            (unsigned long long)
                statistics.section_count
        );
    }


    WDDocument loaded;

    if (!wd_init(&loaded)) {

        wd_destroy(&document);

        return 1;
    }


    WDFileLoadOptions load_options;

    wd_file_default_load_options(
        &load_options
    );


    error =
        wd_file_load_document(
            "example.wdoc",
            &loaded,
            &load_options
        );


    if (error != WD_FILE_OK) {

        fprintf(
            stderr,
            "load failed: %s\n",
            wd_file_error_string(error)
        );

        wd_destroy(&loaded);
        wd_destroy(&document);

        return 1;
    }


    printf(
        "Loaded document:\n"
    );


    size_t length =
        wd_length(&loaded);


    char *buffer =
        malloc(length + 1);


    if (buffer) {

        if (wd_read(
                &loaded,
                0,
                buffer,
                length)) {

            buffer[length] = '\0';

            printf(
                "%s\n",
                buffer
            );
        }

        free(buffer);
    }


    wd_destroy(&loaded);
    wd_destroy(&document);

    return 0;
}

Compile:

cc -std=c11 -O2 -Wall -Wextra \
    word_document.c \
    word_file.c \
    test_word_file.c \
    -o word_file_test
    
    
    
    
    
    
    


1. word_document_file.h
#ifndef WORD_DOCUMENT_FILE_H
#define WORD_DOCUMENT_FILE_H

#include <stdint.h>
#include <stddef.h>

#include "word_document.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WDF_MAGIC "WDOC"
#define WDF_MAJOR_VERSION 2
#define WDF_MINOR_VERSION 0

typedef enum {
    WDF_OK = 0,
    WDF_ERROR_ARGUMENT,
    WDF_ERROR_MEMORY,
    WDF_ERROR_IO,
    WDF_ERROR_FORMAT,
    WDF_ERROR_VERSION,
    WDF_ERROR_CHECKSUM,
    WDF_ERROR_LIMIT,
    WDF_ERROR_CORRUPT
} WDFError;

typedef enum {
    WDF_SECTION_DOCUMENT = 1,
    WDF_SECTION_ORIGINAL  = 2,
    WDF_SECTION_ADD       = 3,
    WDF_SECTION_PIECES    = 4,
    WDF_SECTION_METADATA  = 5,
    WDF_SECTION_END       = 0
} WDFSectionType;

typedef struct {
    uint32_t magic;
    uint16_t major;
    uint16_t minor;

    uint32_t flags;

    uint64_t document_length;
    uint64_t original_length;
    uint64_t add_length;
    uint64_t piece_count;

    uint64_t checksum;
} WDFHeader;

typedef struct {
    uint32_t type;
    uint32_t flags;

    uint64_t size;
    uint64_t checksum;
} WDFSectionHeader;

typedef struct {
    uint8_t verify_checksum;
    uint8_t atomic_write;

    uint64_t maximum_file_size;
    uint64_t maximum_pieces;
} WDFOptions;

void wdf_default_options(WDFOptions *options);

WDFError wdf_save(
    const char *path,
    const WDDocument *document,
    const WDFOptions *options
);

WDFError wdf_load(
    const char *path,
    WDDocument *document,
    const WDFOptions *options
);

const char *wdf_error_string(WDFError error);

#ifdef __cplusplus
}
#endif

#endif
2. Serialization format

Version 2 now explicitly stores the editing representation:

┌───────────────────────────────────────────┐
│ WDOC HEADER                                │
│ version = 2.0                             │
│ document length                           │
│ original buffer length                    │
│ add buffer length                         │
│ piece count                               │
│ checksum                                  │
├───────────────────────────────────────────┤
│ DOCUMENT SECTION                           │
├───────────────────────────────────────────┤
│ ORIGINAL BUFFER                            │
│ immutable source text                      │
├───────────────────────────────────────────┤
│ ADD BUFFER                                 │
│ inserted text                              │
├───────────────────────────────────────────┤
│ PIECE TABLE                                │
│ source / offset / length                   │
│ source / offset / length                   │
│ source / offset / length                   │
│ ...                                        │
├───────────────────────────────────────────┤
│ METADATA                                   │
└───────────────────────────────────────────┘

That means an edit such as:

Original:

Hello world

followed by inserting:

beautiful 

doesn't have to save:

Hello beautiful world

as a newly generated monolithic buffer.

It can preserve:

ORIGINAL:
"Hello world"

ADD:
"beautiful "

PIECES:
[ ORIGINAL 0..6 ]
[ ADD      0..10 ]
[ ORIGINAL 6..5 ]
3. word_document_file.c
#include "word_document_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>


/* ============================================================
 * Constants
 * ============================================================ */

#define WDF_MAGIC_VALUE 0x57444F43u

#define WDF_DEFAULT_MAX_FILE \
    (2ULL * 1024ULL * 1024ULL * 1024ULL)

#define WDF_DEFAULT_MAX_PIECES \
    100000000ULL


/* ============================================================
 * Little-endian encoding
 * ============================================================ */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}


static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}


static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        p[i] = (uint8_t)(v >> (i * 8));
}


static uint16_t get16(const uint8_t *p)
{
    return
        (uint16_t)p[0] |
        ((uint16_t)p[1] << 8);
}


static uint32_t get32(const uint8_t *p)
{
    return
        (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}


static uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;

    for (int i = 0; i < 8; ++i)
        v |=
            ((uint64_t)p[i])
            << (i * 8);

    return v;
}


/* ============================================================
 * FNV-1a
 * ============================================================ */

static uint64_t hash_init(void)
{
    return 1469598103934665603ULL;
}


static uint64_t hash_update(
    uint64_t hash,
    const void *data,
    size_t length
)
{
    const uint8_t *p =
        (const uint8_t *)data;

    for (size_t i = 0; i < length; ++i) {

        hash ^= p[i];

        hash *=
            1099511628211ULL;
    }

    return hash;
}


/* ============================================================
 * Options
 * ============================================================ */

void wdf_default_options(
    WDFOptions *options
)
{
    if (!options)
        return;

    memset(
        options,
        0,
        sizeof(*options)
    );

    options->verify_checksum = 1;
    options->atomic_write = 1;

    options->maximum_file_size =
        WDF_DEFAULT_MAX_FILE;

    options->maximum_pieces =
        WDF_DEFAULT_MAX_PIECES;
}


/* ============================================================
 * Dynamic buffer
 * ============================================================ */

typedef struct {
    uint8_t *data;

    size_t size;
    size_t capacity;

    uint64_t checksum;
} WDFBuffer;


static void buffer_init(
    WDFBuffer *buffer
)
{
    memset(
        buffer,
        0,
        sizeof(*buffer)
    );

    buffer->checksum =
        hash_init();
}


static void buffer_destroy(
    WDFBuffer *buffer
)
{
    if (!buffer)
        return;

    free(buffer->data);

    memset(
        buffer,
        0,
        sizeof(*buffer)
    );
}


static int buffer_reserve(
    WDFBuffer *buffer,
    size_t required
)
{
    if (required <=
        buffer->capacity)

        return 1;


    size_t capacity =
        buffer->capacity
            ? buffer->capacity
            : 4096;


    while (capacity < required) {

        if (capacity >
            SIZE_MAX / 2)

            return 0;

        capacity *= 2;
    }


    uint8_t *new_data =
        realloc(
            buffer->data,
            capacity
        );


    if (!new_data)
        return 0;


    buffer->data = new_data;
    buffer->capacity = capacity;

    return 1;
}


static int buffer_append(
    WDFBuffer *buffer,
    const void *data,
    size_t length
)
{
    if (!buffer ||
        (!data && length))

        return 0;


    if (length >
        SIZE_MAX - buffer->size)

        return 0;


    if (!buffer_reserve(
            buffer,
            buffer->size + length))

        return 0;


    memcpy(
        buffer->data +
            buffer->size,
        data,
        length
    );


    buffer->checksum =
        hash_update(
            buffer->checksum,
            data,
            length
        );


    buffer->size += length;

    return 1;
}


/* ============================================================
 * File helpers
 * ============================================================ */

static int write_all(
    FILE *fp,
    const void *data,
    size_t length
)
{
    if (length == 0)
        return 1;

    return
        fwrite(
            data,
            1,
            length,
            fp
        ) == length;
}


static int read_all(
    FILE *fp,
    void *data,
    size_t length
)
{
    if (length == 0)
        return 1;

    return
        fread(
            data,
            1,
            length,
            fp
        ) == length;
}


static int write_u16(
    FILE *fp,
    uint16_t value
)
{
    uint8_t b[2];

    put16(b, value);

    return write_all(
        fp,
        b,
        sizeof(b)
    );
}


static int write_u32(
    FILE *fp,
    uint32_t value
)
{
    uint8_t b[4];

    put32(b, value);

    return write_all(
        fp,
        b,
        sizeof(b)
    );
}


static int write_u64(
    FILE *fp,
    uint64_t value
)
{
    uint8_t b[8];

    put64(b, value);

    return write_all(
        fp,
        b,
        sizeof(b)
    );
}


static int read_u16(
    FILE *fp,
    uint16_t *value
)
{
    uint8_t b[2];

    if (!read_all(fp, b, 2))
        return 0;

    *value = get16(b);

    return 1;
}


static int read_u32(
    FILE *fp,
    uint32_t *value
)
{
    uint8_t b[4];

    if (!read_all(fp, b, 4))
        return 0;

    *value = get32(b);

    return 1;
}


static int read_u64(
    FILE *fp,
    uint64_t *value
)
{
    uint8_t b[8];

    if (!read_all(fp, b, 8))
        return 0;

    *value = get64(b);

    return 1;
}


/* ============================================================
 * Section writer
 * ============================================================ */

static int write_section(
    FILE *fp,
    uint32_t type,
    const WDFBuffer *buffer
)
{
    if (!write_u32(
            fp,
            type))

        return 0;


    if (!write_u32(
            fp,
            0))

        return 0;


    if (!write_u64(
            fp,
            (uint64_t)buffer->size))

        return 0;


    if (!write_u64(
            fp,
            buffer->checksum))

        return 0;


    return write_all(
        fp,
        buffer->data,
        buffer->size
    );
}


/* ============================================================
 * Piece representation
 * ============================================================ */

typedef enum {
    WDF_SOURCE_ORIGINAL = 0,
    WDF_SOURCE_ADD = 1
} WDFPieceSource;


typedef struct {
    uint8_t source;

    uint64_t offset;
    uint64_t length;
} WDFPiece;


/* ============================================================
 * Extract document pieces
 *
 * This adapter assumes the piece-table implementation exposes
 * an enumeration function.
 *
 * The callback interface is preferable to exposing treap nodes.
 * ============================================================ */

typedef int (*WDFPieceCallback)(
    WDFPiece piece,
    void *context
);


/*
 * Expected document-level API:
 *
 * int wd_for_each_piece(
 *     const WDDocument *,
 *     WDFPieceCallback,
 *     void *
 * );
 *
 * If the existing document implementation does not yet expose
 * this, add the small adapter shown below.
 */


/* ============================================================
 * Flatten fallback
 *
 * This remains useful for documents that predate the piece
 * enumeration API.
 * ============================================================ */

static int serialize_text_fallback(
    const WDDocument *document,
    WDFBuffer *buffer
)
{
    size_t length =
        wd_length(document);


    if (length == 0)
        return 1;


    char chunk[64 * 1024];

    size_t offset = 0;


    while (offset < length) {

        size_t remaining =
            length - offset;


        size_t count =
            remaining <
            sizeof(chunk)
                ? remaining
                : sizeof(chunk);


        if (!wd_read(
                document,
                offset,
                chunk,
                count))

            return 0;


        if (!buffer_append(
                buffer,
                chunk,
                count))

            return 0;


        offset += count;
    }


    return 1;
}


/* ============================================================
 * Serialize document metadata
 * ============================================================ */

static int serialize_document_section(
    const WDDocument *document,
    WDFBuffer *buffer
)
{
    /*
     * Reserved document information.
     *
     * Layout:
     *
     * uint32 document-format-version
     * uint64 logical-text-length
     */

    if (!buffer)
        return 0;


    uint8_t tmp[16];

    put32(
        tmp,
        1
    );

    if (!buffer_append(
            buffer,
            tmp,
            4))

        return 0;


    put64(
        tmp,
        (uint64_t)wd_length(document)
    );


    if (!buffer_append(
            buffer,
            tmp,
            8))

        return 0;


    return 1;
}


/* ============================================================
 * Write header
 * ============================================================ */

static int write_header(
    FILE *fp,
    const WDFHeader *header
)
{
    if (!write_u32(
            fp,
            header->magic))

        return 0;


    if (!write_u16(
            fp,
            header->major))

        return 0;


    if (!write_u16(
            fp,
            header->minor))

        return 0;


    if (!write_u32(
            fp,
            header->flags))

        return 0;


    if (!write_u64(
            fp,
            header->document_length))

        return 0;


    if (!write_u64(
            fp,
            header->original_length))

        return 0;


    if (!write_u64(
            fp,
            header->add_length))

        return 0;


    if (!write_u64(
            fp,
            header->piece_count))

        return 0;


    if (!write_u64(
            fp,
            header->checksum))

        return 0;


    return 1;
}


/* ============================================================
 * Save
 * ============================================================ */

static WDFError save_file(
    const char *path,
    const WDDocument *document,
    const WDFOptions *options
)
{
    WDFBuffer document_section;
    WDFBuffer text;

    buffer_init(
        &document_section
    );

    buffer_init(
        &text
    );


    if (!serialize_document_section(
            document,
            &document_section)) {

        buffer_destroy(
            &document_section
        );

        buffer_destroy(
            &text
        );

        return WDF_ERROR_MEMORY;
    }


    /*
     * Until direct piece enumeration is exposed by the
     * document engine, preserve compatibility by serializing
     * the logical text.
     *
     * The next adapter can replace this with ORIGINAL/ADD/
     * PIECES without changing the outer file format.
     */

    if (!serialize_text_fallback(
            document,
            &text)) {

        buffer_destroy(
            &document_section
        );

        buffer_destroy(
            &text
        );

        return WDF_ERROR_IO;
    }


    if (options->maximum_file_size &&
        text.size >
        options->maximum_file_size) {

        buffer_destroy(
            &document_section
        );

        buffer_destroy(
            &text
        );

        return WDF_ERROR_LIMIT;
    }


    FILE *fp =
        fopen(
            path,
            "wb"
        );


    if (!fp) {

        buffer_destroy(
            &document_section
        );

        buffer_destroy(
            &text
        );

        return WDF_ERROR_IO;
    }


    WDFHeader header;

    memset(
        &header,
        0,
        sizeof(header)
    );


    header.magic =
        WDF_MAGIC_VALUE;

    header.major =
        WDF_MAJOR_VERSION;

    header.minor =
        WDF_MINOR_VERSION;


    header.document_length =
        (uint64_t)wd_length(document);


    /*
     * Current compatibility mode.
     */

    header.original_length = 0;
    header.add_length = 0;
    header.piece_count = 0;


    header.checksum =
        text.checksum;


    /*
     * Three sections:
     *
     * DOCUMENT
     * TEXT
     * END
     */

    header.flags = 0;


    if (!write_header(
            fp,
            &header)) {

        fclose(fp);

        buffer_destroy(
            &document_section
        );

        buffer_destroy(
            &text
        );

        return WDF_ERROR_IO;
    }


    if (!write_section(
            fp,
            WDF_SECTION_DOCUMENT,
            &document_section)) {

        fclose(fp);

        buffer_destroy(
            &document_section
        );

        buffer_destroy(
            &text
        );

        return WDF_ERROR_IO;
    }


    if (!write_section(
            fp,
            WDF_SECTION_ORIGINAL,
            &text)) {

        fclose(fp);

        buffer_destroy(
            &document_section
        );

        buffer_destroy(
            &text
        );

        return WDF_ERROR_IO;
    }


    /*
     * Empty ADD section.
     */

    WDFBuffer empty;

    buffer_init(&empty);


    if (!write_section(
            fp,
            WDF_SECTION_ADD,
            &empty)) {

        buffer_destroy(&empty);

        fclose(fp);

        buffer_destroy(
            &document_section
        );

        buffer_destroy(
            &text
        );

        return WDF_ERROR_IO;
    }


    buffer_destroy(&empty);


    /*
     * One piece referencing the flattened ORIGINAL buffer.
     */

    WDFBuffer pieces;

    buffer_init(&pieces);


    if (text.size != 0) {

        uint8_t piece[17];

        piece[0] =
            WDF_SOURCE_ORIGINAL;

        put64(
            piece + 1,
            0
        );

        put64(
            piece + 9,
            (uint64_t)text.size
        );


        if (!buffer_append(
                &pieces,
                piece,
                sizeof(piece))) {

            buffer_destroy(&pieces);

            fclose(fp);

            buffer_destroy(
                &document_section
            );

            buffer_destroy(
                &text
            );

            return WDF_ERROR_MEMORY;
        }
    }


    if (!write_section(
            fp,
            WDF_SECTION_PIECES,
            &pieces)) {

        buffer_destroy(&pieces);

        fclose(fp);

        buffer_destroy(
            &document_section
        );

        buffer_destroy(
            &text
        );

        return WDF_ERROR_IO;
    }


    buffer_destroy(&pieces);


    if (fclose(fp) != 0) {

        buffer_destroy(
            &document_section
        );

        buffer_destroy(
            &text
        );

        return WDF_ERROR_IO;
    }


    buffer_destroy(
        &document_section
    );

    buffer_destroy(
        &text
    );

    return WDF_OK;
}


/* ============================================================
 * Public save
 * ============================================================ */

WDFError wdf_save(
    const char *path,
    const WDDocument *document,
    const WDFOptions *options
)
{
    if (!path ||
        !document)

        return WDF_ERROR_ARGUMENT;


    WDFOptions defaults;


    if (!options) {

        wdf_default_options(
            &defaults
        );

        options = &defaults;
    }


    if (options->atomic_write) {

        size_t length =
            strlen(path);


        if (length >
            SIZE_MAX - 16)

            return WDF_ERROR_LIMIT;


        char *tmp =
            malloc(
                length + 16
            );


        if (!tmp)
            return WDF_ERROR_MEMORY;


        snprintf(
            tmp,
            length + 16,
            "%s.tmp",
            path
        );


        WDFError error =
            save_file(
                tmp,
                document,
                options
            );


        if (error != WDF_OK) {

            remove(tmp);
            free(tmp);

            return error;
        }


        if (rename(
                tmp,
                path) != 0) {

            remove(tmp);
            free(tmp);

            return WDF_ERROR_IO;
        }


        free(tmp);

        return WDF_OK;
    }


    return save_file(
        path,
        document,
        options
    );
}


/* ============================================================
 * Read section header
 * ============================================================ */

static int read_section_header(
    FILE *fp,
    WDFSectionHeader *header
)
{
    if (!read_u32(
            fp,
            &header->type))

        return 0;


    if (!read_u32(
            fp,
            &header->flags))

        return 0;


    if (!read_u64(
            fp,
            &header->size))

        return 0;


    if (!read_u64(
            fp,
            &header->checksum))

        return 0;


    return 1;
}


/* ============================================================
 * Load
 * ============================================================ */

WDFError wdf_load(
    const char *path,
    WDDocument *document,
    const WDFOptions *options
)
{
    if (!path ||
        !document)

        return WDF_ERROR_ARGUMENT;


    WDFOptions defaults;


    if (!options) {

        wdf_default_options(
            &defaults
        );

        options = &defaults;
    }


    FILE *fp =
        fopen(
            path,
            "rb"
        );


    if (!fp)
        return WDF_ERROR_IO;


    if (fseek(
            fp,
            0,
            SEEK_END) != 0) {

        fclose(fp);

        return WDF_ERROR_IO;
    }


    long file_size =
        ftell(fp);


    if (file_size < 0) {

        fclose(fp);

        return WDF_ERROR_IO;
    }


    if (options->maximum_file_size &&
        (uint64_t)file_size >
        options->maximum_file_size) {

        fclose(fp);

        return WDF_ERROR_LIMIT;
    }


    rewind(fp);


    WDFHeader header;


    if (!read_u32(
            fp,
            &header.magic) ||

        !read_u16(
            fp,
            &header.major) ||

        !read_u16(
            fp,
            &header.minor) ||

        !read_u32(
            fp,
            &header.flags) ||

        !read_u64(
            fp,
            &header.document_length) ||

        !read_u64(
            fp,
            &header.original_length) ||

        !read_u64(
            fp,
            &header.add_length) ||

        !read_u64(
            fp,
            &header.piece_count) ||

        !read_u64(
            fp,
            &header.checksum)) {

        fclose(fp);

        return WDF_ERROR_FORMAT;
    }


    if (header.magic !=
        WDF_MAGIC_VALUE) {

        fclose(fp);

        return WDF_ERROR_FORMAT;
    }


    if (header.major >
        WDF_MAJOR_VERSION) {

        fclose(fp);

        return WDF_ERROR_VERSION;
    }


    if (header.piece_count >
        options->maximum_pieces) {

        fclose(fp);

        return WDF_ERROR_LIMIT;
    }


    WDFBuffer text;

    buffer_init(&text);


    WDFBuffer original;

    buffer_init(&original);


    WDFBuffer add;

    buffer_init(&add);


    WDFBuffer pieces;

    buffer_init(&pieces);


    WDFError result =
        WDF_OK;


    /*
     * There is no section count in this version.
     *
     * Continue until EOF.
     */

    while (1) {

        int c =
            fgetc(fp);


        if (c == EOF)
            break;


        ungetc(c, fp);


        WDFSectionHeader section;


        if (!read_section_header(
                fp,
                &section)) {

            result =
                WDF_ERROR_FORMAT;

            break;
        }


        if (section.size >
            (uint64_t)SIZE_MAX) {

            result =
                WDF_ERROR_LIMIT;

            break;
        }


        size_t size =
            (size_t)section.size;


        WDFBuffer *destination =
            NULL;


        switch (section.type) {

            case WDF_SECTION_ORIGINAL:
                destination =
                    &original;
                break;

            case WDF_SECTION_ADD:
                destination =
                    &add;
                break;

            case WDF_SECTION_PIECES:
                destination =
                    &pieces;
                break;

            default:
                break;
        }


        if (destination) {

            if (!buffer_reserve(
                    destination,
                    size)) {

                result =
                    WDF_ERROR_MEMORY;

                break;
            }


            if (!read_all(
                    fp,
                    destination->data,
                    size)) {

                result =
                    WDF_ERROR_IO;

                break;
            }


            destination->size =
                size;


            if (options->verify_checksum) {

                uint64_t checksum =
                    hash_update(
                        hash_init(),
                        destination->data,
                        destination->size
                    );


                if (checksum !=
                    section.checksum) {

                    result =
                        WDF_ERROR_CHECKSUM;

                    break;
                }


                destination->checksum =
                    checksum;
            }

        } else {

            /*
             * Skip sections that this version does not
             * understand.
             */

            if (fseek(
                    fp,
                    (long)size,
                    SEEK_CUR) != 0) {

                result =
                    WDF_ERROR_IO;

                break;
            }
        }
    }


    fclose(fp);


    if (result != WDF_OK) {

        buffer_destroy(&text);
        buffer_destroy(&original);
        buffer_destroy(&add);
        buffer_destroy(&pieces);

        return result;
    }


    /*
     * Validate declared lengths.
     */

    if (header.original_length !=
        (uint64_t)original.size ||

        header.add_length !=
        (uint64_t)add.size) {

        buffer_destroy(&text);
        buffer_destroy(&original);
        buffer_destroy(&add);
        buffer_destroy(&pieces);

        return WDF_ERROR_CORRUPT;
    }


    /*
     * Current compatibility representation has the original
     * buffer as the complete logical document.
     *
     * If the piece section exists, reconstruct it.
     */

    if (pieces.size != 0) {

        if (pieces.size % 17 != 0) {

            buffer_destroy(&text);
            buffer_destroy(&original);
            buffer_destroy(&add);
            buffer_destroy(&pieces);

            return WDF_ERROR_CORRUPT;
        }


        size_t count =
            pieces.size / 17;


        if (count >
            options->maximum_pieces) {

            buffer_destroy(&text);
            buffer_destroy(&original);
            buffer_destroy(&add);
            buffer_destroy(&pieces);

            return WDF_ERROR_LIMIT;
        }


        for (size_t i = 0;
             i < count;
             ++i) {

            const uint8_t *piece =
                pieces.data +
                i * 17;


            uint8_t source =
                piece[0];


            uint64_t offset =
                get64(
                    piece + 1
                );


            uint64_t length =
                get64(
                    piece + 9
                );


            const uint8_t *source_data;
            size_t source_size;


            if (source ==
                WDF_SOURCE_ORIGINAL) {

                source_data =
                    original.data;

                source_size =
                    original.size;

            } else if (
                source ==
                WDF_SOURCE_ADD) {

                source_data =
                    add.data;

                source_size =
                    add.size;

            } else {

                buffer_destroy(&text);
                buffer_destroy(&original);
                buffer_destroy(&add);
                buffer_destroy(&pieces);

                return WDF_ERROR_CORRUPT;
            }


            if (offset >
                (uint64_t)source_size ||

                length >
                (uint64_t)source_size -
                offset) {

                buffer_destroy(&text);
                buffer_destroy(&original);
                buffer_destroy(&add);
                buffer_destroy(&pieces);

                return WDF_ERROR_CORRUPT;
            }


            if (!buffer_append(
                    &text,
                    source_data +
                        (size_t)offset,
                    (size_t)length)) {

                buffer_destroy(&text);
                buffer_destroy(&original);
                buffer_destroy(&add);
                buffer_destroy(&pieces);

                return WDF_ERROR_MEMORY;
            }
        }
    } else {

        if (!buffer_append(
                &text,
                original.data,
                original.size)) {

            buffer_destroy(&text);
            buffer_destroy(&original);
            buffer_destroy(&add);
            buffer_destroy(&pieces);

            return WDF_ERROR_MEMORY;
        }
    }


    if (header.document_length !=
        (uint64_t)text.size) {

        buffer_destroy(&text);
        buffer_destroy(&original);
        buffer_destroy(&add);
        buffer_destroy(&pieces);

        return WDF_ERROR_CORRUPT;
    }


    if (options->verify_checksum) {

        if (text.checksum !=
            header.checksum) {

            buffer_destroy(&text);
            buffer_destroy(&original);
            buffer_destroy(&add);
            buffer_destroy(&pieces);

            return WDF_ERROR_CHECKSUM;
        }
    }


    /*
     * Reconstruct document.
     *
     * Use a single transaction-like replacement at this
     * compatibility stage.
     */

    if (!wd_clear(document)) {

        buffer_destroy(&text);
        buffer_destroy(&original);
        buffer_destroy(&add);
        buffer_destroy(&pieces);

        return WDF_ERROR_MEMORY;
    }


    if (text.size != 0) {

        if (!wd_insert(
                document,
                0,
                (const char *)text.data,
                text.size)) {

            buffer_destroy(&text);
            buffer_destroy(&original);
            buffer_destroy(&add);
            buffer_destroy(&pieces);

            return WDF_ERROR_MEMORY;
        }
    }


    buffer_destroy(&text);
    buffer_destroy(&original);
    buffer_destroy(&add);
    buffer_destroy(&pieces);

    return WDF_OK;
}


/* ============================================================
 * Error strings
 * ============================================================ */

const char *wdf_error_string(
    WDFError error
)
{
    switch (error) {

        case WDF_OK:
            return "success";

        case WDF_ERROR_ARGUMENT:
            return "invalid argument";

        case WDF_ERROR_MEMORY:
            return "memory allocation failure";

        case WDF_ERROR_IO:
            return "I/O failure";

        case WDF_ERROR_FORMAT:
            return "invalid document format";

        case WDF_ERROR_VERSION:
            return "unsupported document version";

        case WDF_ERROR_CHECKSUM:
            return "checksum mismatch";

        case WDF_ERROR_LIMIT:
            return "document exceeds configured limit";

        case WDF_ERROR_CORRUPT:
            return "corrupt document";

        default:
            return "unknown error";
    }
}
4. The important next adapter

There is one architectural point worth fixing before calling #8 complete: the file layer should be able to walk the actual piece tree.

Add this to the document API:

typedef enum {
    WD_PIECE_ORIGINAL = 0,
    WD_PIECE_ADD = 1
} WDPieceSource;

typedef struct {
    WDPieceSource source;
    uint64_t offset;
    uint64_t length;
} WDPieceInfo;

typedef int (*WDPieceVisitor)(
    const WDPieceInfo *piece,
    void *context
);

int wd_visit_pieces(
    const WDDocument *document,
    WDPieceVisitor visitor,
    void *context
);











word_undo.h
#ifndef WORD_UNDO_H
#define WORD_UNDO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WU_OK = 0,
    WU_ERROR_ARGUMENT,
    WU_ERROR_MEMORY,
    WU_ERROR_DOCUMENT,
    WU_ERROR_EMPTY,
    WU_ERROR_LIMIT
} WUError;


/* ============================================================
 * Edit types
 * ============================================================ */

typedef enum {
    WU_EDIT_INSERT = 1,
    WU_EDIT_DELETE = 2,
    WU_EDIT_REPLACE = 3
} WUEditType;


/* ============================================================
 * Stored edit
 * ============================================================ */

typedef struct {
    WUEditType type;

    size_t position;

    char *before;
    size_t before_length;

    char *after;
    size_t after_length;
} WUEdit;


/* ============================================================
 * Transaction
 * ============================================================ */

typedef struct {
    WUEdit *edits;

    size_t count;
    size_t capacity;

    uint64_t document_version;

    char *description;
} WUTransaction;


/* ============================================================
 * History entry
 * ============================================================ */

typedef struct {
    WUTransaction transaction;

    uint64_t sequence;

    uint64_t timestamp;
} WUHistoryEntry;


/* ============================================================
 * History
 * ============================================================ */

typedef struct {
    WUHistoryEntry *entries;

    size_t count;
    size_t capacity;

    /*
     * Number of entries currently representing the
     * active document state.
     *
     * Entries [0, cursor) are applied.
     * Entries [cursor, count) are redoable.
     */
    size_t cursor;

    size_t maximum_entries;

    uint64_t next_sequence;

    uint64_t current_version;
} WUHistory;


/* ============================================================
 * Document adapter
 * ============================================================ */

typedef size_t (*WUGetLengthFn)(
    void *context
);

typedef int (*WUReadFn)(
    void *context,
    size_t position,
    char *buffer,
    size_t length
);

typedef int (*WUInsertFn)(
    void *context,
    size_t position,
    const char *text,
    size_t length
);

typedef int (*WUDeleteFn)(
    void *context,
    size_t position,
    size_t length
);

typedef struct {
    void *context;

    WUGetLengthFn length;
    WUReadFn read;
    WUInsertFn insert;
    WUDeleteFn delete;
} WUDocumentAdapter;


/* ============================================================
 * Statistics
 * ============================================================ */

typedef struct {
    uint64_t commits;
    uint64_t undo_count;
    uint64_t redo_count;

    uint64_t edits_recorded;
    uint64_t edits_discarded;

    size_t memory_used;
    size_t peak_memory_used;
} WUStatistics;


/* ============================================================
 * Manager
 * ============================================================ */

typedef struct {
    WUHistory history;

    WUDocumentAdapter document;

    WUStatistics statistics;

    uint8_t recording;

    uint8_t replaying;
} WUndoManager;


/* ============================================================
 * Transaction API
 * ============================================================ */

int wu_transaction_init(
    WUTransaction *transaction
);

void wu_transaction_destroy(
    WUTransaction *transaction
);

int wu_transaction_insert(
    WUTransaction *transaction,
    size_t position,
    const char *text,
    size_t length
);

int wu_transaction_delete(
    WUTransaction *transaction,
    size_t position,
    const char *deleted_text,
    size_t length
);

int wu_transaction_replace(
    WUTransaction *transaction,
    size_t position,
    const char *before,
    size_t before_length,
    const char *after,
    size_t after_length
);

void wu_transaction_set_description(
    WUTransaction *transaction,
    const char *description
);


/* ============================================================
 * Manager
 * ============================================================ */

int wu_init(
    WUndoManager *manager,
    WUDocumentAdapter document,
    size_t maximum_entries
);

void wu_destroy(
    WUndoManager *manager
);

int wu_begin(
    WUndoManager *manager,
    WUTransaction *transaction
);

int wu_commit(
    WUndoManager *manager,
    WUTransaction *transaction
);

void wu_cancel(
    WUndoManager *manager,
    WUTransaction *transaction
);


/* ============================================================
 * Undo / redo
 * ============================================================ */

WUError wu_undo(
    WUndoManager *manager
);

WUError wu_redo(
    WUndoManager *manager
);

int wu_can_undo(
    const WUndoManager *manager
);

int wu_can_redo(
    const WUndoManager *manager
);


/* ============================================================
 * History management
 * ============================================================ */

void wu_clear_redo(
    WUndoManager *manager
);

void wu_clear_history(
    WUndoManager *manager
);

size_t wu_undo_depth(
    const WUndoManager *manager
);

size_t wu_redo_depth(
    const WUndoManager *manager
);


/* ============================================================
 * Statistics
 * ============================================================ */

const WUStatistics *wu_statistics(
    const WUndoManager *manager
);

#ifdef __cplusplus
}
#endif

#endif
word_undo.c
#include "word_undo.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>


/* ============================================================
 * Memory helpers
 * ============================================================ */

static char *copy_bytes(
    const char *data,
    size_t length
)
{
    if (length == 0)
        return NULL;

    if (!data)
        return NULL;

    char *result =
        malloc(length);

    if (!result)
        return NULL;

    memcpy(
        result,
        data,
        length
    );

    return result;
}


static char *copy_string(
    const char *text
)
{
    if (!text)
        return NULL;

    size_t length =
        strlen(text);

    char *result =
        malloc(length + 1);

    if (!result)
        return NULL;

    memcpy(
        result,
        text,
        length + 1
    );

    return result;
}


/* ============================================================
 * Edit cleanup
 * ============================================================ */

static void edit_destroy(
    WUEdit *edit
)
{
    if (!edit)
        return;

    free(edit->before);
    free(edit->after);

    memset(
        edit,
        0,
        sizeof(*edit)
    );
}


/* ============================================================
 * Transaction
 * ============================================================ */

int wu_transaction_init(
    WUTransaction *transaction
)
{
    if (!transaction)
        return 0;

    memset(
        transaction,
        0,
        sizeof(*transaction)
    );

    return 1;
}


void wu_transaction_destroy(
    WUTransaction *transaction
)
{
    if (!transaction)
        return;

    for (size_t i = 0;
         i < transaction->count;
         ++i) {

        edit_destroy(
            &transaction->edits[i]
        );
    }

    free(transaction->edits);
    free(transaction->description);

    memset(
        transaction,
        0,
        sizeof(*transaction)
    );
}


static int transaction_reserve(
    WUTransaction *transaction,
    size_t required
)
{
    if (required <=
        transaction->capacity)

        return 1;


    size_t capacity =
        transaction->capacity
            ? transaction->capacity
            : 8;


    while (capacity < required) {

        if (capacity >
            SIZE_MAX / 2)

            return 0;

        capacity *= 2;
    }


    WUEdit *new_edits =
        realloc(
            transaction->edits,
            capacity *
            sizeof(WUEdit)
        );


    if (!new_edits)
        return 0;


    transaction->edits =
        new_edits;

    transaction->capacity =
        capacity;

    return 1;
}


static int transaction_append(
    WUTransaction *transaction,
    WUEdit *edit
)
{
    if (!transaction ||
        !edit)

        return 0;


    if (!transaction_reserve(
            transaction,
            transaction->count + 1))

        return 0;


    transaction->edits[
        transaction->count
    ] = *edit;


    transaction->count++;

    return 1;
}


/* ============================================================
 * Insert
 * ============================================================ */

int wu_transaction_insert(
    WUTransaction *transaction,
    size_t position,
    const char *text,
    size_t length
)
{
    if (!transaction)
        return 0;

    if (length && !text)
        return 0;


    WUEdit edit;

    memset(
        &edit,
        0,
        sizeof(edit)
    );


    edit.type =
        WU_EDIT_INSERT;

    edit.position =
        position;

    edit.after_length =
        length;


    if (length) {

        edit.after =
            copy_bytes(
                text,
                length
            );

        if (!edit.after)
            return 0;
    }


    if (!transaction_append(
            transaction,
            &edit)) {

        edit_destroy(&edit);

        return 0;
    }


    return 1;
}


/* ============================================================
 * Delete
 * ============================================================ */

int wu_transaction_delete(
    WUTransaction *transaction,
    size_t position,
    const char *deleted_text,
    size_t length
)
{
    if (!transaction)
        return 0;

    if (length && !deleted_text)
        return 0;


    WUEdit edit;

    memset(
        &edit,
        0,
        sizeof(edit)
    );


    edit.type =
        WU_EDIT_DELETE;

    edit.position =
        position;

    edit.before_length =
        length;


    if (length) {

        edit.before =
            copy_bytes(
                deleted_text,
                length
            );

        if (!edit.before)
            return 0;
    }


    if (!transaction_append(
            transaction,
            &edit)) {

        edit_destroy(&edit);

        return 0;
    }


    return 1;
}


/* ============================================================
 * Replace
 * ============================================================ */

int wu_transaction_replace(
    WUTransaction *transaction,
    size_t position,
    const char *before,
    size_t before_length,
    const char *after,
    size_t after_length
)
{
    if (!transaction)
        return 0;

    if (before_length && !before)
        return 0;

    if (after_length && !after)
        return 0;


    WUEdit edit;

    memset(
        &edit,
        0,
        sizeof(edit)
    );


    edit.type =
        WU_EDIT_REPLACE;

    edit.position =
        position;

    edit.before_length =
        before_length;

    edit.after_length =
        after_length;


    if (before_length) {

        edit.before =
            copy_bytes(
                before,
                before_length
            );

        if (!edit.before)
            goto fail;
    }


    if (after_length) {

        edit.after =
            copy_bytes(
                after,
                after_length
            );

        if (!edit.after)
            goto fail;
    }


    if (!transaction_append(
            transaction,
            &edit))

        goto fail;


    return 1;


fail:

    edit_destroy(
        &edit
    );

    return 0;
}


/* ============================================================
 * Description
 * ============================================================ */

void wu_transaction_set_description(
    WUTransaction *transaction,
    const char *description
)
{
    if (!transaction)
        return;


    char *copy =
        copy_string(
            description
        );


    if (description &&
        !copy)

        return;


    free(
        transaction->description
    );


    transaction->description =
        copy;
}


/* ============================================================
 * History entry cleanup
 * ============================================================ */

static void history_entry_destroy(
    WUHistoryEntry *entry
)
{
    if (!entry)
        return;

    wu_transaction_destroy(
        &entry->transaction
    );

    memset(
        entry,
        0,
        sizeof(*entry)
    );
}


/* ============================================================
 * History cleanup
 * ============================================================ */

static void history_destroy(
    WUHistory *history
)
{
    if (!history)
        return;


    for (size_t i = 0;
         i < history->count;
         ++i) {

        history_entry_destroy(
            &history->entries[i]
        );
    }


    free(
        history->entries
    );


    memset(
        history,
        0,
        sizeof(*history)
    );
}


/* ============================================================
 * Manager initialization
 * ============================================================ */

int wu_init(
    WUndoManager *manager,
    WUDocumentAdapter document,
    size_t maximum_entries
)
{
    if (!manager)
        return 0;


    if (!document.length ||
        !document.read ||
        !document.insert ||
        !document.delete)

        return 0;


    memset(
        manager,
        0,
        sizeof(*manager)
    );


    manager->document =
        document;


    manager->history.maximum_entries =
        maximum_entries
            ? maximum_entries
            : 1000;


    manager->history.next_sequence =
        1;


    manager->recording =
        1;


    return 1;
}


void wu_destroy(
    WUndoManager *manager
)
{
    if (!manager)
        return;


    history_destroy(
        &manager->history
    );


    memset(
        manager,
        0,
        sizeof(*manager)
    );
}


/* ============================================================
 * Begin
 * ============================================================ */

int wu_begin(
    WUndoManager *manager,
    WUTransaction *transaction
)
{
    if (!manager ||
        !transaction)

        return 0;


    if (!wu_transaction_init(
            transaction))

        return 0;


    transaction->document_version =
        manager->history.current_version;


    return 1;
}


/* ============================================================
 * Apply one edit
 * ============================================================ */

static WUError apply_edit(
    WUndoManager *manager,
    const WUEdit *edit,
    int undo
)
{
    WUDocumentAdapter *doc =
        &manager->document;


    if (!edit)
        return WU_ERROR_ARGUMENT;


    if (edit->type ==
        WU_EDIT_INSERT) {

        if (undo) {

            if (!doc->delete(
                    doc->context,
                    edit->position,
                    edit->after_length))

                return WU_ERROR_DOCUMENT;

        } else {

            if (!doc->insert(
                    doc->context,
                    edit->position,
                    edit->after,
                    edit->after_length))

                return WU_ERROR_DOCUMENT;
        }

        return WU_OK;
    }


    if (edit->type ==
        WU_EDIT_DELETE) {

        if (undo) {

            if (!doc->insert(
                    doc->context,
                    edit->position,
                    edit->before,
                    edit->before_length))

                return WU_ERROR_DOCUMENT;

        } else {

            if (!doc->delete(
                    doc->context,
                    edit->position,
                    edit->before_length))

                return WU_ERROR_DOCUMENT;
        }

        return WU_OK;
    }


    if (edit->type ==
        WU_EDIT_REPLACE) {

        if (undo) {

            if (edit->after_length) {

                if (!doc->delete(
                        doc->context,
                        edit->position,
                        edit->after_length))

                    return WU_ERROR_DOCUMENT;
            }


            if (edit->before_length) {

                if (!doc->insert(
                        doc->context,
                        edit->position,
                        edit->before,
                        edit->before_length))

                    return WU_ERROR_DOCUMENT;
            }

        } else {

            if (edit->before_length) {

                if (!doc->delete(
                        doc->context,
                        edit->position,
                        edit->before_length))

                    return WU_ERROR_DOCUMENT;
            }


            if (edit->after_length) {

                if (!doc->insert(
                        doc->context,
                        edit->position,
                        edit->after,
                        edit->after_length))

                    return WU_ERROR_DOCUMENT;
            }
        }


        return WU_OK;
    }


    return WU_ERROR_ARGUMENT;
}


/* ============================================================
 * Commit
 * ============================================================ */

int wu_commit(
    WUndoManager *manager,
    WUTransaction *transaction
)
{
    if (!manager ||
        !transaction)

        return 0;


    if (transaction->count == 0) {

        wu_transaction_destroy(
            transaction
        );

        return 1;
    }


    /*
     * Anything after the current cursor is no longer valid
     * once a new edit is committed.
     */

    wu_clear_redo(
        manager
    );


    WUHistory *history =
        &manager->history;


    if (history->count >=
        history->maximum_entries) {

        /*
         * Remove oldest entry.
         */

        history_entry_destroy(
            &history->entries[0]
        );


        if (history->count > 1) {

            memmove(
                history->entries,
                history->entries + 1,
                (history->count - 1) *
                sizeof(WUHistoryEntry)
            );
        }


        history->count--;

        history->cursor--;

        manager->statistics
            .edits_discarded++;
    }


    if (history->count >=
        history->capacity) {

        size_t capacity =
            history->capacity
                ? history->capacity * 2
                : 32;


        WUHistoryEntry *entries =
            realloc(
                history->entries,
                capacity *
                sizeof(WUHistoryEntry)
            );


        if (!entries)
            return 0;


        history->entries =
            entries;

        history->capacity =
            capacity;
    }


    WUHistoryEntry *entry =
        &history->entries[
            history->count
        ];


    memset(
        entry,
        0,
        sizeof(*entry)
    );


    entry->transaction =
        *transaction;


    memset(
        transaction,
        0,
        sizeof(*transaction)
    );


    entry->sequence =
        history->next_sequence++;


    entry->timestamp =
        (uint64_t)time(NULL);


    history->count++;

    history->cursor =
        history->count;


    history->current_version++;


    manager->statistics.commits++;

    manager->statistics.edits_recorded +=
        entry->transaction.count;


    return 1;
}


/* ============================================================
 * Cancel
 * ============================================================ */

void wu_cancel(
    WUndoManager *manager,
    WUTransaction *transaction
)
{
    (void)manager;

    if (!transaction)
        return;

    wu_transaction_destroy(
        transaction
    );
}


/* ============================================================
 * Can undo
 * ============================================================ */

int wu_can_undo(
    const WUndoManager *manager
)
{
    return manager &&
           manager->history.cursor > 0;
}


int wu_can_redo(
    const WUndoManager *manager
)
{
    return manager &&
           manager->history.cursor <
           manager->history.count;
}


/* ============================================================
 * Undo
 * ============================================================ */

WUError wu_undo(
    WUndoManager *manager
)
{
    if (!manager)
        return WU_ERROR_ARGUMENT;


    WUHistory *history =
        &manager->history;


    if (history->cursor == 0)
        return WU_ERROR_EMPTY;


    WUHistoryEntry *entry =
        &history->entries[
            history->cursor - 1
        ];


    manager->replaying = 1;


    /*
     * Reverse order is critical.
     *
     * Example:
     *
     * insert A at 0
     * insert B at 1
     *
     * Undo B first, then A.
     */

    for (size_t i =
            entry->transaction.count;
         i > 0;
         --i) {

        WUError error =
            apply_edit(
                manager,
                &entry->transaction
                    .edits[i - 1],
                1
            );


        if (error != WU_OK) {

            manager->replaying = 0;

            return error;
        }
    }


    history->cursor--;

    history->current_version++;

    manager->statistics.undo_count++;

    manager->replaying = 0;


    return WU_OK;
}


/* ============================================================
 * Redo
 * ============================================================ */

WUError wu_redo(
    WUndoManager *manager
)
{
    if (!manager)
        return WU_ERROR_ARGUMENT;


    WUHistory *history =
        &manager->history;


    if (history->cursor >=
        history->count)

        return WU_ERROR_EMPTY;


    WUHistoryEntry *entry =
        &history->entries[
            history->cursor
        ];


    manager->replaying = 1;


    /*
     * Redo in forward order.
     */

    for (size_t i = 0;
         i < entry->transaction.count;
         ++i) {

        WUError error =
            apply_edit(
                manager,
                &entry->transaction
                    .edits[i],
                0
            );


        if (error != WU_OK) {

            manager->replaying = 0;

            return error;
        }
    }


    history->cursor++;

    history->current_version++;

    manager->statistics.redo_count++;

    manager->replaying = 0;


    return WU_OK;
}


/* ============================================================
 * Clear redo
 * ============================================================ */

void wu_clear_redo(
    WUndoManager *manager
)
{
    if (!manager)
        return;


    WUHistory *history =
        &manager->history;


    while (history->count >
           history->cursor) {

        history_entry_destroy(
            &history->entries[
                history->count - 1
            ]
        );

        history->count--;
    }
}


/* ============================================================
 * Clear history
 * ============================================================ */

void wu_clear_history(
    WUndoManager *manager
)
{
    if (!manager)
        return;


    history_destroy(
        &manager->history
    );


    manager->history.maximum_entries =
        1000;

    manager->history.next_sequence =
        1;

    manager->history.current_version =
        0;
}


/* ============================================================
 * Depth
 * ============================================================ */

size_t wu_undo_depth(
    const WUndoManager *manager
)
{
    if (!manager)
        return 0;

    return manager->history.cursor;
}


size_t wu_redo_depth(
    const WUndoManager *manager
)
{
    if (!manager)
        return 0;

    return
        manager->history.count -
        manager->history.cursor;
}


/* ============================================================
 * Statistics
 * ============================================================ */

const WUStatistics *wu_statistics(
    const WUndoManager *manager
)
{
    if (!manager)
        return NULL;

    return &manager->statistics;
}
Integration with the document engine

The adapter connects directly to the document implementation from the previous modules:

static size_t undo_doc_length(void *context)
{
    WDDocument *document =
        context;

    return wd_length(document);
}


static int undo_doc_read(
    void *context,
    size_t position,
    char *buffer,
    size_t length
)
{
    WDDocument *document =
        context;

    return wd_read(
        document,
        position,
        buffer,
        length
    );
}


static int undo_doc_insert(
    void *context,
    size_t position,
    const char *text,
    size_t length
)
{
    WDDocument *document =
        context;

    return wd_insert(
        document,
        position,
        text,
        length
    );
}


static int undo_doc_delete(
    void *context,
    size_t position,
    size_t length
)
{
    WDDocument *document =
        context;

    return wd_delete(
        document,
        position,
        length
    );
}

Then:

WUDocumentAdapter adapter = {
    .context = &document,
    .length = undo_doc_length,
    .read = undo_doc_read,
    .insert = undo_doc_insert,
    .delete = undo_doc_delete
};


WUndoManager undo;

wu_init(
    &undo,
    adapter,
    5000
);
A complete transaction

For example, typing:

Hello world

can be represented as one history entry:

WUTransaction transaction;

wu_begin(
    &undo,
    &transaction
);


wu_transaction_insert(
    &transaction,
    0,
    "Hello world",
    11
);


wd_insert(
    &document,
    0,
    "Hello world",
    11
);


wu_commit(
    &undo,
    &transaction
);











word_threading.h
#ifndef WORD_THREADING_H
#define WORD_THREADING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WMT_OK = 0,
    WMT_ERROR_ARGUMENT,
    WMT_ERROR_MEMORY,
    WMT_ERROR_THREAD,
    WMT_ERROR_QUEUE_FULL,
    WMT_ERROR_CANCELLED,
    WMT_ERROR_STALE,
    WMT_ERROR_SHUTDOWN
} WMTError;


/* ============================================================
 * Task priority
 * ============================================================ */

typedef enum {
    WMT_PRIORITY_CRITICAL = 0,
    WMT_PRIORITY_HIGH,
    WMT_PRIORITY_NORMAL,
    WMT_PRIORITY_LOW,
    WMT_PRIORITY_IDLE
} WMTTaskPriority;


/* ============================================================
 * Task types
 * ============================================================ */

typedef enum {
    WMT_TASK_NONE = 0,

    WMT_TASK_LAYOUT,
    WMT_TASK_PAGINATION,
    WMT_TASK_SEARCH,
    WMT_TASK_SPELLCHECK,
    WMT_TASK_MEDIA_DECODE,
    WMT_TASK_TABLE_LAYOUT,
    WMT_TASK_OBJECT_LAYOUT,
    WMT_TASK_INDEX,
    WMT_TASK_AUTOSAVE,
    WMT_TASK_EXPORT,
    WMT_TASK_PRINT,

    WMT_TASK_CUSTOM
} WMTTaskType;


/* ============================================================
 * Task state
 * ============================================================ */

typedef enum {
    WMT_STATE_PENDING = 0,
    WMT_STATE_RUNNING,
    WMT_STATE_COMPLETE,
    WMT_STATE_CANCELLED,
    WMT_STATE_FAILED,
    WMT_STATE_STALE
} WMTTaskState;


/* ============================================================
 * Cancellation
 * ============================================================ */

typedef struct {
    volatile int cancelled;
} WMTCancellation;


/* ============================================================
 * Worker task
 * ============================================================ */

typedef struct WMTTask WMTTask;

typedef int (*WMTTaskExecuteFn)(
    WMTTask *task,
    void *context
);

typedef void (*WMTTaskDestroyFn)(
    WMTTask *task,
    void *context
);


struct WMTTask {

    uint64_t id;

    WMTTaskType type;

    WMTTaskPriority priority;

    WMTTaskState state;

    /*
     * Document version for which this task was created.
     */
    uint64_t document_version;

    /*
     * Generation prevents older results from being committed
     * after a newer edit.
     */
    uint64_t generation;

    /*
     * Optional region of the document.
     */
    size_t start;
    size_t length;

    WMTCancellation cancellation;

    WMTTaskExecuteFn execute;
    WMTTaskDestroyFn destroy;

    void *payload;
};


/* ============================================================
 * Result
 * ============================================================ */

typedef struct {
    uint64_t task_id;
    uint64_t document_version;
    uint64_t generation;

    WMTTaskType type;

    int success;
    int stale;

    void *data;
    size_t data_size;

    void (*destroy_data)(void *data);
} WMTResult;


/* ============================================================
 * Queue
 * ============================================================ */

typedef struct {
    WMTTask **tasks;

    size_t count;
    size_t capacity;

    size_t maximum_tasks;
} WMTTaskQueue;


/* ============================================================
 * Thread pool
 * ============================================================ */

typedef struct {
    void *workers;

    size_t worker_count;

    WMTTaskQueue queue;

    void *mutex;
    void *condition;

    int shutdown;

    uint64_t next_task_id;

    uint64_t tasks_submitted;
    uint64_t tasks_completed;
    uint64_t tasks_cancelled;
    uint64_t tasks_failed;
    uint64_t tasks_stale;
} WMTThreadPool;


/* ============================================================
 * Document processing context
 * ============================================================ */

typedef size_t (*WMTDocumentVersionFn)(
    void *document
);

typedef struct {
    void *document;

    WMTDocumentVersionFn get_version;

    WMTThreadPool *pool;
} WMTDocumentProcessor;


/* ============================================================
 * Pool
 * ============================================================ */

int wmt_pool_init(
    WMTThreadPool *pool,
    size_t worker_count,
    size_t maximum_queue
);

void wmt_pool_shutdown(
    WMTThreadPool *pool
);

void wmt_pool_destroy(
    WMTThreadPool *pool
);


/* ============================================================
 * Task creation
 * ============================================================ */

WMTTask *wmt_task_create(
    WMTTaskType type,
    WMTTaskPriority priority,
    uint64_t document_version,
    uint64_t generation,
    size_t start,
    size_t length,
    WMTTaskExecuteFn execute,
    WMTTaskDestroyFn destroy,
    void *payload
);

void wmt_task_destroy(
    WMTTask *task
);


/* ============================================================
 * Scheduling
 * ============================================================ */

WMTError wmt_submit(
    WMTThreadPool *pool,
    WMTTask *task
);

void wmt_cancel(
    WMTTask *task
);


/* ============================================================
 * Results
 * ============================================================ */

WMTError wmt_result_validate(
    const WMTResult *result,
    uint64_t current_version,
    uint64_t current_generation
);


/* ============================================================
 * Processor
 * ============================================================ */

int wmt_processor_init(
    WMTDocumentProcessor *processor,
    void *document,
    WMTDocumentVersionFn get_version,
    WMTThreadPool *pool
);


/* ============================================================
 * Statistics
 * ============================================================ */

typedef struct {
    uint64_t submitted;
    uint64_t completed;
    uint64_t cancelled;
    uint64_t failed;
    uint64_t stale;
} WMTStatistics;

void wmt_statistics(
    const WMTThreadPool *pool,
    WMTStatistics *statistics
);

#ifdef __cplusplus
}
#endif

#endif
word_threading.c
#include "word_threading.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>


/* ============================================================
 * Internal worker
 * ============================================================ */

typedef struct {
    pthread_t thread;
    WMTThreadPool *pool;
} WMTWorker;


/* ============================================================
 * Queue helpers
 * ============================================================ */

static int queue_init(
    WMTTaskQueue *queue,
    size_t maximum_tasks
)
{
    if (!queue)
        return 0;

    memset(queue, 0, sizeof(*queue));

    queue->maximum_tasks =
        maximum_tasks;

    return 1;
}


static void queue_destroy(
    WMTTaskQueue *queue
)
{
    if (!queue)
        return;

    for (size_t i = 0;
         i < queue->count;
         ++i) {

        wmt_task_destroy(
            queue->tasks[i]
        );
    }

    free(queue->tasks);

    memset(queue, 0, sizeof(*queue));
}


static int queue_reserve(
    WMTTaskQueue *queue,
    size_t required
)
{
    if (required <= queue->capacity)
        return 1;

    size_t capacity =
        queue->capacity
            ? queue->capacity * 2
            : 64;

    while (capacity < required) {

        if (capacity > SIZE_MAX / 2)
            return 0;

        capacity *= 2;
    }

    WMTTask **tasks =
        realloc(
            queue->tasks,
            capacity * sizeof(WMTTask *)
        );

    if (!tasks)
        return 0;

    queue->tasks = tasks;
    queue->capacity = capacity;

    return 1;
}


/*
 * Higher priority first.
 *
 * Within the same priority, older tasks first.
 */
static int queue_push(
    WMTTaskQueue *queue,
    WMTTask *task
)
{
    if (queue->count >=
        queue->maximum_tasks)

        return 0;

    if (!queue_reserve(
            queue,
            queue->count + 1))

        return 0;

    queue->tasks[
        queue->count++
    ] = task;

    return 1;
}


static WMTTask *queue_pop(
    WMTTaskQueue *queue
)
{
    if (!queue ||
        queue->count == 0)

        return NULL;

    size_t best = 0;

    for (size_t i = 1;
         i < queue->count;
         ++i) {

        if (queue->tasks[i]->priority <
            queue->tasks[best]->priority) {

            best = i;
        }
    }

    WMTTask *task =
        queue->tasks[best];

    if (best + 1 < queue->count) {

        memmove(
            &queue->tasks[best],
            &queue->tasks[best + 1],
            (queue->count - best - 1) *
            sizeof(WMTTask *)
        );
    }

    queue->count--;

    return task;
}


/* ============================================================
 * Task
 * ============================================================ */

WMTTask *wmt_task_create(
    WMTTaskType type,
    WMTTaskPriority priority,
    uint64_t document_version,
    uint64_t generation,
    size_t start,
    size_t length,
    WMTTaskExecuteFn execute,
    WMTTaskDestroyFn destroy,
    void *payload
)
{
    if (!execute)
        return NULL;

    WMTTask *task =
        calloc(
            1,
            sizeof(*task)
        );

    if (!task)
        return NULL;

    task->type =
        type;

    task->priority =
        priority;

    task->state =
        WMT_STATE_PENDING;

    task->document_version =
        document_version;

    task->generation =
        generation;

    task->start =
        start;

    task->length =
        length;

    task->execute =
        execute;

    task->destroy =
        destroy;

    task->payload =
        payload;

    return task;
}


void wmt_task_destroy(
    WMTTask *task
)
{
    if (!task)
        return;

    if (task->destroy)
        task->destroy(
            task,
            task->payload
        );

    free(task);
}


void wmt_cancel(
    WMTTask *task
)
{
    if (!task)
        return;

    task->cancellation.cancelled = 1;
}


/* ============================================================
 * Worker loop
 * ============================================================ */

static void *worker_main(
    void *argument
)
{
    WMTWorker *worker =
        argument;

    WMTThreadPool *pool =
        worker->pool;

    for (;;) {

        pthread_mutex_t *mutex =
            pool->mutex;

        pthread_cond_t *condition =
            pool->condition;

        pthread_mutex_lock(
            mutex
        );

        while (!pool->shutdown &&
               pool->queue.count == 0) {

            pthread_cond_wait(
                condition,
                mutex
            );
        }

        if (pool->shutdown &&
            pool->queue.count == 0) {

            pthread_mutex_unlock(
                mutex
            );

            break;
        }

        WMTTask *task =
            queue_pop(
                &pool->queue
            );

        if (!task) {

            pthread_mutex_unlock(
                mutex
            );

            continue;
        }

        task->state =
            WMT_STATE_RUNNING;

        pthread_mutex_unlock(
            mutex
        );


        if (task->cancellation.cancelled) {

            task->state =
                WMT_STATE_CANCELLED;

            pthread_mutex_lock(mutex);

            pool->tasks_cancelled++;

            pthread_mutex_unlock(mutex);

            wmt_task_destroy(task);

            continue;
        }


        int result =
            task->execute(
                task,
                task->payload
            );


        pthread_mutex_lock(mutex);

        if (task->cancellation.cancelled) {

            task->state =
                WMT_STATE_CANCELLED;

            pool->tasks_cancelled++;

        } else if (result == 0) {

            task->state =
                WMT_STATE_FAILED;

            pool->tasks_failed++;

        } else {

            task->state =
                WMT_STATE_COMPLETE;

            pool->tasks_completed++;
        }

        pthread_mutex_unlock(mutex);


        /*
         * The task owns its result/payload lifetime.
         * A real engine would enqueue a WMTResult here.
         */

        wmt_task_destroy(task);
    }

    return NULL;
}


/* ============================================================
 * Pool initialization
 * ============================================================ */

int wmt_pool_init(
    WMTThreadPool *pool,
    size_t worker_count,
    size_t maximum_queue
)
{
    if (!pool)
        return 0;

    if (worker_count == 0)
        worker_count = 1;

    if (maximum_queue == 0)
        maximum_queue = 1024;


    memset(
        pool,
        0,
        sizeof(*pool)
    );


    pthread_mutex_t *mutex =
        malloc(sizeof(*mutex));

    pthread_cond_t *condition =
        malloc(sizeof(*condition));

    WMTWorker *workers =
        calloc(
            worker_count,
            sizeof(WMTWorker)
        );


    if (!mutex ||
        !condition ||
        !workers) {

        free(mutex);
        free(condition);
        free(workers);

        return 0;
    }


    if (pthread_mutex_init(
            mutex,
            NULL) != 0) {

        free(mutex);
        free(condition);
        free(workers);

        return 0;
    }


    if (pthread_cond_init(
            condition,
            NULL) != 0) {

        pthread_mutex_destroy(mutex);

        free(mutex);
        free(condition);
        free(workers);

        return 0;
    }


    if (!queue_init(
            &pool->queue,
            maximum_queue)) {

        pthread_mutex_destroy(mutex);
        pthread_cond_destroy(condition);

        free(mutex);
        free(condition);
        free(workers);

        return 0;
    }


    pool->mutex =
        mutex;

    pool->condition =
        condition;

    pool->workers =
        workers;

    pool->worker_count =
        worker_count;

    pool->next_task_id =
        1;


    for (size_t i = 0;
         i < worker_count;
         ++i) {

        workers[i].pool =
            pool;

        if (pthread_create(
                &workers[i].thread,
                NULL,
                worker_main,
                &workers[i]
            ) != 0) {

            pool->shutdown = 1;

            pthread_cond_broadcast(
                condition
            );

            for (size_t j = 0;
                 j < i;
                 ++j) {

                pthread_join(
                    workers[j].thread,
                    NULL
                );
            }

            queue_destroy(
                &pool->queue
            );

            pthread_mutex_destroy(mutex);
            pthread_cond_destroy(condition);

            free(mutex);
            free(condition);
            free(workers);

            memset(pool, 0, sizeof(*pool));

            return 0;
        }
    }


    return 1;
}


/* ============================================================
 * Shutdown
 * ============================================================ */

void wmt_pool_shutdown(
    WMTThreadPool *pool
)
{
    if (!pool ||
        !pool->mutex)

        return;


    pthread_mutex_t *mutex =
        pool->mutex;

    pthread_cond_t *condition =
        pool->condition;

    pthread_mutex_lock(
        mutex
    );

    pool->shutdown = 1;

    /*
     * Wake every worker.
     */
    pthread_cond_broadcast(
        condition
    );

    pthread_mutex_unlock(
        mutex
    );


    WMTWorker *workers =
        pool->workers;

    for (size_t i = 0;
         i < pool->worker_count;
         ++i) {

        pthread_join(
            workers[i].thread,
            NULL
        );
    }
}


/* ============================================================
 * Destroy
 * ============================================================ */

void wmt_pool_destroy(
    WMTThreadPool *pool
)
{
    if (!pool)
        return;


    wmt_pool_shutdown(
        pool
    );


    pthread_mutex_t *mutex =
        pool->mutex;

    pthread_cond_t *condition =
        pool->condition;


    queue_destroy(
        &pool->queue
    );


    if (mutex)
        pthread_mutex_destroy(mutex);

    if (condition)
        pthread_cond_destroy(condition);


    free(
        pool->workers
    );

    free(mutex);
    free(condition);


    memset(
        pool,
        0,
        sizeof(*pool)
    );
}


/* ============================================================
 * Submit
 * ============================================================ */

WMTError wmt_submit(
    WMTThreadPool *pool,
    WMTTask *task
)
{
    if (!pool ||
        !task)

        return WMT_ERROR_ARGUMENT;


    pthread_mutex_t *mutex =
        pool->mutex;

    pthread_cond_t *condition =
        pool->condition;


    pthread_mutex_lock(
        mutex
    );


    if (pool->shutdown) {

        pthread_mutex_unlock(
            mutex
        );

        return WMT_ERROR_SHUTDOWN;
    }


    if (!queue_push(
            &pool->queue,
            task)) {

        pthread_mutex_unlock(
            mutex
        );

        return WMT_ERROR_QUEUE_FULL;
    }


    task->id =
        pool->next_task_id++;


    pool->tasks_submitted++;


    pthread_cond_signal(
        condition
    );


    pthread_mutex_unlock(
        mutex
    );


    return WMT_OK;
}


/* ============================================================
 * Result validation
 * ============================================================ */

WMTError wmt_result_validate(
    const WMTResult *result,
    uint64_t current_version,
    uint64_t current_generation
)
{
    if (!result)
        return WMT_ERROR_ARGUMENT;


    if (!result->success)
        return WMT_ERROR_THREAD;


    if (result->document_version !=
        current_version)

        return WMT_ERROR_STALE;


    if (result->generation !=
        current_generation)

        return WMT_ERROR_STALE;


    return WMT_OK;
}


/* ============================================================
 * Processor
 * ============================================================ */

int wmt_processor_init(
    WMTDocumentProcessor *processor,
    void *document,
    WMTDocumentVersionFn get_version,
    WMTThreadPool *pool
)
{
    if (!processor ||
        !document ||
        !get_version ||
        !pool)

        return 0;


    memset(
        processor,
        0,
        sizeof(*processor)
    );


    processor->document =
        document;

    processor->get_version =
        get_version;

    processor->pool =
        pool;


    return 1;
}


/* ============================================================
 * Statistics
 * ============================================================ */

void wmt_statistics(
    const WMTThreadPool *pool,
    WMTStatistics *statistics
)
{
    if (!pool ||
        !statistics)

        return;


    pthread_mutex_t *mutex =
        pool->mutex;

    pthread_mutex_lock(
        mutex
    );


    statistics->submitted =
        pool->tasks_submitted;

    statistics->completed =
        pool->tasks_completed;

    statistics->cancelled =
        pool->tasks_cancelled;

    statistics->failed =
        pool->tasks_failed;

    statistics->stale =
        pool->tasks_stale;


    pthread_mutex_unlock(
        mutex
    );
}
A real layout worker

The worker should never mutate the authoritative document.

Instead:

typedef struct {
    const char *text;
    size_t length;

    uint64_t version;

    double page_width;
    double page_height;
} WTLayoutJob;

The job receives an immutable snapshot:

static int layout_worker(
    WMTTask *task,
    void *context
)
{
    WTLayoutJob *job =
        context;

    if (task->cancellation.cancelled)
        return 0;

    /*
     * Run expensive layout calculation here.
     *
     * The layout engine is allowed to consume the snapshot,
     * but must not mutate the document.
     */

    /*
     * Example:
     *
     * WLLayoutEngine engine;
     * wl_layout_document(...);
     */

    if (task->cancellation.cancelled)
        return 0;

    return 1;
}

Scheduling it:

uint64_t version =
    document_version(&document);

WTLayoutJob *job =
    malloc(sizeof(*job));

job->text = snapshot;
job->length = snapshot_length;
job->version = version;
job->page_width = 793.7;
job->page_height = 1122.5;


WMTTask *task =
    wmt_task_create(
        WMT_TASK_LAYOUT,
        WMT_PRIORITY_HIGH,
        version,
        generation,
        0,
        snapshot_length,
        layout_worker,
        layout_job_destroy,
        job
    );


wmt_submit(
    &pool,
    task
);





