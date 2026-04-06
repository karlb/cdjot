/* djot - djot to HTML converter
 * Reads djot from stdin, writes HTML to stdout.
 * No dependencies beyond libc.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEN(x) (sizeof(x)/sizeof(x[0]))
#define ADDC(b,i) do { if ((i) % BUFSIZ == 0) { \
	b = realloc(b, ((i) + BUFSIZ)); if (!b) die("malloc"); } } while(0); b[i]

typedef int (*Parser)(const char *, const char *, int);

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
static void emit_attrs(const char *id, const char *cls, const char *extra);

static Parser parsers[] = {
	doattr, dorefdef, doheading, doblockquote, docodefence, dodiv,
	dothematicbreak, dotable, dodeflist, dolist, doparagraph,
	dolinebreak, docode, dosurround, dolink, doautolink, doreplace,
};

static struct {
	const char *label; int labellen;
	const char *url; int urllen;
	char attrs[128];
} refs[128];
static int nrefs;

static struct {
	const char *label; int labellen;
	char *content; int contentlen;
	int used;
	int num; /* sequential number assigned on first reference */
} footnotes[64];
static int nfootnotes;
static int footnote_counter;

static int sections[6], nsections;
static int in_container;
static int tight;
static const char *proc_base;

static char pending_id[128];
static char pending_class[128];
static char pending_attrs[256];

static char *used_ids[256];
static int nused_ids;

static void
die(const char *msg)
{
	fprintf(stderr, "djot: %s\n", msg);
	exit(1);
}

static void
hprint(const char *b, const char *e)
{
	for (; b < e; b++) {
		if (*b == '&')      fputs("&amp;", stdout);
		else if (*b == '<') fputs("&lt;", stdout);
		else if (*b == '>') fputs("&gt;", stdout);
		else                fputc(*b, stdout);
	}
}

static const char *
eol(const char *p, const char *e)
{
	while (p < e && *p != '\n') p++;
	return p < e ? p + 1 : e;
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

static int
label_match(const char *a, int alen, const char *b, int blen)
{
	int ai = 0, bi = 0;
	while (ai < alen && bi < blen) {
		/* skip emphasis markers */
		if (a[ai] == '_' || a[ai] == '*') { ai++; continue; }
		if (b[bi] == '_' || b[bi] == '*') { bi++; continue; }
		if (isws(a[ai]) && isws(b[bi])) {
			while (ai < alen && isws(a[ai])) ai++;
			while (bi < blen && isws(b[bi])) bi++;
		} else if (a[ai] == b[bi]) {
			ai++; bi++;
		} else {
			return 0;
		}
	}
	while (ai < alen && (isws(a[ai]) || a[ai] == '_' || a[ai] == '*')) ai++;
	while (bi < blen && (isws(b[bi]) || b[bi] == '_' || b[bi] == '*')) bi++;
	return ai == alen && bi == blen;
}

static int
findref(const char *label, int len, const char **url, int *urllen)
{
	int i;
	for (i = 0; i < nrefs; i++)
		if (label_match(refs[i].label, refs[i].labellen, label, len)) {
			*url = refs[i].url;
			*urllen = refs[i].urllen;
			return i + 1; /* return 1-based index */
		}
	return 0;
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
		fputs("</section>\n", stdout);
		nsections--;
	}
}

static int
parse_attrs(const char *b, const char *e, char *id, int idsz,
    char *cls, int clssz, char *extra, int exsz)
{
	const char *p = b;
	int idn = 0, cn = 0, en = 0;

	if (id) id[0] = '\0';
	if (cls) cls[0] = '\0';
	if (extra) extra[0] = '\0';

	while (p < e) {
		while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
		if (p >= e) break;
		if (*p == '#') {
			/* id */
			p++;
			while (p < e && *p != ' ' && *p != '\t' && *p != '\n'
			    && *p != '}' && idn < idsz - 1)
				id[idn++] = *p++;
			id[idn] = '\0';
		} else if (*p == '.') {
			/* class */
			p++;
			if (cn > 0 && cn < clssz - 1) cls[cn++] = ' ';
			while (p < e && *p != ' ' && *p != '\t' && *p != '\n'
			    && *p != '}' && cn < clssz - 1)
				cls[cn++] = *p++;
			cls[cn] = '\0';
		} else if (*p == '%') {
			/* comment: skip to next % or end */
			p++;
			while (p < e && *p != '%') p++;
			if (p < e) p++; /* skip closing % */
		} else if (isalpha((unsigned char)*p)) {
			/* key=val */
			if (en > 0 && en < exsz - 1) extra[en++] = ' ';
			while (p < e && *p != '=' && *p != ' ' && *p != '}' && en < exsz - 1)
				extra[en++] = *p++;
			if (p < e && *p == '=') {
				extra[en++] = '=';
				p++;
				if (p < e && *p == '"') {
					extra[en++] = '"';
					p++;
					while (p < e && *p != '"' && en < exsz - 1) {
						if (*p == '\\' && p + 1 < e) {
							p++; /* skip backslash */
							if (*p == '\\') {
								extra[en++] = '\\';
								p++;
							} else if (*p == '"') {
								/* escaped quote: emit &quot; */
								if (en + 5 < exsz) {
									memcpy(extra + en, "&quot;", 6);
									en += 6;
								}
								p++;
							} else if (*p == '*') {
								extra[en++] = *p++;
							} else {
								extra[en++] = *p++;
							}
						} else {
							extra[en++] = *p++;
						}
					}
					if (p < e) { extra[en++] = '"'; p++; }
				} else {
					/* wrap unquoted value in quotes */
					if (en < exsz - 1) extra[en++] = '"';
					while (p < e && *p != ' ' && *p != '}' && en < exsz - 1)
						extra[en++] = *p++;
					if (en < exsz - 1) extra[en++] = '"';
				}
			}
			extra[en] = '\0';
		} else {
			p++;
		}
	}
	return idn > 0 || cn > 0 || en > 0;
}

