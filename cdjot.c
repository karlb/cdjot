/* cdjot - djot to HTML converter
 * No dependencies beyond libc.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cdjot.h"

#define LEN(x) (sizeof(x)/sizeof(x[0]))
#define ADDC(b,i) do { if ((i) % BUFSIZ == 0) { \
	b = realloc(b, ((i) + BUFSIZ)); if (!b) die("malloc"); } } while(0); b[i]
#define PUSH(b,i,ch) do { ADDC(b,i) = (ch); (i)++; } while(0)
/* Batch append: copy n bytes from src into buffer b at index *ip, growing as
 * needed in BUFSIZ-aligned chunks (matching ADDC's invariant so subsequent
 * ADDC/PUSH calls remain correct). Replaces per-byte PUSH loops in block
 * parsers' line-collection inner loops. */
#define PUSHRANGE(b,i,src,n) do { \
	int _n = (n); \
	if (_n > 0) { \
		int _ni = (i) + _n; \
		int _oc = (i) == 0 ? 0 : (((i) - 1) / BUFSIZ + 1) * BUFSIZ; \
		int _nc = (_ni - 1) / BUFSIZ + 1; _nc *= BUFSIZ; \
		if (_nc != _oc) { \
			(b) = realloc((b), _nc); \
			if (!(b)) die("malloc"); \
		} \
		memcpy((b) + (i), (src), _n); \
		(i) = _ni; \
	} \
} while (0)
#define GROWBUF(b,len,cap,need) do { if ((len)+(need)>(cap)) { \
	(cap) = ((cap)+(need)) * 2; (b) = realloc((b), (cap)); \
	if (!(b)) die("malloc"); } } while(0)
#define GROWA(arr,n,cap) do { if ((n) >= (cap)) { \
	(cap) = (cap) ? (cap) * 2 : 16; \
	(arr) = realloc((arr), (cap) * sizeof(*(arr))); \
	if (!(arr)) die("malloc"); } } while(0)

static int dotable(const char *b, const char *e, int n);
static int dodeflist(const char *b, const char *e, int n);
static int dodiv(const char *b, const char *e, int n);
static int doattr(const char *b, const char *e, int n);
static int dorefdef(const char *b, const char *e, int n);
static int doheading(const char *b, const char *e, int n);
static int doblockquote(const char *b, const char *e, int n);
static int docodefence(const char *b, const char *e, int n);
static int dothematicbreak(const char *b, const char *e, int n);
static int dolist(const char *b, const char *e, int n);
static int doparagraph(const char *b, const char *e, int n);
static int dolinebreak(const char *b, const char *e, int n);
static int dosurround(const char *b, const char *e, int n);
static int docode(const char *b, const char *e, int n);
static int dolink(const char *b, const char *e, int n);
static int doautolink(const char *b, const char *e, int n);
static int doreplace(const char *b, const char *e, int n);
static void process(const char *b, const char *e, int newblock);
static void hprint(const char *b, const char *e);
static void clear_pending(void);
static int has_pending(void);
static void attr_emit(const char *a);
static void emit_pending(void);

/* Converter state — global for simplicity (smu-style). Not thread-safe;
 * reset at the start of each cdjot_convert() call. */
static struct {
	char *norm; int normlen;
	const char *url; int urllen;
	char *attrs;
} *refs;
static int nrefs, cap_refs;

/* Footnote labels are owned (malloc'd) because they can be added during
 * inline processing from temporary recursion buffers (e.g. a list item's
 * collection buffer that gets freed when the parent dolist returns). The
 * conversion may still reference the same label later, so we copy. */
static struct {
	char *label; int labellen;
	char *content; int contentlen;
	int used;
	int num; /* sequential number assigned on first reference */
} *footnotes;
static int nfootnotes, cap_fn;
static int footnote_counter;

static int sections[6], nsections;
static int in_container;
static int tight;
static const char *proc_base;

/* Cache for doreplace's `{X` scan: a contiguous range [nc_b, nc_eol)
 * in which no `}` exists before the next `\n` or e. Without this, inputs
 * like `{{{{{...` are O(N^2) — every `{` scans to the next `\n`. */
static const char *nc_e;
static const char *nc_b;
static const char *nc_eol;

/* Bracket-match table built once per process() call. bm_match[i] is the
 * offset (relative to bm_base) of the `]` matching `[` at bm_base+i with
 * depth-0 escape-aware matching, or -1 if no such match exists. Replaces
 * dolink's per-call O(N) depth walk; without this, inputs like
 * `[[[[...[](` are O(N^2). The state is per-process-call (saved/restored
 * at recursion); the array memory is owned by the current call. */
static const char *bm_base;
static const char *bm_end;
static long *bm_match;
static long bm_cap;
static long *bm_stack;
static long bm_stack_cap;

/* Single growable output buffer. All emit goes here, then a single fwrite at
 * the end of cdjot_convert. Avoids stdio per-call overhead and chunked write
 * syscalls. */
static char *obuf;
static int olen, ocap;

static char *pending_attrs;
static int cap_pattr;

struct id_entry { char *key; int count; };
static struct id_entry *id_ht; /* heading ID hash table */
static int id_ht_sz, id_ht_cnt;

static void
die(const char *msg)
{
	fprintf(stderr, "cdjot: %s\n", msg);
	exit(1);
}

static void
obuf_grow(int need)
{
	int nc = ocap ? ocap : 4096;
	while (nc < olen + need) nc *= 2;
	obuf = realloc(obuf, nc);
	if (!obuf) die("malloc");
	ocap = nc;
}

static inline void
oputc(char c)
{
	if (olen + 1 > ocap) obuf_grow(1);
	obuf[olen++] = c;
}

static inline void
owrite(const char *s, int len)
{
	if (olen + len > ocap) obuf_grow(len);
	memcpy(obuf + olen, s, len);
	olen += len;
}

static inline void
oputs(const char *s)
{
	owrite(s, strlen(s));
}

static void
oprintf(const char *fmt, ...)
{
	va_list ap;
	int n, avail;
	va_start(ap, fmt);
	avail = ocap - olen;
	n = vsnprintf(obuf + olen, avail > 0 ? avail : 0, fmt, ap);
	va_end(ap);
	if (n < 0) die("vsnprintf");
	if (n >= avail) {
		obuf_grow(n + 1);
		va_start(ap, fmt);
		vsnprintf(obuf + olen, ocap - olen, fmt, ap);
		va_end(ap);
	}
	olen += n;
}

/* ensure *buf has room for at least `need` bytes.
 * *buf may be NULL (realloc handles it); caller must NUL-terminate after use. */
static void
pensure(char **buf, int *cap, int need)
{
	if (need > *cap) {
		*cap = need * 2;
		*buf = realloc(*buf, *cap);
		if (!*buf) die("malloc");
	}
}

static void
build_bracket_match(const char *b, const char *e)
{
	long n, i, top;
	if (bm_base == b && bm_end == e) return;
	n = e - b;
	if (n + 1 > bm_cap) {
		bm_cap = n + 64;
		bm_match = realloc(bm_match, bm_cap * sizeof(*bm_match));
		if (!bm_match) die("malloc");
	}
	if (n + 1 > bm_stack_cap) {
		bm_stack_cap = n + 64;
		bm_stack = realloc(bm_stack, bm_stack_cap * sizeof(*bm_stack));
		if (!bm_stack) die("malloc");
	}
	top = 0;
	for (i = 0; i < n; i++) bm_match[i] = -1;
	for (i = 0; i < n; i++) {
		if (b[i] == '\\' && i + 1 < n) { i++; continue; }
		if (b[i] == '[') {
			bm_stack[top++] = i;
		} else if (b[i] == ']' && top > 0) {
			long j = bm_stack[--top];
			bm_match[j] = i;
		}
	}
	bm_base = b;
	bm_end = e;
}

static void
hprint(const char *b, const char *e)
{
	for (; b < e; b++) {
		if (*b == '&')      oputs("&amp;");
		else if (*b == '<') oputs("&lt;");
		else if (*b == '>') oputs("&gt;");
		else                oputc(*b);
	}
}

static const char *
eol(const char *p, const char *e)
{
	const char *nl = memchr(p, '\n', e - p);
	return nl ? nl + 1 : e;
}

static int
isblankline(const char *p, const char *e)
{
	for (; p < e; p++)
		if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
			return 0;
	return 1;
}

static int
leadc(const char *p, const char *e, char ch)
{
	int n = 0;
	while (p + n < e && p[n] == ch) n++;
	return n;
}

static int
spaces(const char *p, const char *e)
{
	return leadc(p, e, ' ');
}

static int
isasciipunct(int c)
{
	return (c >= 0x21 && c <= 0x2f) || (c >= 0x3a && c <= 0x40)
	    || (c >= 0x5b && c <= 0x60) || (c >= 0x7b && c <= 0x7e);
}

static int
isws(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0;
}

static const char *
trim_end(const char *b, const char *e)
{
	while (e > b && isws(e[-1])) e--;
	return e;
}

static const char *
skip_blanks(const char *p, const char *e)
{
	while (p < e && isblankline(p, eol(p, e)))
		p = eol(p, e);
	return p;
}

/* Normalize a reference label for comparison: drop _ and *, collapse
 * whitespace runs to a single space, trim leading/trailing whitespace.
 * Output buffer must hold at least `len` bytes; the result is never
 * longer than the input. Returns the number of bytes written. */
static int
normalize_label(const char *s, int len, char *out)
{
	int i, o = 0, last_was_ws = 1;
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '_' || c == '*') continue;
		if (isws(c)) {
			if (!last_was_ws) {
				out[o++] = ' ';
				last_was_ws = 1;
			}
		} else {
			out[o++] = c;
			last_was_ws = 0;
		}
	}
	if (o > 0 && out[o-1] == ' ') o--;
	return o;
}

static int
findref(const char *label, int len, const char **url, int *urllen)
{
	char stack[1024], *buf = stack, *heap = NULL;
	int nlen, i, found = 0;
	if (len > (int)sizeof(stack)) {
		heap = malloc(len);
		if (!heap) return 0;
		buf = heap;
	}
	nlen = normalize_label(label, len, buf);
	for (i = 0; i < nrefs; i++)
		if (refs[i].normlen == nlen
		    && !memcmp(refs[i].norm, buf, nlen)) {
			*url = refs[i].url;
			*urllen = refs[i].urllen;
			found = i + 1; /* 1-based index */
			break;
		}
	free(heap);
	return found;
}

static int
findref_range(const char *label, int len, int lo, int hi)
{
	char stack[1024], *buf = stack, *heap = NULL;
	int nlen, i, found = 0;
	if (len > (int)sizeof(stack)) {
		heap = malloc(len);
		if (!heap) return 0;
		buf = heap;
	}
	nlen = normalize_label(label, len, buf);
	for (i = lo; i < hi; i++)
		if (refs[i].normlen == nlen
		    && !memcmp(refs[i].norm, buf, nlen)) {
			found = 1;
			break;
		}
	free(heap);
	return found;
}


static int
make_slug(const char *b, int len, char *out, int outsz)
{
	const char *s = b, *se = b + len;
	int hi = 0;
	while (s < se && isws(*s)) s++;
	while (se > s && isws(se[-1])) se--;
	for (; s < se && hi < outsz - 1; s++) {
		if (*s == '[' && s + 1 < se && s[1] == '^') {
			while (s < se && *s != ']') s++;
			continue;
		}
		if (*s == ' ' || *s == '\t' || *s == '\n') {
			if (hi == 0 || out[hi-1] != '-')
				out[hi++] = '-';
		} else if (isalnum((unsigned char)*s) || *s == '-' || *s == '_')
			out[hi++] = *s;
	}
	while (hi > 0 && out[hi-1] == '-') hi--;
	{
		int ss = 0;
		while (ss < hi && out[ss] == '-') ss++;
		memmove(out, out + ss, hi - ss);
		hi -= ss;
	}
	out[hi] = '\0';
	return hi;
}

static void
close_sections(int level)
{
	while (nsections > 0 && sections[nsections-1] >= level) {
		oputs("</section>\n");
		nsections--;
	}
}

/* Identifier/class/key/unquoted-value chars per djot.js: ASCII alnum
 * plus _ : - (matches the spec's rule for unquoted key=value values). */
#define IS_NAME_CHAR(c) (isalnum((unsigned char)(c)) \
    || (c) == '_' || (c) == ':' || (c) == '-')

/* Attribute lists are a flat run of NUL-terminated name/value pairs in
 * declaration order, closed by an empty name: "id\0x\0class\0y\0\0".
 * djot.js emits attributes in the order they were written, interleaving
 * classes with key=value pairs, so the order has to live in the structure
 * rather than in a fixed emit sequence. Values are stored unescaped and
 * escaped on output. */
enum { ATTR_SET, ATTR_APPEND, ATTR_PREPEND };

/* size of a in bytes, including the closing empty name */
static int
attrs_size(const char *a)
{
	const char *p = a;
	while (*p) {
		p += strlen(p) + 1;
		p += strlen(p) + 1;
	}
	return p - a + 1;
}

/* value of name, or NULL if absent */
static char *
attr_find(const char *a, const char *name)
{
	const char *p = a;
	while (p && *p) {
		const char *v = p + strlen(p) + 1;
		if (!strcmp(p, name)) return (char *)v;
		p = v + strlen(v) + 1;
	}
	return NULL;
}

/* Store name=val. An existing name keeps its position: ATTR_SET replaces
 * the value, ATTR_APPEND/ATTR_PREPEND join to it with a space (class
 * lists accumulate). A new name goes on the end. */
static void
attr_put(char **a, int *cap, const char *name, const char *val, int mode)
{
	int nlen = strlen(name), vlen = strlen(val);
	int size = attrs_size(*a);
	char *old = attr_find(*a, name);

	if (!old) {
		char *p;
		pensure(a, cap, size + nlen + 1 + vlen + 1);
		p = *a + size - 1;	/* the closing empty name */
		memcpy(p, name, nlen + 1);
		p += nlen + 1;
		memcpy(p, val, vlen + 1);
		p[vlen + 1] = '\0';
		return;
	}
	{
		int off = old - *a;
		int olen = strlen(old);
		int newlen = (mode == ATTR_SET) ? vlen : olen + 1 + vlen;
		int tail = size - (off + olen + 1);
		pensure(a, cap, size + newlen - olen);
		old = *a + off;
		memmove(old + newlen + 1, old + olen + 1, tail);
		if (mode == ATTR_APPEND) {
			old[olen] = ' ';
			memcpy(old + olen + 1, val, vlen);
		} else if (mode == ATTR_PREPEND) {
			memmove(old + vlen + 1, old, olen);
			memcpy(old, val, vlen);
			old[vlen] = ' ';
		} else {
			memcpy(old, val, vlen);
		}
		old[newlen] = '\0';
	}
}

/* fold src into a, following the same last-value-wins / classes-accumulate
 * rules that apply within a single {...} spec */
