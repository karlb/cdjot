/* block.c - block-level parser */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "djot.h"

#define STACK_MAX 64

struct blockctx {
	enum blocktype type;
	int blockidx;
	int indent;        /* content column for list items */
	enum listmarker marker;
	int had_blank;     /* seen a blank line in this container */
	int in_para;       /* currently accumulating a paragraph */
};

static struct blockctx stack[STACK_MAX];
static int stackdepth;

static void
doc_addblock(struct doc *doc, struct block b)
{
	if (doc->nblocks >= doc->blockcap) {
		doc->blockcap = doc->blockcap ? doc->blockcap * 2 : 64;
		doc->blocks = realloc(doc->blocks, doc->blockcap * sizeof(struct block));
		if (!doc->blocks) {
			fprintf(stderr, "out of memory\n");
			exit(1);
		}
	}
	doc->blocks[doc->nblocks++] = b;
}

static void
doc_addref(struct doc *doc, struct slice label, struct slice url)
{
	if (doc->nrefs >= doc->refcap) {
		doc->refcap = doc->refcap ? doc->refcap * 2 : 16;
		doc->refs = realloc(doc->refs, doc->refcap * sizeof(struct refdef));
		if (!doc->refs) {
			fprintf(stderr, "out of memory\n");
			exit(1);
		}
	}
	doc->refs[doc->nrefs].label = label;
	doc->refs[doc->nrefs].url = url;
	doc->nrefs++;
}

static int
nextline(const char *input, int inputlen, int pos, const char **linestart)
{
	int i;

	if (pos >= inputlen)
		return 0;
	*linestart = input + pos;
	for (i = pos; i < inputlen; i++) {
		if (input[i] == '\n')
			return i - pos + 1;
	}
	return inputlen - pos;
}

static int
leading_spaces(const char *s, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		if (s[i] != ' ')
			break;
	}
	return i;
}

