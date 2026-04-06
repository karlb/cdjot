/* inline.c - inline-level parser and renderer */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "djot.h"

#define DELIM_MAX 512

enum delimflag {
	DELIM_OPEN  = 1,
	DELIM_CLOSE = 2,
};

struct delim {
	int pos;       /* position in output buffer */
	char ch;       /* '_' or '*' */
	int flags;
	int matched;   /* index of matching delimiter, or -1 */
	int active;
};

/* delimiter stack - saved/restored around recursive calls */
static struct delim delimstack[DELIM_MAX];
static int ndelims;

static int
is_ws(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0;
}

static void
resolve_delimiters(int base)
{
	int i, j;

	for (i = base; i < ndelims; i++) {
		if (!(delimstack[i].flags & DELIM_CLOSE))
			continue;
		if (!delimstack[i].active)
			continue;
		for (j = i - 1; j >= base; j--) {
			if (!delimstack[j].active)
				continue;
			if (delimstack[j].ch != delimstack[i].ch)
				continue;
			if (!(delimstack[j].flags & DELIM_OPEN))
				continue;
			delimstack[j].matched = i;
			delimstack[i].matched = j;
			delimstack[j].active = 0;
			delimstack[i].active = 0;
			/* deactivate delimiters between j and i that have same char
			 * to prevent overlapping */
			break;
		}
	}
}

static int
find_backtick_close(const char *s, int len, int pos, int count)
{
	int i, run;

	for (i = pos; i < len; ) {
		if (s[i] != '`') {
			i++;
			continue;
		}
		run = 0;
		while (i + run < len && s[i + run] == '`')
			run++;
		if (run == count)
			return i;
		i += run;
	}
	return -1;
}

static int
find_bracket_close(const char *s, int len, int pos)
{
	int i, depth;

	depth = 1;
	for (i = pos; i < len; i++) {
		if (s[i] == '\\' && i + 1 < len) {
			i++;
			continue;
		}
		if (s[i] == '[')
			depth++;
		if (s[i] == ']') {
			depth--;
			if (depth == 0)
				return i;
		}
	}
	return -1;
}

static int
find_paren_close(const char *s, int len, int pos)
{
	int i, depth;

	depth = 1;
	for (i = pos; i < len; i++) {
		if (s[i] == '\\' && i + 1 < len) {
			i++;
			continue;
		}
		if (s[i] == '(')
			depth++;
		if (s[i] == ')') {
			depth--;
			if (depth == 0)
				return i;
		}
	}
	return -1;
}

static struct slice
find_ref(struct doc *doc, const char *label, int labellen)
{
	int i;
	struct slice empty = {NULL, 0};

	for (i = 0; i < doc->nrefs; i++) {
		if (doc->refs[i].label.len == labellen
		    && memcmp(doc->refs[i].label.s, label, labellen) == 0)
			return doc->refs[i].url;
	}
	return empty;
}

/* emit a URL: strip newlines, handle backslash escapes, HTML-escape */
static void
emit_url(struct buf *out, const char *s, int len)
{
	int i;

	/* trim leading whitespace */
	while (len > 0 && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) {
		s++;
		len--;
	}
	/* trim trailing whitespace */
	while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t'
	    || s[len-1] == '\n' || s[len-1] == '\r'))
		len--;

	for (i = 0; i < len; i++) {
		if (s[i] == '\n' || s[i] == '\r')
			continue;
		/* backslash escape in URLs */
		if (s[i] == '\\' && i + 1 < len && is_ascii_punct(s[i+1])) {
			i++;
			switch (s[i]) {
			case '&': buf_appendstr(out, "&amp;"); break;
			case '"': buf_appendstr(out, "&quot;"); break;
			default: buf_push(out, s[i]); break;
			}
			continue;
		}
		switch (s[i]) {
		case '&': buf_appendstr(out, "&amp;"); break;
		case '"': buf_appendstr(out, "&quot;"); break;
		default: buf_push(out, s[i]); break;
		}
	}
}