static void
attr_merge(char **a, int *cap, const char *src)
{
	const char *p = src;
	while (p && *p) {
		const char *v = p + strlen(p) + 1;
		attr_put(a, cap, p, v,
		    strcmp(p, "class") ? ATTR_SET : ATTR_APPEND);
		p = v + strlen(v) + 1;
	}
}

/* Parse the content between { and }. On success *outp is an attribute
 * list (possibly empty, e.g. for a lone comment) that the caller frees;
 * on a syntax error *outp is NULL. Returns whether any attrs were found. */
static int
parse_attrs(const char *b, const char *e, char **outp)
{
	const char *p = b;
	int len = e - b;
	int cap = 16;
	char *out = malloc(cap);
	char *name = malloc(len + 1);
	char *val = malloc(len + 1);

	if (!out || !name || !val) die("malloc");
	out[0] = '\0';

	while (p < e) {
		int n = 0;
		while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
		if (p >= e) break;
		if (*p == '#' || *p == '.') {
			int isid = (*p == '#');
			p++;
			while (p < e && *p != ' ' && *p != '\t' && *p != '\n'
			    && *p != '}') {
				if (!IS_NAME_CHAR(*p)) goto fail;
				val[n++] = *p;
				p++;
			}
			val[n] = '\0';
			if (n > 0)
				attr_put(&out, &cap, isid ? "id" : "class",
				    val, isid ? ATTR_SET : ATTR_APPEND);
		} else if (*p == '%') {
			/* comment: skip to next % or end */
			p++;
			while (p < e && *p != '%') p++;
			if (p < e) p++; /* skip closing % */
		} else if (isalpha((unsigned char)*p)) {
			/* key=val */
			int kn = 0;
			while (p < e && *p != '=' && *p != ' ' && *p != '}') {
				if (!IS_NAME_CHAR(*p)) goto fail;
				name[kn++] = *p;
				p++;
			}
			name[kn] = '\0';
			if (p >= e || *p != '=') goto fail; /* bare key */
			p++;
			if (p < e && *p == '"') {
				p++;
				while (p < e && *p != '"') {
					if (*p == '\\' && p + 1 < e) p++;
					if (*p) val[n++] = *p;
					p++;
				}
				if (p < e) p++;
			} else {
				/* unquoted values are restricted; quoted ones aren't */
				while (p < e && *p != ' ' && *p != '}') {
					if (!IS_NAME_CHAR(*p)) goto fail;
					val[n++] = *p;
					p++;
				}
				if (n == 0) goto fail; /* key= with no value */
			}
			val[n] = '\0';
			attr_put(&out, &cap, name, val, ATTR_SET);
		} else {
			goto fail;
		}
	}
	free(name); free(val);
	*outp = out;
	return out[0] != '\0';
fail:
	free(out); free(name); free(val);
	*outp = NULL;
	return 0;
}

/* Attribute content (between the braces) that is nothing but a `%..%`
 * comment. parse_attrs reports these as failures since they yield no
 * attrs, so callers that must consume them check here instead. */
static int
attrs_comment_only(const char *b, const char *e)
{
	const char *p = b;
	while (p < e && (*p == ' ' || *p == '\t' || *p == '\n')) p++;
	if (p >= e || *p != '%') return 0;
	p++;
	while (p < e && *p != '%') p++;
	if (p >= e) return 0;
	p++;
	while (p < e && (*p == ' ' || *p == '\t' || *p == '\n')) p++;
	return p >= e;
}

static void
oputs_attr(const char *s)
{
	for (; *s; s++) {
		if (*s == '&')      oputs("&amp;");
		else if (*s == '<') oputs("&lt;");
		else if (*s == '>') oputs("&gt;");
		else if (*s == '"') oputs("&quot;");
		else                oputc(*s);
	}
}

/* Emit every attribute but skip, which may be NULL. Headings emit their
 * id separately: it goes on the enclosing <section>, or is prepended to
 * the tag when it was auto-generated. */
static void
attr_emit_except(const char *a, const char *skip)
{
	const char *p = a;
	while (p && *p) {
		const char *v = p + strlen(p) + 1;
		if (!skip || strcmp(p, skip)) {
			oputc(' ');
			oputs(p);
			oputs("=\"");
			oputs_attr(v);
			oputc('"');
		}
		p = v + strlen(v) + 1;
	}
}

static void
attr_emit(const char *a)
{
	attr_emit_except(a, NULL);
}

/* Scan for trailing inline attrs {…} starting at p.
 * Returns pointer past '}' if found, or p if not. */
static const char *
scan_inline_attrs(const char *p, const char *e, char **sa)
{
	const char *ab, *ae;
	*sa = NULL;
	if (p >= e || *p != '{') return p;
	ab = p + 1;
	ae = ab;
	while (ae < e && *ae != '}') {
		if (*ae == '\\' && ae + 1 < e) { ae += 2; continue; }
		if (*ae == '"') {
			ae++;
			while (ae < e && *ae != '"') {
				if (*ae == '\\' && ae + 1 < e) ae += 2;
				else ae++;
			}
			if (ae < e) ae++;
		} else ae++;
	}
	if (ae >= e || *ae != '}') return p;
	parse_attrs(ab, ae, sa);
	if (!*sa) return p; /* invalid syntax: leave {...} as literal */
	return ae + 1;
}

static void
emit_fence_lines(const char *b, const char *e, int indent)
{
	const char *q;
	for (q = b; q < e; ) {
		const char *le = eol(q, e);
		int strip = spaces(q, le);
		if (strip > indent) strip = indent;
		hprint(q + strip, le);
		q = le;
	}
}

static void
emit_code_open(const char *info, const char *infoend)
{
	oputs("<pre");
	emit_pending();
	if (info < infoend) {
		oputs("><code class=\"language-");
		hprint(info, infoend);
		oputs("\">");
	} else {
		oputs("><code>");
	}
}

/* check previous line for {attrs} block */
static void
prev_line_attrs(const char *line, const char *start, char **sa)
{
	const char *prev, *pp, *pe;
	if (line <= start) return;
	prev = line - 1;
	while (prev > start && prev[-1] != '\n') prev--;
	pp = prev;
	while (pp < line && (*pp == ' ' || *pp == '\t')) pp++;
	if (pp >= line || *pp != '{') return;
	pe = pp + 1;
	while (pe < line && *pe != '}') pe++;
	if (pe < line && *pe == '}')
		parse_attrs(pp + 1, pe, sa);
}

/* Find or insert key in id_ht; returns pointer to entry */
static int
id_intern(const char *s)
{
	unsigned h;
	int i;
	/* grow at 50% load */
	if (id_ht_cnt * 2 >= id_ht_sz) {
		int oldsz = id_ht_sz, j;
		struct id_entry *old = id_ht;
		id_ht_sz = oldsz ? oldsz * 2 : 64;
		id_ht = calloc(id_ht_sz, sizeof(*id_ht));
		if (!id_ht) die("malloc");
		for (j = 0; j < oldsz; j++) {
			if (!old[j].key) continue;
			for (h = 0, i = 0; old[j].key[i]; i++)
				h = (h * 16777619u) ^ (unsigned char)old[j].key[i];
			i = h & (id_ht_sz - 1);
			while (id_ht[i].key) i = (i + 1) & (id_ht_sz - 1);
			id_ht[i] = old[j];
		}
		free(old);
	}
	for (h = 0, i = 0; s[i]; i++)
		h = (h * 16777619u) ^ (unsigned char)s[i];
	i = h & (id_ht_sz - 1);
	while (id_ht[i].key) {
		if (strcmp(id_ht[i].key, s) == 0) return i;
		i = (i + 1) & (id_ht_sz - 1);
	}
	{
		char *k = malloc(strlen(s) + 1);
		if (!k) die("malloc");
		id_ht[i].key = strcpy(k, s);
	}
	id_ht[i].count = 0;
	id_ht_cnt++;
	return i;
}

static void
dedup_id(char *id, int sz)
{
	int idx = id_intern(id);
	if (id_ht[idx].count++ > 0)
		snprintf(id, sz, "%s-%d", id_ht[idx].key, id_ht[idx].count - 1);
}

static void
clear_pending(void)
{
	pending_attrs[0] = '\0';
}

static int
has_pending(void)
{
	return pending_attrs[0] != '\0';
}

static void
emit_pending(void)
{
	if (has_pending()) {
		attr_emit(pending_attrs);
		clear_pending();
	}
}

/* find next unescaped | that's not inside a backtick span */
static const char *
next_pipe(const char *p, const char *e)
{
	while (p < e) {
		if (*p == '\\' && p + 1 < e) { p += 2; continue; }
		if (*p == '`') {
			int cnt = leadc(p, e, '`');
			const char *q = p + cnt;
			while (q < e) {
				if (*q == '`' && leadc(q, e, '`') == cnt)
					{ p = q + cnt; goto found; }
				q++;
			}
			p = q;
			continue;
		found:
			continue;
		}
		if (*p == '|') return p;
		if (*p == '\n') return NULL;
		p++;
	}
	return NULL;
}

/* check if a line is a separator row (e.g. |:--|---:|) */
static int
is_sep_row(const char *b, const char *e, int **aligns, int *cap_al, int *ncols)
{
	const char *p = b;
	int n = 0;
	if (p >= e || *p != '|') return 0;
	p++;
	/* no leading space allowed after the opening | (djot.js rule) */
	while (p < e && *p != '\n') {
		int left = 0, right = 0;
		if (p < e && *p == ':') { left = 1; p++; }
		if (p >= e || *p != '-') return 0;
		while (p < e && *p == '-') p++;
		if (p < e && *p == ':') { right = 1; p++; }
		while (p < e && *p == ' ') p++;
		GROWA(*aligns, n, *cap_al);
		(*aligns)[n] = left && right ? 3 : left ? 1 : right ? 2 : 0;
		n++;
		if (p >= e || *p != '|') break;
		p++;
		/* optional space after | for next cell */
		while (p < e && *p == ' ') p++;
	}
	*ncols = n;
	return n > 0;
}

static void
emit_cell(const char *b, const char *ce, int is_header, int align)
{
	const char *tag = is_header ? "th" : "td";
	const char *astyle[] = { "", " style=\"text-align: left;\"",
	    " style=\"text-align: right;\"", " style=\"text-align: center;\"" };
	oprintf("<%s%s>", tag, astyle[align & 3]);
	/* trim spaces */
	while (b < ce && *b == ' ') b++;
	while (ce > b && ce[-1] == ' ') ce--;
	process(b, ce, 0);
	oprintf("</%s>\n", tag);
}

static int
dotable(const char *b, const char *e, int n)
{
	const char *p, *line, *cap;
	int *aligns = NULL, naligns = 0, cap_al = 0;
	int is_header = 0;

	if (!n) return 0;
	p = b;
	while (p < e && *p == ' ') p++;
	if (p >= e || *p != '|') return 0;

	/* verify it's a table row (not just | in text) — need at least | cell | */
	{
		const char *le = eol(p, e);
		const char *q = p + 1;
		/* skip to next pipe, respecting escapes and code spans */
		const char *np = next_pipe(q, le);
		if (!np) return 0; /* no closing pipe — not a table */
	}

	oputs("<table>\n");

	/* scan ahead for caption (blank line then ^ after table rows) */
	{
		const char *tl = b;
		while (tl < e) {
			const char *tp = tl;
			while (tp < e && *tp == ' ') tp++;
			if (tp >= e || *tp != '|') break;
			tl = eol(tl, e);
		}
		cap = tl;
		if (cap < e && isblankline(cap, eol(cap, e))) {
			const char *cl = eol(cap, e);
			if (cl < e && *cl == '^') {
				cl++;
				if (cl < e && *cl == ' ') cl++;
				const char *capstart = cl;
				const char *capend = eol(cl, e);
				while (capend < e && !isblankline(capend, eol(capend, e)))
					capend = eol(capend, e);
				const char *ce = trim_end(capstart, capend);
				oputs("<caption>");
				process(capstart, ce, 0);
				oputs("</caption>\n");
				cap = capend; /* remember where caption ends */
			}
		}
	}

	line = b;
	while (line < e) {
		p = line;
		while (p < e && *p == ' ') p++;
		if (p >= e || *p != '|') break;

		/* check if this line is a separator row */
		{
			int sep_ncols;
			if (is_sep_row(p, eol(p, e), &aligns, &cap_al, &sep_ncols)) {
				naligns = sep_ncols;
				is_header = 0;
				line = eol(line, e);
				continue;
			}
		}

		/* check if NEXT line is a separator row → this row is a header,
		 * and pre-load alignment from that separator */
		{
			const char *nextl = eol(line, e);
			if (nextl < e) {
				const char *np = nextl;
				while (np < e && *np == ' ') np++;
				int sn;
				if (np < e && *np == '|' && is_sep_row(np, eol(np, e), &aligns, &cap_al, &sn)) {
					is_header = 1;
					naligns = sn;
				}
			}
		}

		/* parse cells */
		oputs("<tr>\n");
		p++; /* skip leading | */
		{
			int col = 0;
			while (p < eol(line, e)) {
				const char *cellstart = p;
				const char *np = next_pipe(p, eol(line, e));
				if (!np) break;
				/* check if this is the trailing | (nothing after it but whitespace/newline) */
				{
					const char *after = np + 1;
					while (after < eol(line, e) && (*after == ' ' || *after == '\t')) after++;
					if (after >= eol(line, e) || *after == '\n') {
						/* trailing pipe — don't emit this as a cell if empty */
						break;
					}
				}
				{
					int al = (col < naligns) ? aligns[col] : 0;
					emit_cell(cellstart, np, is_header, al);
				}
				p = np + 1;
				col++;
			}
			/* last cell: from current position to end-of-line (before trailing |) */
			{
				const char *le = eol(line, e);
				const char *cellend = le;
				if (cellend > p && cellend[-1] == '\n') cellend--;
				while (cellend > p && cellend[-1] == ' ') cellend--;
				if (cellend > p && cellend[-1] == '|') cellend--;
				if (cellend > p || p < le) {
					int al = (col < naligns) ? aligns[col] : 0;
					emit_cell(p, cellend, is_header, al);
				}
			}
		}
		oputs("</tr>\n");
		is_header = 0;
		line = eol(line, e);
	}

	/* advance past caption if we emitted one */
	if (cap > line) line = cap;

	oputs("</table>\n");
	free(aligns);
	return -(line - b);
}

