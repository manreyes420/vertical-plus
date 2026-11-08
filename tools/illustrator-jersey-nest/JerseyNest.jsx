/*
 * JerseyNest.jsx - mass-produce sublimated jerseys in Adobe Illustrator.
 *
 * Load a roster (size, name, number, shorts size, shorts), and the script
 * duplicates your size templates for every player, writes each player's
 * name / number / size tag, and nests every piece onto artboards of your
 * paper or roll size.
 *
 * Install: File > Scripts > Other Script... (or copy into Illustrator's
 * Presets/<language>/Scripts folder and restart). See README.md for how to
 * set up the template document.
 *
 * ExtendScript is ES3: no let/const, no Array.indexOf, no String.trim, no JSON.
 */

var JN = {};

JN.VERSION = '1.0.0';

/* ------------------------------------------------------------------ */
/* Template conventions                                                */
/* ------------------------------------------------------------------ */

JN.SIZE_LAYER_PREFIXES = ['SIZE_', 'TALLA_', 'TAMANHO_'];
JN.SHORTS_LAYER_PREFIXES = ['SHORTS_', 'PANTALONETA_', 'SHORT_', 'CALCAO_'];
JN.ALL_LAYER_NAMES = ['ALL_SIZES', 'ALL', 'TODAS'];
JN.EXTRAS_LAYER_NAMES = ['EXTRAS', 'OBJECTS', 'OBJETOS'];
JN.DIGITS_LAYER_NAMES = ['DIGITS', 'NUMBERS', 'NUMEROS'];

JN.NAME_TOKENS = ['NAME', 'NOMBRE', 'NOME', 'NOM'];
JN.NUMBER_TOKENS = ['00'];
JN.SIZE_TOKENS = ['SIZE', 'TALLA', 'TAMANHO', 'TAILLE'];
JN.NAME_BOX = 'NAME_BOX';
JN.NUMBER_BOX = 'NUMBER_BOX';
JN.NO_ROTATE_TAG = 'NOROTATE';

JN.UNITS = { 'in': 72, 'cm': 28.346456692913385, 'mm': 2.834645669291339 };

/* Usual garment size order, used when sorting by size. */
JN.SIZE_ORDER = ['2', '4', '6', '8', '10', '12', '14', '16', 'YXS', 'YS', 'YM', 'YL', 'YXL',
    'XXS', '2XS', 'XS', 'S', 'M', 'L', 'XL', 'XXL', '2XL', 'XXXL', '3XL', '4XL', '5XL', '6XL'];

/* ------------------------------------------------------------------ */
/* Small ES3 helpers                                                   */
/* ------------------------------------------------------------------ */

JN.trim = function (s) {
    return String(s === undefined || s === null ? '' : s).replace(/^[\s\u00A0\uFEFF]+|[\s\u00A0\uFEFF]+$/g, '');
};

JN.indexOf = function (arr, v) {
    for (var i = 0; i < arr.length; i++) if (arr[i] === v) return i;
    return -1;
};

JN.stripAccents = function (s) {
    var from = '\u00C1\u00C0\u00C2\u00C3\u00C4\u00C9\u00C8\u00CA\u00CB\u00CD\u00CC\u00CE\u00CF\u00D3\u00D2\u00D4\u00D5\u00D6\u00DA\u00D9\u00DB\u00DC\u00C7\u00D1';
    var to = 'AAAAAEEEEIIIIOOOOOUUUUCN';
    var out = '';
    for (var i = 0; i < s.length; i++) {
        var c = s.charAt(i);
        var k = from.indexOf(c);
        out += k >= 0 ? to.charAt(k) : c;
    }
    return out;
};

/* Upper-case, accent-free, no spaces: used to compare sizes and tokens. */
JN.norm = function (s) {
    return JN.stripAccents(JN.trim(s).toUpperCase()).replace(/\s+/g, '');
};

JN.isYes = function (s) {
    var v = JN.norm(s);
    return v === 'YES' || v === 'Y' || v === 'SI' || v === 'S' || v === 'SIM' || v === 'OUI' ||
        v === '1' || v === 'X' || v === 'TRUE';
};

/* ------------------------------------------------------------------ */
/* CSV / roster parsing                                                */
/* ------------------------------------------------------------------ */

JN.detectDelimiter = function (text) {
    var line = '';
    var lines = text.split('\n');
    for (var i = 0; i < lines.length; i++) {
        if (JN.trim(lines[i]) !== '') { line = lines[i]; break; }
    }
    var cands = [',', ';', '\t'];
    var best = ',', bestCount = -1;
    for (var c = 0; c < cands.length; c++) {
        var n = 0, inQ = false;
        for (var j = 0; j < line.length; j++) {
            var ch = line.charAt(j);
            if (ch === '"') inQ = !inQ;
            else if (!inQ && ch === cands[c]) n++;
        }
        if (n > bestCount) { best = cands[c]; bestCount = n; }
    }
    return best;
};

/* RFC-4180-ish parser: quoted fields, doubled quotes, any delimiter. */
JN.parseCSV = function (text) {
    text = String(text).replace(/^\uFEFF/, '').replace(/\r\n?/g, '\n');
    var delim = JN.detectDelimiter(text);
    var rows = [], row = [], field = '', inQ = false;
    for (var i = 0; i < text.length; i++) {
        var ch = text.charAt(i);
        if (inQ) {
            if (ch === '"') {
                if (text.charAt(i + 1) === '"') { field += '"'; i++; }
                else inQ = false;
            } else field += ch;
        } else if (ch === '"') {
            inQ = true;
        } else if (ch === delim) {
            row.push(field); field = '';
        } else if (ch === '\n') {
            row.push(field); rows.push(row); row = []; field = '';
        } else field += ch;
    }
    if (field !== '' || row.length) { row.push(field); rows.push(row); }
    var out = [];
    for (var r = 0; r < rows.length; r++) {
        var empty = true;
        for (var k = 0; k < rows[r].length; k++) if (JN.trim(rows[r][k]) !== '') { empty = false; break; }
        if (!empty) out.push(rows[r]);
    }
    return out;
};