static int
scan_heading(const char *s, int len, int *contentoff)
{
	int i, level;

	i = leading_spaces(s, len);
	if (i > 3)
		return 0;
	level = 0;
	while (i < len && s[i] == '#') {
		level++;
		i++;
	}
	if (level < 1 || level > 6)
		return 0;
	if (i < len && s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r')
		return 0;
	if (i < len && (s[i] == ' ' || s[i] == '\t'))
		i++;
	*contentoff = i;
	return level;
}

static int
scan_thematic_break(const char *s, int len)
{
	int i, count;

	i = leading_spaces(s, len);
	if (i > 3 || i >= len)
		return 0;
	if (s[i] != '*' && s[i] != '-')
		return 0;
	count = 0;
	for (; i < len; i++) {
		if (s[i] == '*' || s[i] == '-')
			count++;
		else if (s[i] == ' ' || s[i] == '\t')
			continue;
		else if (s[i] == '\n' || s[i] == '\r')
			break;
		else
			return 0;
	}
	return count >= 3;
}

static int
scan_code_fence(const char *s, int len, char *fencech, int *infooff, int *infolen)
{
	int i, flen;

	i = leading_spaces(s, len);
	if (i > 3 || i >= len)
		return 0;
	*fencech = s[i];
	if (*fencech != '`' && *fencech != '~')
		return 0;
	flen = 0;
	while (i < len && s[i] == *fencech) {
		flen++;
		i++;
	}
	if (flen < 3)
		return 0;
	while (i < len && (s[i] == ' ' || s[i] == '\t'))
		i++;
	*infooff = i;
	*infolen = 0;
	while (i + *infolen < len && s[i + *infolen] != '\n' && s[i + *infolen] != '\r')
		(*infolen)++;
	while (*infolen > 0 && (s[*infooff + *infolen - 1] == ' '
	    || s[*infooff + *infolen - 1] == '\t'))
		(*infolen)--;
	/* backtick fence: reject if info string contains backticks */
	if (*fencech == '`') {
		int k;
		for (k = 0; k < *infolen; k++) {
			if (s[*infooff + k] == '`')
				return 0;
		}
	}
	return flen;
}

static int
scan_blockquote(const char *s, int len)
{
	int i;

	i = leading_spaces(s, len);
	if (i > 3 || i >= len)
		return 0;
	if (s[i] != '>')
		return 0;
	i++;
	if (i < len && s[i] != ' ' && s[i] != '\n' && s[i] != '\r')
		return 0;
	if (i < len && s[i] == ' ')
		i++;
	return i;
}

static int
scan_bullet(const char *s, int len, enum listmarker *marker)
{
	int i;

	i = leading_spaces(s, len);
	if (i > 3 || i >= len)
		return 0;
	switch (s[i]) {
	case '-': *marker = LIST_BULLET_DASH; break;
	case '*': *marker = LIST_BULLET_STAR; break;
	case '+': *marker = LIST_BULLET_PLUS; break;
	default: return 0;
	}
	i++;
	if (i >= len || (s[i] != ' ' && s[i] != '\t' && s[i] != '\n'))
		return 0;
	if (i < len && s[i] == ' ')
		i++;
	return i;
}

static int
scan_ordered(const char *s, int len, enum listmarker *marker, int *startnum)
{
	int i, num;

	i = leading_spaces(s, len);
	if (i > 3 || i >= len)
		return 0;
	if (!isdigit((unsigned char)s[i]))
		return 0;
	num = 0;
	while (i < len && isdigit((unsigned char)s[i])) {
		num = num * 10 + (s[i] - '0');
		i++;
	}
	if (i >= len)
		return 0;
	if (s[i] == '.') {
		*marker = LIST_ORD_DOT;
	} else if (s[i] == ')') {
		*marker = LIST_ORD_PAREN;
	} else {
		return 0;
	}
	i++;
	if (i >= len || (s[i] != ' ' && s[i] != '\t' && s[i] != '\n'))
		return 0;
	if (i < len && s[i] == ' ')
		i++;
	*startnum = num;
	return i;
}

static int
scan_refdef(const char *s, int len, struct slice *label, struct slice *url)
{
	int i;

	i = leading_spaces(s, len);
	if (i > 3 || i >= len || s[i] != '[')
		return 0;
	i++;
	label->s = (char *)s + i;
	label->len = 0;
	while (i < len && s[i] != ']' && s[i] != '\n') {
		label->len++;
		i++;
	}
	if (i >= len || s[i] != ']' || label->len == 0)
		return 0;
	i++;
	if (i >= len || s[i] != ':')
		return 0;
	i++;
	while (i < len && (s[i] == ' ' || s[i] == '\t'))
		i++;
	url->s = (char *)s + i;
	url->len = 0;
	while (i + url->len < len && s[i + url->len] != '\n'
	    && s[i + url->len] != '\r' && s[i + url->len] != ' ')
		url->len++;
	if (url->len == 0)
		return 0;
	return 1;
}

static void
close_container(struct doc *doc, int idx)
{
	doc->blocks[idx].end = doc->nblocks;
}

static void
close_stack_above(struct doc *doc, int level)
{
	while (stackdepth > level) {
		stackdepth--;
		close_container(doc, stack[stackdepth].blockidx);
	}
}

/* append text to the content buffer of the most recent block */
static void
append_content(struct doc *doc, const char *s, int len)
{
	struct block *b;

	if (doc->nblocks == 0)
		return;
	b = &doc->blocks[doc->nblocks - 1];
	if (b->content.data == NULL)
		buf_init(&b->content);
	buf_append(&b->content, s, len);
}

/* start a new paragraph block with initial content */
static void
start_para(struct doc *doc, const char *s, int len)
{
	struct block b;

	memset(&b, 0, sizeof(b));
	b.type = BLK_PARA;
	buf_init(&b.content);
	buf_append(&b.content, s, len);
	doc_addblock(doc, b);
}

/* check if any list marker would match */
static int
is_any_list_marker(const char *s, int len)
{
	enum listmarker m;
	int dummy;

	if (scan_bullet(s, len, &m) > 0)
		return 1;
	if (scan_ordered(s, len, &m, &dummy) > 0)
		return 1;
	return 0;
}

void
block_parse(struct doc *doc)
{
	const char *line;
	int pos, linelen;
	int in_code_block;
	char code_fence_ch;
	int code_fence_len;
	int code_fence_indent;
	int top_para; /* paragraph at top level (no containers) */
	int top_heading; /* heading at top level */
	int heading_level;

	stackdepth = 0;
	pos = 0;
	in_code_block = 0;
	code_fence_ch = 0;
	code_fence_len = 0;
	code_fence_indent = 0;
	top_para = 0;
	top_heading = 0;
	heading_level = 0;

	while ((linelen = nextline(doc->input, doc->inputlen, pos, &line)) > 0) {
		const char *cur;
		int curlen;
		int i, off, old_depth;
		int contentoff, infooff, infolen, fencelen, headlevel, startnum;
		char fencech;
		enum listmarker marker;
		struct slice label, url;
		struct block b;

		cur = line;
		curlen = linelen;

		/* inside a code block: look for closing fence only */
		if (in_code_block) {
			int cl;
			char ch2;
			int d1, d2;

			cl = scan_code_fence(cur, curlen, &ch2, &d1, &d2);
			if (cl >= code_fence_len && ch2 == code_fence_ch) {
				int k = leading_spaces(cur, curlen) + cl;
				if (is_blank_line(cur + k, curlen - k)) {
					in_code_block = 0;
					pos += linelen;
					continue;
				}
			}
			{
				int strip = leading_spaces(cur, curlen);
				if (strip > code_fence_indent)
					strip = code_fence_indent;
				append_content(doc, cur + strip, curlen - strip);
			}
			pos += linelen;
			continue;
		}

		/* try to continue open containers */
		old_depth = stackdepth;
		for (i = 0; i < stackdepth; i++) {
			if (stack[i].type == BLK_QUOTE) {
				off = scan_blockquote(cur, curlen);
				if (off > 0) {
					cur += off;
					curlen -= off;
				} else if (is_blank_line(cur, curlen)) {
					/* unprefixed blank line ends the blockquote */
					close_stack_above(doc, i);
					break;
				} else {
					/* lazy continuation: if in a paragraph inside
					 * this quote, continue without the > prefix */
					int found_para = 0;
					int j;
					for (j = i + 1; j < stackdepth; j++) {
						if (stack[j].in_para) {
							found_para = 1;
							break;
						}
					}
					if (found_para || stack[i].in_para) {
						/* lazy: skip remaining container checks */
						break;
					}
					close_stack_above(doc, i);
					break;
				}
			} else if (stack[i].type == BLK_LISTITEM) {
				int sp = leading_spaces(cur, curlen);
				if (is_blank_line(cur, curlen)) {
					stack[i].had_blank = 1;
					break;
				}
				if (sp >= stack[i].indent) {
					/* fully indented continuation */
					cur += stack[i].indent;
					curlen -= stack[i].indent;
				} else if (!stack[i].had_blank) {
					/* no blank line yet */
					if (stack[i].in_para && (sp > 0
					    || !is_any_list_marker(cur, curlen))) {
						/* lazy continuation of paragraph */
						break;
					}
					close_stack_above(doc, i);
					break;
				} else if (sp > 0) {
					/* after blank + indented: sub-content */
					cur += sp;
					curlen -= sp;
				} else {
					/* after blank + no indent: close item */
					close_stack_above(doc, i);
					break;
				}
			} else if (stack[i].type == BLK_LIST) {
				continue;
			}
		}

		/* if we closed containers, any paragraph/heading is done */
		if (stackdepth < old_depth) {
			top_para = 0;
			top_heading = 0;
		}

		/* blank line */
		if (is_blank_line(cur, curlen)) {
			if (top_para)
				top_para = 0;
			if (top_heading)
				top_heading = 0;
			for (i = 0; i < stackdepth; i++) {
				stack[i].in_para = 0;
				stack[i].had_blank = 1;
			}
			memset(&b, 0, sizeof(b));
			b.type = BLK_BLANK;
			doc_addblock(doc, b);
			pos += linelen;
			continue;
		}

		/* heading continuation */
		if (top_heading && stackdepth == 0) {
			int dummy;
			int hlevel = scan_heading(cur, curlen, &dummy);
			if (hlevel > 0 && hlevel != heading_level) {
				/* different heading level: end current heading, process as new */
				top_heading = 0;
				goto newblock;
			}
			/* add newline separator */
			if (doc->nblocks > 0)
				buf_push(&doc->blocks[doc->nblocks-1].content, '\n');
			if (hlevel == heading_level) {
				/* continuation with matching # prefix: strip it */
				append_content(doc, cur + dummy, curlen - dummy);
			} else {
				append_content(doc, cur, curlen);
			}
			pos += linelen;
			continue;
		}

		/* paragraph continuation */
		if (top_para && stackdepth == 0) {
			append_content(doc, cur, curlen);
			pos += linelen;
			continue;
		}
		if (stackdepth > 0 && stack[stackdepth - 1].in_para) {
			append_content(doc, cur, curlen);
			pos += linelen;
			continue;
		}

		/* === new block detection === */
newblock:
		/* reference definition */
		if (scan_refdef(cur, curlen, &label, &url)) {
			doc_addref(doc, label, url);
			pos += linelen;
			continue;
		}

		/* thematic break (must check before bullet list since - is shared) */
		if (scan_thematic_break(cur, curlen)) {
			memset(&b, 0, sizeof(b));
			b.type = BLK_THEMATIC_BREAK;
			doc_addblock(doc, b);
			pos += linelen;
			continue;
		}

		/* heading */
		headlevel = scan_heading(cur, curlen, &contentoff);
		if (headlevel > 0) {
			int clen = curlen - contentoff;
			const char *cstart = cur + contentoff;

			memset(&b, 0, sizeof(b));
			b.type = BLK_HEADING;
			b.level = headlevel;
			/* trim leading whitespace */
			while (clen > 0 && (*cstart == ' ' || *cstart == '\t')) {
				cstart++;
				clen--;
			}
			/* trim trailing whitespace and # */
			while (clen > 0 && (cstart[clen-1] == '\n' || cstart[clen-1] == '\r'
			    || cstart[clen-1] == ' ' || cstart[clen-1] == '\t'))
				clen--;
			while (clen > 0 && cstart[clen-1] == '#')
				clen--;
			while (clen > 0 && (cstart[clen-1] == ' ' || cstart[clen-1] == '\t'))
				clen--;
			buf_init(&b.content);
			buf_append(&b.content, cstart, clen);
			doc_addblock(doc, b);
			if (stackdepth == 0) {
				top_heading = 1;
				heading_level = headlevel;
			}
			pos += linelen;
			continue;
		}

		/* code fence */
		fencelen = scan_code_fence(cur, curlen, &fencech, &infooff, &infolen);
		if (fencelen > 0) {
			memset(&b, 0, sizeof(b));
			b.type = BLK_CODEBLOCK;
			if (infolen > 0) {
				b.info.s = (char *)cur + infooff;
				b.info.len = infolen;
			}
			doc_addblock(doc, b);
			in_code_block = 1;
			code_fence_ch = fencech;
			code_fence_len = fencelen;
			code_fence_indent = leading_spaces(cur, curlen);
			pos += linelen;
			continue;
		}

		/* block quote */
		off = scan_blockquote(cur, curlen);
		if (off > 0) {
			memset(&b, 0, sizeof(b));
			b.type = BLK_QUOTE;
			b.start = doc->nblocks + 1;
			b.end = doc->nblocks + 1;
			doc_addblock(doc, b);
			if (stackdepth < STACK_MAX) {
				memset(&stack[stackdepth], 0, sizeof(stack[0]));
				stack[stackdepth].type = BLK_QUOTE;
				stack[stackdepth].blockidx = doc->nblocks - 1;
				stackdepth++;
			}
			cur += off;
			curlen -= off;
			if (!is_blank_line(cur, curlen))
				goto newblock;
			pos += linelen;
			continue;
		}

		/* bullet list */
		off = scan_bullet(cur, curlen, &marker);
		if (off > 0) {
			int listidx = -1;

			/* close previous item if in same list */
			if (stackdepth >= 2
			    && stack[stackdepth-1].type == BLK_LISTITEM
			    && stack[stackdepth-2].type == BLK_LIST
			    && stack[stackdepth-2].marker == marker) {
				if (stack[stackdepth-1].had_blank)
					doc->blocks[stack[stackdepth-2].blockidx].loose = 1;
				close_container(doc, stack[stackdepth-1].blockidx);
				stackdepth--;
				listidx = stack[stackdepth-1].blockidx;
			} else if (stackdepth >= 1
			    && stack[stackdepth-1].type == BLK_LIST
			    && stack[stackdepth-1].marker == marker) {
				/* item was already closed by container check */
				listidx = stack[stackdepth-1].blockidx;
			}
			if (listidx < 0) {
				/* close any non-matching list */
				while (stackdepth > 0 && (stack[stackdepth-1].type == BLK_LISTITEM
				    || stack[stackdepth-1].type == BLK_LIST)) {
					close_container(doc, stack[stackdepth-1].blockidx);
					stackdepth--;
				}
				/* new list */
				memset(&b, 0, sizeof(b));
				b.type = BLK_LIST;
				b.marker = marker;
				b.start = doc->nblocks + 1;
				doc_addblock(doc, b);
				listidx = doc->nblocks - 1;
				if (stackdepth < STACK_MAX) {
					memset(&stack[stackdepth], 0, sizeof(stack[0]));
					stack[stackdepth].type = BLK_LIST;
					stack[stackdepth].blockidx = listidx;
					stack[stackdepth].marker = marker;
					stackdepth++;
				}
			}
			/* new item */
			memset(&b, 0, sizeof(b));
			b.type = BLK_LISTITEM;
			b.start = doc->nblocks + 1;
			doc_addblock(doc, b);
			if (stackdepth < STACK_MAX) {
				memset(&stack[stackdepth], 0, sizeof(stack[0]));
				stack[stackdepth].type = BLK_LISTITEM;
				stack[stackdepth].blockidx = doc->nblocks - 1;
				stack[stackdepth].indent = off;
				stackdepth++;
			}
			cur += off;
			curlen -= off;
			if (!is_blank_line(cur, curlen))
				goto newblock;
			pos += linelen;
			continue;
		}

		/* ordered list */
		off = scan_ordered(cur, curlen, &marker, &startnum);
		if (off > 0) {
			int listidx = -1;

			if (stackdepth >= 2
			    && stack[stackdepth-1].type == BLK_LISTITEM
			    && stack[stackdepth-2].type == BLK_LIST
			    && stack[stackdepth-2].marker == marker) {
				if (stack[stackdepth-1].had_blank)
					doc->blocks[stack[stackdepth-2].blockidx].loose = 1;
				close_container(doc, stack[stackdepth-1].blockidx);
				stackdepth--;
				listidx = stack[stackdepth-1].blockidx;
			} else if (stackdepth >= 1
			    && stack[stackdepth-1].type == BLK_LIST
			    && stack[stackdepth-1].marker == marker) {
				listidx = stack[stackdepth-1].blockidx;
			}
			if (listidx < 0) {
				while (stackdepth > 0 && (stack[stackdepth-1].type == BLK_LISTITEM
				    || stack[stackdepth-1].type == BLK_LIST)) {
					close_container(doc, stack[stackdepth-1].blockidx);
					stackdepth--;
				}
				memset(&b, 0, sizeof(b));
				b.type = BLK_LIST;
				b.marker = marker;
				b.level = startnum;
				b.start = doc->nblocks + 1;
				doc_addblock(doc, b);
				listidx = doc->nblocks - 1;
				if (stackdepth < STACK_MAX) {
					memset(&stack[stackdepth], 0, sizeof(stack[0]));
					stack[stackdepth].type = BLK_LIST;
					stack[stackdepth].blockidx = listidx;
					stack[stackdepth].marker = marker;
					stackdepth++;
				}
			}
			memset(&b, 0, sizeof(b));
			b.type = BLK_LISTITEM;
			b.start = doc->nblocks + 1;
			doc_addblock(doc, b);
			if (stackdepth < STACK_MAX) {
				memset(&stack[stackdepth], 0, sizeof(stack[0]));
				stack[stackdepth].type = BLK_LISTITEM;
				stack[stackdepth].blockidx = doc->nblocks - 1;
				stack[stackdepth].indent = off;
				stackdepth++;
			}
			cur += off;
			curlen -= off;
			if (!is_blank_line(cur, curlen))
				goto newblock;
			pos += linelen;
			continue;
		}

		/* default: start a new paragraph */
		start_para(doc, cur, curlen);
		if (stackdepth > 0)
			stack[stackdepth - 1].in_para = 1;
		else
			top_para = 1;

		pos += linelen;
	}

	close_stack_above(doc, 0);
}
