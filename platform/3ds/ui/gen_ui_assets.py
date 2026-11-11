#!/usr/bin/env python3
"""Render ui.html with Chromium and write ui_assets.c / ui_assets.h.

Usage: python3 gen_ui_assets.py   (from platform/3ds/ui; needs playwright)

Everything is captured at 1 CSS px = 1 LCD px, once per language (Spanish,
then English with the suffix _en): four 320x240 backgrounds with
the dynamic text hidden, state sprites cropped at their final positions, the
rectangles ui.c draws into, and antialiased glyph atlases for dynamic text."""
import io
import json
import os
import sys

from PIL import Image
from playwright.sync_api import sync_playwright

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_C = os.path.join(HERE, '..', 'ui_assets.c')
OUT_H = os.path.join(HERE, '..', 'ui_assets.h')

FONTS = {  # name: (css size, weight)
    'meta': ('6.5px', 400),
    'metab': ('6.5px', 700),
    'btn': ('7px', 800),
    'chip': ('7.5px', 700),
    'name': ('9px', 800),
    'big': ('11px', 800),
    'val': ('8px', 800),  # 1.0.2 weapons page values
}
CHARS = [chr(c) for c in range(32, 127)] + list('áéíóúÁÉÍÓÚñÑ¿¡✓✗▾▴—·°×')

RECT_IDS = ['main', 'chipF', 'chip3', 'toast', 'slot0', 'slot1', 'slot2',
            'n0', 'n1', 'n2', 't0', 't1', 't2', 'm0', 'm1', 'm2',
            'g0', 'g1', 'g2', 'c0', 'c1', 'c2', 'seg', 's0', 's1', 's2',
            'snd', 'aseg', 'a0', 'a1', 'vm0', 'vm1', 've0', 've1', 'vmb', 'veb', 'perf', 'pb0', 'pb1', 'pb2',
            'pv0', 'pv1', 'pv2', 'detail', 'dcard', 'bf', 'bp', 'br', 'bk',
            # 1.0.2: tabs, weapons page, options page
            'tab0', 'tab1', 'tab2', 'htoast',
            'hph', 'hpv', 'hpt', 'lv', 'hv'] + \
           ['w%d' % i for i in range(8)] + ['wv%d' % i for i in range(8)] + ['wb%d' % i for i in range(8)] + \
           ['kb%d' % i for i in range(4)] + ['kv%d' % i for i in range(4)] + ['u%d' % i for i in range(4)] + \
           ['ol', 'ol0', 'ol1', 'od', 'od0', 'od1', 'op', 'op0', 'op1']

DYNAMIC_TEXT = ['#chipF', '#chip3', '#dchipF', '#dchip3', '#hchipF', '#hchip3', '#ochipF', '#ochip3',
                '#toast', '#htoast', '#otoast',
                '#n0', '#n1', '#n2', '#m0', '#m1', '#m2',
                '#pv0', '#pv1', '#pv2', '#hpv', '#lv', '#hv'] + \
               ['#wv%d' % i for i in range(8)] + ['#kv%d' % i for i in range(4)]
LANGS = ('es', 'en')  # images of the second language get the suffix _en