JN.HEADER_WORDS = ['SIZE', 'TALLA', 'TAMANHO', 'TAILLE', 'NAME', 'NOMBRE', 'NOME', 'NOM',
    'NUMBER', 'NUMERO', 'NUM', 'NO', '#'];

/*
 * Columns, in this order (same as the video):
 *   3 cols: size, name, number
 *   4 cols: size, name, number, shorts(yes/no)
 *   5 cols: size, name, number, shorts size, shorts(yes/no)
 * Numbers are kept as text, so "07" stays "07".
 */
JN.parseRoster = function (text) {
    var rows = JN.parseCSV(text);
    var res = { players: [], warnings: [] };
    if (!rows.length) return res;
    var start = 0;
    var c0 = JN.norm(rows[0][0]), c1 = JN.norm(rows[0][1] || ''), c2 = JN.norm(rows[0][2] || '');
    if (JN.indexOf(JN.HEADER_WORDS, c0) >= 0 || JN.indexOf(JN.HEADER_WORDS, c1) >= 0 ||
        JN.indexOf(JN.HEADER_WORDS, c2) >= 0) start = 1;
    var cols = 0;
    for (var r = start; r < rows.length; r++) {
        var n = rows[r].length;
        while (n > 0 && JN.trim(rows[r][n - 1]) === '') n--;
        if (n > cols) cols = n;
    }
    if (start === 1 && rows[0].length > cols) {
        var hn = rows[0].length;
        while (hn > 0 && JN.trim(rows[0][hn - 1]) === '') hn--;
        cols = Math.max(cols, hn);
    }
    for (var i = start; i < rows.length; i++) {
        var row = rows[i];
        var p = {
            index: res.players.length,
            line: i + 1,
            size: JN.norm(row[0]),
            name: JN.trim(row[1]),
            number: JN.trim(row[2]),
            shortsSize: '',
            shorts: false
        };
        if (cols >= 5) {
            p.shortsSize = JN.norm(row[3]);
            p.shorts = JN.isYes(row[4]);
        } else if (cols === 4) {
            p.shorts = JN.isYes(row[3]);
        }
        if (!p.size) {
            res.warnings.push('Line ' + p.line + ': no size, row skipped.');
            continue;
        }
        res.players.push(p);
    }
    return res;
};

/* ------------------------------------------------------------------ */
/* MaxRects bin packing (bounding-box nesting)                         */
/* ------------------------------------------------------------------ */

/*
 * heuristic 'bssf' = best short side fit (tightest packing, any order)
 * heuristic 'bl'   = bottom-left: fills top-to-bottom, left-to-right, which
 *                    keeps the input order readable (by size / player / piece).
 * The y axis points DOWN inside the bin.
 */
JN.MaxRects = function (w, h) {
    this.w = w;
    this.h = h;
    this.free = [{ x: 0, y: 0, w: w, h: h }];
    this.usedBottom = 0;
    this.count = 0;
};

JN.MaxRects.prototype.find = function (rw, rh, allowRot, heuristic) {
    var best = null, s1 = Infinity, s2 = Infinity;
    var EPS = 1e-6;
    for (var i = 0; i < this.free.length; i++) {
        var f = this.free[i];
        for (var o = 0; o < (allowRot ? 2 : 1); o++) {
            var w = o ? rh : rw, h = o ? rw : rh;
            if (w > f.w + EPS || h > f.h + EPS) continue;
            var a, b;
            if (heuristic === 'bl') {
                a = f.y + h; b = f.x;
            } else {
                var lw = f.w - w, lh = f.h - h;
                a = Math.min(lw, lh); b = Math.max(lw, lh);
            }
            if (a < s1 - EPS || (Math.abs(a - s1) <= EPS && b < s2)) {
                s1 = a; s2 = b;
                best = { x: f.x, y: f.y, w: w, h: h, rot: !!o };
            }
        }
    }
    return best;
};

JN.MaxRects.prototype.place = function (node) {
    var EPS = 1e-6;
    var next = [];
    for (var i = 0; i < this.free.length; i++) {
        var f = this.free[i];
        if (node.x >= f.x + f.w - EPS || node.x + node.w <= f.x + EPS ||
            node.y >= f.y + f.h - EPS || node.y + node.h <= f.y + EPS) {
            next.push(f);
            continue;
        }
        if (node.x > f.x + EPS) next.push({ x: f.x, y: f.y, w: node.x - f.x, h: f.h });
        if (node.x + node.w < f.x + f.w - EPS)
            next.push({ x: node.x + node.w, y: f.y, w: f.x + f.w - node.x - node.w, h: f.h });
        if (node.y > f.y + EPS) next.push({ x: f.x, y: f.y, w: f.w, h: node.y - f.y });
        if (node.y + node.h < f.y + f.h - EPS)
            next.push({ x: f.x, y: node.y + node.h, w: f.w, h: f.y + f.h - node.y - node.h });
    }
    /* prune rectangles contained in another one */
    var pruned = [];
    for (var a = 0; a < next.length; a++) {
        var ra = next[a], contained = false;
        for (var b = 0; b < next.length; b++) {
            if (a === b) continue;
            var rb = next[b];
            if (ra.x >= rb.x - EPS && ra.y >= rb.y - EPS &&
                ra.x + ra.w <= rb.x + rb.w + EPS && ra.y + ra.h <= rb.y + rb.h + EPS) {
                /* identical rectangles: keep only the first */
                if (ra.x === rb.x && ra.y === rb.y && ra.w === rb.w && ra.h === rb.h && a < b) continue;
                contained = true;
                break;
            }
        }
        if (!contained) pruned.push(ra);
    }
    this.free = pruned;
    if (node.y + node.h > this.usedBottom) this.usedBottom = node.y + node.h;
    this.count++;
};

JN.MaxRects.prototype.insert = function (rw, rh, allowRot, heuristic) {
    var node = this.find(rw, rh, allowRot, heuristic);
    if (node) this.place(node);
    return node;
};

/*
 * items: [{w, h, canRotate, group}] in the order to place them.
 * opts:  {binW, binH, spacing, allowRotate, heuristic, firstFit, newBinPerGroup}
 * Returns {bins: [MaxRects], placements: [{item, bin, x, y, rot}], unplaced: [item]}.
 * Spacing is added to every item and to the bin, so the gap between two pieces
 * is exactly `spacing` and no gap is added at the bin edges (margins do that).
 */
