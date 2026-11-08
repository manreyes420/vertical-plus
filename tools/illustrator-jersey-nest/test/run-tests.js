/*
 * Tests for JerseyNest.jsx that run in Node (no Illustrator needed):
 *   node test/run-tests.js
 *
 * 1. Roster parsing (CSV, semicolons, quotes, headers, leading zeros).
 * 2. The MaxRects packer: no overlaps, inside the bin, spacing respected.
 * 3. A full JN.run() against a small mock of Illustrator's object model.
 */
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const SRC = fs.readFileSync(path.join(__dirname, '..', 'JerseyNest.jsx'), 'utf8');

let failures = 0, passes = 0;
function check(cond, msg) {
    if (cond) passes++;
    else { failures++; console.log('FAIL: ' + msg); }
}
function eq(a, b, msg) { check(a === b, msg + ' (got ' + JSON.stringify(a) + ', want ' + JSON.stringify(b) + ')'); }

function load(globals) {
    const ctx = vm.createContext(Object.assign({ Math, String, parseInt, parseFloat, isNaN, Infinity,
        RegExp, Date, Error, encodeURIComponent, decodeURIComponent }, globals || {}));
    vm.runInContext(SRC + '\n;this.JN = JN;', ctx);
    return ctx;
}

/* ---------------- 1. roster ---------------- */
{
    const JN = load().JN;
    let r = JN.parseRoster('Size,Name,Number,Shorts size,Shorts\nM,Smith,7,,yes\nm ,"O\'Neil, Jr",07,L,si\nXL,García,10,,no\n\n');
    eq(r.players.length, 3, 'header skipped, 3 players');
    eq(r.players[0].size, 'M', 'size');
    eq(r.players[0].shorts, true, 'shorts yes');
    eq(r.players[0].shortsSize, '', 'shorts size empty');
    eq(r.players[1].size, 'M', 'size normalized');
    eq(r.players[1].name, "O'Neil, Jr", 'quoted comma name');
    eq(r.players[1].number, '07', 'leading zero kept');
    eq(r.players[1].shortsSize, 'L', 'shorts size');
    eq(r.players[1].shorts, true, 'si = yes');
    eq(r.players[2].shorts, false, 'no');
    eq(r.players[2].name, 'García', 'accent kept in name');

    r = JN.parseRoster('talla;nombre;número;pantaloneta\r\nS;PEREZ;9;sí\r\nL;LOPEZ;23;no\r\n');
    eq(r.players.length, 2, 'semicolon + spanish header');
    eq(r.players[0].shorts, true, 'sí = yes (4 columns)');
    eq(r.players[1].number, '23', 'number');

    r = JN.parseRoster('S\tANA\t1\nM\tBEA\t2');
    eq(r.players.length, 2, 'tab separated, no header');
    eq(r.players[1].name, 'BEA', 'tab name');
    eq(r.players[1].shorts, false, '3 columns = no shorts');

    r = JN.parseRoster('﻿M,ANA,,,\n,NOSIZE,3,,\n');
    eq(r.players.length, 1, 'row without size skipped');
    eq(r.warnings.length, 1, 'warning for skipped row');
    eq(r.players[0].number, '', 'empty number allowed');

    eq(JN.formatTag('{size} #{number} {name}', { size: 'M', number: '7', name: 'ANA' }), 'M #7 ANA', 'tag format');
    eq(JN.cleanPieceName('Sleeve NOROTATE'), 'Sleeve', 'NOROTATE stripped');
    eq(JN.cleanPieceName('Logo x30'), 'Logo', 'quantity stripped');
}

