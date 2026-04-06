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
static int in_container; /* suppress sections inside blockquotes/lists */
static int tight; /* suppress <p> wrapping in tight list items */

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
/* normalize whitespace for comparison: collapse ws to single space */
static int
label_match(const char *a, int alen, const char *b, int blen)
{
	int ai = 0, bi = 0;
	while (ai < alen && bi < blen) {
		if (isws(a[ai]) && isws(b[bi])) {
			while (ai < alen && isws(a[ai])) ai++;
			while (bi < blen && isws(b[bi])) bi++;
		} else if (a[ai] == b[bi]) {
			ai++; bi++;
		} else {
			return 0;
		}
	}
	/* skip trailing ws */
	while (ai < alen && isws(a[ai])) ai++;
	while (bi < blen && isws(b[bi])) bi++;
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
			return 1;
		}
	return 0;
}

/* emit heading id: keep alnum/-/_, space/newline -> '-' */
static void
heading_id(const char *b, const char *e)
{
	char idbuf[256];
	int n = 0;

	while (b < e && isws(*b)) b++;
	while (e > b && isws(e[-1])) e--;
	for (; b < e && n < (int)sizeof(idbuf) - 1; b++) {
		if (*b == ' ' || *b == '\t' || *b == '\n')
			idbuf[n++] = '-';
		else if (isalnum((unsigned char)*b) || *b == '-' || *b == '_')
			idbuf[n++] = *b;
	}
	/* trim leading/trailing '-' */
	while (n > 0 && idbuf[n-1] == '-') n--;
	{
		int s = 0;
		while (s < n && idbuf[s] == '-') s++;
		fwrite(idbuf + s, 1, n - s, stdout);
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
	const char *p, *line;

	if (!n) return 0;
	p = b;
	while (p < e && *p == ' ') p++;
	if (p >= e || *p != '[') return 0;
	p++;
	while (p < e && *p != ']' && *p != '\n') p++;
	if (p >= e || *p != ']') return 0;
	p++;
	if (p >= e || *p != ':') return 0;
	/* consume this line and any indented continuation lines */
	line = eol(b, e);
	while (line < e) {
		int sp = spaces(line, e);
		if (sp == 0 || isblankline(line, eol(line, e)))
			break;
		line = eol(line, e);
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
	/* find end of first line content */
	content = p;
	q = eol(p, e);
	while (q > content && (q[-1] == '\n' || q[-1] == '\r'
	    || q[-1] == ' ' || q[-1] == '\t'))
		q--;
	cend = q;

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
		q = eol(line, e);
		while (q > lp && (q[-1] == '\n' || q[-1] == '\r'
		    || q[-1] == ' ' || q[-1] == '\t'))
			q--;
		for (; lp < q; lp++) {
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

	if (!in_container) {
		close_sections(level);
		fputs("<section id=\"", stdout);
		if (blen > 0)
			heading_id(buf, buf + blen);
		else
			printf("s-%d", nsections + 1);
		fputs("\">\n", stdout);
		if (nsections < 6) sections[nsections++] = level;
	}

	printf("<h%d", level);
	if (in_container) {
		fputs(" id=\"", stdout);
		if (blen > 0) heading_id(buf, buf + blen);
		fputc('"', stdout);
	}
	fputc('>', stdout);
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
		fputs("<blockquote>\n", stdout);
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
	/* info string */
	while (p < e && (*p == ' ' || *p == '\t')) p++;
	info = p;
	while (p < e && *p != '\n') p++;
	infoend = p;
	while (infoend > info && (infoend[-1] == ' ' || infoend[-1] == '\t'))
		infoend--;
	/* backtick fence: info must not contain backticks or spaces */
	if (fch == '`') {
		for (q = info; q < infoend; q++)
			if (*q == '`' || *q == ' ') return 0;
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

/* roman numeral value, -1 if not valid */
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

/*
 * Parse list marker at p. Returns offset past marker+space, or 0.
 * Sets *style: 0=bullet, 1=decimal, 2=lower-alpha, 3=upper-alpha,
 *              4=lower-roman, 5=upper-roman
 * Sets *start to the start number, *delim to the delimiter char.
 * Sets *mch to the bullet character (for bullet lists).
 */
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
			/* look at second item */
			line2 = eol(b, e);
			while (line2 < e && isblankline(line2, eol(line2, e)))
				line2 = eol(line2, e);
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

	/* open list */
	if (style == 0) {
		fputs("<ul>\n", stdout);
	} else {
		fputs("<ol", stdout);
		if (start_num != 1) printf(" start=\"%d\"", start_num);
		fputs(typattr[style], stdout);
		fputs(">\n", stdout);
	}

	/* parse items */
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
				if ((style == 4 && st2 == 2) || (style == 5 && st2 == 3))
					compat = 1;
				if ((style == 2 && st2 == 4) || (style == 3 && st2 == 5))
					compat = 1;
				if (!compat) break;
			}
		}
		indent = item_sp + item_mw; /* content column */

		/* collect one item's content */
		buf = NULL;
		i = 0;
		/* first line: content after marker */
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
			if (sp > marker_col) {
				/* indented past marker column: part of this item */
				int strip;
				if (sp >= indent && !had_blank)
					strip = indent; /* content column */
				else if (sp >= indent && had_blank)
					strip = marker_col + 1; /* post-blank: preserve nesting */
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
				/* could be sibling item or end of list */
				int st2;
				char d2, m2;
				if (scan_marker(line + sp, e, &st2, &(int){0}, &d2, &m2)) {
					if (style == 0 && st2 == 0 && m2 == mch) break;
					if (style != 0 && d2 == delim &&
					    (st2 == style ||
					    (style >= 4 && st2 == style - 2) ||
					    (style <= 3 && st2 == style + 2)))
						break;
				}
			}
			/* lazy continuation (no blank, not a list marker at base) */
			if (!had_blank) {
				/* check for any list marker at sp==marker_col */
				if (sp == marker_col) {
					int st2;
					char d2, m2;
					if (scan_marker(line + sp, e, &st2, &(int){0}, &d2, &m2))
						break;
				}
				q = eol(line, e);
				/* strip up to indent spaces for lazy lines */
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
			break;
		}

		/* check if loose: blank line between this item and next sibling */
		{
			/* check buffer: trailing \n\n means blank consumed into item */
			int saw_blank = (i >= 2 && buf[i-1] == '\n' && buf[i-2] == '\n');
			/* check input: blank still in stream */
			if (saw_blank || (line < e && isblankline(line, eol(line, e)))) {
				const char *lp = line;
				while (lp < e && isblankline(lp, eol(lp, e)))
					lp = eol(lp, e);
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

		/* trim trailing blank lines from item */
		while (i > 0 && buf[i-1] == '\n') i--;
		ADDC(buf, i) = '\0';

		{
			int save_tight = tight;
			int save_cont = in_container;
			in_container = 1;
			fputs("<li>\n", stdout);
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
					/* pre-blank: inline (tight paragraph) */
					const char *pe = split;
					while (pe > bp && (pe[-1] == '\n' || pe[-1] == ' '))
						pe--;
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
	/* find end: blank line */
	end = b;
	while (end < e) {
		p = eol(end, e);
		if (isblankline(end, p))
			break;
		end = p;
	}
	/* trim whitespace */
	while (b < end && (*b == ' ' || *b == '\t')) b++;
	p = end;
	while (p > b && (p[-1] == '\n' || p[-1] == '\r' || p[-1] == ' ' || p[-1] == '\t')) p--;
	if (!tight) fputs("<p>", stdout);
	process(b, p, 0);
	if (!tight) fputs("</p>", stdout);
	fputc('\n', stdout);
	return -(end - start);
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
			/* trim single space from each end (single backtick only) */
			if (count == 1 && codelen >= 2
			    && code[0] == ' ' && code[codelen-1] == ' ') {
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
	int explicit_open = 0; /* {_ or {* */
	int consumed_open = 0; /* extra chars consumed for { */

	if (n) return 0;

	/* check for explicit opener {_ or {* */
	if (*b == '{' && b + 1 < e && (b[1] == '_' || b[1] == '*')) {
		ch = b[1];
		explicit_open = 1;
		consumed_open = 1;
		after = (b + 2 < e) ? b[2] : 0;
	} else {
		ch = *b;
		if (ch != '_' && ch != '*') return 0;
		/* _} is an explicit closer without opener — skip, will be literal */
		if (b + 1 < e && b[1] == '}') return 0;
		after = (b + 1 < e) ? b[1] : 0;
	}

	/* djot opener rule: character after marker is not whitespace
	 * (explicit openers bypass this rule) */
	if (!explicit_open && isws(after)) return 0;

	/* find matching close */
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
				/* delimiter is in text part — don't skip, let it match */
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
			fputs(ch == '_' ? "<em>" : "<strong>", stdout);
			process(start, stop, 0);
			fputs(ch == '_' ? "</em>" : "</strong>", stdout);
			return stop + 1 + consumed_close - b;
		}
	}
	return 0;
}

/* print plain text for image alt: strip markup delimiters */
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
		if (findref(label, labellen, &url, &urllen)) {
			if (img) {
				fputs("<img alt=\"", stdout);
				altprint(text, textend);
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
		else if (run % 2 == 0) { en = run / 2; }
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
	if (*b == '>') { fputs("&gt;", stdout); return 1; }

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

/* concatenate url parts into static buffer, stripping whitespace */
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

/* pre-scan for reference definitions: [label]: url (possibly multi-line) */
static void
scan_refs(const char *b, const char *e)
{
	const char *p, *line, *label, *nextline;
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
					urlbuflen = 0;
					/* URL starts on same line or next line */
					if (p < e && *p != '\n') {
						const char *ue = p;
						while (ue < e && *ue != '\n') ue++;
						urlbuf_add(p, ue);
					}
					/* continuation lines (indented) */
					nextline = eol(line, e);
					while (nextline < e) {
						int sp = spaces(nextline, e);
						if (sp == 0 || isblankline(nextline, eol(nextline, e)))
							break;
						const char *le = eol(nextline, e);
						urlbuf_add(nextline + sp, le);
						nextline = le;
					}
					if (labellen > 0 && nrefs < (int)LEN(refs)) {
						/* copy url to permanent storage */
						char *u = malloc(urlbuflen + 1);
						if (u) {
							memcpy(u, urlbuf, urlbuflen);
							u[urlbuflen] = '\0';
						}
						refs[nrefs].label = label;
						refs[nrefs].labellen = labellen;
						refs[nrefs].url = u;
						refs[nrefs].urllen = urlbuflen;
						nrefs++;
					}
				}
			}
		}
		line = eol(line, e);
	}
}

/* pre-scan headings to create implicit reference definitions */
static void
scan_heading_refs(const char *b, const char *e)
{
	const char *line, *p;

	line = b;
	while (line < e) {
		p = line;
		int sp = spaces(p, e);
		if (sp <= 3) {
			p += sp;
			int lvl = leadc(p, e, '#');
			if (lvl >= 1 && lvl <= 6 && p + lvl < e
			    && (p[lvl] == ' ' || p[lvl] == '\n')) {
				const char *content = p + lvl;
				if (*content == ' ') content++;
				const char *cend = eol(line, e);
				while (cend > content && (cend[-1] == '\n' || cend[-1] == '\r'
				    || cend[-1] == ' ' || cend[-1] == '\t'))
					cend--;
				if (cend > content && nrefs < (int)LEN(refs)
				    && !findref(content, cend - content, &(const char *){0}, &(int){0})) {
					/* build #id URL */
					char idbuf[256];
					int idn = 0;
					const char *s;
					for (s = content; s < cend && idn < (int)sizeof(idbuf) - 2; s++) {
						if (*s == ' ' || *s == '\t' || *s == '\n')
							idbuf[idn++] = '-';
						else if (isalnum((unsigned char)*s) || *s == '-' || *s == '_')
							idbuf[idn++] = *s;
					}
					while (idn > 0 && idbuf[idn-1] == '-') idn--;
					{
						int ss = 0;
						while (ss < idn && idbuf[ss] == '-') ss++;
						int ulen = idn - ss + 1;
						char *u = malloc(ulen + 1);
						if (u) {
							u[0] = '#';
							memcpy(u + 1, idbuf + ss, idn - ss);
							u[ulen] = '\0';
							refs[nrefs].label = content;
							refs[nrefs].labellen = cend - content;
							refs[nrefs].url = u;
							refs[nrefs].urllen = ulen;
							nrefs++;
						}
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
	scan_heading_refs(buf, buf + len);
	process(buf, buf + len, 1);

	/* close remaining sections */
	close_sections(0);

	free(buf);
	return 0;
}