JN.pack = function (items, opts) {
    var sp = opts.spacing || 0;
    var bw = opts.binW + sp, bh = opts.binH + sp;
    var bins = [], placements = [], unplaced = [];
    var lastGroup = null;
    for (var i = 0; i < items.length; i++) {
        var it = items[i];
        var w = it.w + sp, h = it.h + sp;
        var rot = opts.allowRotate && it.canRotate;
        var fitsEmpty = (w <= bw + 1e-6 && h <= bh + 1e-6) || (rot && h <= bw + 1e-6 && w <= bh + 1e-6);
        if (!fitsEmpty) { unplaced.push(it); continue; }
        var forceNew = opts.newBinPerGroup && lastGroup !== null && it.group !== lastGroup;
        lastGroup = it.group;
        var node = null, binIndex = -1;
        if (!forceNew && bins.length) {
            var from = opts.firstFit ? 0 : bins.length - 1;
            for (var b = from; b < bins.length && !node; b++) {
                node = bins[b].insert(w, h, rot, opts.heuristic);
                if (node) binIndex = b;
            }
        }
        if (!node) {
            bins.push(new JN.MaxRects(bw, bh));
            binIndex = bins.length - 1;
            node = bins[binIndex].insert(w, h, rot, opts.heuristic);
        }
        placements.push({ item: it, bin: binIndex, x: node.x, y: node.y, rot: node.rot });
    }
    return { bins: bins, placements: placements, unplaced: unplaced };
};

/* Sort a copy of `items` for the chosen order mode. */
JN.sortItems = function (items, mode) {
    var arr = items.slice(0);
    var key;
    if (mode === 'free') {
        key = function (a, b) {
            var d = Math.max(b.w, b.h) - Math.max(a.w, a.h);
            if (Math.abs(d) > 1e-6) return d;
            return b.w * b.h - a.w * a.h;
        };
    } else {
        var fields = {
            size: ['sizeRank', 'playerIndex', 'pieceRank', 'seq'],
            player: ['playerIndex', 'pieceRank', 'seq'],
            piece: ['pieceRank', 'sizeRank', 'playerIndex', 'seq']
        }[mode];
        key = function (a, b) {
            for (var f = 0; f < fields.length; f++) {
                var d = a[fields[f]] - b[fields[f]];
                if (d) return d;
            }
            return 0;
        };
    }
    /* stable sort: ExtendScript's Array.sort is not guaranteed stable */
    for (var i = 0; i < arr.length; i++) arr[i].__i = i;
    arr.sort(function (a, b) { return key(a, b) || a.__i - b.__i; });
    return arr;
};

JN.sizeRank = function (size, rosterOrder) {
    var k = JN.indexOf(JN.SIZE_ORDER, size);
    if (k >= 0) return k;
    var r = JN.indexOf(rosterOrder, size);
    return 1000 + (r >= 0 ? r : 999);
};

JN.formatTag = function (fmt, ctx) {
    return String(fmt)
        .replace(/\{size\}/gi, ctx.size || '')
        .replace(/\{name\}/gi, ctx.name || '')
        .replace(/\{number\}/gi, ctx.number || '')
        .replace(/\{piece\}/gi, ctx.piece || '')
        .replace(/\{index\}/gi, ctx.index === undefined ? '' : String(ctx.index));
};

/* ================================================================== */
/* Everything below talks to Illustrator.                              */
/* ================================================================== */

/* --- geometry -------------------------------------------------------- */

/*
 * Visible bounds [left, top, right, bottom] (y up), ignoring artwork hidden
 * by clipping masks. Illustrator's own bounds of a clip group include the
 * masked-out artwork, which would ruin the nesting.
 */
JN.bounds = function (item) {
    if (item.typename === 'GroupItem') {
        if (item.clipped) {
            var clip = JN.findClipPath(item);
            if (clip) return clip.geometricBounds;
        }
        var u = null;
        for (var i = 0; i < item.pageItems.length; i++) {
            var c = item.pageItems[i];
            if (c.hidden || c.guides) continue;
            var b = JN.bounds(c);
            if (!b) continue;
            u = u ? [Math.min(u[0], b[0]), Math.max(u[1], b[1]), Math.max(u[2], b[2]), Math.min(u[3], b[3])] : b;
        }
        return u || item.geometricBounds;
    }
    if (item.typename === 'TextFrame') return item.geometricBounds;
    return item.visibleBounds;
};

JN.findClipPath = function (group) {
    for (var i = 0; i < group.pageItems.length; i++) {
        var c = group.pageItems[i];
        if (c.typename === 'PathItem' && c.clipping) return c;
        if (c.typename === 'CompoundPathItem' && c.pathItems.length && c.pathItems[0].clipping) return c;
    }
    return null;
};

/* --- document scanning ------------------------------------------------ */

JN.layerKind = function (name) {
    var n = JN.norm(name);
    var i;
    for (i = 0; i < JN.SIZE_LAYER_PREFIXES.length; i++)
        if (n.indexOf(JN.SIZE_LAYER_PREFIXES[i]) === 0)
            return { kind: 'size', size: n.substr(JN.SIZE_LAYER_PREFIXES[i].length) };
    for (i = 0; i < JN.SHORTS_LAYER_PREFIXES.length; i++)
        if (n.indexOf(JN.SHORTS_LAYER_PREFIXES[i]) === 0)
            return { kind: 'shorts', size: n.substr(JN.SHORTS_LAYER_PREFIXES[i].length) };
    if (JN.indexOf(JN.ALL_LAYER_NAMES, n) >= 0) return { kind: 'all' };
    if (JN.indexOf(JN.EXTRAS_LAYER_NAMES, n) >= 0) return { kind: 'extras' };
    if (JN.indexOf(JN.DIGITS_LAYER_NAMES, n) >= 0) return { kind: 'digits' };
    return null;
};