/* render plain text (strip markup) for image alt text */
static void
render_plain(struct buf *out, const char *s, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		if (s[i] == '\\' && i + 1 < len && is_ascii_punct(s[i+1])) {
			buf_push(out, s[i+1]);
			i++;
		} else if (s[i] == '_' || s[i] == '*') {
			/* skip markup delimiters */
		} else if (s[i] == '`') {
			int count = 0, j2 = i;
			while (j2 < len && s[j2] == '`') { count++; j2++; }
			{
				int end2 = find_backtick_close(s, len, j2, count);
				if (end2 >= 0) {
					html_escape(out, s + j2, end2 - j2);
					i = end2 + count - 1;
				}
			}
		} else if (s[i] == '[') {
			/* handle [text](url) in alt text: extract text */
			int bend = find_bracket_close(s, len, i + 1);
			if (bend >= 0 && bend + 1 < len && s[bend + 1] == '(') {
				int pend = find_paren_close(s, len, bend + 2);
				if (pend >= 0) {
					render_plain(out, s + i + 1, bend - i - 1);
					i = pend;
					continue;
				}
			}
			/* not a link, skip the bracket */
		} else if (s[i] == '\n' || s[i] == '\r') {
			buf_push(out, ' ');
			if (s[i] == '\r' && i + 1 < len && s[i+1] == '\n')
				i++;
		} else {
			html_escape(out, s + i, 1);
		}
	}
}

