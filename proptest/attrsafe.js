#!/usr/bin/env node
// Property test: cdjot's output should never contain a literal '<'
// inside an HTML attribute value. Such a value would either be
// truncated by the browser's tag scanner or silently swallow markup.
//
// Usage: node attrsafe.js <file>...
//   SHOW=N   show first N failures (default 5)
//   CDJOT=path   override binary (default ./cdjot)
//
// The check is intentionally loose: we walk attribute="value" pairs
// via regex and flag values that contain '<'. False positives occur
// when an empty-valued attribute (attr="") sits next to a tag close
// followed by inline content that includes '<' — those are reported
// but rare in normal output.
const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const cdjot = process.env.CDJOT || path.join(__dirname, '..', 'cdjot');
const showLimit = parseInt(process.env.SHOW || '5', 10);

let ok = 0, bad = 0, errs = 0;
for (const f of process.argv.slice(2)) {
    const buf = fs.readFileSync(f);
    let out;
    try { out = execFileSync(cdjot, [], { input: buf, timeout: 5000, maxBuffer: 32 * 1024 * 1024 }).toString(); }
    catch (e) { errs++; continue; }
    const m = out.match(/\b[a-zA-Z][\w:-]*="[^">]*<[^"]*"/);
    if (!m) { ok++; continue; }
    bad++;
    if (bad <= showLimit) {
        console.log(`=== BAD ${f} (${buf.length} bytes): ${m[0].slice(0, 120)} ===`);
        console.log('--- input ---');
        process.stdout.write(buf.slice(0, 200));
        console.log('\n--- output ---');
        process.stdout.write(out.slice(0, 600));
        console.log('\n');
    }
}
console.error(`OK=${ok} BAD=${bad} ERRS=${errs}`);