JN.pieceName = function (item, fallbackIndex) {
    var n = JN.trim(item.name);
    if (!n && item.typename === 'TextFrame') n = 'Text';
    return n || ('Piece ' + (fallbackIndex + 1));
};

/* Piece name without "x30" quantities or the NOROTATE tag. */
JN.cleanPieceName = function (n) {
    return JN.trim(String(n).replace(/\bx\s*\d+\s*$/i, '').replace(new RegExp('[\\s\\-_\\[\\(]*' + JN.NO_ROTATE_TAG + '[\\]\\)]?', 'i'), ''));
};

JN.scanDocument = function (doc) {
    var s = { sizes: {}, shorts: {}, all: [], extras: [], digits: null, layers: [], pieceOrder: [] };
    for (var i = 0; i < doc.layers.length; i++) {
        var L = doc.layers[i];
        var k = JN.layerKind(L.name);
        if (!k) continue;
        s.layers.push(L);
        var pieces = [];
        for (var j = L.pageItems.length - 1; j >= 0; j--) { /* bottom to top = stacking order */
            var it = L.pageItems[j];
            if (it.guides) continue;
            var raw = JN.pieceName(it, pieces.length);
            var clean = JN.cleanPieceName(raw);
            var qty = 1, m = raw.match(/\bx\s*(\d+)\s*$/i);
            if (m) qty = parseInt(m[1], 10);
            pieces.push({
                item: it,
                name: clean,
                qty: qty,
                canRotate: !new RegExp(JN.NO_ROTATE_TAG, 'i').test(raw)
            });
            if (k.kind !== 'digits' && JN.indexOf(s.pieceOrder, JN.norm(clean)) < 0) s.pieceOrder.push(JN.norm(clean));
        }
        if (k.kind === 'size') s.sizes[k.size] = (s.sizes[k.size] || []).concat(pieces);
        else if (k.kind === 'shorts') s.shorts[k.size] = (s.shorts[k.size] || []).concat(pieces);
        else if (k.kind === 'all') s.all = s.all.concat(pieces);
        else if (k.kind === 'extras') s.extras = s.extras.concat(pieces);
        else if (k.kind === 'digits') {
            s.digits = s.digits || {};
            for (var d = 0; d < pieces.length; d++) {
                var dn = JN.trim(pieces[d].item.name);
                if (/^[0-9]$/.test(dn)) s.digits[dn] = pieces[d].item;
            }
        }
    }
    s.hasSizeLayers = false;
    for (var sz in s.sizes) if (s.sizes.hasOwnProperty(sz)) { s.hasSizeLayers = true; break; }
    return s;
};

/* Returns a list of problems; empty means OK. */
JN.validate = function (scan, players) {
    var errs = [];
    var missing = {}, missingShorts = {};
    for (var i = 0; i < players.length; i++) {
        var p = players[i];
        if (scan.hasSizeLayers && !scan.sizes[p.size]) missing[p.size] = (missing[p.size] || 0) + 1;
        if (p.shorts) {
            var ss = p.shortsSize || p.size;
            if (!scan.shorts[ss]) missingShorts[ss] = (missingShorts[ss] || 0) + 1;
        }
    }
    var k;
    for (k in missing) if (missing.hasOwnProperty(k))
        errs.push('No layer "SIZE_' + k + '" for ' + missing[k] + ' player(s) in size ' + k + '.');
    for (k in missingShorts) if (missingShorts.hasOwnProperty(k))
        errs.push('No layer "SHORTS_' + k + '" for ' + missingShorts[k] + ' player(s) who need shorts in size ' + k + '.');
    if (players.length && !scan.hasSizeLayers && !scan.all.length)
        errs.push('A player list is loaded but there are no SIZE_ or ALL_SIZES template layers.');
    else if (!scan.hasSizeLayers && !scan.all.length && !scan.extras.length)
        errs.push('No template layers found. Name layers SIZE_S, SIZE_M, ... (and SHORTS_M, ALL_SIZES, EXTRAS) - see README.');
    if (scan.digits) {
        var miss = [];
        for (var d = 0; d <= 9; d++) if (!scan.digits[String(d)]) miss.push(d);
        if (miss.length) errs.push('DIGITS layer is missing groups named: ' + miss.join(', ') + '.');
    }
    return errs;
};

/* --- personalization -------------------------------------------------- */

JN.collect = function (item, out) {
    if (item.typename === 'TextFrame') {
        var t = JN.norm(item.contents);
        var nm = JN.norm(item.name);
        if (JN.indexOf(JN.NAME_TOKENS, t) >= 0 || nm === 'NAME') out.names.push(item);
        else if (JN.indexOf(JN.NUMBER_TOKENS, t) >= 0 || nm === 'NUMBER') out.numbers.push(item);
        else if (JN.indexOf(JN.SIZE_TOKENS, t) >= 0 || nm === 'SIZE') out.tags.push(item);
        return;
    }
    var n = JN.norm(item.name);
    if (n === JN.NAME_BOX) { out.nameBox = item; return; }
    if (n === JN.NUMBER_BOX) { out.numberBox = item; return; }
    if (item.typename === 'GroupItem') {
        for (var i = 0; i < item.pageItems.length; i++) JN.collect(item.pageItems[i], out);
    }
};

JN.justification = function (tf) {
    try {
        var j = tf.textRange.paragraphAttributes.justification;
        if (j === Justification.LEFT) return 'left';
        if (j === Justification.RIGHT) return 'right';
    } catch (e) {}
    return 'center';
};

/* Shrink `item` to maxW if needed and re-anchor it on the original box. */
JN.fitAndAnchor = function (item, ref, align, maxW, keepProportions) {
    var b = JN.bounds(item);
    var w = b[2] - b[0];
    if (maxW > 0 && w > maxW) {
        var pct = maxW / w * 100;
        var py = keepProportions ? pct : 100;
        item.resize(pct, py, true, true, true, true, py, Transformation.CENTER);
        b = JN.bounds(item);
    }
    var dx;
    if (align === 'left') dx = ref[0] - b[0];
    else if (align === 'right') dx = ref[2] - b[2];
    else dx = (ref[0] + ref[2]) / 2 - (b[0] + b[2]) / 2;
    var dy = (ref[1] + ref[3]) / 2 - (b[1] + b[3]) / 2;
    item.translate(dx, dy);
};