static int
dodeflist(const char *b, const char *e, int n)
{
	const char *p, *line;
	char *buf;
	int i;

	if (!n) return 0;
	p = b;
	if (p >= e || *p != ':') return 0;
	if (p + 1 >= e || (p[1] != ' ' && p[1] != '\n')) return 0;

	oputs("<dl");
	emit_pending();
	oputs(">\n");

	line = b;
	while (line < e) {
		p = line;
		if (*p != ':' || (p + 1 < e && p[1] != ' ' && p[1] != '\n'))
			break;
		p++;
		if (p < e && *p == ' ') p++;

		/* collect term: first para, continuation lines indented by 1+ space
		 * If the term line starts a code fence, treat term as empty
		 * and include the fence in the definition */
		buf = NULL;
		i = 0;
		int term_is_fence = 0;
		{
			const char *tp = p;
			char fc = (tp < e) ? *tp : '\0';
			if ((fc == '`' || fc == '~') && leadc(tp, eol(line, e), fc) >= 3)
				term_is_fence = 1;
			if (!term_is_fence) {
				const char *le = trim_end(p, eol(line, e));
				PUSHRANGE(buf, i, p, le - p);
			}
		}
		line = eol(line, e);
		while (!term_is_fence && line < e && !isblankline(line, eol(line, e))) {
			p = line;
			int sp = spaces(p, e);
			if (sp < 1) break;
			if (*p == ':' && p + 1 < e && (p[1] == ' ' || p[1] == '\n'))
				break;
			p += (sp > 1) ? 1 : sp;
			if (i > 0) PUSH(buf, i, '\n');
			const char *le = trim_end(p, eol(line, e));
			PUSHRANGE(buf, i, p, le - p);
			line = eol(line, e);
		}
		ADDC(buf, i) = '\0';
		oputs("<dt>");
		process(buf, buf + i, 0);
		oputs("</dt>\n");
		free(buf);

		/* skip blank lines */
		if (!term_is_fence)
			while (line < e && isblankline(line, eol(line, e)))
				line = eol(line, e);

		/* collect definition: indented content (2+ spaces) */
		buf = NULL;
		i = 0;
		/* if term was a fence, include the fence line in definition */
		if (term_is_fence) {
			const char *fle = eol(p, e);
			PUSHRANGE(buf, i, p, fle - p);
		}
		while (line < e) {
			if (isblankline(line, eol(line, e))) {
				PUSH(buf, i, '\n');
				line = eol(line, e);
				continue;
			}
			int sp = spaces(line, e);
			if (sp < 2) break;
			{
				const char *le = eol(line, e);
				int strip = (sp > 2) ? 2 : sp;
				PUSHRANGE(buf, i, line + strip, le - (line + strip));
			}
			line = eol(line, e);
		}
		while (i > 0 && buf[i-1] == '\n') i--;
		ADDC(buf, i) = '\0';

		{
			int save = in_container;
			in_container = 1;
			oputs("<dd>\n");
			if (i > 0)
				process(buf, buf + i, 1);
			oputs("</dd>\n");
			in_container = save;
		}
		free(buf);

		/* skip blank lines between items */
		while (line < e && isblankline(line, eol(line, e)))
			line = eol(line, e);
	}

	oputs("</dl>\n");
	return -(line - b);
}

static int
dodiv(const char *b, const char *e, int n)
{
	const char *p, *q, *line, *cls, *clsend;
	int sp, flen;
	char *buf;
	int i;

	if (!n) return 0;
	p = b;
	sp = spaces(p, e);
	if (sp > 3) return 0;
	p += sp;
	if (p >= e || *p != ':') return 0;
	flen = leadc(p, e, ':');
	if (flen < 3) return 0;
	p += flen;
	while (p < e && (*p == ' ' || *p == '\t')) p++;
	cls = p;
	while (p < e && *p != '\n' && *p != ' ') p++;
	clsend = p;
	while (p < e && *p != '\n') p++;
	if (p < e) p++; /* skip \n */

	/* collect content until closing fence, skipping code fences */
	buf = NULL;
	i = 0;
	line = p;
	{
	int in_code = 0;
	int code_flen = 0;
	char code_fch = 0;
	while (line < e) {
		q = line;
		int s = spaces(q, e);
		q += s;
		/* track code fence state */
		if (q < e && (*q == '`' || *q == '~') && leadc(q, e, *q) >= 3) {
			char cfch = *q;
			int cflen = leadc(q, e, cfch);
			if (!in_code) {
				in_code = 1; code_flen = cflen; code_fch = cfch;
			} else if (cfch == code_fch && cflen >= code_flen) {
				in_code = 0;
			}
		}
		if (!in_code) {
			int cl = leadc(q, e, ':');
			if (cl >= flen && isblankline(q + cl, eol(line, e))) {
				line = eol(line, e);
				goto done;
			}
		}
		{
			const char *le = eol(line, e);
			PUSHRANGE(buf, i, line, le - line);
		}
		line = eol(line, e);
	}
	}
done:
	ADDC(buf, i) = '\0';
	{
		int save = in_container;
		in_container = 1;
		/* merge div class name into pending_class */
		if (cls < clsend) {
			char *cbuf = malloc(clsend - cls + 1);
			if (!cbuf) die("malloc");
			memcpy(cbuf, cls, clsend - cls);
			cbuf[clsend - cls] = '\0';
			attr_put(&pending_attrs, &cap_pattr, "class", cbuf,
			    ATTR_APPEND);
			free(cbuf);
		}
		oputs("<div");
		attr_emit(pending_attrs);
		oputs(">\n");
		clear_pending();
		process(buf, buf + i, 1);
		oputs("</div>\n");
		in_container = save;
	}
	free(buf);
	return -(line - b);
}

static int
doattr(const char *b, const char *e, int n)
{
	const char *p, *q;

	if (!n) return 0;
	p = b;
	while (p < e && (*p == ' ' || *p == '\t')) p++;
	if (p >= e || *p != '{') return 0;
	/* find matching } — may span multiple lines, but continuation
	 * lines must be indented beyond the opening { (djot.js rule). */
	{
		int open_col = p - b; /* indentation of opening { */
		q = p + 1;
		while (q < e && *q != '}') {
			if (*q == '\n') {
				const char *nl = q + 1;
				int nsp = 0;
				while (nl < e && *nl == ' ') { nl++; nsp++; }
				if (nsp <= open_col || nl >= e || *nl == '\n')
					return 0;
				q = nl;
				continue;
			}
			q++;
		}
	}
	if (q >= e || *q != '}') return 0;
	{
		const char *r = q + 1;
		while (r < e && (*r == ' ' || *r == '\t')) r++;
		if (r < e && *r != '\n') return 0;
	}
	/* first non-space must be #, ., %, or alpha (key=val) */
	{
		const char *fc = p + 1;
		while (fc < q && (*fc == ' ' || *fc == '\t')) fc++;
		if (fc >= q) {
			/* empty attrs {} — consume silently */
			return -(eol(b, e) - b);
		}
		if (*fc != '#' && *fc != '.' && *fc != '%'
		    && !isalpha((unsigned char)*fc))
			return 0;
	}
	{
		char *ta;
		/* pure comment block — consume the line(s) */
		if (attrs_comment_only(p + 1, q))
			return -(eol(q, e) - b);
		if (!parse_attrs(p + 1, q, &ta)) {
			free(ta);
			return 0;
		}
		attr_merge(&pending_attrs, &cap_pattr, ta);
		free(ta);
	}
	/* consume up to and including the line containing } */
	return -(eol(q, e) - b);
}

static int
dorefdef(const char *b, const char *e, int n)
{
	const char *p, *line;
	int is_footnote;

	if (!n) return 0;
	p = b;
	while (p < e && *p == ' ') p++;
	if (p >= e || *p != '[') return 0;
	p++;
	is_footnote = (p < e && *p == '^');
	while (p < e && *p != ']' && *p != '\n') p++;
	if (p >= e || *p != ']') return 0;
	p++;
	if (p >= e || *p != ':') return 0;
	p++;
	/* reject if URL chunk on this line has internal whitespace
	 * (spec: "None of the chunks of the URL may contain internal
	 * whitespace") */
	if (!is_footnote) {
		const char *up = p;
		while (up < e && (*up == ' ' || *up == '\t')) up++;
		while (up < e && *up != ' ' && *up != '\t' && *up != '\n') up++;
		while (up < e && (*up == ' ' || *up == '\t')) up++;
		if (up < e && *up != '\n') return 0;
	}
	clear_pending();
	line = eol(b, e);
	if (is_footnote) {
		/* footnote def: continuation is blank or indented by 2+ */
		while (line < e) {
			if (isblankline(line, eol(line, e))) {
				line = eol(line, e);
				continue;
			}
			int sp = spaces(line, e);
			if (sp < 2) break;
			line = eol(line, e);
		}
	} else {
		while (line < e) {
			int sp = spaces(line, e);
			if (sp == 0 || isblankline(line, eol(line, e)))
				break;
			line = eol(line, e);
		}
	}
	return -(line - b);
}

static int
doheading(const char *b, const char *e, int n)
{
	const char *p, *q, *content, *cend, *line;
	int sp, level, hl;
	char *buf;
	int blen;

	if (!n) return 0;
	p = b;
	sp = spaces(p, e);
	if (sp > 3) return 0;
	p += sp;
	level = leadc(p, e, '#');
	if (level < 1 || level > 6) return 0;
	p += level;
	if (p < e && *p != ' ' && *p != '\n') return 0;
	if (p < e && *p == ' ') p++;
	content = p;
	cend = trim_end(content, eol(p, e));

	buf = NULL;
	blen = 0;
	while (content < cend && (*content == ' ' || *content == '\t'
	    || *content == '\n' || *content == '\r'))
		content++;
	PUSHRANGE(buf, blen, content, cend - content);
	line = eol(b, e);
	while (line < e && !isblankline(line, eol(line, e))) {
		const char *lp = line;
		int lsp = spaces(lp, e);
		lp += lsp;
		hl = leadc(lp, e, '#');
		if (hl >= 1 && hl <= 6 && hl != level && lp + hl < e
		    && (lp[hl] == ' ' || lp[hl] == '\n'))
			break; /* different heading level */
		if (hl == level && lp + hl < e && (lp[hl] == ' ' || lp[hl] == '\n')) {
			/* same level: strip prefix */
			lp += hl;
			if (lp < e && *lp == ' ') lp++;
		} else {
			lp = line;
		}
		q = trim_end(lp, eol(line, e));
		if (q > lp) {
			if (blen > 0) PUSH(buf, blen, '\n');
			PUSHRANGE(buf, blen, lp, q - lp);
		}
		line = eol(line, e);
	}
	while (blen > 0 && isws(buf[blen-1]))
		blen--;
	ADDC(buf, blen) = '\0';

	{
		char hid[256];
		const char *pid = attr_find(pending_attrs, "id");
		if (pid && pid[0]) {
			snprintf(hid, sizeof(hid) - 12, "%s", pid);
		} else if (blen > 0) {
			make_slug(buf, blen, hid, sizeof(hid) - 12);
		} else {
			snprintf(hid, sizeof(hid), "s-%d", nsections + 1);
		}
		dedup_id(hid, sizeof(hid));

		if (!in_container) {
			close_sections(level);
			oprintf("<section id=\"%s\">\n", hid);
			if (nsections < 6) sections[nsections++] = level;
		}
		oprintf("<h%d", level);
		if (in_container && pid && pid[0]) {
			/* an explicit id keeps its declaration position */
			attr_put(&pending_attrs, &cap_pattr, "id", hid,
			    ATTR_SET);
			attr_emit(pending_attrs);
		} else {
			if (in_container)
				oprintf(" id=\"%s\"", hid);
			attr_emit_except(pending_attrs, "id");
		}
		oputc('>');
		clear_pending();
	}
	process(buf, buf + blen, 0);
	oprintf("</h%d>\n", level);
	free(buf);
	return -(line - b);
}

static int
doblockquote(const char *b, const char *e, int n)
{
	const char *p, *q, *line;
	char *buf;
	int i, sp;

	if (!n) return 0;
	p = b;
	sp = spaces(p, e);
	if (sp > 3) return 0;
	p += sp;
	if (p >= e || *p != '>') return 0;
	p++;
	if (p < e && *p != ' ' && *p != '\n') return 0;

	buf = NULL;
	i = 0;
	line = b;
	{
		int in_para = 0;
		while (line < e) {
			p = line;
			sp = spaces(p, e);
			p += sp;
			if (p < e && *p == '>'
			    && (p + 1 >= e || p[1] == ' ' || p[1] == '\n')) {
				p++;
				if (p < e && *p == ' ') p++;
				q = eol(line, e);
				int blank = isblankline(p, q);
				PUSHRANGE(buf, i, p, q - p);
				in_para = !blank;
			} else if (isblankline(line, eol(line, e))) {
				break;
			} else if (in_para) {
				/* lazy continuation */
				q = eol(line, e);
				PUSHRANGE(buf, i, line, q - line);
			} else {
				break;
			}
			line = eol(line, e);
		}
	}
	ADDC(buf, i) = '\0';
	{
		int save = in_container;
		in_container = 1;
		oputs("<blockquote");
		emit_pending();
		oputs(">\n");
		process(buf, buf + i, 1);
		oputs("</blockquote>\n");
		in_container = save;
	}
	free(buf);
	return -(line - b);
}

static int
docodefence(const char *b, const char *e, int n)
{
	const char *p, *q, *info, *infoend, *line;
	int sp, flen, indent;
	char fch;

	if (!n) return 0;
	p = b;
	sp = spaces(p, e);
	if (sp > 3) return 0;
	indent = sp;
	p += sp;
	fch = *p;
	if (fch != '`' && fch != '~') return 0;
	flen = leadc(p, e, fch);
	if (flen < 3) return 0;
	p += flen;
	while (p < e && (*p == ' ' || *p == '\t')) p++;
	info = p;
	while (p < e && *p != '\n') p++;
	infoend = trim_end(info, p);
	if (fch == '`') {
		for (q = info; q < infoend; q++)
			if (*q == '`') return 0;
		/* backtick fences: spaces in info string only allowed for raw (=fmt) */
		if (!(info < infoend && *info == '=')) {
			for (q = info; q < infoend; q++)
				if (*q == ' ') return 0;
		}
	}
	if (p < e) p++; /* skip \n */

	{
		int is_raw = (info < infoend && *info == '=');
		const char *rawfmt = info + 1;
		int rawfmtlen = infoend - rawfmt;
		/* strip spaces from raw format */
		while (rawfmtlen > 0 && rawfmt[rawfmtlen-1] == ' ') rawfmtlen--;

		line = p;
		while (line < e) {
			q = line;
			int s = spaces(q, e);
			q += s;
			int cl = leadc(q, e, fch);
			if (cl >= flen && isblankline(q + cl, eol(line, e))) {
				if (is_raw && rawfmtlen == 4
				    && !memcmp(rawfmt, "html", 4)) {
					owrite(p, line - p);
				} else if (!is_raw) {
					emit_code_open(info, infoend);
					emit_fence_lines(p, line, indent);
					oputs("</code></pre>\n");
				}
				/* other raw formats: silently drop */
				return -(eol(line, e) - b);
			}
			line = eol(line, e);
		}
		/* unclosed: treat rest as code */
		if (is_raw && rawfmtlen == 4 && !memcmp(rawfmt, "html", 4)) {
			owrite(p, e - p);
		} else if (!is_raw) {
			emit_code_open(info, infoend);
			emit_fence_lines(p, e, indent);
			oputs("</code></pre>\n");
		}
	}
	return -(e - b);
}