static void
emit_attrs(const char *id, const char *cls, const char *extra)
{
	if (id && id[0]) printf(" id=\"%s\"", id);
	if (cls && cls[0]) printf(" class=\"%s\"", cls);
	if (extra && extra[0]) printf(" %s", extra);
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
	fputs("<pre", stdout);
	if (has_pending()) {
		emit_attrs(pending_id, pending_class, pending_attrs);
		clear_pending();
	}
	if (info < infoend) {
		fputs("><code class=\"language-", stdout);
		hprint(info, infoend);
		fputs("\">", stdout);
	} else {
		fputs("><code>", stdout);
	}
}

/* check previous line for {attrs} block */
static void
prev_line_attrs(const char *line, const char *start,
    char *id, int idsz, char *cls, int clssz, char *extra, int exsz)
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
		parse_attrs(pp + 1, pe, id, idsz, cls, clssz, extra, exsz);
}

static void
dedup_id(char *id, int sz)
{
	int i, n;
	char buf[256];

	for (i = 0; i < nused_ids; i++)
		if (strcmp(used_ids[i], id) == 0)
			goto dup;
	/* not a dup — record it */
	if (nused_ids < (int)LEN(used_ids))
		used_ids[nused_ids++] = strcpy(malloc(strlen(id)+1), id);
	return;
dup:
	for (n = 1; n < 100; n++) {
		snprintf(buf, sizeof(buf), "%s-%d", id, n);
		for (i = 0; i < nused_ids; i++)
			if (strcmp(used_ids[i], buf) == 0)
				goto next;
		/* found unique */
		snprintf(id, sz, "%s", buf);
		if (nused_ids < (int)LEN(used_ids))
			used_ids[nused_ids++] = strcpy(malloc(strlen(id)+1), id);
		return;
	next:;
	}
}

static void
clear_pending(void)
{
	pending_id[0] = '\0';
	pending_class[0] = '\0';
	pending_attrs[0] = '\0';
}

static int
has_pending(void)
{
	return pending_id[0] || pending_class[0] || pending_attrs[0];
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
is_sep_row(const char *b, const char *e, int *aligns, int *ncols)
{
	const char *p = b;
	int n = 0;
	if (p >= e || *p != '|') return 0;
	p++;
	while (p < e && *p != '\n') {
		while (p < e && *p == ' ') p++;
		int left = 0, right = 0;
		if (p < e && *p == ':') { left = 1; p++; }
		if (p >= e || *p != '-') return 0;
		while (p < e && *p == '-') p++;
		if (p < e && *p == ':') { right = 1; p++; }
		while (p < e && *p == ' ') p++;
		if (n < 64)
			aligns[n] = left && right ? 3 : left ? 1 : right ? 2 : 0;
		n++;
		if (p < e && *p == '|') p++;
		else break;
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
	printf("<%s%s>", tag, astyle[align & 3]);
	/* trim spaces */
	while (b < ce && *b == ' ') b++;
	while (ce > b && ce[-1] == ' ') ce--;
	process(b, ce, 0);
	printf("</%s>\n", tag);
}

static int
dotable(const char *b, const char *e, int n)
{
	const char *p, *line, *cap;
	int aligns[64] = {0}, naligns = 0;
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

	fputs("<table>\n", stdout);

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
				fputs("<caption>", stdout);
				process(capstart, ce, 0);
				fputs("</caption>\n", stdout);
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
			int sep_aligns[64], sep_ncols;
			if (is_sep_row(p, eol(p, e), sep_aligns, &sep_ncols)) {
				memcpy(aligns, sep_aligns, sizeof(int) * (sep_ncols < 64 ? sep_ncols : 64));
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
				int sa[64], sn;
				if (np < e && *np == '|' && is_sep_row(np, eol(np, e), sa, &sn)) {
					is_header = 1;
					memcpy(aligns, sa, sizeof(int) * (sn < 64 ? sn : 64));
					naligns = sn;
				}
			}
		}

		/* parse cells */
		fputs("<tr>\n", stdout);
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
		fputs("</tr>\n", stdout);
		is_header = 0;
		line = eol(line, e);
	}

	/* advance past caption if we emitted one */
	if (cap > line) line = cap;

	fputs("</table>\n", stdout);
	return -(line - b);
}

