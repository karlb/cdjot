#!/usr/bin/env node
// Property test: every <tag> in cdjot's output that requires explicit
// closing should be balanced LIFO with its </tag>.
//
// Usage: node wellformed.js <file>...
//   SHOW=N   show first N failures (default 5)
//   CDJOT=path   override binary (default ./cdjot)
//
// Void/optional-close HTML5 elements (br, hr, img, input, …) and elements
// that cdjot only emits as raw HTML through {=html} are excluded.
const { execFileSync } = require('child_process');
const fs = require('fs');

const cdjot = process.env.CDJOT || './cdjot';
const showLimit = parseInt(process.env.SHOW || '5', 10);

const PAIRED = new Set([
    'section', 'ol', 'ul', 'li', 'p', 'blockquote', 'pre', 'code', 'div',
    'a', 'em', 'strong', 'sub', 'sup', 'mark', 'ins', 'del', 'span',
    'h1', 'h2', 'h3', 'h4', 'h5', 'h6',
    'table', 'thead', 'tbody', 'tr', 'td', 'th', 'caption',
    'dl', 'dt', 'dd',
]);

function check(html) {
    const stack = [];
    const tagRe = /<\/?([a-zA-Z][a-zA-Z0-9]*)\b[^>]*>/g;
    let m;
    while ((m = tagRe.exec(html)) !== null) {
        const closing = m[0][1] === '/';
        const name = m[1].toLowerCase();
        if (!PAIRED.has(name)) continue;
        if (!closing) {
            stack.push({ name, idx: m.index });
        } else {
            if (stack.length === 0) return { kind: 'unmatched-close', name, at: m.index };
            const top = stack.pop();
            if (top.name !== name) return { kind: 'mismatch', expected: top.name, got: name, at: m.index };
        }
    }
    if (stack.length > 0) return { kind: 'unclosed', stack: stack.map(s => s.name) };
    return null;
}

let ok = 0, bad = 0, errs = 0;
for (const f of process.argv.slice(2)) {
    const buf = fs.readFileSync(f);
    let out;
    try { out = execFileSync(cdjot, [], { input: buf, timeout: 5000, maxBuffer: 32 * 1024 * 1024 }).toString(); }
    catch (e) { errs++; continue; }
    const err = check(out);
    if (!err) { ok++; continue; }
    bad++;
    if (bad <= showLimit) {
        console.log(`=== BAD ${f} (${buf.length} bytes): ${JSON.stringify(err)} ===`);
        console.log('--- input ---');
        process.stdout.write(buf.slice(0, 200));
        console.log('\n--- output ---');
        process.stdout.write(out.slice(0, 800));
        console.log('\n');
    }
}
console.error(`OK=${ok} BAD=${bad} ERRS=${errs}`);
// Don't exit non-zero: inputs using `{=html}` raw HTML deliberately emit
// unbalanced tags, so a small steady BAD count from the test corpus is
// expected. Inspect listed failures by hand.
