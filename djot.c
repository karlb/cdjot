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

/* forward declarations */
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

static Parser parsers[] = {
	dorefdef, doheading, doblockquote, docodefence, dothematicbreak,
	dolist, doparagraph,
	dolinebreak, docode, dosurround, dolink, doautolink, doreplace,
};

/* reference link definitions */
static struct { const char *label; int labellen; const char *url; int urllen; } refs[128];
static int nrefs;

/* heading section stack */
static int sections[6], nsections;

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

/* find end of current line, returns pointer past \n (or at end) */
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

/* count leading ch characters */
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

/* look up a reference definition */
static int
findref(const char *label, int len, const char **url, int *urllen)
{
	int i;
	for (i = 0; i < nrefs; i++)
		if (refs[i].labellen == len && !memcmp(refs[i].label, label, len)) {
			*url = refs[i].url;
			*urllen = refs[i].urllen;
			return 1;
		}
	return 0;
}

/* emit heading id: keep alnum/-/_, space/newline -> '-' */
static void
heading_id(const char *b, const char *e)
{
	/* trim */
	while (b < e && isws(*b)) b++;
	while (e > b && isws(e[-1])) e--;
	for (; b < e; b++) {
		if (*b == ' ' || *b == '\t' || *b == '\n')
			fputc('-', stdout);
		else if (isalnum((unsigned char)*b) || *b == '-' || *b == '_')
			fputc(*b, stdout);
	}
}

static void
close_sections(int level)
{
	while (nsections > 0 && sections[nsections-1] >= level) {
		fputs("</section>\n", stdout);
		nsections--;
	}
}

/* --- block parsers --- */