static int
dodeflist(const char *b, const char *e, int n)
{
	const char *p, *q, *line;
	char *buf;
	int i;

	if (!n) return 0;
	p = b;
	if (p >= e || *p != ':') return 0;
	if (p + 1 >= e || (p[1] != ' ' && p[1] != '\n')) return 0;

	fputs("<dl>\n", stdout);

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
			char fc = *tp;
			if ((fc == '`' || fc == '~') && leadc(tp, eol(line, e), fc) >= 3)
				term_is_fence = 1;
			if (!term_is_fence) {
				const char *le = trim_end(p, eol(line, e));
				for (q = p; q < le; q++) { ADDC(buf, i) = *q; i++; }
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
			if (i > 0) { ADDC(buf, i) = '\n'; i++; }
			const char *le = trim_end(p, eol(line, e));
			for (q = p; q < le; q++) { ADDC(buf, i) = *q; i++; }
			line = eol(line, e);
		}
		ADDC(buf, i) = '\0';
		fputs("<dt>", stdout);
		process(buf, buf + i, 0);
		fputs("</dt>\n", stdout);
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
			for (q = p; q < fle; q++) {
				ADDC(buf, i) = *q;
				i++;
			}
		}
		while (line < e) {
			if (isblankline(line, eol(line, e))) {
				ADDC(buf, i) = '\n';
				i++;
				line = eol(line, e);
				continue;
			}
			int sp = spaces(line, e);
			if (sp < 2) break;
			{
				const char *le = eol(line, e);
				int strip = (sp > 2) ? 2 : sp;
				for (q = line + strip; q < le; q++) {
					ADDC(buf, i) = *q;
					i++;
				}
			}
			line = eol(line, e);
		}
		while (i > 0 && buf[i-1] == '\n') i--;
		ADDC(buf, i) = '\0';

		{
			int save = in_container;
			in_container = 1;
			fputs("<dd>\n", stdout);
			if (i > 0)
				process(buf, buf + i, 1);
			fputs("</dd>\n", stdout);
			in_container = save;
		}
		free(buf);

		/* skip blank lines between items */
		while (line < e && isblankline(line, eol(line, e)))
			line = eol(line, e);
	}

	fputs("</dl>\n", stdout);
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
		if ((*q == '`' || *q == '~') && leadc(q, e, *q) >= 3) {
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
			for (q = line; q < le; q++) {
				ADDC(buf, i) = *q;
				i++;
			}
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
			int cn = strlen(pending_class);
			if (cn > 0 && cn < (int)sizeof(pending_class) - 1)
				pending_class[cn++] = ' ';
			while (cls < clsend && cn < (int)sizeof(pending_class) - 1)
				pending_class[cn++] = *cls++;
			pending_class[cn] = '\0';
		}
		fputs("<div", stdout);
		emit_attrs(pending_id, pending_class, pending_attrs);
		fputs(">\n", stdout);
		clear_pending();
		process(buf, buf + i, 1);
		fputs("</div>\n", stdout);
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
	/* find matching } — may span multiple lines */
	q = p + 1;
	while (q < e && *q != '}') q++;
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
		char tid[128], tcls[128], textra[256];
		/* check if entire content is a comment */
		{
			const char *cc = p + 1;
			while (cc < q && (*cc == ' ' || *cc == '\t' || *cc == '\n')) cc++;
			if (cc < q && *cc == '%') {
				cc++;
				while (cc < q && *cc != '%') cc++;
				if (cc < q && *cc == '%') {
					cc++;
					while (cc < q && (*cc == ' ' || *cc == '\t' || *cc == '\n')) cc++;
					if (cc >= q) {
						/* pure comment block — consume the line(s) */
						return -(eol(q, e) - b);
					}
				}
			}
		}
		if (!parse_attrs(p + 1, q, tid, sizeof(tid),
		    tcls, sizeof(tcls), textra, sizeof(textra)))
			return 0;
		/* merge into pending */
		if (tid[0])
			snprintf(pending_id, sizeof(pending_id), "%s", tid);
		if (tcls[0]) {
			if (pending_class[0]) {
				int n = strlen(pending_class);
				snprintf(pending_class + n, sizeof(pending_class) - n,
				    " %s", tcls);
			} else {
				snprintf(pending_class, sizeof(pending_class), "%s", tcls);
			}
		}
		if (textra[0]) {
			/* later key=val overrides earlier */
			snprintf(pending_attrs, sizeof(pending_attrs), "%s", textra);
		}
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
	for (q = content; q < cend; q++) {
		ADDC(buf, blen) = *q;
		blen++;
	}
	line = eol(b, e);
	while (line < e && !isblankline(line, eol(line, e))) {
		const char *lp = line;
		int lsp = spaces(lp, e);
		lp += lsp;
		hl = leadc(lp, e, '#');
		if (hl >= 1 && hl <= 6 && hl != level && lp + hl < e
		    && (lp[hl] == ' ' || lp[hl] == '\n'))
			break; /* different heading level */
		if (blen > 0) {
			ADDC(buf, blen) = '\n';
			blen++;
		}
		if (hl == level && lp + hl < e && (lp[hl] == ' ' || lp[hl] == '\n')) {
			/* same level: strip prefix */
			lp += hl;
			if (lp < e && *lp == ' ') lp++;
		} else {
			lp = line;
		}
		q = trim_end(lp, eol(line, e));
		for (; lp < q; lp++) {
			ADDC(buf, blen) = *lp;
			blen++;
		}
		line = eol(line, e);
	}
	while (blen > 0 && isws(buf[blen-1]))
		blen--;
	ADDC(buf, blen) = '\0';

	{
		char hid[256];
		if (pending_id[0]) {
			snprintf(hid, sizeof(hid), "%s", pending_id);
		} else if (blen > 0) {
			make_slug(buf, blen, hid, sizeof(hid));
		} else {
			snprintf(hid, sizeof(hid), "s-%d", nsections + 1);
		}
		dedup_id(hid, sizeof(hid));

		if (!in_container) {
			close_sections(level);
			printf("<section id=\"%s\">\n", hid);
			if (nsections < 6) sections[nsections++] = level;
		}
		printf("<h%d", level);
		if (in_container)
			printf(" id=\"%s\"", hid);
		if (pending_class[0])
			printf(" class=\"%s\"", pending_class);
		fputc('>', stdout);
		clear_pending();
	}
	process(buf, buf + blen, 0);
	printf("</h%d>\n", level);
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
				for (; p < q; p++) { ADDC(buf, i) = *p; i++; }
				in_para = !blank;
			} else if (isblankline(line, eol(line, e))) {
				break;
			} else if (in_para) {
				/* lazy continuation */
				q = eol(line, e);
				for (p = line; p < q; p++) { ADDC(buf, i) = *p; i++; }
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
		fputs("<blockquote", stdout);
		if (has_pending()) {
			emit_attrs(pending_id, pending_class, pending_attrs);
			clear_pending();
		}
		fputs(">\n", stdout);
		process(buf, buf + i, 1);
		fputs("</blockquote>\n", stdout);
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
					fwrite(p, 1, line - p, stdout);
				} else if (!is_raw) {
					emit_code_open(info, infoend);
					emit_fence_lines(p, line, indent);
					fputs("</code></pre>\n", stdout);
				}
				/* other raw formats: silently drop */
				return -(eol(line, e) - b);
			}
			line = eol(line, e);
		}
		/* unclosed: treat rest as code */
		if (is_raw && rawfmtlen == 4 && !memcmp(rawfmt, "html", 4)) {
			fwrite(p, 1, e - p, stdout);
		} else if (!is_raw) {
			emit_code_open(info, infoend);
			emit_fence_lines(p, e, indent);
			fputs("</code></pre>\n", stdout);
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
	fputs("<hr", stdout);
	if (has_pending()) {
		emit_attrs(pending_id, pending_class, pending_attrs);
		clear_pending();
	}
	fputs(">\n", stdout);
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
			while (r < q && isdigit((unsigned char)*r))
				num = num * 10 + (*r++ - '0');
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
		while (p < e && isdigit((unsigned char)*p))
			num = num * 10 + (*p++ - '0');
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
		if (is_task) {
			if (pending_class[0]) {
				int cn = strlen(pending_class);
				if (cn + 10 < (int)sizeof(pending_class)) {
					memmove(pending_class + 10, pending_class, cn + 1);
					memcpy(pending_class, "task-list ", 10);
				}
			} else {
				snprintf(pending_class, sizeof(pending_class), "task-list");
			}
		}
		fputs("<ul", stdout);
		if (has_pending()) {
			emit_attrs(pending_id, pending_class, pending_attrs);
			clear_pending();
		} else if (is_task) {
			fputs(" class=\"task-list\"", stdout);
		}
		fputs(">\n", stdout);
	} else {
		fputs("<ol", stdout);
		if (start_num != 1) printf(" start=\"%d\"", start_num);
		fputs(typattr[style], stdout);
		if (has_pending()) {
			emit_attrs(pending_id, pending_class, pending_attrs);
			clear_pending();
		}
		fputs(">\n", stdout);
	}

	loose = 0;
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
		for (; p < q; p++) { ADDC(buf, i) = *p; i++; }
		line = q;

		while (line < e) {
			if (isblankline(line, eol(line, e))) {
				had_blank = 1;
				ADDC(buf, i) = '\n';
				i++;
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
				for (p = line + strip; p < q; p++) {
					ADDC(buf, i) = *p;
					i++;
				}
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
					for (p = line + strip; p < q; p++) {
						ADDC(buf, i) = *p;
						i++;
					}
				}
				line = q;
				continue;
			}
			/* after blank: allow lazy if last content was a sub-list */
			{
				const char *lb = buf + i;
				/* skip trailing blanks in buffer */
				while (lb > buf && lb[-1] == '\n') lb--;
				/* find start of last line */
				{
					const char *ls = lb;
					while (ls > buf && ls[-1] != '\n') ls--;
					int st2;
					char d2, m2;
					if (scan_marker(ls + spaces(ls, lb), lb,
					    &st2, &(int){0}, &d2, &m2)) {
						/* last content is sub-list: lazy OK */
						q = eol(line, e);
						for (p = line; p < q; p++) {
							ADDC(buf, i) = *p;
							i++;
						}
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
			fputs("<li>\n", stdout);
			/* task list checkbox */
			if (is_task && i >= 3 && buf[0] == '['
			    && (buf[1] == ' ' || buf[1] == 'x')
			    && buf[2] == ']') {
				int checked = (buf[1] == 'x');
				printf("<input disabled=\"\" type=\"checkbox\"%s/>\n",
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
			if (!loose) {
				const char *sp2;
				for (sp2 = buf; sp2 < buf + i; ) {
					const char *le = eol(sp2, buf + i);
					if (isblankline(sp2, le)) {
						const char *after = skip_blanks(le, buf + i);
						if (after < buf + i) {
							int st2;
							char d2, m2;
							const char *np = after + spaces(after, buf + i);
							if (!scan_marker(np, buf + i, &st2, &(int){0}, &d2, &m2))
								loose = 1; /* text after blank = loose */
						}
						break;
					}
					sp2 = le;
				}
			}
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
					fputc('\n', stdout);
					/* post-blank: block */
					process(split, be, 1);
				} else {
					process(buf, buf + i, 1);
				}
			}
			tight = save_tight;
			in_container = save_cont;
		}
		fputs("</li>\n", stdout);
		free(buf);
		buf = NULL;
		had_blank = 0;
	}
	}

	fputs(style == 0 ? "</ul>\n" : "</ol>\n", stdout);
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

	/* pre-process: transform word{attrs} into [word]{attrs} */
	{
		char *nbuf = NULL;
		int nlen = 0, ncap = 0;
		const char *s = b;
		int transformed = 0;

		while (s < p) {
			if (*s == '\\' && s + 1 < p) {
				/* copy escape + next char */
				if (nlen + 2 > ncap) { ncap = (ncap + 2) * 2; nbuf = realloc(nbuf, ncap); }
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
					if (nlen >= ncap) { ncap = (ncap + 1) * 2; nbuf = realloc(nbuf, ncap); }
					nbuf[nlen++] = *s++;
				}
				continue;
			}
			if (*s == '{' && !(s > b && s[-1] == '\\') && !(s > b && s[-1] == ']')) {
				int preceded_by_word = (s > b && s[-1] != ' ' && s[-1] != '\n'
				    && s[-1] != '\t');
				/* check if this looks like {attrs} (not {_ {* etc.) */
				if (s + 1 < p && (s[1] == '#' || s[1] == '.'
				    || s[1] == '%' || isalpha((unsigned char)s[1]))) {
					/* find matching } */
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
						char tid[128], tcls[128], textra[256];
						if (parse_attrs(s + 1, q, tid, sizeof(tid),
						    tcls, sizeof(tcls), textra, sizeof(textra))
						    || (q == s + 1)) {
							if (preceded_by_word) {
								/* found valid inline attrs — find word start */
								int wstart = nlen;
								/* walk back past non-space content */
								while (wstart > 0 && nbuf[wstart-1] != ' '
								    && nbuf[wstart-1] != '\n'
								    && nbuf[wstart-1] != '\t'
								    && nbuf[wstart-1] != '>')
									wstart--;
								/* insert [ before word */
								if (nlen + 2 > ncap) {
									ncap = (ncap + 2) * 2;
									nbuf = realloc(nbuf, ncap);
								}
								memmove(nbuf + wstart + 1, nbuf + wstart, nlen - wstart);
								nbuf[wstart] = '[';
								nlen++;
								/* add ] before {attrs} */
								if (nlen >= ncap) { ncap = (ncap + 1) * 2; nbuf = realloc(nbuf, ncap); }
								nbuf[nlen++] = ']';
								/* copy {attrs} verbatim */
								while (s <= q) {
									if (nlen >= ncap) { ncap = (ncap + 1) * 2; nbuf = realloc(nbuf, ncap); }
									nbuf[nlen++] = *s++;
								}
							} else {
								/* attrs preceded by space/start — consume silently */
								s = q + 1;
							}
							transformed = 1;
							continue;
						}
					}
				}
				/* also handle {} — consume silently */
				if (s + 1 < p && s[1] == '}') {
					s += 2;
					transformed = 1;
					continue;
				}
			}
			if (nlen >= ncap) { ncap = (ncap + 1) * 2; nbuf = realloc(nbuf, ncap); }
			nbuf[nlen++] = *s++;
		}

		if (!tight) {
			fputs("<p", stdout);
			if (has_pending()) {
				if (pending_id[0])
					dedup_id(pending_id, sizeof(pending_id));
				emit_attrs(pending_id, pending_class, pending_attrs);
			}
			clear_pending();
			fputc('>', stdout);
		}
		if (transformed) {
			process(nbuf, nbuf + nlen, 0);
		} else {
			process(b, p, 0);
		}
		free(nbuf);
	}
	if (!tight) fputs("</p>", stdout);
	fputc('\n', stdout);
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
			fputs("<br>\n", stdout);
			return p + 1 - b;
		}
		if (b + 1 < e && b[1] == ' ') {
			fputs("&nbsp;", stdout);
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
		fputc('\n', stdout);
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
			/* trim single space from each end (single backtick only) */
			if (count == 1 && codelen >= 2
			    && code[0] == ' ' && code[codelen-1] == ' ') {
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
						fwrite(code, 1, codelen, stdout);
					}
					/* other formats: silently drop */
					return fe + 1 - start;
				}
			}
			if (math) {
				printf("<span class=\"math %s\">%s",
				    math == 1 ? "inline" : "display",
				    math == 1 ? "\\(" : "\\[");
				hprint(code, code + codelen);
				fputs(math == 1 ? "\\)" : "\\]", stdout);
				fputs("</span>", stdout);
			} else {
				fputs("<code>", stdout);
				hprint(code, code + codelen);
				fputs("</code>", stdout);
			}
			return p + count - start;
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
	fputs("<code>", stdout);
	hprint(code, code + codelen);
	fputs("</code>", stdout);
	return e - start;
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
	if (!explicit_open) {
		int run = leadc(b, e, ch);
		if (run > 1) {
			const char *rp = b + run;
			while (rp + run <= e) {
				if (*rp != ch) { rp++; continue; }
				int crun = leadc(rp, e, ch);
				if (crun >= run) {
					char bb = (rp > b + run) ? rp[-1] : 0;
					if (!isws(bb) && rp > b + run) {
						int j;
						for (j = 0; j < run; j++)
							fputs(ch == '_' ? "<em>" : ch == '*' ? "<strong>"
							    : ch == '~' ? "<sub>" : "<sup>", stdout);
						process(b + run, rp, 0);
						for (j = 0; j < run; j++)
							fputs(ch == '_' ? "</em>" : ch == '*' ? "</strong>"
							    : ch == '~' ? "</sub>" : "</sup>", stdout);
						return rp + run - b;
					}
				}
				rp += crun;
			}
		}
	}

	/* find matching close (single delimiter) */
	start = b + 1 + consumed_open;
	for (p = start; p < e; p++) {
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
			int explicit_close = (p + 1 < e && p[1] == '}');
			int consumed_close = explicit_close ? 1 : 0;

			/* explicit openers only match explicit closers */
			if (explicit_open && !explicit_close) continue;
			if (!explicit_open && explicit_close) continue;

			/* djot closer rule: char before marker is not whitespace
			 * (explicit closers bypass this rule) */
			if (!explicit_close) {
				char bb = (p > start) ? p[-1] : 0;
				if (isws(bb)) continue;
			}
			if (p == start) continue; /* no empty emphasis */
			/* reject if content is all delimiter chars */
			if (!explicit_open) {
				const char *t;
				int alldelim = 1;
				for (t = start; t < p; t++)
					if (*t != ch) { alldelim = 0; break; }
				if (alldelim) continue;
			}
			stop = p;
			{
				const char *otag, *ctag;
				if (ch == '_')      { otag = "<em>"; ctag = "</em>"; }
				else if (ch == '*') { otag = "<strong>"; ctag = "</strong>"; }
				else if (ch == '~') { otag = "<sub>"; ctag = "</sub>"; }
				else if (ch == '^') { otag = "<sup>"; ctag = "</sup>"; }
				else if (ch == '+') { otag = "<ins>"; ctag = "</ins>"; }
				else if (ch == '-') { otag = "<del>"; ctag = "</del>"; }
				else if (ch == '=') { otag = "<mark>"; ctag = "</mark>"; }
				else                { otag = "<em>"; ctag = "</em>"; }
				fputs(otag, stdout);
				process(start, stop, 0);
				fputs(ctag, stdout);
			}
			return stop + 1 + consumed_close - b;
		}
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
		}
		hprint(b, b + 1);
		next:;
	}
}