static int
dothematicbreak(const char *b, const char *e, int n)
{
	const char *p;
	int sp, count;

	if (!n) return 0;
	p = b;
	sp = spaces(p, e);
	if (sp > 3) return 0;
	p += sp;
	if (p >= e || (*p != '*' && *p != '-')) return 0;
	count = 0;
	for (; p < e && *p != '\n'; p++) {
		if (*p == '*' || *p == '-') count++;
		else if (*p != ' ' && *p != '\t') return 0;
	}
	if (count < 3) return 0;
	oputs("<hr");
	emit_pending();
	oputs(">\n");
	return -(eol(b, e) - b);
}

static int
roman_val(const char *b, const char *e, int upper)
{
	static const struct { const char *s; int v; } rtab[] = {
		{"m",1000},{"cm",900},{"d",500},{"cd",400},{"c",100},{"xc",90},
		{"l",50},{"xl",40},{"x",10},{"ix",9},{"v",5},{"iv",4},{"i",1}
	};
	int val = 0, prev = 10000;
	const char *p = b;
	unsigned int i;

	if (p >= e || !isalpha((unsigned char)*p)) return -1;
	while (p < e) {
		int found = 0;
		for (i = 0; i < LEN(rtab); i++) {
			int l = strlen(rtab[i].s);
			if (p + l <= e) {
				int match;
				if (upper) {
					match = (l == 1) ? (toupper((unsigned char)rtab[i].s[0]) == *p)
					    : (toupper((unsigned char)rtab[i].s[0]) == p[0]
					    && toupper((unsigned char)rtab[i].s[1]) == p[1]);
				} else {
					match = !strncmp(p, rtab[i].s, l);
				}
				if (match && rtab[i].v <= prev) {
					val += rtab[i].v;
					prev = rtab[i].v;
					p += l;
					found = 1;
					break;
				}
			}
		}
		if (!found) return -1;
	}
	return val > 0 ? val : -1;
}

/* Returns offset past marker+space, or 0.
 * style: 0=bullet 1=decimal 2=lower-alpha 3=upper-alpha 4=lower-roman 5=upper-roman */
static int
scan_marker(const char *p, const char *e, int *style, int *start,
    char *delim, char *mch)
{
	const char *q, *s = p;
	int num;

	if (p >= e) return 0;

	/* bullet */
	if (*p == '-' || *p == '*' || *p == '+') {
		if (p + 1 >= e || (p[1] != ' ' && p[1] != '\t' && p[1] != '\n'))
			return 0;
		*style = 0; *mch = *p; *delim = *p;
		p++;
		if (p < e && *p == ' ') p++;
		return p - s;
	}

	/* (X) style */
	if (*p == '(') {
		p++;
		q = p;
		while (q < e && *q != ')' && *q != '\n') q++;
		if (q >= e || *q != ')') return 0;
		if (q + 1 >= e || (q[1] != ' ' && q[1] != '\n')) return 0;
		/* try decimal */
		if (isdigit((unsigned char)*p)) {
			num = 0;
			const char *r = p;
			while (r < q && isdigit((unsigned char)*r)) {
				if (num > 99999999) return 0;
				num = num * 10 + (*r++ - '0');
			}
			if (r == q) {
				*style = 1; *start = num; *delim = '(';
				q += 2; /* past ) and space */
				if (q[-1] != ' ' && q[-1] != '\n') q--;
				return q - s;
			}
		}
		/* single letter: alpha first */
		if (isalpha((unsigned char)*p) && q == p + 1) {
			*style = isupper((unsigned char)*p) ? 3 : 2;
			*start = tolower((unsigned char)*p) - 'a' + 1;
			*delim = '(';
			q++;
			if (q < e && *q == ' ') q++;
			return q - s;
		}
		/* multi-letter: try roman */
		num = roman_val(p, q, isupper((unsigned char)*p));
		if (num > 0) {
			*style = isupper((unsigned char)*p) ? 5 : 4;
			*start = num;
			*delim = '(';
			q++;
			if (q < e && *q == ' ') q++;
			return q - s;
		}
		return 0;
	}

	/* decimal: N. or N) */
	if (isdigit((unsigned char)*p)) {
		num = 0;
		while (p < e && isdigit((unsigned char)*p)) {
			if (num > 99999999) return 0;
			num = num * 10 + (*p++ - '0');
		}
		if (p >= e || (*p != '.' && *p != ')')) return 0;
		*delim = *p++;
		if (p >= e || (*p != ' ' && *p != '\n')) return 0;
		if (*p == ' ') p++;
		*style = 1; *start = num;
		return p - s;
	}

	/* alpha or roman: X. or X) */
	if (isalpha((unsigned char)*p)) {
		q = p;
		while (q < e && isalpha((unsigned char)*q)) q++;
		if (q >= e || (*q != '.' && *q != ')')) return 0;
		if (q + 1 >= e || (q[1] != ' ' && q[1] != '\n')) return 0;
		/* single letter: try alpha first */
		if (q == p + 1) {
			*style = isupper((unsigned char)*p) ? 3 : 2;
			*start = tolower((unsigned char)*p) - 'a' + 1;
			*delim = *q++;
			if (q < e && *q == ' ') q++;
			return q - s;
		}
		/* multi-letter: try roman */
		num = roman_val(p, q, isupper((unsigned char)*p));
		if (num > 0) {
			*style = isupper((unsigned char)*p) ? 5 : 4;
			*start = num;
			*delim = *q++;
			if (q < e && *q == ' ') q++;
			return q - s;
		}
		return 0;
	}
	return 0;
}

/* check if item content has a blank line between non-list text blocks */
static int
item_has_inner_blank(const char *buf, int len)
{
	const char *be = buf + len;
	const char *sp2;
	for (sp2 = buf; sp2 < be; ) {
		const char *le = eol(sp2, be);
		if (isblankline(sp2, le)) {
			const char *after = skip_blanks(le, be);
			if (after < be) {
				int st2;
				char d2, m2;
				const char *np = after + spaces(after, be);
				if (!scan_marker(np, be, &st2, &(int){0}, &d2, &m2))
					return 1;
			}
			break;
		}
		sp2 = le;
	}
	return 0;
}

static int
dolist(const char *b, const char *e, int n)
{
	static const char *typattr[] = { "", "", " type=\"a\"", " type=\"A\"",
	    " type=\"i\"", " type=\"I\"" };
	const char *p, *q, *line;
	char *buf;
	int i, sp, indent, loose, had_blank;
	int style, start_num;
	char delim, mch;

	if (!n) return 0;
	p = b;
	sp = spaces(p, e);
	if (sp > 3) return 0;
	p += sp;

	indent = scan_marker(p, e, &style, &start_num, &delim, &mch);
	if (!indent) return 0;
	indent += sp;

	/* disambiguate alpha vs roman: check second item */
	if (style == 2 || style == 3) {
		/* single-letter alpha — could be roman? */
		const char *p2 = p + indent - sp;
		const char *line2;
		int rval = roman_val(p, p + 1, style == 3);
		if (rval > 0) {
			line2 = skip_blanks(eol(b, e), e);
			if (line2 < e) {
				int st2, sn2;
				char d2, m2;
				p2 = line2 + spaces(line2, e);
				if (scan_marker(p2, e, &st2, &sn2, &d2, &m2)) {
					if ((st2 == 4 || st2 == 5) && d2 == delim) {
						/* second item is roman — switch */
						style = (style == 2) ? 4 : 5;
						start_num = rval;
					} else if (st2 == style && d2 == delim) {
						/* second item is also alpha, check if repeated roman */
						int rval2 = roman_val(p2, p2 + 1, style == 3);
						if (rval2 == rval) {
							style = (style == 2) ? 4 : 5;
							start_num = rval;
						}
					}
				}
			}
		}
	}

	/* detect task list: first item starts with [ ] or [x] */
	int is_task = 0;
	if (style == 0) {
		const char *tc = p + indent - sp;
		if (tc + 2 < e && tc[0] == '[' && (tc[1] == ' ' || tc[1] == 'x')
		    && tc[2] == ']' && (tc + 3 >= e || tc[3] == ' ' || tc[3] == '\n'))
			is_task = 1;
	}

	if (style == 0) {
		if (is_task)
			attr_put(&pending_attrs, &cap_pattr, "class",
			    "task-list", ATTR_PREPEND);
		oputs("<ul");
		if (has_pending()) {
			attr_emit(pending_attrs);
			clear_pending();
		}
		oputs(">\n");
	} else {
		oputs("<ol");
		if (start_num != 1) oprintf(" start=\"%d\"", start_num);
		oputs(typattr[style]);
		emit_pending();
		oputs(">\n");
	}

	/* pre-scan: check if list is loose (has blank lines between
	 * same-level items). Must be done before emitting any items
	 * so ALL items get <p> wrapping when loose.
	 * Note: item_has_inner_blank() in the main loop also sets loose
	 * for blanks within an item, which doesn't need pre-scanning
	 * since it affects the item itself and all following items. */
	loose = 0;
	{
		const char *scan = b;
		int prev_at_top = 1; /* was prev non-blank line at top level? */
		while (scan < e && !loose) {
			const char *le = eol(scan, e);
			if (isblankline(scan, le)) {
				const char *nb = skip_blanks(scan, e);
				if (nb >= e) break;
				int lsp = spaces(nb, e);
				/* blank followed by same-level marker, AND
				 * previous content was also at top level →
				 * blank between items → loose.
				 * (blank after deeper content is just a
				 * sub-list ending, not inter-item blank) */
				if (lsp == sp && prev_at_top) {
					int st2; char d2, m2;
					if (scan_marker(nb + lsp, e, &st2,
					    &(int){0}, &d2, &m2))
						loose = 1;
				}
				/* blank followed by indented non-marker
				 * continuation → inner blank → loose
				 * (but not if followed by a sub-list marker,
				 * which is just sub-content) */
				if (lsp > sp && prev_at_top) {
					int st2; char d2, m2;
					if (!scan_marker(nb + lsp, e, &st2,
					    &(int){0}, &d2, &m2))
						loose = 1;
				}
				scan = nb;
				continue;
			}
			/* track whether this line is at top level */
			{
				int lsp = spaces(scan, e);
				if (lsp == sp) {
					int st2; char d2, m2;
					if (!scan_marker(scan + lsp, e, &st2,
					    &(int){0}, &d2, &m2))
						break; /* non-marker: list ended */
					if (style == 0 ?
					    (st2 != 0 || m2 != mch) :
					    (d2 != delim))
						break;
					prev_at_top = 1;
				} else {
					prev_at_top = 0;
				}
			}
			scan = le;
		}
	}
	had_blank = 0;
	line = b;
	{
	int marker_col = sp;
	while (line < e) {
		int item_sp, item_mw;

		/* verify this line starts with the same marker type */
		item_sp = spaces(line, e);
		p = line + item_sp;
		{
			int st2;
			char d2, m2;
			item_mw = scan_marker(p, e, &st2, &(int){0}, &d2, &m2);
			if (!item_mw) break;
			if (style == 0) { if (st2 != 0 || m2 != mch) break; }
			else if (d2 != delim) break;
			else if (st2 != style) {
				int compat = 0;
				if ((style == 4 && st2 == 2) || (style == 5 && st2 == 3)) {
					/* roman list, alpha item: only if char is a roman digit */
					const char *rchars = (style == 5) ? "IVXLCDM" : "ivxlcdm";
					if (strchr(rchars, p[p[0] == '(' ? 1 : 0]))
						compat = 1;
				}
				if ((style == 2 && st2 == 4) || (style == 3 && st2 == 5))
					compat = 1;
				if (!compat) break;
			}
		}
		indent = item_sp + item_mw; /* content column */

		buf = NULL;
		i = 0;
		p = line + indent;
		q = eol(line, e);
		PUSHRANGE(buf, i, p, q - p);
		line = q;

		while (line < e) {
			if (isblankline(line, eol(line, e))) {
				had_blank = 1;
				PUSH(buf, i, '\n');
				line = eol(line, e);
				continue;
			}
			sp = spaces(line, e);
			if (sp > marker_col) {
				int strip;
				if (sp >= indent && !had_blank)
					strip = indent;
				else
					strip = marker_col + 1;
				q = eol(line, e);
				PUSHRANGE(buf, i, line + strip, q - (line + strip));
				line = q;
				continue;
			}
			/* at or before marker column */
			if (sp == marker_col) {
				int st2;
				char d2, m2;
				if (scan_marker(line + sp, e, &st2, &(int){0}, &d2, &m2)) {
					/* sibling marker → new item */
					if (style == 0 && st2 == 0 && m2 == mch) break;
					if (style != 0 && d2 == delim &&
					    (st2 == style ||
					    (style >= 4 && st2 == style - 2) ||
					    (style <= 3 && st2 == style + 2)))
						break;
					/* any other marker without preceding blank → end */
					if (!had_blank) break;
				}
			}
			if (!had_blank) {
				q = eol(line, e);
				{
					int strip = sp;
					if (strip > indent) strip = indent;
					else if (strip > marker_col + 1) strip = marker_col + 1;
					PUSHRANGE(buf, i, line + strip, q - (line + strip));
				}
				line = q;
				continue;
			}
			/* after blank: allow lazy if last content was a sub-list.
			 * A real sub-list is preceded by a blank line in the
			 * buffer; otherwise the marker-looking line is just
			 * indented literal content of the parent item. */
			{
				const char *lb = buf + i;
				/* skip trailing blanks in buffer */
				while (lb > buf && lb[-1] == '\n') lb--;
				/* find start of last line */
				{
					const char *ls = lb;
					while (ls > buf && ls[-1] != '\n') ls--;
					int preceded_by_blank = (ls >= buf + 2
					    && ls[-1] == '\n' && ls[-2] == '\n');
					int st2;
					char d2, m2;
					if (preceded_by_blank && scan_marker(
					    ls + spaces(ls, lb), lb,
					    &st2, &(int){0}, &d2, &m2)) {
						/* last content is sub-list: lazy OK */
						q = eol(line, e);
						PUSHRANGE(buf, i, line, q - line);
						line = q;
						continue;
					}
				}
			}
			break;
		}

		/* blank between items (not after sub-list) → loose */
		{
			int saw_blank = (i >= 2 && buf[i-1] == '\n' && buf[i-2] == '\n');
			if (saw_blank) {
				/* check line before trailing blank: if it's a sub-list marker,
				 * the blank is within sub-content, not between items */
				const char *lb = buf + i - 2;
				while (lb > buf && lb[-1] != '\n') lb--;
				{
					int st2;
					char d2, m2;
					const char *np = lb + spaces(lb, buf + i);
					if (scan_marker(np, buf + i, &st2, &(int){0}, &d2, &m2))
						saw_blank = 0;
				}
			}
			if (saw_blank || (line < e && isblankline(line, eol(line, e)))) {
				const char *lp = skip_blanks(line, e);
				if (lp < e) {
					int st2;
					char d2, m2;
					const char *np = lp + spaces(lp, e);
					if (spaces(lp, e) == marker_col
					    && scan_marker(np, e, &st2, &(int){0}, &d2, &m2))
						loose = 1;
				}
				line = lp;
			}
		}

		while (i > 0 && buf[i-1] == '\n') i--;
		ADDC(buf, i) = '\0';

		{
			int save_tight = tight;
			int save_cont = in_container;
			in_container = 1;
			oputs("<li>\n");
			/* task list checkbox */
			if (is_task && i >= 3 && buf[0] == '['
			    && (buf[1] == ' ' || buf[1] == 'x')
			    && buf[2] == ']') {
				int checked = (buf[1] == 'x');
				oprintf("<input disabled=\"\" type=\"checkbox\"%s/>\n",
				    checked ? " checked=\"\"" : "");
				/* skip [ ] or [x] and trailing space */
				{
					int skip = 3;
					if (skip < i && buf[skip] == ' ') skip++;
					memmove(buf, buf + skip, i - skip + 1);
					i -= skip;
				}
			}
			/* check if item has blank between text blocks → loose */
			if (!loose && item_has_inner_blank(buf, i))
				loose = 1;
			if (!loose) tight = 1;
			/* find first blank line to split inline vs block content */
			{
				const char *bp = buf, *be = buf + i;
				const char *split = NULL;
				const char *sp2;
				for (sp2 = bp; sp2 < be; ) {
					const char *le = eol(sp2, be);
					if (isblankline(sp2, le)) {
						split = sp2;
						break;
					}
					sp2 = le;
				}
				if (split && !loose) {
					const char *pe = trim_end(bp, split);
					while (bp < pe && (*bp == ' ' || *bp == '\t'))
						bp++;
					process(bp, pe, 0);
					oputc('\n');
					/* post-blank: block */
					process(split, be, 1);
				} else {
					process(buf, buf + i, 1);
				}
			}
			tight = save_tight;
			in_container = save_cont;
		}
		oputs("</li>\n");
		free(buf);
		buf = NULL;
		had_blank = 0;
	}
	}

	oputs(style == 0 ? "</ul>\n" : "</ol>\n");
	return -(line - b);
}

