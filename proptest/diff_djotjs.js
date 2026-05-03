#!/usr/bin/env node
// Differential test: compare cdjot's output against the reference
// djot.js implementation on each input file. Useful as an oracle for
// fuzzer-found inputs — most divergences are intentional cdjot
// behavior, but new ones are worth investigating.
//
// Setup: npm install @djot/djot   (run from this directory)
// Usage: node diff_djotjs.js <file>...
//   SHOW=N   show first N divergences (default 5)
//   CDJOT=path   override binary (default ../cdjot relative to this script)
const { parse, renderHTML } = require('@djot/djot');
const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const cdjot = process.env.CDJOT || path.join(__dirname, '..', 'cdjot');
const showLimit = parseInt(process.env.SHOW || '5', 10);

let diffs = 0, errs = 0, ok = 0;
for (const f of process.argv.slice(2)) {
    const buf = fs.readFileSync(f);
    let cOut, jOut;
    try { cOut = execFileSync(cdjot, [], { input: buf, timeout: 5000, maxBuffer: 32 * 1024 * 1024 }).toString(); }
    catch (e) { errs++; continue; }
    try {
        jOut = renderHTML(parse(buf.toString('utf8'), { sourcePositions: false }));
    } catch (e) { continue; /* djot.js threw — can't compare */ }
    if (cOut === jOut) { ok++; continue; }
    diffs++;
    if (diffs <= showLimit) {
        console.log(`=== DIFF ${f} (${buf.length} bytes) ===`);
        console.log('--- input ---');
        process.stdout.write(buf.slice(0, 200));
        console.log('\n--- cdjot ---');
        process.stdout.write(cOut.slice(0, 400));
        console.log('\n--- djot.js ---');
        process.stdout.write(jOut.slice(0, 400));
        console.log('\n');
    }
}
console.error(`OK=${ok} DIFFS=${diffs} ERRS=${errs}`);