/* ---------------- 2. packer ---------------- */
function overlaps(a, b) {
    return a.x < b.x + b.w - 1e-6 && b.x < a.x + a.w - 1e-6 && a.y < b.y + b.h - 1e-6 && b.y < a.y + a.h - 1e-6;
}
{
    const JN = load().JN;
    let seed = 7;
    const rnd = () => (seed = (seed * 16807) % 2147483647) / 2147483647;
    for (const heuristic of ['bssf', 'bl']) {
        for (const rot of [false, true]) {
            const items = [];
            for (let i = 0; i < 120; i++) items.push({ w: 20 + rnd() * 300, h: 20 + rnd() * 500, canRotate: true, group: null });
            items.push({ w: 2000, h: 50, canRotate: true }); /* too wide even rotated */
            const sp = 9;
            const res = JN.pack(JN.sortItems(items, 'free'), { binW: 1000, binH: 1400, spacing: sp,
                allowRotate: rot, heuristic, firstFit: heuristic === 'bssf' });
            eq(res.unplaced.length, 1, `${heuristic} rot=${rot}: oversize item unplaced`);
            eq(res.placements.length, 120, `${heuristic} rot=${rot}: all others placed`);
            const byBin = {};
            let bad = 0;
            for (const p of res.placements) {
                const w = p.rot ? p.item.h : p.item.w, h = p.rot ? p.item.w : p.item.h;
                if (!rot && p.rot) bad++;
                if (p.x < -1e-6 || p.y < -1e-6 || p.x + w > 1000 + 1e-6 || p.y + h > 1400 + 1e-6) bad++;
                /* inflate by half the spacing on each side: neighbours must keep `sp` apart */
                (byBin[p.bin] = byBin[p.bin] || []).push({ x: p.x, y: p.y, w: w + sp, h: h + sp });
            }
            for (const k in byBin) {
                const L = byBin[k];
                for (let i = 0; i < L.length; i++) for (let j = i + 1; j < L.length; j++) if (overlaps(L[i], L[j])) bad++;
            }
            eq(bad, 0, `${heuristic} rot=${rot}: inside bin, no overlaps, spacing kept`);
            let area = 0;
            for (const p of res.placements) area += p.item.w * p.item.h;
            console.log(`  packer ${heuristic} rot=${rot}: ${res.bins.length} sheets, fill ${(100 * area / (res.bins.length * 1000 * 1400)).toFixed(1)}%`);
        }
    }
    /* new bin per group */
    const items = [{ w: 10, h: 10, group: 'A' }, { w: 10, h: 10, group: 'A' }, { w: 10, h: 10, group: 'B' }];
    const res = JN.pack(items, { binW: 100, binH: 100, spacing: 0, heuristic: 'bl', newBinPerGroup: true });
    eq(res.bins.length, 2, 'new bin per group');
    /* ordered sort */
    const s = JN.sortItems([{ sizeRank: 2, playerIndex: 0, pieceRank: 0, seq: 0 }, { sizeRank: 1, playerIndex: 1, pieceRank: 1, seq: 1 },
        { sizeRank: 1, playerIndex: 1, pieceRank: 0, seq: 2 }], 'size');
    eq(s.map(x => x.seq).join(), '2,1,0', 'sort by size then piece');
}