static int
doparagraph(const char *b, const char *e, int n)
{
	const char *p, *end, *start;

	if (!n) return 0;
	start = b;
	end = b;
	while (end < e) {
		p = eol(end, e);
		if (isblankline(end, p))
			break;
		end = p;
	}
	while (b < end && (*b == ' ' || *b == '\t')) b++;
	p = trim_end(b, end);

	/* Pre-process: transform word{attrs} into [word]{attrs} so that
	 * dolink's span syntax [text]{attrs} handles inline attributes.
	 * Scans for {…} blocks preceded by a word, inserts [ before the word
	 * and ] before the {. Skips code spans, escapes, and known {X openers
	 * (emphasis/quotes). The transformed buffer is only used if changes
	 * were made; otherwise the original range is processed directly. */
	{
		int ncap = (p - b) + 64;
		char *nbuf = malloc(ncap);
		int nlen = 0;
		const char *s = b;
		int transformed = 0;
		/* Once a `{` scan has confirmed there is no `}` from some position
		 * onward, every later `{` would also fail. Skip the O(N) inner
		 * scan in that case; without this, inputs like `{{{{{{...` are
		 * O(N^2). See fuzz/afl-out hangs. */
		const char *no_close_after = p;

		if (!nbuf) die("malloc");
		while (s < p) {
			if (*s == '\\' && s + 1 < p) {
				GROWBUF(nbuf, nlen, ncap, 2);
				nbuf[nlen++] = *s++;
				nbuf[nlen++] = *s++;
				continue;
			}
			if (*s == '`') {
				/* skip code spans */
				int cnt = leadc(s, p, '`');
				const char *q = s + cnt;
				while (q < p) {
					if (*q == '`' && leadc(q, p, '`') == cnt) {
						q += cnt; break;
					}
					q++;
				}
				while (s < q) {
					GROWBUF(nbuf, nlen, ncap, 1);
					nbuf[nlen++] = *s++;
				}
				continue;
			}
			if (*s == '{' && !(s > b && s[-1] == '\\') && !(s > b && s[-1] == ']')) {
				if (s >= no_close_after) goto copy_char;
				/* skip {attrs} after inline closers — parsers handle them */
				if (s > b && (s[-1] == '`' || s[-1] == '>'
				    || s[-1] == '*' || s[-1] == '_'
				    || s[-1] == '~' || s[-1] == '^'))
					goto copy_char;
				/* skip {attrs} after explicit surround closers +} -} =} */
				if (s > b + 1 && s[-1] == '}'
				    && (s[-2] == '+' || s[-2] == '-' || s[-2] == '='))
					goto copy_char;
				/* skip {attrs} after [text](url) — dolink handles link attrs */
				if (s > b && s[-1] == ')') {
					const char *rp = s - 2;
					int d = 1;
					while (rp >= b && d > 0) {
						if (*rp == ')') d++;
						if (*rp == '(') d--;
						rp--;
					}
					if (rp >= b && *rp == ']') {
						/* found ](...)  before { — skip this {attrs} */
						goto copy_char;
					}
				}
				int preceded_by_word = (s > b && s[-1] != ' ' && s[-1] != '\n'
				    && s[-1] != '\t');
				/* skip known {X openers (emphasis, quotes) */
				if (s + 1 < p && (s[1] == '_' || s[1] == '*' || s[1] == '~'
				    || s[1] == '^' || s[1] == '+' || s[1] == '-' || s[1] == '='
				    || s[1] == '\'' || s[1] == '"'))
					goto copy_char;
				/* find matching } (handles quotes and escapes) */
				{
					const char *q = s + 1;
					while (q < p && *q != '}') {
						if (*q == '\\' && q + 1 < p) q += 2;
						else if (*q == '"') {
							q++;
							while (q < p && *q != '"') {
								if (*q == '\\' && q + 1 < p) q += 2;
								else q++;
							}
							if (q < p) q++;
						} else q++;
					}
					if (q < p && *q == '}') {
						/* validate attrs */
						char *ta;
						/* empty {}, or a comment carrying no attrs. A
						 * comment at the very start is left to doreplace:
						 * a multi-line one there was already rejected as a
						 * block attribute and stays literal (djot.js). */
						int empty = (q == s + 1)
						    || (s > b && attrs_comment_only(s + 1, q));
						int valid = parse_attrs(s + 1, q, &ta) || empty;
						free(ta);
						if (valid) {
							if (empty) {
								/* nothing to attach — consume silently */
								s = q + 1;
								transformed = 1;
								continue;
							}
							if (preceded_by_word) {
								/* find word start — stop at space, tags, quotes,
								 * and emphasis delimiters if they have a matching closer */
								int stop_at_emph = 0;
								{
									const char *r = q + 1;
									while (r < p && (*r == ' ' || *r == '\t')) r++;
									if (r < p && (*r == '*' || *r == '_'))
										stop_at_emph = 1;
								}
								int wstart = nlen;
								while (wstart > 0
								    && nbuf[wstart-1] != ' '
								    && nbuf[wstart-1] != '\n'
								    && nbuf[wstart-1] != '\t'
								    && nbuf[wstart-1] != '>'
								    && nbuf[wstart-1] != '"'
								    && !(stop_at_emph && (nbuf[wstart-1] == '*'
								        || nbuf[wstart-1] == '_')))
									wstart--;
								/* insert [ before word */
								GROWBUF(nbuf, nlen, ncap, 2);
								memmove(nbuf + wstart + 1, nbuf + wstart, nlen - wstart);
								nbuf[wstart] = '[';
								nlen++;
								/* add ] before {attrs} */
								GROWBUF(nbuf, nlen, ncap, 1);
								nbuf[nlen++] = ']';
								/* copy {attrs}, escaping unescaped * and _ in quoted values */
								while (s <= q) {
									if (*s == '"' && s > b && s < q) {
										GROWBUF(nbuf, nlen, ncap, 1);
										nbuf[nlen++] = *s++;
										while (s < q && *s != '"') {
											if (*s == '\\' && s + 1 < q) {
												GROWBUF(nbuf, nlen, ncap, 2);
												nbuf[nlen++] = *s++;
												nbuf[nlen++] = *s++;
												continue;
											}
											if (*s == '*' || *s == '_') {
												GROWBUF(nbuf, nlen, ncap, 2);
												nbuf[nlen++] = '\\';
											}
											GROWBUF(nbuf, nlen, ncap, 1);
											nbuf[nlen++] = *s++;
										}
									}
									GROWBUF(nbuf, nlen, ncap, 1);
									nbuf[nlen++] = *s++;
								}
							} else {
								/* attrs preceded by space/start — consume silently */
								s = q + 1;
							}
							transformed = 1;
							continue;
						}
						/* not valid attrs — copy {..} literally to prevent
						 * smart replacement of contents (e.g. -- in {1--}) */
						while (s <= q) {
							GROWBUF(nbuf, nlen, ncap, 1);
							nbuf[nlen++] = *s++;
						}
						transformed = 1;
						continue;
					}
					/* no closing `}` in [s+1, p): cache so later `{`s skip */
					no_close_after = s;
				}
			}
		copy_char:
			GROWBUF(nbuf, nlen, ncap, 1);
			nbuf[nlen++] = *s++;
		}

		if (!tight) {
			oputs("<p");
			if (has_pending()) {
				const char *pid = attr_find(pending_attrs, "id");
				if (pid && pid[0]) {
					char idbuf[256];
					snprintf(idbuf, sizeof(idbuf), "%s", pid);
					dedup_id(idbuf, sizeof(idbuf));
					attr_put(&pending_attrs, &cap_pattr,
					    "id", idbuf, ATTR_SET);
				}
				attr_emit(pending_attrs);
			}
			clear_pending();
			oputc('>');
		}
		if (transformed) {
			process(nbuf, nbuf + nlen, 0);
		} else {
			process(b, p, 0);
		}
		free(nbuf);
	}
	if (!tight) oputs("</p>");
	oputc('\n');
	return -(end - start);
}

static int
dolinebreak(const char *b, const char *e, int n)
{
	const char *p;

	if (n) return 0;
	if (*b == '\\') {
		p = b + 1;
		while (p < e && (*p == ' ' || *p == '\t')) p++;
		if (p < e && (*p == '\n' || *p == '\r')) {
			if (*p == '\r' && p + 1 < e && p[1] == '\n') p++;
			oputs("<br>\n");
			return p + 1 - b;
		}
		/* trailing backslash at end of paragraph content */
		if (p >= e) {
			oputs("<br>\n");
			return p - b;
		}
		if (b + 1 < e && b[1] == ' ') {
			oputs("&nbsp;");
			return 2;
		}
		if (b + 1 < e && isasciipunct(b[1])) {
			hprint(b + 1, b + 2);
			return 2;
		}
		return 0;
	}
	if (*b == '\n') {
		const char *q = b + 1;
		while (q < e && (*q == ' ' || *q == '\t')) q++;
		oputc('\n');
		return q - b;
	}
	return 0;
}

static int
docode(const char *b, const char *e, int n)
{
	const char *p, *code, *start;
	int count, run, codelen;
	int math = 0; /* 0=none, 1=inline, 2=display */

	if (n) return 0;

	start = b;
	/* check for math prefix: $$ or $ before backtick */
	if (*b == '$') {
		if (b + 1 < e && b[1] == '$') {
			if (b + 2 < e && b[2] == '`') { math = 2; b += 2; }
			else return 0;
		} else if (b + 1 < e && b[1] == '`') {
			math = 1; b += 1;
		} else {
			return 0;
		}
	}

	if (*b != '`') return 0;
	count = leadc(b, e, '`');
	p = b + count;
	while (p < e) {
		if (*p != '`') { p++; continue; }
		run = leadc(p, e, '`');
		if (run == count) {
			const char *after = p + count;
			code = b + count;
			codelen = p - code;
			/* trim single space from each end if content
			 * starts or ends with a backtick */
			if (codelen >= 2
			    && code[0] == ' ' && code[codelen-1] == ' '
			    && (code[1] == '`' || code[codelen-2] == '`')) {
				code++;
				codelen -= 2;
			}
			/* check for raw suffix {=format} — format must have no spaces */
			if (!math && after < e && *after == '{' && after + 1 < e
			    && after[1] == '=') {
				const char *fe = after + 2;
				while (fe < e && *fe != '}' && *fe != '\n'
				    && *fe != ' ') fe++;
				if (fe < e && *fe == '}') {
					const char *fmt = after + 2;
					int fmtlen = fe - fmt;
					if (fmtlen == 4 && !memcmp(fmt, "html", 4)) {
						/* raw html: output unescaped */
						owrite(code, codelen);
					}
					/* other formats: silently drop */
					return fe + 1 - start;
				}
			}
			if (math) {
				oprintf("<span class=\"math %s\">%s",
				    math == 1 ? "inline" : "display",
				    math == 1 ? "\\(" : "\\[");
				hprint(code, code + codelen);
				oputs(math == 1 ? "\\)" : "\\]");
				oputs("</span>");
				return p + count - start;
			} else {
				char *sa = NULL;
				const char *aend = after;
				/* parse inline attrs unless {=format} attempt */
				if (!(after < e && *after == '{' && after + 1 < e
				    && after[1] == '='))
					aend = scan_inline_attrs(after, e, &sa);
				oputs("<code");
				attr_emit(sa);
				oputc('>');
				hprint(code, code + codelen);
				oputs("</code>");
				free(sa);
				return aend - start;
			}
		}
		p += run;
	}
	if (math) return 0; /* unclosed math: don't match */
	/* unclosed: implicit close at end */
	code = b + count;
	codelen = e - code;
	while (codelen > 0 && isws(code[codelen-1])) codelen--;
	if (codelen >= 2 && code[0] == ' ' && code[codelen-1] == ' ') {
		code++; codelen -= 2;
	}
	oputs("<code>");
	hprint(code, code + codelen);
	oputs("</code>");
	return e - start;
}

static const struct { char ch; const char *open, *close; } surround_tags[] = {
	{'_', "<em>", "</em>"}, {'*', "<strong>", "</strong>"},
	{'~', "<sub>", "</sub>"}, {'^', "<sup>", "</sup>"},
	{'+', "<ins>", "</ins>"}, {'-', "<del>", "</del>"},
	{'=', "<mark>", "</mark>"},
};

static void
surround_lookup(char ch, const char **otag, const char **ctag)
{
	unsigned int i;
	for (i = 0; i < LEN(surround_tags); i++)
		if (surround_tags[i].ch == ch) {
			*otag = surround_tags[i].open;
			*ctag = surround_tags[i].close;
			return;
		}
	*otag = "<em>"; *ctag = "</em>";
}

