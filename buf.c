/* buf.c - growable buffer and utility functions */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "djot.h"

void
buf_init(struct buf *b)
{
	b->cap = 1024;
	b->len = 0;
	b->data = malloc(b->cap);
	if (!b->data) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
}

static void
buf_grow(struct buf *b, int need)
{
	while (b->cap < need) {
		b->cap *= 2;
	}
	b->data = realloc(b->data, b->cap);
	if (!b->data) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
}

void
buf_push(struct buf *b, char c)
{
	if (b->len + 1 > b->cap)
		buf_grow(b, b->len + 1);
	b->data[b->len++] = c;
}

void
buf_append(struct buf *b, const char *s, int len)
{
	if (len <= 0)
		return;
	if (b->len + len > b->cap)
		buf_grow(b, b->len + len);
	memcpy(b->data + b->len, s, len);
	b->len += len;
}

void
buf_appendstr(struct buf *b, const char *s)
{
	buf_append(b, s, strlen(s));
}

void
buf_free(struct buf *b)
{
	free(b->data);
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

void
html_escape(struct buf *b, const char *s, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		switch (s[i]) {
		case '&':
			buf_appendstr(b, "&amp;");
			break;
		case '<':
			buf_appendstr(b, "&lt;");
			break;
		case '>':
			buf_appendstr(b, "&gt;");
			break;
		default:
			buf_push(b, s[i]);
			break;
		}
	}
}

void
url_escape(struct buf *b, const char *s, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		switch (s[i]) {
		case '&':
			buf_appendstr(b, "&amp;");
			break;
		case '<':
			buf_appendstr(b, "&lt;");
			break;
		case '>':
			buf_appendstr(b, "&gt;");
			break;
		case '"':
			buf_appendstr(b, "&quot;");
			break;
		default:
			buf_push(b, s[i]);
			break;
		}
	}
}

int
is_ascii_punct(int c)
{
	return (c >= 0x21 && c <= 0x2f)
	    || (c >= 0x3a && c <= 0x40)
	    || (c >= 0x5b && c <= 0x60)
	    || (c >= 0x7b && c <= 0x7e);
}

int
is_blank_line(const char *s, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n')
			return 0;
	}
	return 1;
}

struct slice
slice_trim(struct slice s)
{
	while (s.len > 0 && (s.s[0] == ' ' || s.s[0] == '\t')) {
		s.s++;
		s.len--;
	}
	while (s.len > 0 && (s.s[s.len-1] == ' ' || s.s[s.len-1] == '\t'
	    || s.s[s.len-1] == '\r' || s.s[s.len-1] == '\n')) {
		s.len--;
	}
	return s;
}