/* Replace a "00" text frame with vector digit artwork of the same height. */
JN.vectorNumber = function (tf, number, digits) {
    var dup = tf.duplicate();
    var outlined = dup.createOutline();
    var ref = outlined.geometricBounds;
    outlined.remove();
    var g = tf.layer.groupItems.add();
    g.move(tf, ElementPlacement.PLACEBEFORE);
    var x = 0, tallest = 0;
    for (var i = 0; i < number.length; i++) {
        var src = digits[number.charAt(i)];
        if (!src) continue;
        var d = src.duplicate(g, ElementPlacement.PLACEATEND);
        d.hidden = false;
        var b = JN.bounds(d);
        d.translate(x - b[0], -b[3]); /* sit every digit on the same baseline */
        x += (b[2] - b[0]) * 1.04;
        if (b[1] - b[3] > tallest) tallest = b[1] - b[3];
    }
    tf.remove();
    if (!g.pageItems.length) { g.remove(); return null; }
    var refH = ref[1] - ref[3];
    if (tallest > 0) {
        var pct = refH / tallest * 100;
        g.resize(pct, pct, true, true, true, true, pct, Transformation.CENTER);
    }
    g.name = 'NUMBER ' + number;
    return { item: g, ref: ref };
};

/*
 * Fill in name, number and size tag inside one duplicated piece.
 * Returns the piece's artwork, which is a new object when the piece itself was
 * a lone text frame that got replaced (vector digits, outlines), or null when
 * it was removed (a player without a number).
 */
JN.personalize = function (item, player, pieceName, opts, digits) {
    var root = item;
    var f = { names: [], numbers: [], tags: [], nameBox: null, numberBox: null };
    JN.collect(item, f);
    var pb = JN.bounds(item);
    var pieceW = pb[2] - pb[0];
    var name = player ? (opts.uppercase ? player.name.toUpperCase() : player.name) : '';
    var number = player ? player.number : '';
    var nameMax = f.nameBox ? (JN.bounds(f.nameBox)[2] - JN.bounds(f.nameBox)[0]) : pieceW * opts.namePct / 100;
    var numMax = f.numberBox ? (JN.bounds(f.numberBox)[2] - JN.bounds(f.numberBox)[0]) : 0;
    var toOutline = [];
    var i, tf, ref, align;

    for (i = 0; i < f.names.length; i++) {
        tf = f.names[i];
        if (!player) continue;
        if (!name) { if (tf === root) root = null; tf.remove(); continue; }
        ref = tf.geometricBounds; align = JN.justification(tf);
        tf.contents = name;
        JN.fitAndAnchor(tf, ref, align, nameMax, opts.keepProportions);
        toOutline.push(tf);
    }
    for (i = 0; i < f.numbers.length; i++) {
        tf = f.numbers[i];
        if (!player) continue;
        if (!number) { if (tf === root) root = null; tf.remove(); continue; }
        ref = tf.geometricBounds; align = JN.justification(tf);
        if (digits && opts.vectorDigits) {
            var wasRoot = tf === root;
            var v = JN.vectorNumber(tf, number, digits);
            if (v) JN.fitAndAnchor(v.item, v.ref, align, numMax, true);
            if (wasRoot) root = v ? v.item : null;
        } else {
            tf.contents = number;
            JN.fitAndAnchor(tf, ref, align, numMax, true);
            toOutline.push(tf);
        }
    }
    for (i = 0; i < f.tags.length; i++) {
        tf = f.tags[i];
        ref = tf.geometricBounds; align = JN.justification(tf);
        tf.contents = JN.formatTag(opts.tagFormat, {
            size: player ? player.pieceSize : '', name: name, number: number, piece: pieceName,
            index: player ? player.index + 1 : ''
        });
        JN.fitAndAnchor(tf, ref, align, 0, true);
        toOutline.push(tf);
    }
    if (f.nameBox) f.nameBox.remove();
    if (f.numberBox) f.numberBox.remove();
    if (opts.outlineText) {
        for (i = 0; i < toOutline.length; i++) {
            try {
                var o = toOutline[i].createOutline();
                if (toOutline[i] === root) root = o;
            } catch (e) {}
        }
    }
    return root;
};

/* --- settings persistence --------------------------------------------- */

JN.settingsFile = function () {
    return new File(Folder.userData + '/JerseyNest-settings.txt');
};

JN.DEFAULTS = {
    units: 'in', paperW: '63', paperH: '0', margin: '0.5', spacing: '0.25', fitLast: 'true',
    order: 'free', newPerGroup: 'false', rotate: 'true', uppercase: 'true', namePct: '80',
    keepProportions: 'false', vectorDigits: 'true', outlineText: 'false', tagFormat: '{size}',
    roster: ''
};

JN.loadSettings = function () {
    var s = {}, k;
    for (k in JN.DEFAULTS) if (JN.DEFAULTS.hasOwnProperty(k)) s[k] = JN.DEFAULTS[k];
    var f = JN.settingsFile();
    if (f.exists && f.open('r')) {
        f.encoding = 'UTF-8';
        var txt = f.read();
        f.close();
        var lines = txt.split('\n');
        for (var i = 0; i < lines.length; i++) {
            var eq = lines[i].indexOf('=');
            if (eq < 1) continue;
            k = lines[i].substr(0, eq);
            if (s.hasOwnProperty(k)) s[k] = decodeURIComponent(lines[i].substr(eq + 1));
        }
    }
    return s;
};

JN.saveSettings = function (s) {
    var f = JN.settingsFile();
    f.encoding = 'UTF-8';
    if (!f.open('w')) return;
    for (var k in s) if (s.hasOwnProperty(k)) f.writeln(k + '=' + encodeURIComponent(s[k]));
    f.close();
};

JN.readTextFile = function (file) {
    file.encoding = 'UTF-8';
    file.open('r');
    var t = file.read();
    file.close();
    /* Excel "CSV (Comma delimited)" is ANSI, not UTF-8: retry if it decoded badly */
    if (t.indexOf('\uFFFD') >= 0) {
        file.encoding = 'CP1252';
        file.open('r');
        t = file.read();
        file.close();
    }
    return t;
};