static int
dosurround(const char *b, const char *e, int n)
{
	const char *p, *start, *stop;
	char ch, after;
	int explicit_open = 0; /* {_ or {* */
	int consumed_open = 0; /* extra chars consumed for { */

	if (n) return 0;

	/* check for explicit opener {_ {* {~ {^ {+ {- {= */
	if (*b == '{' && b + 1 < e && (b[1] == '_' || b[1] == '*'
	    || b[1] == '~' || b[1] == '^'
	    || b[1] == '+' || b[1] == '-' || b[1] == '=')) {
		ch = b[1];
		explicit_open = 1;
		consumed_open = 1;
		after = (b + 2 < e) ? b[2] : 0;
	} else {
		ch = *b;
		if (ch != '_' && ch != '*' && ch != '~' && ch != '^') return 0;
		/* _} is an explicit closer without opener — skip, will be literal */
		if (b + 1 < e && b[1] == '}') return 0;
		after = (b + 1 < e) ? b[1] : 0;
	}

	/* djot opener rule: character after marker is not whitespace
	 * (explicit openers bypass this rule) */
	if (!explicit_open && isws(after)) return 0;

	/* handle runs: N identical delimiters matched by N closers */
	int run = 0;
	if (!explicit_open) {
		run = leadc(b, e, ch);
		if (run > 1) {
			const char *rp = b + run;
			while (rp + run <= e) {
				if (*rp != ch) { rp++; continue; }
				int crun = leadc(rp, e, ch);
				if (crun >= run) {
					char bb = (rp > b + run) ? rp[-1] : 0;
					if (!isws(bb) && rp > b + run) {
						const char *ot, *ct;
						int j, otlen;
						char *sa;
						const char *end;
						surround_lookup(ch, &ot, &ct);
						end = scan_inline_attrs(rp + run, e, &sa);
						otlen = strlen(ot);
						for (j = 0; j < run; j++) {
							if (j == 0 && end > rp + run) {
								owrite(ot, otlen - 1);
								attr_emit(sa);
								oputc('>');
							} else {
								oputs(ot);
							}
						}
						process(b + run, rp, 0);
						for (j = 0; j < run; j++)
							oputs(ct);
						free(sa);
						return end - b;
					}
				}
				rp += crun;
			}
		}
		/* No `ch` after the run: no closer possible. Emit run as
		 * literal and skip past it; otherwise process() would call
		 * dosurround on each char in the run, scanning to e each
		 * time — O(N^2) for long delim runs. */
		if (b + run >= e || !memchr(b + run, ch, (size_t)(e - b - run))) {
			int j;
			for (j = 0; j < run; j++) oputc(ch);
			return run;
		}
	}

	/* find matching close (single delimiter)
	 * Track inner openers so the closest opener wins when
	 * multiple openers compete for the same closer. */
	start = b + 1 + consumed_open;
	{
	int inner_openers = 0;
	int seen_nondelim = 0;
	for (p = start; p < e; p++) {
		if (*p != ch) seen_nondelim = 1;
		if (*p == '\\' && p + 1 < e) { p++; continue; }
		/* skip explicit openers {_ and closers _} that aren't ours */
		if (*p == '{' && p + 1 < e && p[1] == ch) {
			p++; /* skip the { and the delimiter will be skipped by loop */
			continue;
		}
		if (*p == '`') { /* skip code spans */
			int cnt = leadc(p, e, '`');
			const char *q = p + cnt;
			while (q < e) {
				if (*q != '`') { q++; continue; }
				int r = leadc(q, e, '`');
				if (r == cnt) { p = q + r - 1; break; }
				q += r;
			}
			if (q >= e) break;
			continue;
		}
		/* stop at ]( — emphasis shouldn't span across link syntax */
		if (*p == ']' && p + 1 < e && p[1] == '(')
			break;
		if (*p == '<') { /* skip autolinks */
			const char *q = p + 1;
			while (q < e && *q != '>' && *q != '<' && *q != '\n') q++;
			if (q < e && *q == '>' &&
			    (memchr(p+1, ':', q-p-1) || memchr(p+1, '@', q-p-1))) {
				p = q;
				continue;
			}
		}
		/* skip [text](url) only if url contains our delimiter */
		if (*p == '[') {
			/* No matching `]` at all → cannot form a link, skip the
			 * scans below. Without this, `*[[[[...` (no `]`) is
			 * O(N^2) inside dosurround. */
			{
				ptrdiff_t idx;
				build_bracket_match(proc_base, e);
				idx = p - proc_base;
				if (idx >= 0 && idx < bm_end - bm_base
				    && bm_match[idx] < 0)
					continue;
			}
			/* Quick check: any `](` ahead? If not, this `[` cannot
			 * start a link form — skip the depth walk below. */
			int has_paren_close = 0;
			{
				const char *r = p + 1;
				while ((r = memchr(r, ']', e - r))) {
					if (r + 1 < e && r[1] == '(') {
						has_paren_close = 1; break;
					}
					r++;
				}
			}
			if (!has_paren_close) continue;
			/* find ] and check for (url) */
			int depth = 1;
			const char *q = p + 1;
			int has_delim = 0;
			while (q < e && depth > 0) {
				if (*q == '\\' && q + 1 < e) { q += 2; continue; }
				if (*q == '[') depth++;
				if (*q == ']') depth--;
				if (*q == ch) has_delim = 1;
				q++;
			}
			if (q < e && *q == '(' && has_delim) {
				/* check if link completes — if not, stop scanning */
				int d2 = 1;
				const char *q2 = q + 1;
				while (q2 < e && d2 > 0) {
					if (*q2 == '(') d2++;
					if (*q2 == ')') d2--;
					q2++;
				}
				if (d2 != 0) break; /* incomplete link: stop emphasis scan */
				/* complete link with delim in text: don't skip */
			} else if (q < e && *q == '(' && !has_delim) {
				/* skip the url part only */
				depth = 1; q++;
				while (q < e && depth > 0) {
					if (*q == '(') depth++;
					if (*q == ')') depth--;
					q++;
				}
				p = q - 1;
				continue;
			}
		}
		if (*p == ch) {
			int is_closer = 0, is_opener = 0;
			int explicit_close = (p + 1 < e && p[1] == '}');

			/* explicit openers only match explicit closers */
			if (explicit_open && !explicit_close) continue;
			if (!explicit_open && explicit_close) continue;

			/* check closer validity */
			if (explicit_close) {
				if (p > start) is_closer = 1;
			} else {
				char bb = (p > start) ? p[-1] : 0;
				if (p > start && !isws(bb))
					is_closer = 1;
			}

			/* check opener validity (char after not whitespace) */
			if (!explicit_open) {
				char aa = (p + 1 < e) ? p[1] : 0;
				if (p + 1 < e && !isws(aa))
					is_opener = 1;
			}

			if (p == start) { is_closer = 0; is_opener = 1; }

			if (is_closer && inner_openers > 0) {
				/* a closer opener claims this closer */
				inner_openers--;
				continue;
			}
			if (is_closer) {
				/* reject if content is all delimiter chars */
				if (!explicit_open && !seen_nondelim) continue;
				stop = p;
				{
					const char *otag, *ctag;
					char *sa;
					const char *after_close, *aend;
					surround_lookup(ch, &otag, &ctag);
					after_close = stop + 1 + (explicit_close ? 1 : 0);
					aend = scan_inline_attrs(after_close, e, &sa);
					if (aend > after_close) {
						int otlen = strlen(otag);
						owrite(otag, otlen - 1);
						attr_emit(sa);
						oputc('>');
					} else {
						oputs(otag);
					}
					process(start, stop, 0);
					oputs(ctag);
					free(sa);
					return aend - b;
				}
			}
			if (is_opener)
				inner_openers++;
		}
	}
	}
	/* Single-delim scan failed. For run >= 3 the inner_openers logic
	 * is the same regardless of where in the run we start, so no inner
	 * position can succeed either. Emit run as literal to avoid
	 * O(N^2) repeated scans by process(). (For run<=2 inner positions
	 * may still succeed, so we leave those to the next dosurround.) */
	if (run >= 3) {
		int j;
		for (j = 0; j < run; j++) oputc(ch);
		return run;
	}
	return 0;
}

static void
altprint(const char *b, const char *e)
{
	for (; b < e; b++) {
		if (*b == '_' || *b == '*') continue;
		if (*b == '\\' && b + 1 < e && isasciipunct(b[1])) { b++; hprint(b, b+1); continue; }
		if (*b == '`') {
			int cnt = leadc(b, e, '`');
			const char *q = b + cnt;
			while (q < e) {
				if (*q != '`') { q++; continue; }
				if (leadc(q, e, '`') == cnt) {
					hprint(b + cnt, q);
					b = q + cnt - 1;
					goto next;
				}
				q += leadc(q, e, '`');
			}
		}
		if (*b == '[') {
			const char *q = b + 1;
			int d = 1;
			while (q < e && d > 0) {
				if (*q == '[') d++;
				if (*q == ']') d--;
				q++;
			}
			if (q < e && *q == '(') {
				altprint(b + 1, q - 1);
				d = 1; q++;
				while (q < e && d > 0) {
					if (*q == '(') d++;
					if (*q == ')') d--;
					q++;
				}
				b = q - 1;
				continue;
			}
			if (q < e && *q == '[') {
				/* reference link: [text][ref] or [text][] */
				const char *r = q + 1;
				while (r < e && *r != ']') r++;
				if (r < e && *r == ']') {
					altprint(b + 1, q - 1);
					b = r;
					continue;
				}
			}
		}
		hprint(b, b + 1);
		next:;
	}
}

static void
emit_url(const char *b, const char *e)
{
	for (; b < e; b++) {
		/* collapse whitespace runs containing a newline */
		if (*b == ' ' || *b == '\t' || *b == '\n' || *b == '\r') {
			const char *s = b;
			int nl = 0;
			while (b < e && (*b == ' ' || *b == '\t'
			    || *b == '\n' || *b == '\r')) {
				if (*b == '\n' || *b == '\r') nl = 1;
				b++;
			}
			if (!nl)
				for (; s < b; s++) oputc(*s);
			b--;
			continue;
		}
		if (*b == '\\' && b + 1 < e && isasciipunct(b[1])) {
			b++;
			if (*b == '&') oputs("&amp;");
			else if (*b == '"') oputs("&quot;");
			else if (*b == '<') oputs("&lt;");
			else if (*b == '>') oputs("&gt;");
			else oputc(*b);
			continue;
		}
		if (*b == '&') oputs("&amp;");
		else if (*b == '"') oputs("&quot;");
		else if (*b == '<') oputs("&lt;");
		else if (*b == '>') oputs("&gt;");
		else oputc(*b);
	}
}

static int
dolink(const char *b, const char *e, int n)
{
	const char *p, *q, *text, *textend, *dest, *destend;
	int img, depth;
	const char *url;
	int urllen;

	if (n) return 0;
	img = 0;
	p = b;
	if (*p == '!' && p + 1 < e && p[1] == '[') { img = 1; p++; }
	if (*p != '[') return 0;

	/* footnote reference [^label] */
	if (!img && p + 1 < e && p[1] == '^') {
		const char *fl = p + 2;
		const char *fe = fl;
		while (fe < e && *fe != ']' && *fe != '\n') fe++;
		if (fe < e && *fe == ']') {
			int fi, found = -1;
			for (fi = 0; fi < nfootnotes; fi++) {
				if (footnotes[fi].labellen == (int)(fe - fl)
				    && !memcmp(footnotes[fi].label, fl, fe - fl)) {
					found = fi;
					break;
				}
			}
			/* if not found, create an empty footnote entry */
			if (found < 0) {
				int ll = fe - fl;
				char *lcpy = malloc(ll);
				if (!lcpy) die("malloc");
				memcpy(lcpy, fl, ll);
				GROWA(footnotes, nfootnotes, cap_fn);
				found = nfootnotes;
				footnotes[found].label = lcpy;
				footnotes[found].labellen = ll;
				footnotes[found].content = NULL;
				footnotes[found].contentlen = 0;
				footnotes[found].used = 0;
				footnotes[found].num = 0;
				nfootnotes++;
			}
			if (found >= 0) {
				int first = !footnotes[found].num;
				footnotes[found].used = 1;
				if (first)
					footnotes[found].num = ++footnote_counter;
				int num = footnotes[found].num;
				if (first)
					oprintf("<a id=\"fnref%d\" href=\"#fn%d\" role=\"doc-noteref\"><sup>%d</sup></a>",
					    num, num, num);
				else
					oprintf("<a href=\"#fn%d\" role=\"doc-noteref\"><sup>%d</sup></a>",
					    num, num);
				return fe + 1 - b;
			}
		}
	}

	/* Find matching `]` via the precomputed bracket-match table. Without
	 * this, inputs with many unmatched `[` (e.g. `[[[[...[](`) cause an
	 * O(N) depth walk per `[`, total O(N^2). */
	build_bracket_match(proc_base, e);
	{
		ptrdiff_t idx = p - proc_base;
		long mi;
		if (idx < 0 || idx >= bm_end - bm_base) return 0;
		mi = bm_match[idx];
		if (mi < 0) return 0;
		q = proc_base + mi;
	}
	text = p + 1;
	textend = q;
	q++; /* past ] */

	if (q < e && *q == '(') {
		dest = q + 1;
		depth = 1;
		for (q = dest; q < e; q++) {
			if (*q == '\\' && q + 1 < e) { q++; continue; }
			if (*q == '(') depth++;
			if (*q == ')' && --depth == 0) break;
		}
		if (q >= e) return 0;
		destend = q;
		while (dest < destend && isws(*dest)) dest++;
		while (destend > dest && isws(destend[-1])) destend--;
		if (img) {
			const char *after = q + 1;
			oputs("<img alt=\"");
			altprint(text, textend);
			oputs("\" src=\"");
			emit_url(dest, destend);
			oputs("\"");
			/* check for inline attributes {.class #id ...} */
			if (after < e && *after == '{') {
				const char *ab = after + 1;
				const char *ae = ab;
				while (ae < e && *ae != '}') ae++;
				if (ae < e && *ae == '}') {
					char *sa = NULL;
					parse_attrs(ab, ae, &sa);
					if (sa) {
						attr_emit(sa);
						free(sa);
						q = ae; /* advance past } */
					}
				}
			}
			oputs(">");
		} else {
			char *sa = NULL;
			const char *after = q + 1;
			/* check for inline attributes {.class #id key=val ...} */
			if (after < e && *after == '{') {
				const char *ab = after + 1;
				const char *ae = ab;
				while (ae < e && *ae != '}') {
					if (*ae == '\\' && ae + 1 < e) { ae += 2; continue; }
					if (*ae == '"') {
						ae++;
						while (ae < e && *ae != '"') {
							if (*ae == '\\' && ae + 1 < e) ae += 2;
							else ae++;
						}
						if (ae < e) ae++;
					} else ae++;
				}
				if (ae < e && *ae == '}') {
					parse_attrs(ab, ae, &sa);
					if (sa) q = ae;
				}
			}
			oputs("<a href=\"");
			emit_url(dest, destend);
			oputs("\"");
			attr_emit(sa);
			oputs(">");
			process(text, textend, 0);
			oputs("</a>");
			free(sa);
		}
		return q + 1 - b;
	}
	if (q < e && *q == '[') {
		/* reference link [text][ref] */
		const char *ref = q + 1;
		for (q = ref; q < e && *q != ']'; q++)
			if (*q == '\\' && q + 1 < e) q++;
		if (q >= e) return 0;
		const char *refend = q;
		const char *label = ref;
		int labellen = refend - ref;
		if (labellen == 0) { label = text; labellen = textend - text; }
		{
			int ri = findref(label, labellen, &url, &urllen);
			if (ri) {
				/* check for inline {attrs} after ] */
				const char *rattr = "";
				char *inline_attr = NULL;
				const char *after_ref = q + 1;
				if (after_ref < e && *after_ref == '{') {
					const char *ab = after_ref + 1;
					const char *ae = ab;
					while (ae < e && *ae != '}' && *ae != '\n') ae++;
					if (ae < e && *ae == '}') {
						parse_attrs(ab, ae, &inline_attr);
						if (inline_attr) q = ae; /* consume the {attrs} */
					}
				}
				rattr = (inline_attr && inline_attr[0]) ? inline_attr : refs[ri-1].attrs;
				if (img) {
					oputs("<img alt=\"");
					altprint(text, textend);
					oputs("\" src=\"");
					emit_url(url, url + urllen);
					oputc('"');
					attr_emit(rattr);
					oputs(">");
				} else {
					oputs("<a href=\"");
					emit_url(url, url + urllen);
					oputc('"');
					attr_emit(rattr);
					oputs(">");
					process(text, textend, 0);
					oputs("</a>");
				}
				free(inline_attr);
				return q + 1 - b;
			}
		}
		/* no matching ref: render as link without href */
		if (img) {
			oputs("<img alt=\"");
			altprint(text, textend);
			oputs("\">");
		} else {
			oputs("<a>");
			process(text, textend, 0);
			oputs("</a>");
		}
		return q + 1 - b;
	}
	/* span syntax [text]{.class #id ...} — may span lines */
	if (q < e && *q == '{') {
		const char *ab = q + 1;
		const char *ae = ab;
		while (ae < e && *ae != '}') ae++;
		if (ae < e && *ae == '}') {
			char *sa = NULL;
			parse_attrs(ab, ae, &sa);
			/* a span is only a span when a valid attribute follows;
			 * otherwise the brackets stay literal (matches djot.js) */
			if (!sa) return 0;
			oputs("<span");
			attr_emit(sa);
			oputc('>');
			process(text, textend, 0);
			oputs("</span>");
			free(sa);
			return ae + 1 - b;
		}
	}
	return 0;
}