static void
emit_url(const char *b, const char *e)
{
	for (; b < e; b++) {
		if (*b == '\n' || *b == '\r') continue;
		if (*b == '\\' && b + 1 < e && isasciipunct(b[1])) {
			b++;
			if (*b == '&') fputs("&amp;", stdout);
			else if (*b == '"') fputs("&quot;", stdout);
			else fputc(*b, stdout);
			continue;
		}
		if (*b == '&') fputs("&amp;", stdout);
		else if (*b == '"') fputs("&quot;", stdout);
		else fputc(*b, stdout);
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
			if (found < 0 && nfootnotes < (int)LEN(footnotes)) {
				found = nfootnotes;
				footnotes[found].label = fl;
				footnotes[found].labellen = fe - fl;
				footnotes[found].content = NULL;
				footnotes[found].contentlen = 0;
				footnotes[found].used = 0;
				footnotes[found].num = 0;
				nfootnotes++;
			}
			if (found >= 0) {
				footnotes[found].used = 1;
				if (!footnotes[found].num)
					footnotes[found].num = ++footnote_counter;
				int num = footnotes[found].num;
				printf("<a id=\"fnref%d\" href=\"#fn%d\" role=\"doc-noteref\"><sup>%d</sup></a>",
				    num, num, num);
				return fe + 1 - b;
			}
		}
	}

	depth = 1;
	text = p + 1;
	for (q = text; q < e; q++) {
		if (*q == '\\' && q + 1 < e) { q++; continue; }
		if (*q == '[') depth++;
		if (*q == ']' && --depth == 0) break;
	}
	if (q >= e) return 0;
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
			fputs("<img alt=\"", stdout);
			altprint(text, textend);
			fputs("\" src=\"", stdout);
			emit_url(dest, destend);
			fputs("\">", stdout);
		} else {
			fputs("<a href=\"", stdout);
			emit_url(dest, destend);
			fputs("\">", stdout);
			process(text, textend, 0);
			fputs("</a>", stdout);
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
				char inline_attr[256] = {0};
				const char *after_ref = q + 1;
				if (after_ref < e && *after_ref == '{') {
					const char *ab = after_ref + 1;
					const char *ae = ab;
					while (ae < e && *ae != '}' && *ae != '\n') ae++;
					if (ae < e && *ae == '}') {
						char aid[1], acls[1];
						parse_attrs(ab, ae, aid, 1, acls, 1,
						    inline_attr, sizeof(inline_attr));
						q = ae; /* consume the {attrs} */
					}
				}
				rattr = inline_attr[0] ? inline_attr : refs[ri-1].attrs;
				if (img) {
					fputs("<img alt=\"", stdout);
					altprint(text, textend);
					fputs("\" src=\"", stdout);
					emit_url(url, url + urllen);
					fputc('"', stdout);
					if (rattr[0]) printf(" %s", rattr);
					fputs(">", stdout);
				} else {
					fputs("<a href=\"", stdout);
					emit_url(url, url + urllen);
					fputc('"', stdout);
					if (rattr[0]) printf(" %s", rattr);
					fputs(">", stdout);
					process(text, textend, 0);
					fputs("</a>", stdout);
				}
				return q + 1 - b;
			}
		}
		/* no matching ref: render as link without href */
		if (img) {
			fputs("<img alt=\"", stdout);
			altprint(text, textend);
			fputs("\">", stdout);
		} else {
			fputs("<a>", stdout);
			process(text, textend, 0);
			fputs("</a>", stdout);
		}
		return q + 1 - b;
	}
	/* span syntax [text]{.class #id ...} — may span lines */
	if (q < e && *q == '{') {
		const char *ab = q + 1;
		const char *ae = ab;
		while (ae < e && *ae != '}') ae++;
		if (ae < e && *ae == '}') {
			char sid[128], scls[128], sextra[256];
			parse_attrs(ab, ae, sid, sizeof(sid),
			    scls, sizeof(scls), sextra, sizeof(sextra));
			fputs("<span", stdout);
			emit_attrs(sid, scls, sextra);
			fputc('>', stdout);
			process(text, textend, 0);
			fputs("</span>", stdout);
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
	if (memchr(b + 1, '@', p - b - 1) && !memchr(b + 1, ':', p - b - 1)) {
		fputs("<a href=\"mailto:", stdout);
		hprint(b + 1, p);
		fputs("\">", stdout);
		hprint(b + 1, p);
		fputs("</a>", stdout);
	} else {
		fputs("<a href=\"", stdout);
		emit_url(b + 1, p);
		fputs("\">", stdout);
		hprint(b + 1, p);
		fputs("</a>", stdout);
	}
	return p + 1 - b;
}

static const char *smartreplace[][2] = {
	{ "...",  "\xe2\x80\xa6" },
};

static int
doreplace(const char *b, const char *e, int n)
{
	unsigned int i, l;
	int run, em, en;
	char before, after;
	int can_open, can_close;
	if (n) return 0;

	if (*b == '-' && b + 1 < e && b[1] == '-') {
		run = leadc(b, e, '-');
		em = 0; en = 0;
		if (run % 3 == 0)      { em = run / 3; }
		else if (run % 3 == 1) { en = 2; em = (run - 4) / 3; }
		else if (run % 2 == 0) { en = run / 2; }
		else                   { en = 1; em = (run - 2) / 3; }
		while (em-- > 0) fputs("\xe2\x80\x94", stdout);
		while (en-- > 0) fputs("\xe2\x80\x93", stdout);
		return run;
	}

	for (i = 0; i < LEN(smartreplace); i++) {
		l = strlen(smartreplace[i][0]);
		if ((int)(e - b) >= (int)l && !strncmp(b, smartreplace[i][0], l)) {
			fputs(smartreplace[i][1], stdout);
			return l;
		}
	}

	/* explicit quote markers: {' → left, '} → right */
	if (*b == '{' && b + 1 < e && (b[1] == '\'' || b[1] == '"')) {
		int dbl = (b[1] == '"');
		fputs(dbl ? "\xe2\x80\x9c" : "\xe2\x80\x98", stdout);
		return 2;
	}

	if (*b == '"' || *b == '\'') {
		before = (b > proc_base) ? *(b - 1) : 0;
		after = (b + 1 < e) ? b[1] : 0;
		/* explicit closer: '} or "} */
		if (after == '}') {
			fputs(*b == '"' ? "\xe2\x80\x9d" : "\xe2\x80\x99", stdout);
			return 2; /* consume the } too */
		}
		/* ' before digit or after ] is always apostrophe */
		if (*b == '\'' && (isdigit((unsigned char)after) || before == ']')) {
			fputs("\xe2\x80\x99", stdout);
			return 1;
		}
		can_open = !isws(after) && (isws(before) || isasciipunct(before) || before == 0);
		can_close = !isws(before) && (isws(after) || isasciipunct(after) || after == 0);
		if (*b == '"') {
			fputs(can_open && !can_close ? "\xe2\x80\x9c" : "\xe2\x80\x9d", stdout);
		} else if (can_close && !can_open) {
			fputs("\xe2\x80\x99", stdout);
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
			fputs(stack == 0 ? "\xe2\x80\x98" : "\xe2\x80\x99", stdout);
		} else {
			fputs("\xe2\x80\x99", stdout); /* intra-word: apostrophe */
		}
		return 1;
	}

	/* spaces before hard break: consume "ws \ ws newline" as <br> */
	if ((*b == ' ' || *b == '\t') && !n) {
		const char *q = b;
		while (q < e && (*q == ' ' || *q == '\t')) q++;
		if (q < e && *q == '\\') {
			const char *r = q + 1;
			while (r < e && (*r == ' ' || *r == '\t')) r++;
			if (r < e && (*r == '\n' || *r == '\r')) {
				if (*r == '\r' && r + 1 < e && r[1] == '\n') r++;
				fputs("<br>\n", stdout);
				return r + 1 - b;
			}
		}
	}

	if (*b == '&') { fputs("&amp;", stdout); return 1; }
	if (*b == '<') { fputs("&lt;", stdout); return 1; }
	if (*b == '>') { fputs("&gt;", stdout); return 1; }

	return 0;
}

static void
process(const char *b, const char *e, int newblock)
{
	const char *p;
	const char *save_base = proc_base;
	int affected;
	unsigned int i;

	proc_base = b;
	for (p = b; p < e; ) {
		if (newblock) {
			int had_blank = 0;
			while (p < e && *p == '\n') {
				had_blank = 1;
				if (++p == e) return;
			}
			if (had_blank && has_pending())
				clear_pending();
		}

		for (i = 0; i < LEN(parsers); i++)
			if ((affected = parsers[i](p, e, newblock)))
				break;

		if (affected) {
			p += abs(affected);
		} else {
			fputc(*p++, stdout);
		}

		if (p < e && p[0] == '\n' && p + 1 < e && p[1] == '\n')
			newblock = 1;
		else
			newblock = affected < 0;
	}
	proc_base = save_base;
}

static char urlbuf[4096];
static int urlbuflen;

static void
urlbuf_add(const char *b, const char *e)
{
	while (b < e && urlbuflen < (int)sizeof(urlbuf) - 1) {
		if (*b != '\n' && *b != '\r' && *b != ' ' && *b != '\t')
			urlbuf[urlbuflen++] = *b;
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
	struct { const char *content; int len; const char *line; } hdefs[128];
	int nhdefs = 0;

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
					for (; fp < cend; fp++) {
						ADDC(fnbuf, fni) = *fp;
						fni++;
					}
				}
				line = eol(line, e);
				/* collect continuation lines (blank or indented by 2+) */
				while (line < e) {
					if (isblankline(line, eol(line, e))) {
						ADDC(fnbuf, fni) = '\n';
						fni++;
						line = eol(line, e);
						continue;
					}
					int csp = spaces(line, e);
					if (csp < 2) break;
					if (fni > 0) {
						ADDC(fnbuf, fni) = '\n';
						fni++;
					}
					const char *le = eol(line, e);
					const char *lp = line + 2;
					const char *lend = trim_end(lp, le);
					for (; lp < lend; lp++) {
						ADDC(fnbuf, fni) = *lp;
						fni++;
					}
					line = eol(line, e);
				}
				while (fni > 0 && fnbuf[fni-1] == '\n') fni--;
				ADDC(fnbuf, fni) = '\0';
				if (ll > 0 && nfootnotes < (int)LEN(footnotes)) {
					footnotes[nfootnotes].label = fl;
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
						const char *ue = p;
						while (ue < e && *ue != '\n') ue++;
						urlbuf_add(p, ue);
					}
					const char *nextline = eol(line, e);
					while (nextline < e) {
						int ns = spaces(nextline, e);
						if (ns == 0 || isblankline(nextline, eol(nextline, e)))
							break;
						const char *le = eol(nextline, e);
						urlbuf_add(nextline + ns, le);
						nextline = le;
					}
					if (labellen > 0 && nrefs < (int)LEN(refs)) {
						char *u = malloc(urlbuflen + 1);
						if (u) {
							memcpy(u, urlbuf, urlbuflen);
							u[urlbuflen] = '\0';
						}
						refs[nrefs].label = label;
						refs[nrefs].labellen = labellen;
						refs[nrefs].url = u;
						refs[nrefs].urllen = urlbuflen;
						refs[nrefs].attrs[0] = '\0';
						prev_line_attrs(line, b,
						    &(char){0}, 1, &(char){0}, 1,
						    refs[nrefs].attrs, sizeof(refs[nrefs].attrs));
						nrefs++;
					}
					line = eol(line, e);
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
				if (cend > content && nhdefs < (int)LEN(hdefs)) {
					hdefs[nhdefs].content = content;
					hdefs[nhdefs].len = cend - content;
					hdefs[nhdefs].line = line;
					nhdefs++;
				}
			}
		}

		line = eol(line, e);
	}

	/* register heading auto-refs (skipping headings with explicit refs) */
	{
		int hi;
		for (hi = 0; hi < nhdefs && nrefs < (int)LEN(refs); hi++) {
			if (findref(hdefs[hi].content, hdefs[hi].len,
			    &(const char *){0}, &(int){0}))
				continue;
			char custom_id[128] = {0};
			prev_line_attrs(hdefs[hi].line, b,
			    custom_id, sizeof(custom_id),
			    &(char){0}, 1, &(char){0}, 1);
			char idbuf[256];
			int idn;
			if (custom_id[0]) {
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
					refs[nrefs].label = hdefs[hi].content;
					refs[nrefs].labellen = hdefs[hi].len;
					refs[nrefs].url = u;
					refs[nrefs].urllen = ulen;
					nrefs++;
				}
			}
		}
	}
}

