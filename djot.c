/* djot.c - djot to HTML converter */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "djot.h"

static char *
read_stdin(int *len)
{
	char *buf;
	int cap, n;

	cap = 4096;
	*len = 0;
	buf = malloc(cap);
	if (!buf) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	while ((n = fread(buf + *len, 1, cap - *len, stdin)) > 0) {
		*len += n;
		if (*len >= cap) {
			cap *= 2;
			buf = realloc(buf, cap);
			if (!buf) {
				fprintf(stderr, "out of memory\n");
				exit(1);
			}
		}
	}
	return buf;
}

/* generate a heading id from text: lowercase, spaces->hyphens, strip punctuation */
static void
make_heading_id(struct buf *out, const char *s, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		if (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') {
			buf_push(out, '-');
		} else if (isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '_') {
			buf_push(out, s[i]);
		}
	}
}

static void
render_inline(struct buf *out, struct doc *doc, struct buf *content)
{
	const char *s;
	int len;

	if (!content->data || content->len == 0)
		return;

	s = content->data;
	len = content->len;

	/* trim leading/trailing whitespace */
	while (len > 0 && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) {
		s++;
		len--;
	}
	while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t'
	    || s[len-1] == '\n' || s[len-1] == '\r'))
		len--;

	if (len > 0)
		inline_render(out, doc, s, len);
}

void
render_blocks(struct buf *out, struct doc *doc, int start, int end, int insection)
{
	static int empty_id_counter = 0;
	int i, j;
	int section_levels[6];
	int nsections = 0;

	for (i = start; i < end; i++) {
		struct block *b = &doc->blocks[i];

		switch (b->type) {
		case BLK_BLANK:
		case BLK_REFDEF:
			break;

		case BLK_PARA:
			buf_appendstr(out, "<p>");
			render_inline(out, doc, &b->content);
			buf_appendstr(out, "</p>\n");
			break;

		case BLK_HEADING: {
			char tag[16];
			struct buf idbuf;

			/* close sections of equal or deeper level */
			if (!insection) {
				while (nsections > 0 && section_levels[nsections-1] >= b->level) {
					buf_appendstr(out, "</section>\n");
					nsections--;
				}
			}

			buf_init(&idbuf);
			{
				const char *ids = b->content.data;
				int idlen = b->content.len;
				/* trim whitespace for id generation */
				while (idlen > 0 && (*ids == '\n' || *ids == '\r'
				    || *ids == ' ' || *ids == '\t')) {
					ids++;
					idlen--;
				}
				while (idlen > 0 && (ids[idlen-1] == '\n'
				    || ids[idlen-1] == '\r'
				    || ids[idlen-1] == ' '
				    || ids[idlen-1] == '\t'))
					idlen--;
				make_heading_id(&idbuf, ids, idlen);
			}
			if (idbuf.len == 0) {
				char tmp[16];
				empty_id_counter++;
				snprintf(tmp, sizeof(tmp), "s-%d", empty_id_counter);
				buf_appendstr(&idbuf, tmp);
			}

			if (!insection) {
				buf_appendstr(out, "<section id=\"");
				buf_append(out, idbuf.data, idbuf.len);
				buf_appendstr(out, "\">\n");
				if (nsections < 6)
					section_levels[nsections++] = b->level;
			}

			snprintf(tag, sizeof(tag), "<h%d", b->level);
			buf_appendstr(out, tag);
			if (insection) {
				buf_appendstr(out, " id=\"");
				buf_append(out, idbuf.data, idbuf.len);
				buf_appendstr(out, "\"");
			}
			buf_appendstr(out, ">");
			render_inline(out, doc, &b->content);
			snprintf(tag, sizeof(tag), "</h%d>\n", b->level);
			buf_appendstr(out, tag);
			buf_free(&idbuf);
			break;
		}

		case BLK_CODEBLOCK:
			if (b->info.len > 0) {
				buf_appendstr(out, "<pre><code class=\"language-");
				html_escape(out, b->info.s, b->info.len);
				buf_appendstr(out, "\">");
			} else {
				buf_appendstr(out, "<pre><code>");
			}
			if (b->content.data)
				html_escape(out, b->content.data, b->content.len);
			buf_appendstr(out, "</code></pre>\n");
			break;

		case BLK_THEMATIC_BREAK:
			buf_appendstr(out, "<hr>\n");
			break;

		case BLK_QUOTE:
			buf_appendstr(out, "<blockquote>\n");
			render_blocks(out, doc, b->start, b->end, 1);
			buf_appendstr(out, "</blockquote>\n");
			if (b->end > i + 1)
				i = b->end - 1;
			break;

		case BLK_LIST: {
			int is_ordered = (b->marker == LIST_ORD_DOT
			    || b->marker == LIST_ORD_PAREN);

			if (is_ordered) {
				if (b->level != 1) {
					char startattr[32];
					snprintf(startattr, sizeof(startattr),
					    "<ol start=\"%d\">\n", b->level);
					buf_appendstr(out, startattr);
				} else {
					buf_appendstr(out, "<ol>\n");
				}
			} else {
				buf_appendstr(out, "<ul>\n");
			}

			for (j = b->start; j < b->end; j++) {
				struct block *item = &doc->blocks[j];
				if (item->type != BLK_LISTITEM)
					continue;
				buf_appendstr(out, "<li>\n");
				if (b->loose) {
					render_blocks(out, doc, item->start, item->end, 1);
				} else {
					int k;
					for (k = item->start; k < item->end; k++) {
						struct block *child = &doc->blocks[k];
						if (child->type == BLK_PARA) {
							render_inline(out, doc, &child->content);
							buf_push(out, '\n');
						} else if (child->type == BLK_LIST) {
							render_blocks(out, doc, k, k + 1, 1);
							if (child->end > k + 1)
								k = child->end - 1;
						} else if (child->type != BLK_BLANK) {
							render_blocks(out, doc, k, k + 1, 1);
						}
					}
				}
				buf_appendstr(out, "</li>\n");
				if (item->end > j + 1)
					j = item->end - 1;
			}

			if (is_ordered)
				buf_appendstr(out, "</ol>\n");
			else
				buf_appendstr(out, "</ul>\n");
			if (b->end > i + 1)
				i = b->end - 1;
			break;
		}

		case BLK_LISTITEM:
			/* handled by BLK_LIST */
			break;
		}
	}

	if (!insection) {
		while (nsections > 0) {
			buf_appendstr(out, "</section>\n");
			nsections--;
		}
	}
}

int
main(void)
{
	struct doc doc;
	struct buf out;
	char *input;
	int len, i;

	input = read_stdin(&len);

	memset(&doc, 0, sizeof(doc));
	doc.input = input;
	doc.inputlen = len;

	block_parse(&doc);

	buf_init(&out);
	render_blocks(&out, &doc, 0, doc.nblocks, 0);

	fwrite(out.data, 1, out.len, stdout);

	buf_free(&out);
	free(input);
	for (i = 0; i < doc.nblocks; i++) {
		if (doc.blocks[i].content.data)
			buf_free(&doc.blocks[i].content);
	}
	free(doc.blocks);
	free(doc.refs);

	return 0;
}