static int
doautolink(const char *b, const char *e, int n)
{
	const char *p;

	if (n || *b != '<') return 0;
	for (p = b + 1; p < e && *p != '>' && *p != '<' && *p != '\n'; p++);
	if (p >= e || *p != '>') return 0;
	if (!memchr(b + 1, ':', p - b - 1) && !memchr(b + 1, '@', p - b - 1))
		return 0;
	{
		char *sa;
		const char *aend = scan_inline_attrs(p + 1, e, &sa);
		if (memchr(b + 1, '@', p - b - 1) && !memchr(b + 1, ':', p - b - 1)) {
			oputs("<a href=\"mailto:");
			hprint(b + 1, p);
			oputs("\"");
		} else {
			oputs("<a href=\"");
			emit_url(b + 1, p);
			oputs("\"");
		}
		attr_emit(sa);
		oputs(">");
		hprint(b + 1, p);
		oputs("</a>");
		free(sa);
		return aend - b;
	}
}

static int
doreplace(const char *b, const char *e, int n)
{
	int run, em, en;
	char before, after;
	int can_open, can_close;
	if (n) return 0;

	/* inline comment: {% ... %} — consume without output. Single
	 * line only; multi-line {% %} is handled at block level by doattr. */
	if (*b == '{' && b + 1 < e && b[1] == '%') {
		const char *q = b + 2;
		while (q < e && *q != '\n') {
			if (*q == '%' && q + 1 < e && q[1] == '}')
				return q + 2 - b;
			q++;
		}
		return 0;
	}

	/* {X...} where X is not a known opener: output literally to prevent
	 * smart replacement of contents (e.g. {1--} should not become {1–}) */
	if (*b == '{' && b + 1 < e
	    && b[1] != '_' && b[1] != '*' && b[1] != '~' && b[1] != '^'
	    && b[1] != '+' && b[1] != '-' && b[1] != '='
	    && b[1] != '\'' && b[1] != '"'
	    && b[1] != '#' && b[1] != '.' && b[1] != '%'
	    && !isalpha((unsigned char)b[1])) {
		if (!(nc_e == e && b >= nc_b && b < nc_eol)) {
			const char *q = b + 1;
			while (q < e && *q != '}' && *q != '\n') q++;
			if (q < e && *q == '}') {
				hprint(b, q + 1);
				return q + 1 - b;
			}
			/* scan stopped at `\n` or e without finding `}`: cache
			 * the run so later `{`s on this line short-circuit. */
			nc_e = e;
			nc_b = b;
			nc_eol = q;
		}
	}

	if (*b == '-' && b + 1 < e && b[1] == '-') {
		run = leadc(b, e, '-');
		em = 0; en = 0;
		if (run % 3 == 0)      { em = run / 3; }
		else if (run % 3 == 1) { en = 2; em = (run - 4) / 3; }
		else if (run % 2 == 0) { en = run / 2; }
		else                   { en = 1; em = (run - 2) / 3; }
		while (em-- > 0) oputs("\xe2\x80\x94");
		while (en-- > 0) oputs("\xe2\x80\x93");
		return run;
	}

	if (e - b >= 3 && !strncmp(b, "...", 3)) {
		oputs("\xe2\x80\xa6");
		return 3;
	}

	/* explicit quote markers: {' → left, '} → right */
	if (*b == '{' && b + 1 < e && (b[1] == '\'' || b[1] == '"')) {
		int dbl = (b[1] == '"');
		oputs(dbl ? "\xe2\x80\x9c" : "\xe2\x80\x98");
		return 2;
	}

	if (*b == '"' || *b == '\'') {
		before = (b > proc_base) ? *(b - 1) : 0;
		after = (b + 1 < e) ? b[1] : 0;
		/* explicit closer: '} or "} */
		if (after == '}') {
			oputs(*b == '"' ? "\xe2\x80\x9d" : "\xe2\x80\x99");
			return 2; /* consume the } too */
		}
		/* ' before digit or after ] is always apostrophe */
		if (*b == '\'' && (isdigit((unsigned char)after) || before == ']')) {
			oputs("\xe2\x80\x99");
			return 1;
		}
		can_open = !isws(after) && (isws(before) || isasciipunct(before) || before == 0);
		can_close = !isws(before) && (isws(after) || isasciipunct(after) || after == 0);
		if (*b == '"') {
			/* " after = is always opening (attribute value context) */
			if (before == '=') {
				oputs("\xe2\x80\x9c");
				return 1;
			}
			oputs(can_open && !can_close ? "\xe2\x80\x9c" : "\xe2\x80\x9d");
		} else if (can_close && !can_open) {
			oputs("\xe2\x80\x99");
		} else if (can_open) {
			/* look-ahead: simulate stack matching to check if this
			 * opener has a closer. Unmatched openers → apostrophe */
			int stack = 1;
			const char *q;
			for (q = b + 1; q < e && stack > 0; q++) {
				if (*q == '\'') {
					char qb = q[-1], qa = (q+1 < e) ? q[1] : 0;
					int qo = !isws(qa) && (isws(qb) || isasciipunct(qb));
					int qc = !isws(qb) && (isws(qa) || isasciipunct(qa) || qa == 0);
					if (qc) { stack--; if (stack == 0) break; }
					if (qo) stack++;
				}
			}
			oputs(stack == 0 ? "\xe2\x80\x98" : "\xe2\x80\x99");
		} else {
			oputs("\xe2\x80\x99"); /* intra-word: apostrophe */
		}
		return 1;
	}

	/* spaces before hard break: consume "ws \ ws newline" as <br>.
	 * Mid-run spaces skip the forward scan: the previous iteration
	 * (one space earlier in the same run) already concluded — without
	 * this, a long run of spaces is O(N^2). */
	if ((*b == ' ' || *b == '\t') && !n
	    && !(b > proc_base && (b[-1] == ' ' || b[-1] == '\t'))) {
		const char *q = b;
		while (q < e && (*q == ' ' || *q == '\t')) q++;
		if (q < e && *q == '\\') {
			const char *r = q + 1;
			while (r < e && (*r == ' ' || *r == '\t')) r++;
			if (r < e && (*r == '\n' || *r == '\r')) {
				if (*r == '\r' && r + 1 < e && r[1] == '\n') r++;
				oputs("<br>\n");
				return r + 1 - b;
			}
		}
	}

	if (*b == '&') { oputs("&amp;"); return 1; }
	if (*b == '<') { oputs("&lt;"); return 1; }
	if (*b == '>') { oputs("&gt;"); return 1; }

	return 0;
}

static void
process(const char *b, const char *e, int newblock)
{
	const char *p;
	const char *save_base = proc_base;
	const char *save_nc_e = nc_e;
	const char *save_nc_b = nc_b;
	const char *save_nc_eol = nc_eol;
	const char *save_bm_base = bm_base;
	const char *save_bm_end = bm_end;
	long *save_bm_match = bm_match;
	long save_bm_cap = bm_cap;
	int affected;
	int allow_block = newblock;

	proc_base = b;
	nc_e = NULL;
	nc_b = NULL;
	nc_eol = NULL;
	bm_base = NULL;
	bm_end = NULL;
	bm_match = NULL;
	bm_cap = 0;
	for (p = b; p < e; ) {
		if (newblock) {
			int had_blank = 0;
			while (p < e) {
				const char *le = eol(p, e);
				if (!isblankline(p, le)) break;
				had_blank = 1;
				p = le;
				if (p >= e) {
					nc_e = save_nc_e;
					nc_b = save_nc_b;
					nc_eol = save_nc_eol;
					free(bm_match);
					bm_base = save_bm_base;
					bm_end = save_bm_end;
					bm_match = save_bm_match;
					bm_cap = save_bm_cap;
					return;
				}
			}
			if (had_blank && has_pending())
				clear_pending();
		}

		affected = 0;
		if (newblock) {
			/* dispatch block parsers by first non-space byte */
			const char *q = p;
			while (q < e && (*q == ' ' || *q == '\t')) q++;
			if (q < e) {
				switch ((unsigned char)*q) {
				case '{': affected = doattr(p, e, 1); break;
				case '[': affected = dorefdef(p, e, 1); break;
				case '#': affected = doheading(p, e, 1); break;
				case '>': affected = doblockquote(p, e, 1); break;
				case '`':
				case '~': affected = docodefence(p, e, 1); break;
				case ':':
					if (!(affected = dodiv(p, e, 1)))
						affected = dodeflist(p, e, 1);
					break;
				case '*':
				case '-':
					if (!(affected = dothematicbreak(p, e, 1)))
						affected = dolist(p, e, 1);
					break;
				case '+':
				case '(':
				case '0': case '1': case '2': case '3': case '4':
				case '5': case '6': case '7': case '8': case '9':
					affected = dolist(p, e, 1);
					break;
				case '|': affected = dotable(p, e, 1); break;
				default:
					if ((*q >= 'a' && *q <= 'z')
					    || (*q >= 'A' && *q <= 'Z'))
						affected = dolist(p, e, 1);
					break;
				}
			}
			if (!affected)
				affected = doparagraph(p, e, 1);
		} else {
			/* dispatch inline parsers by first byte */
			switch ((unsigned char)*p) {
			case '\\':
			case '\n':
				affected = dolinebreak(p, e, 0);
				break;
			case '`':
			case '$':
				affected = docode(p, e, 0);
				break;
			case '_':
			case '*':
			case '~':
			case '^':
				affected = dosurround(p, e, 0);
				break;
			case '{':
				if (!(affected = dosurround(p, e, 0)))
					affected = doreplace(p, e, 0);
				break;
			case '[':
			case '!':
				affected = dolink(p, e, 0);
				break;
			case '<':
				if (!(affected = doautolink(p, e, 0)))
					affected = doreplace(p, e, 0);
				break;
			case '-':
			case '.':
			case '\'':
			case '"':
			case ' ':
			case '\t':
			case '&':
			case '>':
				affected = doreplace(p, e, 0);
				break;
			}
		}

		if (affected) {
			p += abs(affected);
		} else {
			oputc(*p++);
		}

		if (allow_block && p < e && p[0] == '\n' && p + 1 < e && p[1] == '\n')
			newblock = 1;
		else
			newblock = affected < 0;
	}
	proc_base = save_base;
	nc_e = save_nc_e;
	nc_b = save_nc_b;
	nc_eol = save_nc_eol;
	free(bm_match);
	bm_base = save_bm_base;
	bm_end = save_bm_end;
	bm_match = save_bm_match;
	bm_cap = save_bm_cap;
}

static char *urlbuf;
static int urlbuflen, cap_url;

static void
urlbuf_add(const char *b, const char *e)
{
	while (b < e) {
		if (*b != '\n' && *b != '\r' && *b != ' ' && *b != '\t') {
			GROWBUF(urlbuf, urlbuflen, cap_url, 1);
			urlbuf[urlbuflen++] = *b;
		}
		b++;
	}
}