/* --- dialog ----------------------------------------------------------- */

JN.dialog = function (scan, s) {
    var w = new Window('dialog', 'Jersey Nest ' + JN.VERSION);
    w.orientation = 'column';
    w.alignChildren = 'fill';

    /* roster */
    var pr = w.add('panel', undefined, 'Player list  (size, name, number, shorts size, shorts yes/no)');
    pr.alignChildren = 'fill';
    var top = pr.add('group');
    var loadBtn = top.add('button', undefined, 'Load CSV...');
    var clearBtn = top.add('button', undefined, 'Clear');
    var count = top.add('statictext', undefined, '');
    count.characters = 40;
    var roster = pr.add('edittext', undefined, s.roster, { multiline: true, scrolling: true });
    roster.preferredSize = [520, 170];
    pr.add('statictext', undefined, 'Paste from Excel / Sheets or type: M, SMITH, 7, , yes');

    function refreshCount() {
        var r = JN.parseRoster(roster.text);
        var shorts = 0, sizes = {}, list = [];
        for (var i = 0; i < r.players.length; i++) {
            if (r.players[i].shorts) shorts++;
            var z = r.players[i].size;
            if (!sizes[z]) { sizes[z] = 0; list.push(z); }
            sizes[z]++;
        }
        var parts = [];
        for (var j = 0; j < list.length; j++) parts.push(list[j] + ':' + sizes[list[j]]);
        count.text = r.players.length + ' players, ' + shorts + ' shorts   ' + parts.join('  ');
    }
    roster.onChanging = refreshCount;
    loadBtn.onClick = function () {
        var f = File.openDialog('Choose the player list', '*.csv;*.txt;*.tsv');
        if (!f) return;
        roster.text = JN.readTextFile(f);
        refreshCount();
    };
    clearBtn.onClick = function () { roster.text = ''; refreshCount(); };

    /* paper */
    var pp = w.add('panel', undefined, 'Paper');
    pp.orientation = 'row';
    pp.alignChildren = 'center';
    pp.add('statictext', undefined, 'Units');
    var units = pp.add('dropdownlist', undefined, ['in', 'cm', 'mm']);
    units.selection = Math.max(0, JN.indexOf(['in', 'cm', 'mm'], s.units));
    function num(label, val, tip) {
        pp.add('statictext', undefined, label);
        var e = pp.add('edittext', undefined, val);
        e.characters = 5;
        if (tip) e.helpTip = tip;
        return e;
    }
    var pw = num('Width', s.paperW, 'Printable width of the sheet or roll');
    var ph = num('Height', s.paperH, '0 = roll: one long artboard, trimmed to the artwork');
    var mg = num('Margin', s.margin, 'Empty border inside each artboard');
    var spc = num('Spacing', s.spacing, 'Gap between pieces');
    var fitLast = w.add('checkbox', undefined, 'Trim the last artboard to its artwork (saves paper)');
    fitLast.value = s.fitLast === 'true';

    /* layout */
    var pl = w.add('panel', undefined, 'Placement');
    pl.orientation = 'row';
    pl.add('statictext', undefined, 'Order');
    var ORDERS = ['free', 'size', 'player', 'piece'];
    var order = pl.add('dropdownlist', undefined,
        ['Best fit (least paper)', 'By size', 'By player (keep kits together)', 'By piece (fronts, backs...)']);
    order.selection = Math.max(0, JN.indexOf(ORDERS, s.order));
    var newPer = pl.add('checkbox', undefined, 'New artboard per group');
    newPer.value = s.newPerGroup === 'true';
    var rot = pl.add('checkbox', undefined, 'Allow 90\u00B0 rotation');
    rot.value = s.rotate === 'true';
    rot.helpTip = 'Name a piece "... NOROTATE" to keep that piece upright';
    function syncNewPer() { newPer.enabled = order.selection.index !== 0; }
    order.onChange = syncNewPer;
    syncNewPer();

    /* text */
    var pt = w.add('panel', undefined, 'Names and numbers');
    pt.alignChildren = 'left';
    var r1 = pt.add('group');
    var upper = r1.add('checkbox', undefined, 'UPPERCASE names');
    upper.value = s.uppercase === 'true';
    r1.add('statictext', undefined, 'Max name width');
    var npct = r1.add('edittext', undefined, s.namePct);
    npct.characters = 4;
    npct.helpTip = 'Percent of the piece width, used when the piece has no NAME_BOX';
    r1.add('statictext', undefined, '% of piece');
    var keep = r1.add('checkbox', undefined, 'Shrink proportionally');
    keep.value = s.keepProportions === 'true';
    keep.helpTip = 'Off: long names get narrower but keep their height';
    var r2 = pt.add('group');
    var vec = r2.add('checkbox', undefined, 'Use vector digits (DIGITS layer)');
    vec.value = s.vectorDigits === 'true' && !!scan.digits;
    vec.enabled = !!scan.digits;
    var outl = r2.add('checkbox', undefined, 'Convert text to outlines');
    outl.value = s.outlineText === 'true';
    var r3 = pt.add('group');
    r3.add('statictext', undefined, 'Size tag text');
    var tag = r3.add('edittext', undefined, s.tagFormat);
    tag.characters = 24;
    r3.add('statictext', undefined, '{size} {name} {number} {piece} {index}');

    /* summary of what was found */
    var found = [];
    for (var z in scan.sizes) if (scan.sizes.hasOwnProperty(z)) found.push(z + '(' + scan.sizes[z].length + ')');
    var fs = [];
    for (var z2 in scan.shorts) if (scan.shorts.hasOwnProperty(z2)) fs.push(z2);
    w.add('statictext', undefined, 'Templates: ' + (found.length ? found.join(' ') : 'no SIZE_ layers') +
        (fs.length ? '   Shorts: ' + fs.join(' ') : '') +
        (scan.all.length ? '   All sizes: ' + scan.all.length : '') +
        (scan.extras.length ? '   Extras: ' + scan.extras.length : '') +
        (scan.digits ? '   Digits: yes' : ''));

    var bg = w.add('group');
    bg.alignment = 'right';
    bg.add('button', undefined, 'Cancel', { name: 'cancel' });
    bg.add('button', undefined, 'Generate', { name: 'ok' });

    refreshCount();
    if (w.show() !== 1) return null;

    return {
        units: units.selection.text, paperW: pw.text, paperH: ph.text, margin: mg.text, spacing: spc.text,
        fitLast: String(fitLast.value), order: ORDERS[order.selection.index], newPerGroup: String(newPer.value),
        rotate: String(rot.value), uppercase: String(upper.value), namePct: npct.text,
        keepProportions: String(keep.value), vectorDigits: String(vec.value), outlineText: String(outl.value),
        tagFormat: tag.text, roster: roster.text
    };
};