/* ---------------- 3. mock Illustrator ---------------- */
function makeIllustrator() {
    const Justification = { LEFT: 'L', CENTER: 'C', RIGHT: 'R' };
    const Transformation = { CENTER: 'center' };
    const ElementPlacement = { PLACEATEND: 'end', PLACEATBEGINNING: 'begin', PLACEBEFORE: 'before', PLACEAFTER: 'after' };
    const ZOrderMethod = { BRINGTOFRONT: 'front' };

    function coll(arr) { return arr; } /* arrays already have length + index */
    function detach(item) {
        if (!item.parent) return;
        const a = item.parent.pageItems, i = a.indexOf(item);
        if (i >= 0) a.splice(i, 1);
        item.parent = null;
    }
    function layerOf(item) { let p = item.parent; while (p && p.typename !== 'Layer') p = p.parent; return p; }
    function insert(item, target, placement) {
        detach(item);
        if (placement === 'before' || placement === 'after') {
            const parent = target.parent, a = parent.pageItems, i = a.indexOf(target);
            a.splice(placement === 'before' ? i : i + 1, 0, item);
            item.parent = parent;
        } else {
            if (placement === 'begin') target.pageItems.unshift(item); else target.pageItems.push(item);
            item.parent = target;
        }
        return item;
    }

    class Item {
        constructor(typename, name) { this.typename = typename; this.name = name || ''; this.hidden = false; this.locked = false; this.guides = false; this.parent = null; }
        get layer() { return layerOf(this); }
        get visibleBounds() { return this.geometricBounds; }
        remove() { detach(this); this.removed = true; }
        move(rel, placement) { return insert(this, rel, placement); }
        duplicate(target, placement) {
            const c = this.clone();
            if (!target) return insert(c, this, 'before');
            return insert(c, target, placement || 'begin');
        }
    }
    class Rect extends Item {
        constructor(typename, name, b) { super(typename, name); this.b = b.slice(); this.clipping = false; }
        get geometricBounds() { return this.b.slice(); }
        clone() { const c = new Rect(this.typename, this.name, this.b); c.clipping = this.clipping; c.hidden = this.hidden; return c; }
        translate(dx, dy) { this.b = [this.b[0] + dx, this.b[1] + dy, this.b[2] + dx, this.b[3] + dy]; }
        scaleAbout(cx, cy, sx, sy) { const b = this.b; this.b = [cx + (b[0] - cx) * sx, cy + (b[1] - cy) * sy, cx + (b[2] - cx) * sx, cy + (b[3] - cy) * sy]; }
        rotateAbout(cx, cy) { /* 90 deg CCW */ const b = this.b; const pts = [[b[0], b[1]], [b[2], b[3]]].map(([x, y]) => [cx - (y - cy), cy + (x - cx)]);
            this.b = [Math.min(pts[0][0], pts[1][0]), Math.max(pts[0][1], pts[1][1]), Math.max(pts[0][0], pts[1][0]), Math.min(pts[0][1], pts[1][1])]; }
        resize(px, py) { const b = this.b; this.scaleAbout((b[0] + b[2]) / 2, (b[1] + b[3]) / 2, px / 100, py / 100); }
        rotate() { const b = this.b; this.rotateAbout((b[0] + b[2]) / 2, (b[1] + b[3]) / 2); }
    }
    class Text extends Item {
        /* point text: anchor (x, baseline-ish top y), width = 0.6 * size * chars */
        constructor(name, contents, x, top, size, just) { super('TextFrame', name); this.contents = contents; this.x = x; this.top = top; this.size = size; this.sx = 1; this.sy = 1; this.just = just || 'C'; }
        get textRange() { return { paragraphAttributes: { justification: this.just } }; }
        get geometricBounds() {
            if (this.frozen) return this.frozen.slice();
            const w = 0.6 * this.size * String(this.contents).length * this.sx, h = this.size * this.sy;
            const l = this.just === 'L' ? this.x : this.just === 'R' ? this.x - w : this.x - w / 2;
            return [l, this.top, l + w, this.top - h];
        }
        clone() { const c = new Text(this.name, this.contents, this.x, this.top, this.size, this.just); c.sx = this.sx; c.sy = this.sy; c.frozen = this.frozen && this.frozen.slice(); return c; }
        translate(dx, dy) { this.x += dx; this.top += dy; if (this.frozen) { const f = this.frozen; this.frozen = [f[0] + dx, f[1] + dy, f[2] + dx, f[3] + dy]; } }
        scaleAbout(cx, cy, sx, sy) { this.x = cx + (this.x - cx) * sx; this.top = cy + (this.top - cy) * sy; this.sx *= sx; this.sy *= sy; }
        rotateAbout(cx, cy) { const r = this.toRect(); r.rotateAbout(cx, cy); this.frozen = r.b; }
        resize(px, py) { const b = this.geometricBounds; const cx = (b[0] + b[2]) / 2, cy = (b[1] + b[3]) / 2; this.sx *= px / 100; this.sy *= py / 100;
            const nb = this.geometricBounds; this.translate(cx - (nb[0] + nb[2]) / 2, cy - (nb[1] + nb[3]) / 2); }
        toRect() { return new Rect('PathItem', '', this.geometricBounds); }
        createOutline() { const g = new Group('', [this.toRect()]); insert(g, this, 'before'); detach(this); return g; }
    }
    class Group extends Item {
        constructor(name, children, clipped) { super('GroupItem', name); this.pageItems = []; this.clipped = !!clipped; (children || []).forEach(c => insert(c, this, 'end')); }
        get groupItems() { const self = this; return { add() { return insert(new Group(''), self, 'begin'); } }; }
        get geometricBounds() { /* like Illustrator: includes masked art */
            let u = null; for (const c of this.pageItems) { const b = c.geometricBounds; u = u ? [Math.min(u[0], b[0]), Math.max(u[1], b[1]), Math.max(u[2], b[2]), Math.min(u[3], b[3])] : b; }
            return u || [0, 0, 0, 0]; }
        clone() { const g = new Group(this.name, this.pageItems.map(c => c.clone()), this.clipped); g.hidden = this.hidden; return g; }
        translate(dx, dy) { this.pageItems.forEach(c => c.translate(dx, dy)); }
        scaleAbout(cx, cy, sx, sy) { this.pageItems.forEach(c => c.scaleAbout(cx, cy, sx, sy)); }
        rotateAbout(cx, cy) { this.pageItems.forEach(c => c.rotateAbout(cx, cy)); }
        resize(px, py) { const b = this.geometricBounds; this.scaleAbout((b[0] + b[2]) / 2, (b[1] + b[3]) / 2, px / 100, py / 100); }
        rotate() { const b = this.geometricBounds; this.rotateAbout((b[0] + b[2]) / 2, (b[1] + b[3]) / 2); }
    }
    class Layer {
        constructor(name) { this.typename = 'Layer'; this.name = name; this.locked = false; this.visible = true; this.pageItems = []; this.layers = []; const self = this;
            this.layers.add = () => { const l = new Layer(''); self.layers.push(l); return l; }; }
        get groupItems() { const self = this; return { add() { return insert(new Group(''), self, 'begin'); } }; }
        zOrder() {}
    }

    const doc = { layers: [], artboards: [] };
    doc.layers.add = () => { const l = new Layer('Layer'); doc.layers.unshift(l); return l; };
    doc.artboards.add = (rect) => {
        for (const v of rect) if (Math.abs(v) > 8191) throw new Error('outside canvas');
        const ab = { artboardRect: rect, name: '' }; doc.artboards.push(ab); return ab;
    };
    doc.artboards.add([0, 792, 612, 0]);

    return { doc, Rect, Text, Group, Layer, globals: { Justification, Transformation, ElementPlacement, ZOrderMethod } };
}

