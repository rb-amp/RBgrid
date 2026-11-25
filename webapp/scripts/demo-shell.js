#!/usr/bin/env node
/**
 * The demo's own index.html (08.11).
 *
 * demo.rbgrid.io and the hosted app used to serve the same shell. The demo build gets its own title,
 * description, canonical, og tags and a visible «About this demo» block; the app's public/index.html
 * stays as it is (a canonical to the demo there would deindex the app, and «simulated data» is false
 * on a real hub).
 *
 * The block sits after <div id="app">, not inside it: React replaces the mount node's children. Its
 * text is true on every URL that serves this shell (/, ?scenario=…, ?demo=true): it lists the five
 * households as links (marketing 08.11), root-relative so they resolve from any variant. It inherits the body colour the app's theme sets, so it reads in light and dark.
 * Run after the demo build: node scripts/demo-shell.js build-demo
 */
const fs = require('fs');
const path = require('path');

const DIR = process.argv[2] || 'build-demo';
const FILE = path.join(DIR, 'index.html');

const URL = 'https://demo.rbgrid.io/';
const TITLE = 'RBgrid live demo: per-circuit energy monitor, no install';
const DESCRIPTION = 'The RBgrid web app running on simulated data from an example household: live power, per-circuit history and what each circuit costs. Nothing to install.';

const ABOUT = `<aside id="about-demo" lang="en">
      <details>
        <summary><h2>About this demo</h2></summary>
        <p>Everything here is the real RBgrid web app running on <strong>simulated data</strong>. The households are made up and so are their numbers — nothing on this page is a measurement of a real home, a real circuit or a real device.</p>
        <p>There is nothing to install to look around: it runs in this browser, with no hub and no account.</p>
        <p>Five example households, the same app with different data:</p>
        <ul>
          <li><a href="/?scenario=flat">A flat</a> — a single-phase supply, with the rooms and the big appliances measured separately.</li>
          <li><a href="/?scenario=solar">A home with solar</a> — panels on the roof, exporting what the house doesn't use.</li>
          <li><a href="/?scenario=heatpump-ev">A house with a heat pump and an EV</a> — a three-phase supply and two large, irregular loads.</li>
          <li><a href="/?scenario=workshop">A workshop</a> — a three-phase supply with machines on their own circuits.</li>
          <li><a href="/?scenario=carpark">A car park</a> — charging stations, each one its own measurement point.</li>
        </ul>
        <p>To see your own home's numbers instead of these, you need an RBgrid hub and at least one module. <a href="https://www.rbgrid.io/get-started">What you need to get started</a></p>
      </details>
    </aside>`;

const STYLE = `<style>
      #about-demo { max-width: 72ch; margin: 0 auto; padding: 16px; font: 14px/1.5 'IBM Plex Sans', system-ui, sans-serif; color: inherit; opacity: .85; }
      #about-demo summary { cursor: pointer; }
      #about-demo h2 { display: inline; font-size: 15px; font-weight: 600; margin: 0; }
      #about-demo a { color: inherit; text-decoration: underline; }
    </style>`;

const esc = (s) => s.replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;');

function replaceOnce(html, re, to, what) {
  if (!re.test(html)) throw new Error(`demo-shell: ${what} not found in ${FILE}`);
  return html.replace(re, to);
}

let html = fs.readFileSync(FILE, 'utf8');
html = replaceOnce(html, /<title>[^<]*<\/title>/, `<title>${esc(TITLE)}</title>`, '<title>');
html = replaceOnce(html, /<meta\s+name="description"\s+content="[^"]*"\s*\/?>/, `<meta name="description" content="${esc(DESCRIPTION)}"/>`, 'meta description');
html = replaceOnce(html, /<\/head>/, [
  `<link rel="canonical" href="${URL}"/>`,
  `<meta property="og:type" content="website"/>`,
  `<meta property="og:url" content="${URL}"/>`,
  `<meta property="og:title" content="${esc(TITLE)}"/>`,
  `<meta property="og:description" content="${esc(DESCRIPTION)}"/>`,
  STYLE,
  '</head>',
].join(''), '</head>');
html = replaceOnce(html, /<div id="app"><\/div>/, `<div id="app"></div>${ABOUT}`, '<div id="app"></div>');
fs.writeFileSync(FILE, html);
console.log(`demo-shell: ${FILE} → demo title, description, canonical, og, «About this demo»`);