/* --- progress window -------------------------------------------------- */

JN.Progress = function (total) {
    this.w = new Window('palette', 'Jersey Nest');
    this.label = this.w.add('statictext', undefined, 'Working...');
    this.label.characters = 40;
    this.bar = this.w.add('progressbar', undefined, 0, Math.max(1, total));
    this.bar.preferredSize = [320, 12];
    this.w.show();
};
JN.Progress.prototype.set = function (v, text) {
    this.bar.value = v;
    if (text) this.label.text = text;
    this.w.update();
};
JN.Progress.prototype.close = function () { this.w.close(); };

/* --- main ------------------------------------------------------------- */

JN.run = function () {
    if (!app.documents.length) {
        alert('Open the document that holds your jersey templates first.');
        return;
    }
    var doc = app.activeDocument;
    var scan = JN.scanDocument(doc);
    var s = JN.loadSettings();
    var ui = JN.dialog(scan, s);
    if (!ui) return;
    JN.saveSettings(ui);

    var roster = JN.parseRoster(ui.roster);
    var players = roster.players;
    if (!players.length && !scan.extras.length) {
        alert('The player list is empty.');
        return;
    }
    var errs = JN.validate(scan, players);
    if (errs.length) {
        alert('Fix these before generating:\n\n' + errs.join('\n'));
        return;
    }

    /* Large Canvas documents report coordinates divided by scaleFactor. */
    var SF = 1;
    try { if (doc.scaleFactor) SF = doc.scaleFactor; } catch (e) {}
    var u = JN.UNITS[ui.units] / SF;
    var paperW = parseFloat(ui.paperW) * u;
    var paperH = parseFloat(ui.paperH) * u;
    var margin = (parseFloat(ui.margin) || 0) * u;
    var spacing = (parseFloat(ui.spacing) || 0) * u;
    var MAX_AB = 16383; /* Illustrator's artboard size limit, in document units */
    var roll = !(paperH > 0);
    if (!(paperW > 0)) { alert('Enter a paper width.'); return; }
    if (paperW > MAX_AB) { alert('Paper width is larger than Illustrator allows. Use a Large Canvas document (see README).'); return; }
    /* Roll: segments of ~45% of the canvas (about 100 in, or 1000 in on a Large
       Canvas), each trimmed to its artwork, so they still fit beside the
       existing artboards. */
    if (roll) paperH = Math.floor(MAX_AB * 0.45);
    if (paperH > MAX_AB) { alert('Paper height is larger than Illustrator allows. Use a Large Canvas document (see README).'); return; }
    var binW = paperW - 2 * margin, binH = paperH - 2 * margin;
    if (binW <= 0 || binH <= 0) { alert('Margins are larger than the paper.'); return; }

    var opts = {
        uppercase: ui.uppercase === 'true',
        namePct: parseFloat(ui.namePct) || 0,
        keepProportions: ui.keepProportions === 'true',
        vectorDigits: ui.vectorDigits === 'true',
        outlineText: ui.outlineText === 'true',
        tagFormat: ui.tagFormat
    };

    /* Build the job list: one entry per piece to produce. */
    var rosterSizes = [];
    for (var i = 0; i < players.length; i++)
        if (JN.indexOf(rosterSizes, players[i].size) < 0) rosterSizes.push(players[i].size);
    var jobs = [];
    function addJob(piece, player, size, kitPart) {
        jobs.push({ piece: piece, player: player, size: size, kitPart: kitPart });
    }
    for (i = 0; i < players.length; i++) {
        var p = players[i];
        var list = scan.hasSizeLayers ? scan.sizes[p.size] : [];
        var k;
        for (k = 0; k < list.length; k++) addJob(list[k], p, p.size, 'jersey');
        for (k = 0; k < scan.all.length; k++) addJob(scan.all[k], p, p.size, 'all');
        if (p.shorts) {
            var ss = p.shortsSize || p.size;
            var sl = scan.shorts[ss];
            for (k = 0; k < sl.length; k++) addJob(sl[k], p, ss, 'shorts');
        }
    }
    for (i = 0; i < scan.extras.length; i++)
        for (var q = 0; q < scan.extras[i].qty; q++) addJob(scan.extras[i], null, '', 'extra');

    if (!jobs.length) { alert('Nothing to generate.'); return; }

    /* Unlock and show template layers while duplicating; restored at the end. */
    var restore = [];
    for (i = 0; i < scan.layers.length; i++) {
        var L = scan.layers[i];
        restore.push({ layer: L, locked: L.locked, visible: L.visible });
        L.locked = false;
        L.visible = true;
    }

    var stamp = new Date();
    var out = doc.layers.add();
    out.name = 'NEST ' + stamp.getFullYear() + '-' + (stamp.getMonth() + 1) + '-' + stamp.getDate() + ' ' +
        stamp.getHours() + 'h' + (stamp.getMinutes() < 10 ? '0' : '') + stamp.getMinutes();
    out.zOrder(ZOrderMethod.BRINGTOFRONT);

    var prog = new JN.Progress(jobs.length * 2);
    var items = [];
    var problems = [];
    try {
        for (i = 0; i < jobs.length; i++) {
            var j = jobs[i];
            if (i % 5 === 0) prog.set(i, 'Personalizing ' + (i + 1) + ' / ' + jobs.length);
            var dup = j.piece.item.duplicate(out, ElementPlacement.PLACEATEND);
            dup.hidden = false;
            dup.locked = false;
            if (j.player) j.player.pieceSize = j.size;
            try {
                dup = JN.personalize(dup, j.player, j.piece.name, opts, scan.digits);
            } catch (e1) {
                problems.push('Line ' + (j.player ? j.player.line : '-') + ' ' + j.piece.name + ': ' + e1.message);
            }
            if (!dup) continue; /* e.g. a lone "00" for a player without a number */
            dup.name = j.player
                ? (j.size + ' ' + (j.player.number || '') + ' ' + j.player.name + ' - ' + j.piece.name)
                : j.piece.name;
            var b = JN.bounds(dup);
            items.push({
                art: dup,
                w: b[2] - b[0],
                h: b[1] - b[3],
                canRotate: j.piece.canRotate,
                seq: i,
                playerIndex: j.player ? j.player.index : 1e6,
                sizeRank: j.player ? JN.sizeRank(j.size, rosterSizes) : 1e6,
                pieceRank: JN.indexOf(scan.pieceOrder, JN.norm(j.piece.name)),
                group: null
            });
            var it = items[items.length - 1];
            it.group = ui.order === 'size' ? 'S' + it.sizeRank
                : ui.order === 'player' ? 'P' + it.playerIndex
                : ui.order === 'piece' ? 'C' + it.pieceRank : null;
        }

        prog.set(jobs.length, 'Nesting ' + items.length + ' pieces...');
        var sorted = JN.sortItems(items, ui.order);
        var res = JN.pack(sorted, {
            binW: binW, binH: binH, spacing: spacing,
            allowRotate: ui.rotate === 'true',
            heuristic: ui.order === 'free' ? 'bssf' : 'bl',
            firstFit: ui.order === 'free',
            newBinPerGroup: ui.order !== 'free' && ui.newPerGroup === 'true'
        });

        /* Artboards: a row starting to the right of the existing ones. */
        var ex = null;
        for (i = 0; i < doc.artboards.length; i++) {
            var r = doc.artboards[i].artboardRect;
            ex = ex ? [Math.min(ex[0], r[0]), Math.max(ex[1], r[1]), Math.max(ex[2], r[2]), Math.min(ex[3], r[3])] : r;
        }
        var gap = 36 / SF;
        var boards = [];
        var x0 = ex[2] + gap * 4, y0 = ex[1];
        var cursorX = x0, cursorY = y0, rowH = 0;
        for (var bi = 0; bi < res.bins.length; bi++) {
            var hUsed = paperH;
            var isLast = bi === res.bins.length - 1;
            if (roll || (isLast && ui.fitLast === 'true')) {
                hUsed = Math.min(paperH, res.bins[bi].usedBottom - spacing + 2 * margin);
                if (hUsed < 2 * margin + 1) hUsed = 2 * margin + 1;
            }
            var rect = [cursorX, cursorY, cursorX + paperW, cursorY - hUsed];
            var ab;
            try {
                ab = doc.artboards.add(rect);
            } catch (e2) {
                /* ran off the canvas horizontally: start a new row below */
                cursorX = x0;
                cursorY = cursorY - rowH - gap;
                rowH = 0;
                rect = [cursorX, cursorY, cursorX + paperW, cursorY - hUsed];
                try {
                    ab = doc.artboards.add(rect);
                } catch (e3) {
                    throw new Error('Out of canvas space after ' + bi + ' artboard(s). Use a Large Canvas ' +
                        'document, a smaller paper height, or delete old NEST artboards.');
                }
            }
            ab.name = 'Nest ' + (bi + 1) + ' of ' + res.bins.length;
            boards.push(rect);
            cursorX += paperW + gap;
            if (hUsed > rowH) rowH = hUsed;
        }

        for (i = 0; i < res.placements.length; i++) {
            if (i % 10 === 0) prog.set(jobs.length + i, 'Placing ' + (i + 1) + ' / ' + res.placements.length);
            var pl = res.placements[i];
            var art = pl.item.art;
            if (pl.rot) art.rotate(90);
            var ab2 = boards[pl.bin];
            var bb = JN.bounds(art);
            art.translate(ab2[0] + margin + pl.x - bb[0], ab2[1] - margin - pl.y - bb[1]);
        }

        var unplacedLayer = null;
        if (res.unplaced.length) {
            unplacedLayer = out.layers.add();
            unplacedLayer.name = 'UNPLACED - larger than the paper';
            for (i = 0; i < res.unplaced.length; i++)
                res.unplaced[i].art.move(unplacedLayer, ElementPlacement.PLACEATEND);
        }
    } finally {
        prog.close();
        for (i = 0; i < restore.length; i++) {
            restore[i].layer.visible = restore[i].visible;
            restore[i].layer.locked = restore[i].locked;
        }
    }

    /* Summary */
    var totalLen = 0;
    for (i = 0; i < boards.length; i++) totalLen += boards[i][1] - boards[i][3];
    var lenIn = totalLen * SF / 72;
    var shortsCount = 0;
    for (i = 0; i < players.length; i++) if (players[i].shorts) shortsCount++;
    var msg = 'Done.\n\n' +
        players.length + ' players, ' + shortsCount + ' shorts\n' +
        res.placements.length + ' pieces on ' + boards.length + ' artboard(s)\n' +
        'Paper used: ' + (Math.round(lenIn * 10) / 10) + ' in  /  ' + (Math.round(lenIn * 2.54 * 10) / 10) + ' cm of length\n' +
        'Output layer: "' + out.name + '"';
    if (res.unplaced.length) msg += '\n\n' + res.unplaced.length + ' piece(s) are larger than the paper and were moved to the UNPLACED layer.';
    if (roster.warnings.length) msg += '\n\nList warnings:\n' + roster.warnings.slice(0, 10).join('\n');
    if (problems.length) msg += '\n\nProblems:\n' + problems.slice(0, 10).join('\n');
    alert(msg);
};

if (typeof app !== 'undefined' && typeof Window !== 'undefined') {
    try {
        JN.run();
    } catch (err) {
        alert('Jersey Nest stopped: ' + err.message + (err.line ? ' (line ' + err.line + ')' : ''));
    }
}