static int
dorefdef(const char *b, const char *e, int n)
{
	const char *p;

	if (!n) return 0;
	p = b;
	while (p < e && *p == ' ') p++;
	if (p >= e || *p != '[') return 0;
	p++;
	while (p < e && *p != ']' && *p != '\n') p++;
	if (p >= e || *p != ']') return 0;
	p++;
	if (p >= e || *p != ':') return 0;
	/* consume the whole line */
	return -(eol(b, e) - b);
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
	/* trim trailing # and whitespace from first line */
	content = p;
	q = eol(p, e) - 1; /* before \n */
	if (q > content && *q == '\n') q--;
	while (q > content && *q == ' ') q--;
	while (q > content && *q == '#') q--;
	while (q > content && *q == ' ') q--;
	cend = q + 1;

	/* collect continuation lines into buf */
	buf = NULL;
	blen = 0;
	/* first line */
	while (content < cend && (*content == ' ' || *content == '\t'
	    || *content == '\n' || *content == '\r'))
		content++;
	for (q = content; q < cend; q++) {
		ADDC(buf, blen) = *q;
		blen++;
	}
	/* continuation: lines until blank or different heading level */
	line = eol(b, e);
	while (line < e && !isblankline(line, eol(line, e))) {
		const char *lp = line;
		int lsp = spaces(lp, e);
		lp += lsp;
		hl = leadc(lp, e, '#');
		if (hl >= 1 && hl <= 6 && hl != level && lp + hl < e
		    && (lp[hl] == ' ' || lp[hl] == '\n'))
			break; /* different heading level */
		ADDC(buf, blen) = '\n';
		blen++;
		if (hl == level && lp + hl < e && (lp[hl] == ' ' || lp[hl] == '\n')) {
			/* same level: strip prefix */
			lp += hl;
			if (lp < e && *lp == ' ') lp++;
		} else {
			lp = line;
		}
		q = eol(line, e);
		while (q > lp && (q[-1] == '\n' || q[-1] == '\r'
		    || q[-1] == ' ' || q[-1] == '\t'))
			q--;
		for (; lp <= q; lp++) {
			ADDC(buf, blen) = *lp;
			blen++;
		}
		line = eol(line, e);
	}
	/* trim trailing whitespace from heading content */
	while (blen > 0 && (buf[blen-1] == '\n' || buf[blen-1] == '\r'
	    || buf[blen-1] == ' ' || buf[blen-1] == '\t'))
		blen--;
	ADDC(buf, blen) = '\0';

	close_sections(level);
	fputs("<section id=\"", stdout);
	if (blen > 0)
		heading_id(buf, buf + blen);
	else
		printf("s-%d", nsections + 1);
	fputs("\">\n", stdout);
	if (nsections < 6) sections[nsections++] = level;

	printf("<h%d>", level);
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

	/* collect lines: continuation while '> ' prefix or lazy */
	buf = NULL;
	i = 0;
	line = b;
	{
		int in_para = 0;
		while (line < e) {
			p = line;
			sp = spaces(p, e);
			p += sp;
			if (p < e && *p == '>') {
				p++;
				if (p < e && *p != ' ' && *p != '\n') break;
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
	fputs("<blockquote>\n", stdout);
	process(buf, buf + i, 1);
	fputs("</blockquote>\n", stdout);
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
	/* info string */
	while (p < e && (*p == ' ' || *p == '\t')) p++;
	info = p;
	while (p < e && *p != '\n') p++;
	infoend = p;
	while (infoend > info && (infoend[-1] == ' ' || infoend[-1] == '\t'))
		infoend--;
	/* backtick fence: info must not contain backticks */
	if (fch == '`') {
		for (q = info; q < infoend; q++)
			if (*q == '`') return 0;
	}
	if (p < e) p++; /* skip \n */

	/* find closing fence */
	line = p;
	while (line < e) {
		q = line;
		int s = spaces(q, e);
		q += s;
		int cl = leadc(q, e, fch);
		if (cl >= flen && isblankline(q + cl, eol(line, e))) {
			/* found closing fence */
			if (info < infoend) {
				fputs("<pre><code class=\"language-", stdout);
				hprint(info, infoend);
				fputs("\">", stdout);
			} else {
				fputs("<pre><code>", stdout);
			}
			/* print content, stripping indent */
			for (q = p; q < line; ) {
				const char *le = eol(q, line);
				int strip = spaces(q, le);
				if (strip > indent) strip = indent;
				hprint(q + strip, le);
				q = le;
			}
			fputs("</code></pre>\n", stdout);
			return -(eol(line, e) - b);
		}
		line = eol(line, e);
	}
	/* unclosed: treat rest as code */
	if (info < infoend) {
		fputs("<pre><code class=\"language-", stdout);
		hprint(info, infoend);
		fputs("\">", stdout);
	} else {
		fputs("<pre><code>", stdout);
	}
	for (q = p; q < e; ) {
		const char *le = eol(q, e);
		int strip = spaces(q, le);
		if (strip > indent) strip = indent;
		hprint(q + strip, le);
		q = le;
	}
	fputs("</code></pre>\n", stdout);
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
	fputs("<hr>\n", stdout);
	return -(eol(b, e) - b);
}

static int
dolist(const char *b, const char *e, int n)
{
	const char *p, *q, *line;
	char *buf;
	int i, sp, indent, ordered, start_num, loose, had_blank;
	char marker;

	if (!n) return 0;
	p = b;
	sp = spaces(p, e);
	if (sp > 3) return 0;
	p += sp;

	/* detect marker */
	ordered = 0;
	marker = *p;
	if (marker == '-' || marker == '*' || marker == '+') {
		p++;
	} else if (isdigit((unsigned char)*p)) {
		start_num = 0;
		while (p < e && isdigit((unsigned char)*p))
			start_num = start_num * 10 + (*p++ - '0');
		if (p >= e || (*p != '.' && *p != ')')) return 0;
		marker = *p++;
		ordered = 1;
	} else {
		return 0;
	}
	if (p >= e || (*p != ' ' && *p != '\t' && *p != '\n')) return 0;
	if (p < e && *p == ' ') p++;
	indent = p - b;

	/* open list */
	if (ordered) {
		if (start_num != 1)
			printf("<ol start=\"%d\">\n", start_num);
		else
			fputs("<ol>\n", stdout);
	} else {
		fputs("<ul>\n", stdout);
	}

	/* parse items */
	loose = 0;
	had_blank = 0;
	line = b;
	while (line < e) {
		/* verify this line starts with the same marker */
		p = line;
		sp = spaces(p, e);
		p += sp;
		if (!ordered) {
			if (*p != marker) break;
		} else {
			while (p < e && isdigit((unsigned char)*p)) p++;
			if (p >= e || *p != marker) break;
		}
		/* collect one item's content */
		buf = NULL;
		i = 0;
		/* first line: skip marker */
		p = line + indent;
		q = eol(line, e);
		for (; p < q; p++) { ADDC(buf, i) = *p; i++; }
		line = q;

		/* continuation lines */
		while (line < e) {
			if (isblankline(line, eol(line, e))) {
				had_blank = 1;
				ADDC(buf, i) = '\n';
				i++;
				line = eol(line, e);
				continue;
			}
			sp = spaces(line, e);
			p = line + sp;
			/* new item at same level? */
			if (sp < indent) {
				/* check if it's a matching marker */
				if (!ordered && *p == marker && p + 1 < e
				    && (p[1] == ' ' || p[1] == '\n'))
					break;
				if (ordered && isdigit((unsigned char)*p)) {
					q = p;
					while (q < e && isdigit((unsigned char)*q)) q++;
					if (q < e && *q == marker && q + 1 < e
					    && (q[1] == ' ' || q[1] == '\n'))
						break;
				}
				/* sub-list after blank? */
				if (had_blank && sp > 0) {
					/* indented content: sub-content */
					q = eol(line, e);
					for (p = line + sp; p < q; p++) {
						ADDC(buf, i) = *p;
						i++;
					}
					line = q;
					continue;
				}
				/* lazy continuation (no blank, in paragraph) */
				if (!had_blank) {
					/* don't lazy-continue if line is any list marker */
					if ((*p == '-' || *p == '*' || *p == '+')
					    && p + 1 < e && (p[1] == ' ' || p[1] == '\n'))
						break;
					if (isdigit((unsigned char)*p)) {
						q = p;
						while (q < e && isdigit((unsigned char)*q)) q++;
						if (q < e && (*q == '.' || *q == ')')
						    && q + 1 < e && (q[1] == ' ' || q[1] == '\n'))
							break;
					}
					q = eol(line, e);
					for (p = line; p < q; p++) {
						ADDC(buf, i) = *p;
						i++;
					}
					line = q;
					continue;
				}
				break;
			}
			/* fully indented continuation */
			q = eol(line, e);
			for (p = line + indent; p < q; p++) {
				ADDC(buf, i) = *p;
				i++;
			}
			line = q;
		}

		/* trim trailing blank lines from item */
		while (i > 0 && buf[i-1] == '\n') i--;
		ADDC(buf, i) = '\0';

		if (had_blank) loose = 1;

		fputs("<li>\n", stdout);
		if (loose)
			process(buf, buf + i, 1);
		else
			process(buf, buf + i, 0);
		fputc('\n', stdout);
		fputs("</li>\n", stdout);
		free(buf);
		buf = NULL;
		had_blank = 0;
	}

	fputs(ordered ? "</ol>\n" : "</ul>\n", stdout);
	return -(line - b);
}

static int
doparagraph(const char *b, const char *e, int n)
{
	const char *p, *end;

	if (!n) return 0;
	/* find end: blank line */
	end = b;
	while (end < e) {
		p = eol(end, e);
		if (isblankline(end, p))
			break;
		end = p;
	}
	/* trim trailing whitespace */
	p = end;
	while (p > b && (p[-1] == '\n' || p[-1] == '\r' || p[-1] == ' ' || p[-1] == '\t')) p--;
	fputs("<p>", stdout);
	process(b, p, 0);
	fputs("</p>\n", stdout);
	return -(end - b);
}

/* --- inline parsers --- */

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
		fputc('\n', stdout);
		return 1;
	}
	return 0;
}

static int
docode(const char *b, const char *e, int n)
{
	const char *p, *code;
	int count, run, codelen;

	if (n || *b != '`') return 0;
	count = leadc(b, e, '`');
	p = b + count;
	/* find matching close */
	while (p < e) {
		if (*p != '`') { p++; continue; }
		run = leadc(p, e, '`');
		if (run == count) {
			code = b + count;
			codelen = p - code;
			if (codelen >= 2 && code[0] == ' ' && code[codelen-1] == ' ') {
				code++;
				codelen -= 2;
			}
			fputs("<code>", stdout);
			hprint(code, code + codelen);
			fputs("</code>", stdout);
			return p + count - b;
		}
		p += run;
	}
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
	return e - b;
}

static int
dosurround(const char *b, const char *e, int n)
{
	const char *p, *start, *stop;
	char ch, after;

	if (n) return 0;
	ch = *b;
	if (ch != '_' && ch != '*') return 0;

	/* djot opener rule: character after marker is not whitespace */
	after = (b + 1 < e) ? b[1] : 0;
	if (isws(after)) return 0;

	/* find matching close */
	start = b + 1;
	for (p = start; p < e; p++) {
		if (*p == '\\' && p + 1 < e) { p++; continue; }
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
		if (*p == '[') { /* skip link syntax */
			int depth = 1;
			const char *q = p + 1;
			while (q < e && depth > 0) {
				if (*q == '\\' && q + 1 < e) { q += 2; continue; }
				if (*q == '[') depth++;
				if (*q == ']') depth--;
				q++;
			}
			if (q < e && *q == '(') {
				depth = 1; q++;
				while (q < e && depth > 0) {
					if (*q == '(') depth++;
					if (*q == ')') depth--;
					q++;
				}
			}
			p = q - 1;
			continue;
		}
		if (*p == ch) {
			/* djot closer rule: character before marker is not whitespace */
			char bb = (p > start) ? p[-1] : 0;
			if (isws(bb)) continue;
			stop = p;
			fputs(ch == '_' ? "<em>" : "<strong>", stdout);
			process(start, stop, 0);
			fputs(ch == '_' ? "</em>" : "</strong>", stdout);
			return stop + 1 - b;
		}
	}
	return 0;
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

	/* find ] */
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
		/* inline link */
		dest = q + 1;
		depth = 1;
		for (q = dest; q < e; q++) {
			if (*q == '\\' && q + 1 < e) { q++; continue; }
			if (*q == '(') depth++;
			if (*q == ')' && --depth == 0) break;
		}
		if (q >= e) return 0;
		destend = q;
		/* trim whitespace from dest */
		while (dest < destend && isws(*dest)) dest++;
		while (destend > dest && isws(destend[-1])) destend--;
		if (img) {
			fputs("<img alt=\"", stdout);
			hprint(text, textend);
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
		if (findref(label, labellen, &url, &urllen)) {
			if (img) {
				fputs("<img alt=\"", stdout);
				hprint(text, textend);
				fputs("\" src=\"", stdout);
				emit_url(url, url + urllen);
				fputs("\">", stdout);
			} else {
				fputs("<a href=\"", stdout);
				emit_url(url, url + urllen);
				fputs("\">", stdout);
				process(text, textend, 0);
				fputs("</a>", stdout);
			}
			return q + 1 - b;
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
	(void)e; /* may be unused */

	if (n) return 0;

	/* smart dashes */
	if (*b == '-' && b + 1 < e && b[1] == '-') {
		run = leadc(b, e, '-');
		em = 0; en = 0;
		if (run % 3 == 0)      { em = run / 3; }
		else if (run % 3 == 1) { en = 2; em = (run - 4) / 3; }
		else                   { en = 1; em = (run - 2) / 3; }
		while (em-- > 0) fputs("\xe2\x80\x94", stdout);
		while (en-- > 0) fputs("\xe2\x80\x93", stdout);
		return run;
	}

	/* smart replace table */
	for (i = 0; i < LEN(smartreplace); i++) {
		l = strlen(smartreplace[i][0]);
		if ((int)(e - b) >= (int)l && !strncmp(b, smartreplace[i][0], l)) {
			fputs(smartreplace[i][1], stdout);
			return l;
		}
	}

	/* smart quotes */
	if (*b == '"' || *b == '\'') {
		before = (b > e - (e - b)) ? 0 : *(b - 1);
		after = (b + 1 < e) ? b[1] : 0;
		/* ' before digit is always apostrophe */
		if (*b == '\'' && isdigit((unsigned char)after)) {
			fputs("\xe2\x80\x99", stdout);
			return 1;
		}
		can_open = !isws(after) && (isws(before) || isasciipunct(before) || before == 0);
		can_close = !isws(before) && (isws(after) || isasciipunct(after) || after == 0);
		if (*b == '"') {
			fputs(can_open && !can_close ? "\xe2\x80\x9c" : "\xe2\x80\x9d", stdout);
		} else {
			if (can_close && !can_open) fputs("\xe2\x80\x99", stdout);
			else if (can_open && !can_close) fputs("\xe2\x80\x98", stdout);
			else fputs("\xe2\x80\x99", stdout); /* ambiguous: apostrophe */
		}
		return 1;
	}

	/* HTML escapes */
	if (*b == '&') { fputs("&amp;", stdout); return 1; }
	if (*b == '<') { fputs("&lt;", stdout); return 1; }

	return 0;
}

/* --- main dispatch --- */

static void
process(const char *b, const char *e, int newblock)
{
	const char *p;
	int affected;
	unsigned int i;

	for (p = b; p < e; ) {
		if (newblock)
			while (p < e && *p == '\n')
				if (++p == e) return;

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
}

/* pre-scan for reference definitions: [label]: url */
static void
scan_refs(const char *b, const char *e)
{
	const char *p, *line, *label, *url, *urlend;
	int labellen;

	line = b;
	while (line < e) {
		p = line;
		while (p < e && *p == ' ') p++;
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
					url = p;
					while (p < e && *p != '\n' && *p != ' ') p++;
					urlend = p;
					if (labellen > 0 && nrefs < (int)LEN(refs)) {
						refs[nrefs].label = label;
						refs[nrefs].labellen = labellen;
						refs[nrefs].url = url;
						refs[nrefs].urllen = urlend - url;
						nrefs++;
					}
				}
			}
		}
		line = eol(line, e);
	}
}

int
main(void)
{
	char *buf = NULL;
	int len = 0, cap = 0;
	int n;

	/* read stdin */
	do {
		cap += BUFSIZ;
		buf = realloc(buf, cap);
		if (!buf) die("malloc");
		n = fread(buf + len, 1, cap - len, stdin);
		len += n;
	} while (n > 0);

	scan_refs(buf, buf + len);
	process(buf, buf + len, 1);

	/* close remaining sections */
	close_sections(0);

	free(buf);
	return 0;
}