def rgb565(img):
    img = img.convert('RGB')
    out = []
    for r, g, b in img.getdata():
        out.append(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return out


def main():
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(viewport={'width': 400, 'height': 1200},
                                device_scale_factor=1)
        images, R = {}, None
        for lang in LANGS:
            page.goto('file://' + os.path.join(HERE, 'ui.html'))
            page.wait_for_timeout(200)
            if lang != 'es':
                page.evaluate("document.querySelectorAll('[data-en]').forEach(e => e.textContent = e.dataset.en)")
                page.wait_for_timeout(50)
            got, rects = capture(page)
            if R is None: R = rects
            else:
                for k in R:
                    if any(abs(a - b) > 0.01 for a, b in zip(R[k], rects[k])):
                        raise SystemExit(f'layout differs in {lang}: {k} {R[k]} vs {rects[k]}')
            suffix = '' if lang == 'es' else '_' + lang
            for name, v in got.items(): images[name + suffix] = v
        fonts = glyphs(page)
        browser.close()
    write(images, R, fonts)


def capture(page):
    def rects():
        return page.evaluate('''ids => {
          const out = {};
          for (const id of ids) {
            const e = document.getElementById(id);
            const r = e.getBoundingClientRect();
            const s = e.closest('.screen').getBoundingClientRect();
            out[id] = [r.left - s.left, r.top - s.top, r.width, r.height];
          }
          return out; }''', RECT_IDS)

    def js(code):
        page.evaluate(code)

    def shot(screen):
        png = page.locator('#' + screen).screenshot()
        return Image.open(io.BytesIO(png)).convert('RGB')

    R = rects()
    images = {}

    def crop(img, rect, grow=0):
        x, y, w, h = rect
        box = (int(x) - grow, int(y) - grow,
               int(x) - grow + round(w) + 2 * grow,
               int(y) - grow + round(h) + 2 * grow)
        return img.crop(box), (box[0], box[1])

    # Base state: nothing selected, all dynamic text transparent.
    sel = ', '.join(DYNAMIC_TEXT)
    js(f"document.querySelectorAll('{sel}').forEach(e => e.classList.add('notext'))")
    js("document.querySelectorAll('.slot').forEach(e => e.classList.remove('sel'))")

    # Sprites that must be captured with content visible.
    main = shot('main')
    images['thumb_empty'] = crop(main, R['t0'])
    # Buttons in each state.
    images['btn_save'] = crop(main, R['g0'])
    images['btn_load'] = crop(main, R['c0'])
    images['btn_load_off'] = crop(main, R['c2'])
    js("document.getElementById('g0').classList.add('hot'); document.getElementById('c0').classList.add('hot')")
    hot = shot('main')
    images['btn_save_hot'] = crop(hot, R['g0'])
    images['btn_load_hot'] = crop(hot, R['c0'])
    js("document.getElementById('g0').classList.remove('hot'); document.getElementById('c0').classList.remove('hot')")
    # Segmented control, one image per selected option.
    for i in range(3):
        js(f"for (let k = 0; k < 3; ++k) document.getElementById('s' + k).classList.toggle('on', k == {i})")
        images['seg%d' % i] = crop(shot('main'), R['seg'])
    # Bars, full.
    images['vol_full'] = crop(main, R['vmb'])
    # Music source control: SNES on, MSU-1 on, and SNES on with no pack.
    for name, on, na in (('aseg_snes', 0, False), ('aseg_msu', 1, False), ('aseg_none', 0, True)):
        js(f"for (let k = 0; k < 2; ++k) document.getElementById('a' + k).classList.toggle('on', k == {on});"
           f"document.getElementById('a1').classList.toggle('na', {str(na).lower()})")
        images[name] = crop(shot('main'), R['aseg'])
    js("document.getElementById('a1').classList.remove('na')")
    for name, color in (('ok', '#59e39a'), ('cyan', '#5fd0ff'), ('warn', '#ffb54d'), ('bad', '#ff6b6b')):
        js(f"""for (let k = 0; k < 3; ++k) document.getElementById('pb' + k).innerHTML =
             '<i style="display:block;height:100%;width:100%;background:{color}"></i>'""")
        images['mini_' + name] = crop(shot('main'), R['pb0'])
    js("for (let k = 0; k < 3; ++k) document.getElementById('pb' + k).innerHTML = ''")
    # Chips and toast backgrounds.
    img = shot('main')
    images['chip_ok'] = crop(img, R['chipF'])
    images['chip_cyan'] = crop(img, R['chip3'])
    images['toast_ok'] = crop(img, R['toast'])
    js("document.getElementById('chipF').className = 'chip w notext'; document.getElementById('toast').classList.add('e')")
    img = shot('main')
    images['chip_warn'] = crop(img, R['chipF'])
    images['toast_err'] = crop(img, R['toast'])
    # Slot cards without content, unselected and selected, per position.
    js("document.querySelectorAll('.slot > *').forEach(e => e.classList.add('hide'))")
    img = shot('main')
    for i in range(3):
        images['card%d' % i] = crop(img, R['slot%d' % i], 1)
    js("document.querySelectorAll('.slot').forEach(e => e.classList.add('sel'))")
    img = shot('main')
    for i in range(3):
        images['card%d_sel' % i] = crop(img, R['slot%d' % i], 1)

    # 1.0.2 weapons page: bar sprites (full), then the background without
    # the bars that ui.c draws (life track and every fill).
    hud = shot('hud')
    images['hp_full'] = crop(hud, R['hpt'])
    for i in range(8):
        images['wfull%d' % i] = crop(hud, R['wb%d' % i])
    images['tfull'] = crop(hud, R['kb0'])
    js("document.getElementById('hpb').classList.add('hide')")
    images['hp_track'] = crop(shot('hud'), R['hpt'])
    js("document.querySelectorAll('.up').forEach(e => e.classList.add('on'))")
    hud = shot('hud')
    for i in range(4):
        images['up_on%d' % i] = crop(hud, R['u%d' % i])
    js("document.querySelectorAll('.up').forEach(e => e.classList.remove('on'))")
    js("document.querySelectorAll('.wfill, .tfill').forEach(e => e.classList.add('hide'));"
       "document.getElementById('hpt').style.background = 'none'")
    # Options page: each two-way control in both states.
    for seg in ('ol', 'od', 'op'):
        for i in range(2):
            js(f"for (let k = 0; k < 2; ++k) document.getElementById('{seg}' + k).classList.toggle('on', k == {i})")
            images['%s%d' % (seg, i)] = crop(shot('opts'), R[seg])

    # Backgrounds: no toast, no bars, no chips, slots and seg drawn later.
    js("document.querySelectorAll('.slot').forEach(e => e.classList.remove('sel'))")
    js("['toast','htoast','otoast','vfill','vfill2','chipF','chip3','dchipF','dchip3','hchipF','hchip3','ochipF','ochip3']"
       ".forEach(id => document.getElementById(id).classList.add('hide'))")
    images['bg_main'] = (shot('main'), (0, 0))
    images['bg_detail'] = (shot('detail'), (0, 0))
    images['bg_hud'] = (shot('hud'), (0, 0))
    images['bg_opts'] = (shot('opts'), (0, 0))
    return images, R


def glyphs(page):
    fonts = {}
    page.set_viewport_size({'width': 2000, 'height': 600})
    for fname, (size, weight) in FONTS.items():
        html = ['<div style="position:relative;background:#000;width:2000px;height:40px">']
        for i, ch in enumerate(CHARS):
            esc = '&#%d;' % ord(ch)
            html.append(f'<span id="g{i}" style="position:absolute;left:{i * 16 + 3}px;top:4px;'
                        f'font:{weight} {size} \'DejaVu Sans\';color:#fff;white-space:pre">{esc}</span>')
        html.append('</div>')
        page.set_content('<html><body style="margin:0;background:#000">' + ''.join(html) + '</body></html>')
        page.wait_for_timeout(50)
        boxes = page.evaluate('n => { const o = []; for (let i = 0; i < n; ++i) {'
                              ' const r = document.getElementById("g" + i).getBoundingClientRect();'
                              ' o.push([r.width, r.height]); } return o; }', len(CHARS))
        png = Image.open(io.BytesIO(page.screenshot(clip={'x': 0, 'y': 0, 'width': 2000, 'height': 40}))).convert('RGB')
        h = round(boxes[0][1])
        glyphs = []
        for i, ch in enumerate(CHARS):
            adv = boxes[i][0]
            w = min(16, int(adv) + 6)
            cell = png.crop((i * 16, 4, i * 16 + w, 4 + h))
            alpha = [max(px) for px in cell.getdata()]
            glyphs.append((ord(ch), w, round(adv * 64), alpha))
        fonts[fname] = (h, glyphs)
    return fonts


def write(images, R, fonts):
    c = ['/* Generated by ui/gen_ui_assets.py from ui/ui.html. Do not edit. */',
         '#include "ui_assets.h"', '']
    h = ['/* Generated by ui/gen_ui_assets.py from ui/ui.html. Do not edit. */',
         '#pragma once', '#include <stdint.h>', '',
         'typedef struct UiImage { int16_t x, y; uint16_t w, h; const uint16_t *px; } UiImage;',
         'typedef struct UiRect { int16_t x, y, w, h; } UiRect;',
         'typedef struct UiGlyph { uint16_t code; uint8_t w; uint16_t adv64; uint32_t offset; } UiGlyph;',
         'typedef struct UiFont { uint8_t h; uint16_t count; const UiGlyph *glyphs; const uint8_t *alpha; } UiFont;',
         '']
    seen = {}  # identical pixels (most sprites in both languages) are stored once
    for name, (img, (x, y)) in images.items():
        px = rgb565(img)
        key = (img.width, img.height, tuple(px))
        if key in seen:
            array = seen[key]
        else:
            array = seen[key] = 'px_' + name
            c.append(f'static const uint16_t {array}[{len(px)}] = {{')
            for k in range(0, len(px), 24):
                c.append('  ' + ','.join('0x%04x' % v for v in px[k:k + 24]) + ',')
            c.append('};')
        c.append(f'const UiImage ui_{name} = {{{x}, {y}, {img.width}, {img.height}, {array}}};')
        h.append(f'extern const UiImage ui_{name};')
    h.append('')
    for rid, (x, y, w, hh) in R.items():
        h.append(f'#define UI_R_{rid.upper()} ((UiRect){{{round(x)}, {round(y)}, {round(w)}, {round(hh)}}})')
    h.append('')
    for fname, (fh, glyphs) in fonts.items():
        alpha, table, off = [], [], 0
        for code, w, adv, a in glyphs:
            table.append(f'{{0x{code:04x}, {w}, {adv}, {off}}}')
            alpha.extend(a)
            off += len(a)
        c.append(f'static const uint8_t alpha_{fname}[{len(alpha)}] = {{')
        for k in range(0, len(alpha), 32):
            c.append('  ' + ','.join(str(v) for v in alpha[k:k + 32]) + ',')
        c.append('};')
        c.append(f'static const UiGlyph glyphs_{fname}[{len(table)}] = {{')
        for k in range(0, len(table), 6):
            c.append('  ' + ', '.join(table[k:k + 6]) + ',')
        c.append('};')
        c.append(f'const UiFont ui_font_{fname} = {{{fh}, {len(table)}, glyphs_{fname}, alpha_{fname}}};')
        h.append(f'extern const UiFont ui_font_{fname};')
    open(OUT_C, 'w').write('\n'.join(c) + '\n')
    open(OUT_H, 'w').write('\n'.join(h) + '\n')
    json.dump({k: v for k, v in R.items()}, open(os.path.join(HERE, 'rects.json'), 'w'), indent=1)
    print('wrote', OUT_C, os.path.getsize(OUT_C), 'bytes')


if __name__ == '__main__':
    sys.exit(main())
