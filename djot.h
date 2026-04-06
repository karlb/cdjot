/* djot.h - djot to HTML converter */

#ifndef DJOT_H
#define DJOT_H

#include <stdlib.h>

enum blocktype {
	BLK_PARA,
	BLK_HEADING,
	BLK_CODEBLOCK,
	BLK_QUOTE,
	BLK_THEMATIC_BREAK,
	BLK_LIST,
	BLK_LISTITEM,
	BLK_REFDEF,
	BLK_BLANK,
};

enum listmarker {
	LIST_BULLET_DASH,
	LIST_BULLET_STAR,
	LIST_BULLET_PLUS,
	LIST_ORD_DOT,
	LIST_ORD_PAREN,
};

struct buf {
	char *data;
	int len;
	int cap;
};

struct slice {
	char *s;
	int len;
};

struct block {
	enum blocktype type;
	int level;       /* heading level, or nesting depth */
	int start;       /* first child index (containers) or start number (ordered lists) */
	int end;         /* last child index + 1 (containers) */
	int loose;       /* lists: tight vs loose */
	enum listmarker marker;
	struct buf content;    /* text content (owned, must be freed) */
	struct slice info;     /* code fence info string */
};

struct refdef {
	struct slice label;
	struct slice url;
};

struct doc {
	char *input;
	int inputlen;
	struct block *blocks;
	int nblocks;
	int blockcap;
	struct refdef *refs;
	int nrefs;
	int refcap;
};

/* buf.c */
void buf_init(struct buf *b);
void buf_push(struct buf *b, char c);
void buf_append(struct buf *b, const char *s, int len);
void buf_appendstr(struct buf *b, const char *s);
void buf_free(struct buf *b);

/* util functions in buf.c */
void html_escape(struct buf *b, const char *s, int len);
void url_escape(struct buf *b, const char *s, int len);
int  is_ascii_punct(int c);
int  is_blank_line(const char *s, int len);
struct slice slice_trim(struct slice s);

/* block.c */
void block_parse(struct doc *doc);

/* inline.c */
void inline_render(struct buf *out, struct doc *doc, const char *s, int len);

/* djot.c */
void render_blocks(struct buf *out, struct doc *doc, int start, int end, int insection);

#endif