{
    const M = makeIllustrator();
    const { doc, Rect, Text, Group, Layer } = M;

    /* piece = clip group (mask + oversized background art) + NAME + 00 + SIZE tag */
    function piece(name, x, y, w, h, withText) {
        const mask = new Rect('PathItem', 'mask', [x, y, x + w, y - h]); mask.clipping = true;
        const art = new Rect('PathItem', 'bg', [x - 200, y + 200, x + w + 200, y - h - 200]); /* hidden by mask */
        const clip = new Group('clip', [mask, art], true);
        const kids = [clip];
        if (withText) {
            kids.unshift(new Text('', 'NAME', x + w / 2, y - 30, 20, 'C'));
            kids.unshift(new Text('', '00', x + w / 2, y - 80, 60, 'C'));
        }
        kids.unshift(new Text('', 'SIZE', x + 10, y - h + 12, 6, 'L'));
        return new Group(name, kids);
    }
    function sizeLayer(lname, scale) {
        const L = new Layer(lname);
        [piece('Front', 0, 0, 300 * scale, 400 * scale, true), piece('Back', 0, 0, 300 * scale, 400 * scale, true),
            piece('Sleeve', 0, 0, 150 * scale, 120 * scale, false), piece('Collar NOROTATE', 0, 0, 200 * scale, 20, false)]
            .forEach(p => { L.pageItems.push(p); p.parent = L; });
        return L;
    }
    const S = sizeLayer('SIZE_S', 1), Ml = sizeLayer('size_m', 1.1);
    S.locked = true; S.visible = false;
    const SH = new Layer('SHORTS_M');
    [piece('Short L', 0, 0, 220, 300, true), piece('Short R', 0, 0, 220, 300, false)].forEach(p => { SH.pageItems.push(p); p.parent = SH; });
    const EX = new Layer('EXTRAS');
    const logo = new Rect('PathItem', 'Logo x3', [0, 0, 50, -50]); EX.pageItems.push(logo); logo.parent = EX;
    doc.layers.push(S, Ml, SH, EX);

    let alerts = [];
    const ctx = load(Object.assign({}, M.globals, { alert: (m) => alerts.push(m), Window: function () {} }));
    const JN = ctx.JN;
    const roster = 'size,name,number,shorts size,shorts\nS,ANA,7,M,yes\nM,MAXIMILIANO GONZALEZ,10,,yes\nm,BEA,,,no\n';
    JN.loadSettings = () => ({});
    JN.saveSettings = () => {};
    JN.Progress = function () {}; JN.Progress.prototype.set = () => {}; JN.Progress.prototype.close = () => {};
    JN.dialog = () => ({ units: 'in', paperW: '20', paperH: '0', margin: '0.5', spacing: '0.25', fitLast: 'true',
        order: 'free', newPerGroup: 'false', rotate: 'true', uppercase: 'true', namePct: '80', keepProportions: 'false',
        vectorDigits: 'false', outlineText: 'false', tagFormat: '{size}-{number}', roster });
    ctx.app = { documents: [doc], activeDocument: doc };

    JN.run();
    const done = alerts.join('\n');
    check(/^Done\./.test(done), 'run finished: ' + done);

    const out = doc.layers[0];
    check(/^NEST /.test(out.name), 'output layer created on top');
    /* 3 players x 4 pieces + 2 shorts x 2 pieces + 3 logos */
    eq(out.pageItems.length, 12 + 4 + 3, 'piece count');
    eq(S.locked, true, 'template layer lock restored');
    eq(S.visible, false, 'template layer visibility restored');
    eq(S.pageItems.length, 4, 'templates untouched');

    const texts = [];
    (function walk(it) { if (it.typename === 'TextFrame') texts.push(it); if (it.pageItems) it.pageItems.forEach(walk); })(out);
    const contents = texts.map(t => t.contents);
    check(contents.indexOf('ANA') >= 0 && contents.indexOf('MAXIMILIANO GONZALEZ') >= 0, 'names written (uppercase)');
    check(contents.indexOf('NAME') < 0 && contents.indexOf('00') < 0 && contents.indexOf('SIZE') < 0, 'no placeholders left');
    check(contents.indexOf('S-7') >= 0 && contents.indexOf('M-10') >= 0, 'size tags formatted');
    check(contents.indexOf('M-7') >= 0, "ANA's shorts are size M (shorts size column)");
    eq(texts.filter(t => t.contents === 'BEA').length, 2, 'BEA front+back names, no number');

    /* long name was squeezed to <= 80% of the piece width, centred on the original spot */
    const long = texts.filter(t => t.contents === 'MAXIMILIANO GONZALEZ');
    for (const t of long) {
        const g = t.parent; const b = JN.bounds(g); const tb = t.geometricBounds;
        check(tb[2] - tb[0] <= 0.8 * (b[2] - b[0]) + 1e-6, 'long name fits max width');
        check(Math.abs(t.sy - 1) < 1e-9, 'non-proportional shrink keeps height');
    }

    /* every placed piece is inside an artboard, inside margins, and none overlap */
    const boards = doc.artboards.slice(1);
    check(boards.length >= 1, 'artboards created: ' + boards.length);
    const m = 0.5 * 72, sp = 0.25 * 72;
    const rects = out.pageItems.map(p => ({ p, b: JN.bounds(p) }));
    let outside = 0, overlap = 0;
    for (const r of rects) {
        const inside = boards.some(ab => { const a = ab.artboardRect;
            return r.b[0] >= a[0] + m - 1e-6 && r.b[2] <= a[2] - m + 1e-6 && r.b[1] <= a[1] - m + 1e-6 && r.b[3] >= a[3] + m - 1e-6; });
        if (!inside) outside++;
    }
    for (let i = 0; i < rects.length; i++) for (let j = i + 1; j < rects.length; j++) {
        const a = rects[i].b, b = rects[j].b;
        if (a[0] < b[2] + sp - 1e-6 && b[0] < a[2] + sp - 1e-6 && a[3] < b[1] + sp - 1e-6 && b[3] < a[1] + sp - 1e-6) overlap++;
    }
    eq(outside, 0, 'all pieces inside artboard margins');
    eq(overlap, 0, 'no overlaps, spacing kept');
    const ab = boards[0].artboardRect;
    eq(ab[2] - ab[0], 20 * 72, 'roll width 20in');
    check(ab[1] - ab[3] < 8000, 'roll artboard trimmed to artwork');

    /* missing size layer is reported, nothing generated */
    alerts = [];
    const before = doc.layers.length;
    JN.dialog = () => ({ units: 'in', paperW: '20', paperH: '30', margin: '0', spacing: '0', fitLast: 'false', order: 'player',
        newPerGroup: 'true', rotate: 'false', uppercase: 'false', namePct: '80', keepProportions: 'true', vectorDigits: 'false',
        outlineText: 'false', tagFormat: '{size}', roster: 'XL,ZED,1,,no' });
    JN.run();
    check(/SIZE_XL/.test(alerts.join()), 'missing SIZE_XL reported');
    eq(doc.layers.length, before, 'nothing generated when templates are missing');

    /* by-player, fixed sheets, a new artboard per player */
    alerts = [];
    const abBefore = doc.artboards.length;
    JN.dialog = () => ({ units: 'in', paperW: '20', paperH: '30', margin: '0', spacing: '0', fitLast: 'false', order: 'player',
        newPerGroup: 'true', rotate: 'false', uppercase: 'false', namePct: '80', keepProportions: 'true', vectorDigits: 'false',
        outlineText: 'true', tagFormat: '{size}', roster: 'S,Ana,1,,no\nM,Bea,2,,no' });
    JN.run();
    check(/^Done\./.test(alerts.join()), 'by-player run finished');
    eq(doc.artboards.length - abBefore, 3, 'one artboard per player, plus one for the EXTRAS logos');
    const out2 = doc.layers[0];
    let leftText = 0; (function walk(it) { if (it.typename === 'TextFrame') leftText++; if (it.pageItems) it.pageItems.forEach(walk); })(out2);
    eq(leftText, 0, 'outline option converts all text');
}

