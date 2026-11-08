# Jersey Nest: Illustrator script for sublimated team orders

Load a player list (size, name, number, shorts), and the script:

1. duplicates your template pieces for every player, in that player's size (plus shorts when the list asks for them),
2. writes each player's **name**, **number** and an optional **size tag** into the pieces, shrinking long names to fit,
3. nests every piece onto artboards the size of your paper or roll, with margins and spacing.

Your templates are never changed. Everything goes on a new `NEST <date>` layer and new artboards to the right of your existing ones.

This works like the workflow in the Aimari Nest PRO tutorial, written as a free ExtendScript for Illustrator CS6 through 2026.

## Install

- One-off: **File > Scripts > Other Script...** and pick `JerseyNest.jsx`.
- Permanent: copy `JerseyNest.jsx` to `Adobe Illustrator <version>/Presets/<language>/Scripts/` and restart Illustrator. It then appears under **File > Scripts**.

## Set up the template document (once per design)

Layer names tell the script what each piece is. Case and accents don't matter.

| Layer | Contains |
| --- | --- |
| `SIZE_S`, `SIZE_M`, `SIZE_L`, `SIZE_XL`, ... | That size's jersey pieces: front, back, sleeves, collar. One **group per piece**. |
| `SHORTS_M`, `SHORTS_L`, ... *(optional)* | Shorts pieces per size. Used only for players with `yes` in the shorts column. |
| `ALL_SIZES` *(optional)* | Pieces made for every player whatever their size. For DTF or vinyl names and numbers only, put just a `NAME` and a `00` text here and skip the SIZE_ layers. |
| `EXTRAS` *(optional)* | Extra objects with a quantity in the name, e.g. a group named `Logo x30` gives 30 copies. |
| `DIGITS` *(optional)* | Vector numbers: ten groups named `0` to `9` (fill, outline and shadow grouped together). Use this when the number font isn't installed or the numbers are multi-layer artwork. |

`TALLA_` / `PANTALONETA_` also work in place of `SIZE_` / `SHORTS_`.

### Inside each piece group

```
Back                  <- group, named after the piece (shown in the output names)
├── NAME              <- point text, literally "NAME" (or NOMBRE / NOME)
├── 00                <- point text, literally "00"
├── SIZE              <- optional small point text, literally "SIZE" (or TALLA), replaced by the size tag
├── NAME_BOX          <- optional rectangle: the widest the name may get
├── NUMBER_BOX        <- optional rectangle: the widest the number may get
└── <clip group>      <- the pattern shape as a clipping mask over the design
```

- Set the font, size, colour and position of `NAME` and `00` exactly as they should print. The real name and number replace the text in place, and keep its paragraph alignment (left, center or right).
- Keep the texts **outside** the clipping mask but inside the piece group, and as live point text (not outlines, not area text).
- Pieces without a name or number simply don't have those texts.
- `NAME_BOX` / `NUMBER_BOX` are deleted from the output. With no `NAME_BOX`, names may use up to the "Max name width" percentage of the piece width.
- Add `NOROTATE` to a piece's name (e.g. `Front NOROTATE`) to keep it upright when rotation is on.
- The bounds used for nesting are the clipping mask's, not the oversized artwork hidden behind it.

### Roll printing and Large Canvas

A normal Illustrator canvas is 227 in (5.77 m) square. For long rolls, make the document a **Large Canvas** document (new document with a width or height over 227 in, or over 600 cm), then paste your templates into it. The script detects Large Canvas and converts units itself.

## The player list

One row per player, columns in this order:

| size | name | number | shorts size | shorts |
| --- | --- | --- | --- | --- |
| M | SMITH | 7 | | yes |
| M | JONES | 10 | L | yes |
| L | LEE | 07 | | no |

- **shorts size** is only needed when it differs from the jersey size. You can leave the column out entirely (4 columns: size, name, number, shorts), or leave both shorts columns out (3 columns).
- **shorts**: `yes`, `y`, `si`, `sí`, `x` or `1` generate shorts. Anything else means no shorts.
- A header row is optional. Commas, semicolons (Excel in Spanish/Portuguese locales) and tabs all work, as does UTF-8 or Excel's ANSI CSV.
- Numbers stay text, so `07` prints as `07`. A blank number or name removes that text from the player's pieces.
- Click **Load CSV...**, or paste straight from Excel / Google Sheets into the box. See `sample-roster.csv`.

## Options

| Option | What it does |
| --- | --- |
| Width / Height | Printable paper size. **Height 0 = roll**: long artboards trimmed to the artwork. |
| Margin / Spacing | Margin is the empty border inside each artboard. Spacing is the gap between pieces. |
| Trim the last artboard | Cuts the last sheet down to its artwork. |
| Order: Best fit | Tightest packing, any piece anywhere. Uses the least paper. |
| Order: By size / By player / By piece | Fills each artboard top-to-bottom in that order, so the output is easy to sort after printing. *New artboard per group* starts a fresh artboard for each size, player or piece type. |
| Allow 90° rotation | Lets the packer turn pieces when that saves paper. |
| UPPERCASE names | Converts names to capitals. |
| Max name width / Shrink proportionally | Long names are squeezed to fit. Proportional shrinks height too; otherwise only width. |
| Use vector digits | Builds numbers from the `DIGITS` layer, scaled to the height of the `00` text. |
| Convert text to outlines | Outlines the names, numbers and tags for RIP/print. |
| Size tag text | Text for the `SIZE` placeholder: `{size}`, `{name}`, `{number}`, `{piece}`, `{index}`. E.g. `{size} #{number}`. |

Settings are remembered between runs (saved to `JerseyNest-settings.txt` in your user data folder).

When it finishes, the script reports the pieces placed, the artboards used and the paper length consumed. Pieces bigger than the paper go on an `UNPLACED` sub-layer.

## Nesting quality

Placement uses MaxRects packing on each piece's **bounding box**: the "simple" engine in the video. It packs tightly and is fast for hundreds of pieces, but it won't slide a sleeve into the curve of a front panel the way true-shape nesting does (Aimari "PRO" engine, Deepnest, SVGnest). For rectangles like fronts and backs the difference is small. For many irregular pieces, expect roughly 5-15% more paper than true-shape nesting.

## Tests

```
node test/run-tests.js
```

The tests cover CSV parsing, the packer (no overlaps, spacing and margins respected), and full runs against a mock of Illustrator's object model. They don't replace a real run in Illustrator.
