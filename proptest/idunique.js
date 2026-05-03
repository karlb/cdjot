#!/usr/bin/env node
// Property test: in cdjot's HTML output every id="X" appears at most
// once and every href="#X" resolves to an existing id="X" in the same
// document.
//
// Duplicate ids violate the HTML5 spec and break browser anchor
// scrolling and ARIA references; dangling hash hrefs typically signal
// a footnote-call/definition mismatch or a broken heading auto-ref.
//
// Usage: node idunique.js <file>...
//   SHOW=N        show first N failures (default 5)
//   CDJOT=path    override binary (default ./cdjot)
const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const cdjot = process.env.CDJOT || path.join(__dirname, '..', 'cdjot');
const showLimit = parseInt(process.env.SHOW || '5', 10);

function check(html) {
    const ids = new Map();
    const dupes = [];
    const idRe = /\bid="([^"]*)"/g;
    let m;
    while ((m = idRe.exec(html)) !== null) {
        const v = m[1];
        if (ids.has(v)) dupes.push(v);
        else ids.set(v, m.index);
    }
    const dangling = [];
    const hrefRe = /\bhref="#([^"]+)"/g;
    while ((m = hrefRe.exec(html)) !== null) {
        if (!ids.has(m[1])) dangling.push(m[1]);
    }
    if (dupes.length === 0 && dangling.length === 0) return null;
    return { dupes: [...new Set(dupes)].slice(0, 5), dangling: [...new Set(dangling)].slice(0, 5) };
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