/* vector digits: "00" replaced by digit artwork at the height of the outlined "00" */
{
    const M = makeIllustrator();
    const { doc, Rect, Text, Group, Layer } = M;
    const L = new Layer('ALL_SIZES');
    const num = new Text('', '00', 100, 0, 72, 'C');
    L.pageItems.push(num); num.parent = L;
    const D = new Layer('DIGITS');
    for (let d = 0; d <= 9; d++) { const g = new Group(String(d), [new Rect('PathItem', '', [0, 0, 30, -40])]); D.pageItems.push(g); g.parent = D; }
    D.visible = false;
    doc.layers.push(L, D);
    const alerts = [];
    const ctx = load(Object.assign({}, M.globals, { alert: (m) => alerts.push(m), Window: function () {} }));
    const JN = ctx.JN;
    JN.loadSettings = () => ({}); JN.saveSettings = () => {};
    JN.Progress = function () {}; JN.Progress.prototype.set = () => {}; JN.Progress.prototype.close = () => {};
    JN.dialog = () => ({ units: 'cm', paperW: '50', paperH: '50', margin: '1', spacing: '1', fitLast: 'false', order: 'free',
        newPerGroup: 'false', rotate: 'false', uppercase: 'true', namePct: '80', keepProportions: 'true', vectorDigits: 'true',
        outlineText: 'false', tagFormat: '{size}', roster: 'M,A,23,,no\nL,B,7,,no\nS,C,,,no' });
    ctx.app = { documents: [doc], activeDocument: doc };
    JN.run();
    check(/^Done\./.test(alerts.join()), 'DTF / vector digits run finished: ' + alerts.join());
    const out = doc.layers[0];
    eq(out.pageItems.length, 2, 'one number per player; none for a player without a number');
    const g23 = out.pageItems.find(p => /^M 23 A/.test(p.name) && p.typename === 'GroupItem');
    check(!!g23, 'number 23 built from digits');
    if (g23) {
        eq(g23.pageItems.length, 2, 'two digit groups');
        const b = JN.bounds(g23);
        check(Math.abs((b[1] - b[3]) - 72) < 1e-6, 'digits scaled to the height of the 00 text');
    }
    const ab = doc.artboards[1].artboardRect, m = 72 / 2.54;
    for (const p of out.pageItems) {
        const b = JN.bounds(p);
        check(b[0] >= ab[0] + m - 1e-6 && b[1] <= ab[1] - m + 1e-6, p.name + ' was placed on the artboard');
    }
    eq(D.visible, false, 'DIGITS layer visibility restored');
}

console.log(`\n${passes} passed, ${failures} failed`);
process.exit(failures ? 1 : 0);