static void
emit_endnotes(void)
{
	int i, any = 0;

	for (i = 0; i < nfootnotes; i++)
		if (footnotes[i].used) { any = 1; break; }
	if (!any) return;

	/* sort by sequential number to emit in reference order */
	fputs("<section role=\"doc-endnotes\">\n<hr>\n<ol>\n", stdout);
	{
		int num;
		for (num = 1; num <= footnote_counter; num++) {
			for (i = 0; i < nfootnotes; i++)
				if (footnotes[i].used && footnotes[i].num == num)
					break;
			if (i >= nfootnotes) continue;

			printf("<li id=\"fn%d\">\n", num);
			if (footnotes[i].contentlen > 0) {
				/* check if content has block elements (blank line or code fence) */
				int has_blocks = 0;
				{
					const char *sp;
					for (sp = footnotes[i].content; sp < footnotes[i].content + footnotes[i].contentlen; ) {
						const char *le = eol(sp, footnotes[i].content + footnotes[i].contentlen);
						if (isblankline(sp, le)) { has_blocks = 1; break; }
						/* code fence? */
						const char *tp = sp;
						while (tp < le && *tp == ' ') tp++;
						if (tp < le && (*tp == '`' || *tp == '~') && leadc(tp, le, *tp) >= 3)
							{ has_blocks = 1; break; }
						sp = le;
					}
				}
				if (has_blocks) {
					/* find last paragraph in content and process in two parts:
					 * everything before last para, then last para with backlink */
					const char *fc = footnotes[i].content;
					int fcl = footnotes[i].contentlen;
					const char *lastpara = fc;
					const char *sp;
					/* find the start of the last paragraph */
					for (sp = fc; sp < fc + fcl; ) {
						const char *le = eol(sp, fc + fcl);
						if (isblankline(sp, le)) {
							const char *after = skip_blanks(le, fc + fcl);
							if (after < fc + fcl)
								lastpara = after;
						}
						sp = le;
					}
					{
						int save_cont = in_container;
						in_container = 1;
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
							printf("<p><a href=\"#fnref%d\" role=\"doc-backlink\">\xe2\x86\xa9\xef\xb8\x8e</a></p>\n", num);
						} else {
							/* last paragraph: emit inline with backlink */
							const char *pe = trim_end(lastpara, fc + fcl);
							while (lastpara < pe && (*lastpara == ' ' || *lastpara == '\t'))
								lastpara++;
							fputs("<p>", stdout);
							process(lastpara, pe, 0);
							printf("<a href=\"#fnref%d\" role=\"doc-backlink\">\xe2\x86\xa9\xef\xb8\x8e</a>", num);
							fputs("</p>\n", stdout);
						}
						in_container = save_cont;
					}
				} else {
					fputs("<p>", stdout);
					process(footnotes[i].content,
					    footnotes[i].content + footnotes[i].contentlen, 0);
					printf("<a href=\"#fnref%d\" role=\"doc-backlink\">\xe2\x86\xa9\xef\xb8\x8e</a>", num);
					fputs("</p>\n", stdout);
				}
			} else {
				printf("<p><a href=\"#fnref%d\" role=\"doc-backlink\">\xe2\x86\xa9\xef\xb8\x8e</a></p>\n", num);
			}
			fputs("</li>\n", stdout);
		}
	}
	fputs("</ol>\n</section>\n", stdout);
}

int
main(void)
{
	char *buf = NULL;
	int len = 0, cap = 0;
	int n;

	do {
		cap += BUFSIZ;
		buf = realloc(buf, cap);
		if (!buf) die("malloc");
		n = fread(buf + len, 1, cap - len, stdin);
		len += n;
	} while (n > 0);

	prescan(buf, buf + len);
	process(buf, buf + len, 1);

	close_sections(0);
	emit_endnotes();

	free(buf);
	for (n = 0; n < nrefs; n++)
		free((char *)refs[n].url);
	for (n = 0; n < nused_ids; n++)
		free(used_ids[n]);
	return 0;
}
