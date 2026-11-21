// Writes TombEngine/Renderer/TouchIcons.h, the images of the on-screen touch controls
// (see TombEngine/Specific/Input/TouchControls.h), from the files beside this script:
//
//   svg/<icon>.svg    high-res icons, 512 x 512, and svg/button.svg, ring.svg and thumb.svg,
//                     the circles. The engine draws them with nanosvg at the size they show.
//   touch_glyphs.png  low-res icons: TRX's touch glyphs, 7 x 2 cells of 16 pixels with a
//                     15 x 15 icon each. Written as pixel masks, drawn as solid squares.
//
//   node make_touch_icons.mjs <output TouchIcons.h>

import fs from 'fs';
import path from 'path';
import zlib from 'zlib';

const HERE = path.dirname(new URL(import.meta.url).pathname);
const ICONS = ['jump', 'crouch', 'walk', 'run', 'draw', 'look', 'action', 'roll', 'menu', 'pause', 'bullet', 'swim']; // TouchIcon order.
const SHAPES = ['button', 'ring', 'thumb']; // TouchShape order.
const LOW_RES_COLUMNS = 7;
const LOW_RES_CELL = 16;
const LOW_RES_SIZE = 15;
const RAW_STRING_MAX = 16000; // MSVC's limit for one string literal is 16380 bytes.

const output = process.argv[2];
if (!output) {
  console.error('usage: node make_touch_icons.mjs <output TouchIcons.h>');
  process.exit(1);
}

// Reads an 8-bit palette PNG into RGBA pixels.
function readPalettePng(file) {
  const data = fs.readFileSync(file);
  let pos = 8;
  let header = null, palette = null, transparency = null;
  const idat = [];
  while (pos < data.length) {
    const length = data.readUInt32BE(pos);
    const type = data.toString('latin1', pos + 4, pos + 8);
    const chunk = data.subarray(pos + 8, pos + 8 + length);
    if (type === 'IHDR') header = { width: chunk.readUInt32BE(0), height: chunk.readUInt32BE(4), depth: chunk[8], colorType: chunk[9] };
    else if (type === 'PLTE') palette = chunk;
    else if (type === 'tRNS') transparency = chunk;
    else if (type === 'IDAT') idat.push(chunk);
    pos += 12 + length;
  }
  if (header.depth !== 8 || header.colorType !== 3)
    throw new Error(`${file}: only 8-bit palette PNGs are read`);

  const stride = header.width;
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const pixels = Buffer.alloc(header.width * header.height * 4);
  let previous = Buffer.alloc(stride);
  for (let y = 0; y < header.height; y++) {
    const filter = raw[y * (stride + 1)];
    const line = Buffer.from(raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1)));
    for (let x = 0; x < stride; x++) {
      const a = x > 0 ? line[x - 1] : 0, b = previous[x], c = x > 0 ? previous[x - 1] : 0;
      const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
      const predictor = [0, a, b, (a + b) >> 1, pa <= pb && pa <= pc ? a : pb <= pc ? b : c][filter];
      line[x] = (line[x] + predictor) & 255;
    }
    for (let x = 0; x < header.width; x++) {
      const index = line[x], out = (y * header.width + x) * 4;
      pixels[out] = palette[index * 3]; pixels[out + 1] = palette[index * 3 + 1]; pixels[out + 2] = palette[index * 3 + 2];
      pixels[out + 3] = transparency && index < transparency.length ? transparency[index] : 255;
    }
    previous = line;
  }
  return { width: header.width, pixels };
}

// Low-res masks: ' ' clear, '.' black, '+' half-clear black, '#' white.
function lowResMasks() {
  const image = readPalettePng(path.join(HERE, 'touch_glyphs.png'));
  return ICONS.map((name, index) => {
    const cellX = (index % LOW_RES_COLUMNS) * LOW_RES_CELL, cellY = Math.floor(index / LOW_RES_COLUMNS) * LOW_RES_CELL;
    const rows = [];
    for (let y = 0; y < LOW_RES_SIZE; y++) {
      let row = '';
      for (let x = 0; x < LOW_RES_SIZE; x++) {
        const i = ((cellY + y) * image.width + cellX + x) * 4;
        const alpha = image.pixels[i + 3];
        row += alpha < 16 ? ' ' : alpha < 200 ? '+' : image.pixels[i] > 128 ? '#' : '.';
      }
      rows.push(row);
    }
    return { name, rows };
  });
}

// An SVG file as a C++ raw string literal, without its comments.
function svgLiteral(name) {
  const svg = fs.readFileSync(path.join(HERE, 'svg', `${name}.svg`), 'utf8')
    .replace(/<!--[\s\S]*?-->/g, '')
    .split('\n').map(line => line.trim()).filter(line => line).join('\n');
  if (svg.length > RAW_STRING_MAX)
    throw new Error(`svg/${name}.svg: ${svg.length} bytes, more than one string literal can hold`);
  return `\t\t// ${name}\n\t\tR"svg(${svg})svg",`;
}

const lines = [
  '#pragma once',
  '',
  '// Images of the on-screen touch controls (see Specific/Input/TouchControls.h). Written by',
  '// Tools/TouchIcons/make_touch_icons.mjs; edit the files there instead.',
  '',
  'namespace TEN::Renderer',
  '{',
  '\t// High-res icons in TouchIcon order, SVGs of 512 x 512. Most are icons of Font Awesome Free',
  '\t// 6.7.2 by Fonticons, Inc. (https://fontawesome.com), licensed CC BY 4.0.',
  `\tconstexpr const char* TOUCH_ICON_SVGS[${ICONS.length}] =`,
  '\t{',
  ...ICONS.map(svgLiteral),
  '\t};',
  '',
  '\t// Circles in TouchShape order, SVGs of 100 x 100. A shape with the id "ring" gets the',
  '\t// border width of the touch controls, its outer edge staying where the SVG has it.',
  `\tconstexpr const char* TOUCH_SHAPE_SVGS[${SHAPES.length}] =`,
  '\t{',
  ...SHAPES.map(svgLiteral),
  '\t};',
  '',
  '\t// Low-res icons in TouchIcon order: TRX\'s touch glyphs, drawn as solid squares. Each row',
  '\t// is a line of pixels: \' \' clear, \'.\' black, \'+\' half-clear black, \'#\' white.',
  `\tconstexpr auto TOUCH_ICON_LOW_RES_SIZE = ${LOW_RES_SIZE};`,
  '',
  `\tconstexpr const char* TOUCH_ICONS_LOW_RES[${ICONS.length}][TOUCH_ICON_LOW_RES_SIZE] =`,
  '\t{',
  ...lowResMasks().map(({ name, rows }) => [`\t\t// ${name}`, '\t\t{', ...rows.map(row => `\t\t\t"${row}",`), '\t\t},'].join('\n')),
  '\t};',
  '}',
  '',
];

fs.writeFileSync(output, lines.join('\n'));
console.log(`${output}: ${ICONS.length} icons, ${SHAPES.length} shapes`);