/* pre-scan: collect reference defs, footnote defs, and heading auto-refs */
static void
prescan(const char *b, const char *e)
{
	const char *p, *line, *label;
	int labellen, sp;
	/* deferred heading refs: added only if no explicit ref exists */
	struct hdef { const char *content; int len; const char *line; };
	struct hdef *hdefs = NULL;
	int nhdefs = 0, cap_hdefs = 0;

	line = b;
	while (line < e) {
		p = line;
		while (p < e && *p == ' ') p++;
		sp = p - line;

		/* footnote definition: [^label]: content (may span multiple indented lines) */
		if (p + 2 < e && p[0] == '[' && p[1] == '^') {
			const char *fl = p + 2;
			const char *fp = fl;
			while (fp < e && *fp != ']' && *fp != '\n') fp++;
			if (fp < e && *fp == ']' && fp + 1 < e && fp[1] == ':') {
				int ll = fp - fl;
				fp += 2;
				while (fp < e && (*fp == ' ' || *fp == '\t')) fp++;
				/* collect first line content */
				char *fnbuf = NULL;
				int fni = 0;
				{
					const char *cend = trim_end(fp, eol(line, e));
					PUSHRANGE(fnbuf, fni, fp, cend - fp);
				}
				line = eol(line, e);
				/* collect continuation lines (blank or indented by 2+) */
				while (line < e) {
					if (isblankline(line, eol(line, e))) {
						PUSH(fnbuf, fni, '\n');
						line = eol(line, e);
						continue;
					}
					int csp = spaces(line, e);
					if (csp < 2) break;
					if (fni > 0)
						PUSH(fnbuf, fni, '\n');
					const char *le = eol(line, e);
					const char *lp = line + 2;
					const char *lend = trim_end(lp, le);
					PUSHRANGE(fnbuf, fni, lp, lend - lp);
					line = eol(line, e);
				}
				while (fni > 0 && fnbuf[fni-1] == '\n') fni--;
				ADDC(fnbuf, fni) = '\0';
				fnbuf = realloc(fnbuf, fni + 1);
				if (ll > 0) {
					char *lcpy = malloc(ll);
					if (!lcpy) die("malloc");
					memcpy(lcpy, fl, ll);
					GROWA(footnotes, nfootnotes, cap_fn);
					footnotes[nfootnotes].label = lcpy;
					footnotes[nfootnotes].labellen = ll;
					footnotes[nfootnotes].content = fnbuf;
					footnotes[nfootnotes].contentlen = fni;
					footnotes[nfootnotes].used = 0;
					footnotes[nfootnotes].num = 0;
					nfootnotes++;
				} else {
					free(fnbuf);
				}
				continue;
			}
		}

		/* reference definition: [label]: url */
		if (p < e && *p == '[') {
			p++;
			label = p;
			while (p < e && *p != ']' && *p != '\n') p++;
			if (p < e && *p == ']') {
				labellen = p - label;
				p++;
				if (p < e && *p == ':') {
					p++;
					while (p < e && (*p == ' ' || *p == '\t')) p++;
					urlbuflen = 0;
					if (p < e && *p != '\n') {
						const char *us = p;
						const char *ue = us;
						while (ue < e && *ue != ' ' && *ue != '\t'
						    && *ue != '\n') ue++;
						/* reject ref def if URL chunk has trailing
						 * non-whitespace content (spec: no internal
						 * whitespace in URL chunks) */
						{
							const char *tr = ue;
							while (tr < e && (*tr == ' ' || *tr == '\t'))
								tr++;
							if (tr < e && *tr != '\n') {
								line = eol(line, e);
								continue;
							}
						}
						urlbuf_add(us, ue);
					}
					const char *nextline = eol(line, e);
					while (nextline < e) {
						int ns = spaces(nextline, e);
						if (ns == 0 || isblankline(nextline, eol(nextline, e)))
							break;
						/* a new refdef starts a sibling, not a
						 * continuation — stop gobbling */
						if (nextline + ns < e && nextline[ns] == '[')
							break;
						const char *le = eol(nextline, e);
						urlbuf_add(nextline + ns, le);
						nextline = le;
					}
					if (labellen > 0) {
						GROWA(refs, nrefs, cap_refs);
						char *u = malloc(urlbuflen + 1);
						if (u) {
							if (urlbuflen > 0)
								memcpy(u, urlbuf, urlbuflen);
							u[urlbuflen] = '\0';
						}
						char *nbuf = malloc(labellen);
						if (!nbuf) die("malloc");
						refs[nrefs].normlen =
						    normalize_label(label, labellen, nbuf);
						refs[nrefs].norm = nbuf;
						refs[nrefs].url = u;
						refs[nrefs].urllen = urlbuflen;
						refs[nrefs].attrs = NULL;
						prev_line_attrs(line, b, &refs[nrefs].attrs);
						if (!refs[nrefs].attrs) {
							refs[nrefs].attrs = malloc(1);
							if (refs[nrefs].attrs)
								refs[nrefs].attrs[0] = '\0';
						}
						nrefs++;
					}
					line = nextline;
					continue;
				}
			}
		}

		/* heading: defer auto-reference until after all explicit refs */
		if (sp <= 3) {
			p = line + sp;
			int lvl = leadc(p, e, '#');
			if (lvl >= 1 && lvl <= 6 && p + lvl < e
			    && (p[lvl] == ' ' || p[lvl] == '\n')) {
				const char *content = p + lvl;
				if (*content == ' ') content++;
				const char *cend = trim_end(content, eol(line, e));
				if (cend > content) {
					GROWA(hdefs, nhdefs, cap_hdefs);
					hdefs[nhdefs].content = content;
					hdefs[nhdefs].len = cend - content;
					hdefs[nhdefs].line = line;
					nhdefs++;
				}
			}
		}

		line = eol(line, e);
	}

	/* register heading auto-refs; explicit refs shadow these at lookup */
	{
		int hi;
		int href_start = nrefs;
		for (hi = 0; hi < nhdefs; hi++) {
			if (findref_range(hdefs[hi].content, hdefs[hi].len,
			    href_start, nrefs))
				continue;
			char *hattrs = NULL;
			const char *custom_id;
			prev_line_attrs(hdefs[hi].line, b, &hattrs);
			custom_id = hattrs ? attr_find(hattrs, "id") : NULL;
			char idbuf[256];
			int idn;
			if (custom_id && custom_id[0]) {
				idn = strlen(custom_id);
				if (idn > (int)sizeof(idbuf) - 2) idn = sizeof(idbuf) - 2;
				memcpy(idbuf, custom_id, idn);
				idbuf[idn] = '\0';
			} else {
				idn = make_slug(hdefs[hi].content, hdefs[hi].len,
				    idbuf, sizeof(idbuf));
			}
			{
				int ulen = idn + 1;
				char *u = malloc(ulen + 1);
				if (u) {
					u[0] = '#';
					memcpy(u + 1, idbuf, idn);
					u[ulen] = '\0';
					GROWA(refs, nrefs, cap_refs);
					char *nbuf = malloc(hdefs[hi].len);
					if (!nbuf) die("malloc");
					refs[nrefs].normlen = normalize_label(
					    hdefs[hi].content, hdefs[hi].len, nbuf);
					refs[nrefs].norm = nbuf;
					refs[nrefs].url = u;
					refs[nrefs].urllen = ulen;
					refs[nrefs].attrs = malloc(1);
					if (refs[nrefs].attrs)
						refs[nrefs].attrs[0] = '\0';
					nrefs++;
				}
			}
			free(hattrs);
		}
	}
	free(hdefs);
}

static void
emit_endnotes(void)
{
	int i, any = 0;

	for (i = 0; i < nfootnotes; i++)
		if (footnotes[i].used) { any = 1; break; }
	if (!any) return;

	/* sort by sequential number to emit in reference order */
	oputs("<section role=\"doc-endnotes\">\n<hr>\n<ol>\n");
	{
		int num;
		for (num = 1; num <= footnote_counter; num++) {
			for (i = 0; i < nfootnotes; i++)
				if (footnotes[i].used && footnotes[i].num == num)
					break;
			if (i >= nfootnotes) continue;

			oprintf("<li id=\"fn%d\">\n", num);
			if (footnotes[i].contentlen > 0) {
				const char *fc = footnotes[i].content;
				int fcl = footnotes[i].contentlen;
				/* single scan: detect blocks and find last paragraph */
				int has_blocks = 0;
				const char *lastpara = fc;
				{
					const char *sp;
					for (sp = fc; sp < fc + fcl; ) {
						const char *le = eol(sp, fc + fcl);
						if (isblankline(sp, le)) {
							has_blocks = 1;
							const char *after = skip_blanks(le, fc + fcl);
							if (after < fc + fcl)
								lastpara = after;
							sp = after;
						} else {
							const char *tp = sp;
							while (tp < le && *tp == ' ') tp++;
							if (tp < le && (*tp == '`' || *tp == '~')
							    && leadc(tp, le, *tp) >= 3)
								has_blocks = 1;
							sp = le;
						}
					}
				}
				if (has_blocks) {
					int save_cont = in_container;
					in_container = 1;
					{
						/* emit everything before last paragraph */
						if (lastpara > fc)
							process(fc, lastpara, 1);
						/* check if last paragraph is a block element (code fence etc.) */
						const char *lp = lastpara;
						while (lp < fc + fcl && *lp == ' ') lp++;
						int last_is_block = 0;
						if (lp < fc + fcl && (*lp == '`' || *lp == '~')
						    && leadc(lp, fc + fcl, *lp) >= 3)
							last_is_block = 1;
						if (last_is_block) {
							process(lastpara, fc + fcl, 1);
							oprintf("<p><a href=\"#fnref%d\" role=\"doc-backlink\">\xe2\x86\xa9\xef\xb8\x8e</a></p>\n", num);
						} else {
							/* last paragraph: emit inline with backlink */
							const char *pe = trim_end(lastpara, fc + fcl);
							while (lastpara < pe && (*lastpara == ' ' || *lastpara == '\t'))
								lastpara++;
							oputs("<p>");
							process(lastpara, pe, 0);
							oprintf("<a href=\"#fnref%d\" role=\"doc-backlink\">\xe2\x86\xa9\xef\xb8\x8e</a>", num);
							oputs("</p>\n");
						}
						in_container = save_cont;
					}
				} else {
					oputs("<p>");
					process(footnotes[i].content,
					    footnotes[i].content + footnotes[i].contentlen, 0);
					oprintf("<a href=\"#fnref%d\" role=\"doc-backlink\">\xe2\x86\xa9\xef\xb8\x8e</a>", num);
					oputs("</p>\n");
				}
			} else {
				oprintf("<p><a href=\"#fnref%d\" role=\"doc-backlink\">\xe2\x86\xa9\xef\xb8\x8e</a></p>\n", num);
			}
			oputs("</li>\n");
		}
	}
	oputs("</ol>\n</section>\n");
}

int
cdjot_convert(FILE *out, const char *buf, size_t len)
{
	int i;

	/* init state — pre-size output buffer to ~2x input (HTML output is
	 * typically 1.5-3x source size); avoids most reallocs on big inputs. */
	olen = 0;
	ocap = (int)(len * 2);
	if (ocap < 4096) ocap = 4096;
	obuf = malloc(ocap);
	if (!obuf) die("malloc");
	refs = NULL; nrefs = 0; cap_refs = 0;
	footnotes = NULL; nfootnotes = 0; cap_fn = 0;
	footnote_counter = 0;
	nsections = 0;
	in_container = 0;
	tight = 0;
	proc_base = NULL;
	id_ht = NULL; id_ht_sz = 0; id_ht_cnt = 0;
	urlbuf = NULL; urlbuflen = 0; cap_url = 0;
	nc_e = NULL;
	nc_b = NULL;
	nc_eol = NULL;
	bm_base = NULL;
	bm_end = NULL;
	bm_match = NULL;
	bm_cap = 0;
	bm_stack = NULL;
	bm_stack_cap = 0;

	cap_pattr = 16;
	pending_attrs = malloc(cap_pattr);
	if (!pending_attrs) die("malloc");
	pending_attrs[0] = '\0';

	prescan(buf, buf + len);
	process(buf, buf + len, 1);

	close_sections(0);
	emit_endnotes();

	if (olen > 0) fwrite(obuf, 1, olen, out);
	free(obuf);
	obuf = NULL; olen = 0; ocap = 0;

	for (i = 0; i < nrefs; i++) {
		free(refs[i].norm);
		free((char *)refs[i].url);
		free(refs[i].attrs);
	}
	free(refs);
	for (i = 0; i < nfootnotes; i++) {
		free(footnotes[i].label);
		free(footnotes[i].content);
	}
	free(footnotes);
	for (i = 0; i < id_ht_sz; i++)
		free(id_ht[i].key);
	free(id_ht);
	free(urlbuf);
	free(pending_attrs);
	free(bm_stack);

	refs = NULL; footnotes = NULL; id_ht = NULL;
	urlbuf = NULL;
	pending_attrs = NULL;
	bm_stack = NULL; bm_stack_cap = 0;

	return 0;
}

#ifndef CDJOT_NO_MAIN
static char *
readall(FILE *f, int *outlen)
{
	char *buf = NULL;
	int len = 0, cap = 0, n;
	struct stat st;

	/* If the fd backs a regular file, fstat tells us the size and we can
	 * allocate exactly once. Otherwise (pipe, terminal) fall through to
	 * doubling growth. */
	if (fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
		cap = st.st_size + 1;
		buf = malloc(cap);
		if (!buf) die("malloc");
		while ((n = fread(buf + len, 1, cap - len, f)) > 0)
			len += n;
		if (!ferror(f)) {
			*outlen = len;
			return buf;
		}
		free(buf);
		*outlen = 0;
		return NULL;
	}

	do {
		cap = cap ? cap * 2 : BUFSIZ;
		buf = realloc(buf, cap);
		if (!buf) die("malloc");
		n = fread(buf + len, 1, cap - len, f);
		len += n;
	} while (n > 0);
	if (ferror(f)) {
		free(buf);
		*outlen = 0;
		return NULL;
	}
	*outlen = len;
	return buf;
}

int
main(int argc, char *argv[])
{
	int i, ret = 0;

	signal(SIGPIPE, SIG_DFL);

	for (i = 1; i < argc; i++) {
		if (!strcmp("-v", argv[i]) || !strcmp("--version", argv[i])) {
			fprintf(stderr, "cdjot 0.1\n");
			return 0;
		} else if (!strcmp("-h", argv[i]) || !strcmp("--help", argv[i])) {
			fprintf(stderr,
				"cdjot - convert djot to HTML\n"
				"\n"
				"Usage: %s [-hv] [file ...]\n"
				"\n"
				"Example:\n"
				"  printf '# hi\\n' | %s\n"
				"\n"
				"Options:\n"
				"  -h, --help     show this help\n"
				"  -v, --version  show version\n",
				argv[0], argv[0]);
			return 0;
		} else if (!strcmp("--", argv[i])) {
			i++;
			break;
		} else if (argv[i][0] == '-' && argv[i][1] != '\0') {
			fprintf(stderr, "Usage: %s [-hv] [file ...]\n", argv[0]);
			return 2;
		} else {
			break;
		}
	}

	if (i >= argc) {
		/* no file arguments: read stdin */
		int len;
		char *buf = readall(stdin, &len);
		if (!buf) {
			fprintf(stderr, "cdjot: read error\n");
			return 1;
		}
		cdjot_convert(stdout, buf, len);
		free(buf);
	} else {
		for (; i < argc; i++) {
			FILE *source;
			int len;
			char *buf;

			if (!strcmp("-", argv[i])) {
				source = stdin;
			} else {
				source = fopen(argv[i], "r");
				if (!source) {
					fprintf(stderr, "cdjot: cannot open '%s'\n", argv[i]);
					ret = 1;
					continue;
				}
			}
			buf = readall(source, &len);
			if (source != stdin)
				fclose(source);
			if (!buf) {
				fprintf(stderr, "cdjot: read error: '%s'\n", argv[i]);
				ret = 1;
				continue;
			}
			cdjot_convert(stdout, buf, len);
			free(buf);
		}
	}
	return ret;
}
#endif /* CDJOT_NO_MAIN */