void
inline_render(struct buf *out, struct doc *doc, const char *s, int len)
{
	int i, j, btcount, end, flags, is_image;
	char before, after;
	struct slice url;
	int delim_base;

	/* save delimiter stack depth for nested calls */
	delim_base = ndelims;

	i = 0;
	while (i < len) {
		/* skip trailing newline */
		if (s[i] == '\n' || s[i] == '\r') {
			if (i + 1 < len && s[i] == '\r' && s[i+1] == '\n') {
				buf_push(out, '\n');
				i += 2;
			} else {
				buf_push(out, '\n');
				i++;
			}
			continue;
		}

		/* backslash escape */
		if (s[i] == '\\' && i + 1 < len) {
			/* hard break: \ + optional spaces/tabs + newline */
			int k = i + 1;
			while (k < len && (s[k] == ' ' || s[k] == '\t'))
				k++;
			if (k < len && (s[k] == '\n' || s[k] == '\r')) {
				/* strip trailing whitespace before the backslash */
				while (out->len > 0 && (out->data[out->len-1] == ' '
				    || out->data[out->len-1] == '\t'))
					out->len--;
				buf_appendstr(out, "<br>\n");
				k++;
				if (k < len && s[k-1] == '\r' && s[k] == '\n')
					k++;
				i = k;
				continue;
			}
			if (s[i+1] == ' ') {
				buf_appendstr(out, "&nbsp;");
				i += 2;
				continue;
			}
			if (is_ascii_punct(s[i+1])) {
				html_escape(out, s + i + 1, 1);
				i += 2;
				continue;
			}
			buf_push(out, '\\');
			i++;
			continue;
		}

		/* backtick code span */
		if (s[i] == '`') {
			btcount = 0;
			j = i;
			while (j < len && s[j] == '`') {
				btcount++;
				j++;
			}
			end = find_backtick_close(s, len, j, btcount);
			if (end >= 0) {
				const char *code;
				int codelen;

				code = s + j;
				codelen = end - j;
				if (codelen >= 2 && code[0] == ' ' && code[codelen-1] == ' ') {
					code++;
					codelen -= 2;
				}
				buf_appendstr(out, "<code>");
				html_escape(out, code, codelen);
				buf_appendstr(out, "</code>");
				i = end + btcount;
				continue;
			}
			/* implicitly closed at end of paragraph */
			{
				const char *code = s + j;
				int codelen = len - j;
				/* trim trailing whitespace */
				while (codelen > 0 && (code[codelen-1] == '\n'
				    || code[codelen-1] == '\r'
				    || code[codelen-1] == ' '
				    || code[codelen-1] == '\t'))
					codelen--;
				if (codelen >= 2 && code[0] == ' '
				    && code[codelen-1] == ' ') {
					code++;
					codelen -= 2;
				}
				buf_appendstr(out, "<code>");
				html_escape(out, code, codelen);
				buf_appendstr(out, "</code>");
				i = len;
				continue;
			}
		}

		/* autolink */
		if (s[i] == '<') {
			j = i + 1;
			while (j < len && s[j] != '>' && s[j] != '<' && s[j] != '\n')
				j++;
			if (j < len && s[j] == '>') {
				const char *content = s + i + 1;
				int clen = j - i - 1;
				if (clen > 0 && (memchr(content, ':', clen) || memchr(content, '@', clen))) {
					if (memchr(content, '@', clen) && !memchr(content, ':', clen)) {
						buf_appendstr(out, "<a href=\"mailto:");
						html_escape(out, content, clen);
						buf_appendstr(out, "\">");
						html_escape(out, content, clen);
						buf_appendstr(out, "</a>");
					} else {
						buf_appendstr(out, "<a href=\"");
						url_escape(out, content, clen);
						buf_appendstr(out, "\">");
						html_escape(out, content, clen);
						buf_appendstr(out, "</a>");
					}
					i = j + 1;
					continue;
				}
			}
			buf_appendstr(out, "&lt;");
			i++;
			continue;
		}

		/* image or link */
		is_image = 0;
		if (s[i] == '!' && i + 1 < len && s[i+1] == '[') {
			is_image = 1;
			i++;
		}
		if (s[i] == '[') {
			int bracket_end = find_bracket_close(s, len, i + 1);
			if (bracket_end < 0) {
				if (is_image)
					buf_push(out, '!');
				buf_push(out, '[');
				i++;
				continue;
			}
			j = bracket_end + 1;
			if (j < len && s[j] == '(') {
				/* inline link [text](url) */
				int paren_end = find_paren_close(s, len, j + 1);
				if (paren_end >= 0) {
					if (is_image) {
						buf_appendstr(out, "<img alt=\"");
						render_plain(out, s + i + 1, bracket_end - i - 1);
						buf_appendstr(out, "\" src=\"");
						emit_url(out, s + j + 1, paren_end - j - 1);
						buf_appendstr(out, "\">");
					} else {
						buf_appendstr(out, "<a href=\"");
						emit_url(out, s + j + 1, paren_end - j - 1);
						buf_appendstr(out, "\">");
						inline_render(out, doc, s + i + 1, bracket_end - i - 1);
						buf_appendstr(out, "</a>");
					}
					i = paren_end + 1;
					continue;
				}
			}
			if (j < len && s[j] == '[') {
				/* reference link [text][ref] */
				int ref_end = find_bracket_close(s, len, j + 1);
				if (ref_end >= 0) {
					const char *label;
					int labellen;
					if (ref_end == j + 1) {
						label = s + i + 1;
						labellen = bracket_end - i - 1;
					} else {
						label = s + j + 1;
						labellen = ref_end - j - 1;
					}
					url = find_ref(doc, label, labellen);
					if (url.s) {
						if (is_image) {
							buf_appendstr(out, "<img alt=\"");
							render_plain(out, s + i + 1, bracket_end - i - 1);
							buf_appendstr(out, "\" src=\"");
							url_escape(out, url.s, url.len);
							buf_appendstr(out, "\">");
						} else {
							buf_appendstr(out, "<a href=\"");
							url_escape(out, url.s, url.len);
							buf_appendstr(out, "\">");
							inline_render(out, doc, s + i + 1, bracket_end - i - 1);
							buf_appendstr(out, "</a>");
						}
						i = ref_end + 1;
						continue;
					}
				}
			}
			if (is_image)
				buf_push(out, '!');
			buf_push(out, '[');
			i++;
			continue;
		}

		/* emphasis _ and strong * */
		if (s[i] == '_' || s[i] == '*') {
			char ch = s[i];
			int run = 0;
			int start = i;

			/* count consecutive delimiter chars */
			while (i < len && s[i] == ch) {
				run++;
				i++;
			}

			before = (start > 0) ? s[start-1] : 0;
			after = (i < len) ? s[i] : 0;

			flags = 0;
			if (!is_ws(after))
				flags |= DELIM_OPEN;
			if (!is_ws(before))
				flags |= DELIM_CLOSE;

			/* each char in the run is a separate delimiter */
			for (j = 0; j < run && ndelims < DELIM_MAX; j++) {
				delimstack[ndelims].pos = out->len + j;
				delimstack[ndelims].ch = ch;
				delimstack[ndelims].flags = flags;
				delimstack[ndelims].matched = -1;
				delimstack[ndelims].active = 1;
				ndelims++;
			}
			/* emit placeholder chars */
			for (j = 0; j < run; j++)
				buf_push(out, ch);
			continue;
		}

		/* smart punctuation: dashes */
		if (s[i] == '-' && i + 1 < len && s[i+1] == '-') {
			int run = 0;
			int k = i;
			while (k < len && s[k] == '-') {
				run++;
				k++;
			}
			i = k;
			/* decompose into em-dashes (3) and en-dashes (2) */
			{
				int em = 0, en = 0;
				if (run % 3 == 0) {
					em = run / 3;
				} else if (run % 3 == 1) {
					en = 2;
					em = (run - 4) / 3;
				} else {
					en = 1;
					em = (run - 2) / 3;
				}
				while (em-- > 0)
					buf_appendstr(out, "\xe2\x80\x94");
				while (en-- > 0)
					buf_appendstr(out, "\xe2\x80\x93");
			}
			continue;
		}
		if (s[i] == '.' && i + 2 < len && s[i+1] == '.' && s[i+2] == '.') {
			buf_appendstr(out, "\xe2\x80\xa6");
			i += 3;
			continue;
		}
		if (s[i] == '"' || s[i] == '\'') {
			int can_open, can_close;
			before = (i > 0) ? s[i-1] : 0;
			after = (i + 1 < len) ? s[i+1] : 0;
			/* special case: ' before digit is always apostrophe */
			if (s[i] == '\'' && isdigit((unsigned char)after)) {
				buf_appendstr(out, "\xe2\x80\x99");
				i++;
				continue;
			}
			/* left-flanking: after not ws, AND before is ws/punct/start */
			can_open = !is_ws(after) &&
			    (is_ws(before) || is_ascii_punct(before) || before == 0);
			/* right-flanking: before not ws, AND after is ws/punct/end */
			can_close = !is_ws(before) &&
			    (is_ws(after) || is_ascii_punct(after) || after == 0);
			if (s[i] == '"') {
				if (can_open && !can_close)
					buf_appendstr(out, "\xe2\x80\x9c");
				else if (can_close)
					buf_appendstr(out, "\xe2\x80\x9d");
				else
					buf_appendstr(out, "\xe2\x80\x9c");
			} else {
				if (can_close && !can_open)
					buf_appendstr(out, "\xe2\x80\x99");
				else if (can_open && !can_close)
					buf_appendstr(out, "\xe2\x80\x98");
				else if (can_open && can_close)
					/* ambiguous: prefer right for apostrophe */
					buf_appendstr(out, "\xe2\x80\x99");
				else
					buf_appendstr(out, "\xe2\x80\x98");
			}
			i++;
			continue;
		}

		/* plain character */
		html_escape(out, s + i, 1);
		i++;
	}

	/* resolve emphasis/strong delimiters */
	resolve_delimiters(delim_base);

	/* patch output buffer: replace delimiter placeholders with tags */
	if (ndelims > delim_base) {
		struct buf tmp;
		int prev, d;

		buf_init(&tmp);
		prev = 0;
		for (d = delim_base; d < ndelims; d++) {
			int pos = delimstack[d].pos;
			if (pos > prev)
				buf_append(&tmp, out->data + prev, pos - prev);
			if (delimstack[d].matched >= 0) {
				if (delimstack[d].matched > d) {
					if (delimstack[d].ch == '_')
						buf_appendstr(&tmp, "<em>");
					else
						buf_appendstr(&tmp, "<strong>");
				} else {
					if (delimstack[d].ch == '_')
						buf_appendstr(&tmp, "</em>");
					else
						buf_appendstr(&tmp, "</strong>");
				}
			} else {
				buf_push(&tmp, delimstack[d].ch);
			}
			prev = pos + 1;
		}
		if (prev < out->len)
			buf_append(&tmp, out->data + prev, out->len - prev);
		free(out->data);
		out->data = tmp.data;
		out->len = tmp.len;
		out->cap = tmp.cap;
	}

	/* restore delimiter stack */
	ndelims = delim_base;
}
