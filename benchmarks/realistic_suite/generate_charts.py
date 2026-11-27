#!/usr/bin/env python3
"""
Generate transparent-background (RGBA) high-DPI benchmark charts for Vortex v1.3.0
with the Vortex logo rendered cleanly in the top-left corner of every image.
"""
import json
import math
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

SUITE_DIR = Path(__file__).resolve().parent
REPO_ROOT = SUITE_DIR.parent.parent
ASSETS_DIR = REPO_ROOT / "docs" / "assets"
RESULTS_PATH = SUITE_DIR / "results_v1.3.0.json"
LOGO_PATH = ASSETS_DIR / "vortex_logo.png"

S = 2  # 2x supersampling


def px(v):
    return int(round(v * S))


def load_font(name, size):
    candidates = [
        Path(r"C:\Windows\Fonts") / name,
        Path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
    ]
    for p in candidates:
        if p.exists():
            return ImageFont.truetype(str(p), px(size))
    return ImageFont.load_default()


FONTS = {
    "title": load_font("segoeuib.ttf", 18.0),
    "sub": load_font("segoeui.ttf", 11.2),
    "h2": load_font("segoeuib.ttf", 13.2),
    "h3": load_font("segoeuib.ttf", 11.8),
    "label": load_font("segoeuib.ttf", 12.4),
    "label_sm": load_font("segoeui.ttf", 10.3),
    "mono": load_font("consola.ttf", 11.2),
    "mono_b": load_font("consolab.ttf", 11.8),
    "mono_sm": load_font("consola.ttf", 10.2),
    "pill": load_font("consolab.ttf", 9.8),
}


def prepare_logo(size_1x):
    logo = Image.open(LOGO_PATH).convert("RGBA")
    target = px(size_1x)
    return logo.resize((target, target), Image.Resampling.LANCZOS)


def blend_layer(base_img, draw_fn):
    """Draw onto a fresh RGBA layer and proper alpha-composite onto base_img."""
    layer = Image.new("RGBA", base_img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    draw_fn(d)
    return Image.alpha_composite(base_img, layer)


def rounded_rect(base_img, box, radius, fill=None, outline=None, width=1):
    def _fn(d):
        d.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)
    return blend_layer(base_img, _fn)


def gradient_bar(base_img, x0, y0, w, h, c_start, c_end, radius=6):
    if w <= 0:
        return base_img
    bar = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    bd = ImageDraw.Draw(bar)
    for x in range(w):
        t = x / max(1, w - 1)
        col = tuple(int(c_start[k] + (c_end[k] - c_start[k]) * t) for k in range(4))
        bd.line([(x, 0), (x, h)], fill=col)
    mask = Image.new("L", (w, h), 0)
    md = ImageDraw.Draw(mask)
    md.rounded_rectangle([0, 0, w - 1, h - 1], radius=px(radius), fill=255)
    bar.putalpha(mask)
    layer = Image.new("RGBA", base_img.size, (0, 0, 0, 0))
    layer.paste(bar, (x0, y0), bar)
    return Image.alpha_composite(base_img, layer)


def create_card_canvas(width, height, title, subtitle, badge_text="v1.3.0 · REALISTIC BENCHMARKS"):
    img = Image.new("RGBA", (px(width), px(height)), (0, 0, 0, 0))
    pad = 10
    card_box = [px(pad), px(pad), px(width - pad), px(height - pad)]

    # Main floating card (corners outside rounded card stay alpha=0!)
    img = rounded_rect(
        img,
        card_box,
        radius=px(18),
        fill=(11, 16, 28, 245),
        outline=(148, 163, 184, 72),
        width=px(1.5),
    )

    # Top accent gradient stripe
    img = gradient_bar(
        img,
        px(pad + 22),
        px(pad + 1),
        px(width - 2 * pad - 44),
        px(3.5),
        (79, 70, 229, 255),
        (16, 185, 129, 255),
        radius=2,
    )

    # Top-left Vortex logo on subtle dark-indigo badge
    logo_x, logo_y = px(30), px(26)
    def _glow(d):
        d.ellipse(
            [logo_x - px(4), logo_y - px(4), logo_x + px(54), logo_y + px(54)],
            fill=(30, 41, 68, 185),
            outline=(52, 211, 153, 125),
            width=px(1.3),
        )
    img = blend_layer(img, _glow)
    logo = prepare_logo(50)
    img.alpha_composite(logo, (logo_x, logo_y))

    # Right pill + divider
    tmp_draw = ImageDraw.Draw(img)
    tb = tmp_draw.textbbox((0, 0), badge_text, font=FONTS["pill"])
    bw = tb[2] - tb[0] + px(22)
    bx1 = px(width - 30)
    bx0 = bx1 - bw
    by0, by1 = px(31), px(57)
    img = rounded_rect(
        img,
        [bx0, by0, bx1, by1],
        radius=px(13),
        fill=(16, 185, 129, 36),
        outline=(52, 211, 153, 145),
        width=px(1.2),
    )

    def _header_txt(d):
        d.text((px(96), px(27)), title, font=FONTS["title"], fill=(248, 250, 252, 255))
        d.text((px(96), px(54)), subtitle, font=FONTS["sub"], fill=(148, 163, 184, 240))
        d.text((bx0 + px(11), by0 + px(6)), badge_text, font=FONTS["pill"], fill=(110, 231, 183, 255))
        d.line([px(30), px(88), px(width - 30), px(88)], fill=(148, 163, 184, 48), width=px(1))

    return blend_layer(img, _header_txt)


def build_total_chart(data):
    W, H = 1240, 770
    results = data["results"]
    by_name = {r["name"]: r for r in results}
    cpu = data["host"].get("cpu", "AMD Ryzen 5 5500")
    main_entries = [r for r in results if r["name"] != "Python 3.12"]
    main_entries.sort(key=lambda r: r["total_ms"])
    runs = main_entries[0].get("runs", 5)

    img = create_card_canvas(
        W, H,
        "Vortex v1.3.0 — Benchmark Realiste 9 Langages (Temps Total, 5 Taches)",
        f"{cpu} (6C/12T) · IEEE-754 strict sans -ffast-math · Mediane {runs} runs · Vortex --release & --safe",
    )

    fastest_ms = main_entries[0]["total_ms"]
    max_val = max(r["total_ms"] for r in main_entries)
    max_axis = max(400.0, math.ceil(max_val / 100.0) * 100.0)

    bar_x0 = px(384)
    bar_w = px(550)
    grid_top = px(102)
    grid_bot = px(574)

    def _grid(d):
        for i in range(5):
            tick = int(round(max_axis * i / 4.0))
            gx = bar_x0 + int(bar_w * (tick / max_axis))
            d.line([gx, grid_top, gx, grid_bot], fill=(148, 163, 184, 34), width=px(1))
            lbl = f"{tick} ms"
            tb = d.textbbox((0, 0), lbl, font=FONTS["mono_sm"])
            d.text((gx - (tb[2] - tb[0]) // 2, grid_bot + px(7)), lbl, font=FONTS["mono_sm"], fill=(148, 163, 184, 195))
    img = blend_layer(img, _grid)

    y = 108
    row_h = 52
    for idx, r in enumerate(main_entries, 1):
        name = r["name"]
        sub = r["sublabel"]
        tot = r["total_ms"]
        ratio = tot / fastest_ms
        is_rel = name == "Vortex (--release)"
        is_safe = name == "Vortex (--safe)"

        if is_rel:
            img = rounded_rect(
                img,
                [px(28), px(y - 5), px(W - 28), px(y + row_h - 9)],
                radius=px(10),
                fill=(16, 185, 129, 34),
                outline=(52, 211, 153, 145),
                width=px(1.3),
            )
        elif is_safe:
            img = rounded_rect(
                img,
                [px(28), px(y - 5), px(W - 28), px(y + row_h - 9)],
                radius=px(10),
                fill=(99, 102, 241, 34),
                outline=(129, 140, 248, 150),
                width=px(1.3),
            )

        img = rounded_rect(
            img,
            [bar_x0, px(y + 10), bar_x0 + bar_w, px(y + 29)],
            radius=px(6),
            fill=(24, 34, 54, 195),
        )
        fill_w = max(px(8), int(bar_w * min(tot, max_axis) / max_axis))
        if is_rel:
            img = gradient_bar(img, bar_x0, px(y + 10), fill_w, px(19), (79, 70, 229, 255), (16, 185, 129, 255), radius=6)
        elif is_safe:
            img = gradient_bar(img, bar_x0, px(y + 10), fill_w, px(19), (139, 92, 246, 255), (56, 189, 248, 255), radius=6)
        elif idx <= 3:
            img = gradient_bar(img, bar_x0, px(y + 10), fill_w, px(19), (56, 189, 248, 240), (59, 130, 246, 240), radius=6)
        else:
            img = rounded_rect(img, [bar_x0, px(y + 10), bar_x0 + fill_w, px(y + 29)], radius=px(6), fill=(82, 98, 122, 235))

        def _row_txt(d, idx=idx, y=y, name=name, sub=sub, tot=tot, ratio=ratio, is_rel=is_rel, is_safe=is_safe):
            rank_col = (110, 231, 183, 255) if is_rel else ((199, 210, 254, 255) if is_safe else (100, 116, 139, 225))
            name_col = (255, 255, 255, 255) if (is_rel or is_safe) else (226, 232, 240, 245)
            sub_col = (167, 243, 208, 210) if is_rel else ((199, 210, 254, 210) if is_safe else (148, 163, 184, 215))
            d.text((px(42), px(y + 4)), f"{idx:02d}", font=FONTS["mono_b"], fill=rank_col)
            d.text((px(76), px(y + 2)), name, font=FONTS["label"], fill=name_col)
            d.text((px(76), px(y + 21)), sub, font=FONTS["label_sm"], fill=sub_col)

            val_txt = f"{tot:6.2f} ms"
            rat_txt = f"{ratio:.2f}x"
            val_col = (110, 231, 183, 255) if is_rel else ((199, 210, 254, 255) if is_safe else (241, 245, 249, 245))
            rat_col = (110, 231, 183, 255) if is_rel else ((199, 210, 254, 255) if is_safe else (148, 163, 184, 225))
            d.text((px(956), px(y + 11)), val_txt, font=FONTS["mono_b"], fill=val_col)
            tb_r = d.textbbox((0, 0), rat_txt, font=FONTS["mono_b"])
            d.text((px(W - 42) - (tb_r[2] - tb_r[0]), px(y + 11)), rat_txt, font=FONTS["mono_b"], fill=rat_col)
        img = blend_layer(img, _row_txt)
        y += row_h

    # Out-of-scale row (Python 3.12 only; Vortex --safe is now a full ranked bar above!)
    if "Python 3.12" in by_name:
        cy = 606
        row = by_name["Python 3.12"]
        img = rounded_rect(
            img,
            [px(28), px(cy), px(W - 28), px(cy + 44)],
            radius=px(9),
            fill=(30, 41, 59, 135),
            outline=(71, 85, 105, 105),
            width=px(1.1),
        )
        img = rounded_rect(
            img,
            [bar_x0, px(cy + 13), bar_x0 + bar_w, px(cy + 31)],
            radius=px(5),
            fill=(15, 23, 42, 195),
            outline=(100, 116, 139, 95),
            width=px(1),
        )

        def _oos_txt(d, cy=cy, row=row):
            d.text((px(42), px(cy + 13)), "10", font=FONTS["mono_b"], fill=(148, 163, 184, 210))
            d.text((px(76), px(cy + 5)), "Python 3.12", font=FONTS["label"], fill=(203, 213, 225, 235))
            d.text((px(76), px(cy + 24)), "Interprete CPython sans JIT (T5 GIL monothread)", font=FONTS["label_sm"], fill=(148, 163, 184, 210))
            scale_note = f"HORS ECHELLE ({row['total_ms'] / fastest_ms:.1f}x vs #1)"
            tb_sn = d.textbbox((0, 0), scale_note, font=FONTS["mono_sm"])
            d.text((bar_x0 + (bar_w - (tb_sn[2] - tb_sn[0])) // 2, px(cy + 15)), scale_note, font=FONTS["mono_sm"], fill=(148, 163, 184, 225))
            d.text((px(956), px(cy + 14)), f"{row['total_ms']:8.2f} ms", font=FONTS["mono_b"], fill=(148, 163, 184, 225))
            rt = f"{row['total_ms'] / fastest_ms:.1f}x"
            tbr = d.textbbox((0, 0), rt, font=FONTS["mono_b"])
            d.text((px(W - 42) - (tbr[2] - tbr[0]), px(cy + 14)), rt, font=FONTS["mono_b"], fill=(148, 163, 184, 210))
        img = blend_layer(img, _oos_txt)

    vx = by_name["Vortex (--release)"]
    vxs = by_name["Vortex (--safe)"]
    img = rounded_rect(
        img,
        [px(28), px(664), px(W - 28), px(742)],
        radius=px(8),
        fill=(15, 23, 42, 205),
        outline=(148, 163, 184, 48),
        width=px(1),
    )
    ft1 = (
        f"Vortex --release: T1={vx['t1_ms']}ms · T2={vx['t2_ms']}ms · T3={vx['t3_ms']}ms · "
        f"T4={vx['t4_ms']}ms · T5={vx['t5_ms']}ms | AOT=0ms, Start+JIT={vx['startup_ms']}ms"
    )
    ft2 = (
        f"Vortex --safe   : T1={vxs['t1_ms']}ms · T2={vxs['t2_ms']}ms · T3={vxs['t3_ms']}ms · "
        f"T4={vxs['t4_ms']}ms · T5={vxs['t5_ms']}ms | Bornes + arenes ON · Checksums 100% verifies"
    )
    img = blend_layer(img, lambda d: (
        d.text((px(42), px(674)), ft1, font=FONTS["mono_sm"], fill=(167, 243, 208, 235)),
        d.text((px(42), px(704)), ft2, font=FONTS["mono_sm"], fill=(199, 210, 254, 235)),
    ))

    out = ASSETS_DIR / "bench_realistic_total.png"
    img.resize((W, H), Image.Resampling.LANCZOS).save(out, "PNG")
    return out


def build_breakdown_chart(data):
    W, H = 1240, 840
    results = data["results"]
    img = create_card_canvas(
        W, H,
        "Detail par Epreuve (T1..T5) & Latence AOT / Startup JIT",
        "Memes algorithmes sur tous les langages · Vortex --release (sans bornes) ET Vortex --safe (bornes+arenes) inclus",
    )

    show_names = [
        ("C (Clang -O3)", "C -O3"),
        ("C++ (Clang++ -O3)", "C++ -O3"),
        ("Rust (-O3)", "Rust -O3"),
        ("Vortex (--release)", "Vortex --rel"),
        ("C# (.NET v4.8)", "C# .NET4.8"),
        ("Go (1.27)", "Go 1.27"),
        ("Java (OpenJDK 21)", "Java 21"),
        ("Vortex (--safe)", "Vortex --safe"),
        ("Node.js (v26 V8)", "Node v26"),
    ]
    by_name = {r["name"]: r for r in results}

    panels = [
        ("T1 · N-Body Gravite (f64, 600x25)", "t1_ms", "ms"),
        ("T2 · Quicksort Hoare (500k i64)", "t2_ms", "ms"),
        ("T3 · MatMul Dense f64 (320x320x320)", "t3_ms", "ms"),
        ("T4 · Crible Eratosthene (10M)", "t4_ms", "ms"),
        ("T5 · Mandelbrot Multi-Coeur (12T)", "t5_ms", "ms"),
        ("Latence AOT (indigo) + Startup/JIT (cyan/vert/violet)", "aot_start", "ms"),
    ]

    pw, ph = 576, 218
    cols_x = [30, 634]
    rows_y = [104, 340, 576]

    for p_idx, (ptitle, field, unit) in enumerate(panels):
        px0 = cols_x[p_idx % 2]
        py0 = rows_y[p_idx // 2]
        img = rounded_rect(
            img,
            [px(px0), px(py0), px(px0 + pw), px(py0 + ph)],
            radius=px(12),
            fill=(15, 23, 40, 215),
            outline=(148, 163, 184, 56),
            width=px(1),
        )
        img = blend_layer(img, lambda d, t=ptitle, x=px0, y=py0: d.text((px(x + 16), px(y + 10)), t, font=FONTS["h3"], fill=(241, 245, 249, 250)))

        if field != "aot_start":
            vals = [(full, short, by_name[full][field]) for full, short in show_names if full in by_name]
            vmax = max(v for _, _, v in vals) * 1.12
            bx0 = px(px0 + 120)
            bw_max = px(348)
            ry = py0 + 36
            for full, short, val in vals:
                is_rel = full == "Vortex (--release)"
                is_safe = full == "Vortex (--safe)"
                if is_rel:
                    img = rounded_rect(
                        img,
                        [px(px0 + 10), px(ry - 1), px(px0 + pw - 10), px(ry + 17)],
                        radius=px(5),
                        fill=(16, 185, 129, 32),
                        outline=(52, 211, 153, 95),
                        width=px(0.8),
                    )
                elif is_safe:
                    img = rounded_rect(
                        img,
                        [px(px0 + 10), px(ry - 1), px(px0 + pw - 10), px(ry + 17)],
                        radius=px(5),
                        fill=(99, 102, 241, 32),
                        outline=(129, 140, 248, 95),
                        width=px(0.8),
                    )
                img = rounded_rect(img, [bx0, px(ry + 3), bx0 + bw_max, px(ry + 13)], radius=px(4), fill=(26, 36, 56, 195))
                fw = max(px(5), int(bw_max * (val / vmax)))
                if is_rel:
                    img = gradient_bar(img, bx0, px(ry + 3), fw, px(10), (79, 70, 229, 255), (16, 185, 129, 255), radius=4)
                elif is_safe:
                    img = gradient_bar(img, bx0, px(ry + 3), fw, px(10), (139, 92, 246, 255), (56, 189, 248, 255), radius=4)
                else:
                    img = rounded_rect(img, [bx0, px(ry + 3), bx0 + fw, px(ry + 13)], radius=px(4), fill=(82, 98, 122, 235))
                txt_col = (110, 231, 183, 255) if is_rel else ((199, 210, 254, 255) if is_safe else (203, 213, 225, 235))
                val_col = (110, 231, 183, 255) if is_rel else ((199, 210, 254, 255) if is_safe else (226, 232, 240, 235))
                img = blend_layer(img, lambda d, s=short, v=val, tc=txt_col, vc=val_col, r=ry, x=px0: (
                    d.text((px(x + 16), px(r + 1)), s, font=FONTS["mono_sm"], fill=tc),
                    d.text((bx0 + bw_max + px(10), px(r + 1)), f"{v:6.2f} {unit}", font=FONTS["mono_sm"], fill=vc),
                ))
                ry += 19
        else:
            cap = 1800.0
            bx0 = px(px0 + 120)
            bw_max = px(308)
            ry = py0 + 36
            for full, short in show_names:
                if full not in by_name:
                    continue
                r = by_name[full]
                aot = r["compile_ms"]
                st = r["startup_ms"]
                is_rel = full == "Vortex (--release)"
                is_safe = full == "Vortex (--safe)"
                if is_rel:
                    img = rounded_rect(
                        img,
                        [px(px0 + 10), px(ry - 1), px(px0 + pw - 10), px(ry + 17)],
                        radius=px(5),
                        fill=(16, 185, 129, 32),
                        outline=(52, 211, 153, 95),
                        width=px(0.8),
                    )
                elif is_safe:
                    img = rounded_rect(
                        img,
                        [px(px0 + 10), px(ry - 1), px(px0 + pw - 10), px(ry + 17)],
                        radius=px(5),
                        fill=(99, 102, 241, 32),
                        outline=(129, 140, 248, 95),
                        width=px(0.8),
                    )
                img = rounded_rect(img, [bx0, px(ry + 3), bx0 + bw_max, px(ry + 13)], radius=px(4), fill=(26, 36, 56, 195))
                wa = int(bw_max * min(aot, cap) / cap)
                ws = max(px(4), int(bw_max * min(st, max(0.0, cap - min(aot, cap))) / cap))
                if wa > 0:
                    img = rounded_rect(img, [bx0, px(ry + 3), bx0 + wa, px(ry + 13)], radius=px(4), fill=(99, 102, 241, 230))
                bar_col = (16, 185, 129, 250) if is_rel else ((139, 92, 246, 250) if is_safe else (56, 189, 248, 235))
                img = rounded_rect(
                    img,
                    [bx0 + wa, px(ry + 3), min(bx0 + bw_max, bx0 + wa + ws), px(ry + 13)],
                    radius=px(4),
                    fill=bar_col,
                )
                lbl = f"{int(round(aot))}+{int(round(st))}ms"
                txt_col = (110, 231, 183, 255) if is_rel else ((199, 210, 254, 255) if is_safe else (203, 213, 225, 235))
                val_col = (110, 231, 183, 255) if is_rel else ((199, 210, 254, 255) if is_safe else (226, 232, 240, 235))
                img = blend_layer(img, lambda d, s=short, l=lbl, tc=txt_col, vc=val_col, r=ry, x=px0: (
                    d.text((px(x + 16), px(r + 1)), s, font=FONTS["mono_sm"], fill=tc),
                    d.text((bx0 + bw_max + px(8), px(r + 1)), l, font=FONTS["mono_sm"], fill=vc),
                ))
                ry += 19

    out = ASSETS_DIR / "bench_realistic_breakdown.png"
    img.resize((W, H), Image.Resampling.LANCZOS).save(out, "PNG")
    return out


def build_silicon_chart(data):
    W, H = 1240, 640
    sil = data["silicon"]
    cpu = sil.get("cpu_name", "AMD Ryzen 5 5500")
    gpu = sil.get("gpu_name", "gfx1031")
    img = create_card_canvas(
        W, H,
        "Acceleration Silicium Optionnelle — import \"std/hw\" (CPU AVX2 BLIS & GPU HwBuf)",
        f"{cpu} (AVX2+FMA3, L1=32K · L2=512K · L3=16M) + {gpu} (20 CU OpenCL, HwBuf VRAM + Cache) · Separe du benchmark pur",
        badge_text="std/hw · SILICON v2",
    )

    f64_naive_gf = round(0.268435456 / (sil["vx_scalar_f64_512_ms"] / 1000.0), 2)
    f64_vx_sp = round(sil["vx_scalar_f64_512_ms"] / sil["vx_hw_f64_512_ms"], 1)
    f64_c_sp = round(sil["c_f64_512_ms"] / sil["vx_hw_f64_512_ms"], 1)
    res_ms = sil.get("vx_hw_f32_gpu_resident_ms", sil["vx_hw_f32_gpu_1024_ms"])
    res_gf = sil.get("vx_hw_f32_gpu_resident_gflops", sil["vx_hw_f32_gpu_gflops"])
    pipe_ms = sil.get("vx_hw_f32_pipeline_ms", 0.0)
    cache_hits = sil.get("vx_hw_gpu_cache_hits", 0)

    left_rows = [
        ("Vortex Auto-Vec AVX2 (1T)", f"{sil['vx_scalar_f64_512_ms']:.2f} ms · boucle i-k-j sans FMA", f64_naive_gf, (82, 98, 122, 235), False),
        ("C (Clang -O3 Auto-Vec 1T)", f"{sil['c_f64_512_ms']:.2f} ms · boucle i-k-j stricte", sil["c_f64_512_gflops"], (59, 130, 246, 235), False),
        ("Vortex std/hw (BLIS AVX2 6C)", f"{sil['vx_hw_f64_512_ms']:.2f} ms · route L3_CACHE (err 0.0)", sil["vx_hw_f64_gflops"], None, True),
    ]
    right_rows = [
        ("C (Clang -O3 Auto-Vec 1T)", f"{sil['c_f32_1024_ms']:.2f} ms · boucle i-k-j stricte", sil["c_f32_1024_gflops"], (59, 130, 246, 235), False),
        ("Vortex std/hw CPU (AVX2 6C)", f"{sil['vx_hw_f32_cpu_1024_ms']:.2f} ms · BLIS 4x16 + FMA3", sil["vx_hw_f32_cpu_gflops"], (99, 102, 241, 240), False),
        (f"Vortex std/hw GPU (PCIe H2D+D2H)", f"{sil['vx_hw_f32_gpu_1024_ms']:.2f} ms · 64x64 float4 + transferts PCIe", sil["vx_hw_f32_gpu_gflops"], (56, 189, 248, 240), False),
        (f"Vortex std/hw VRAM (HwBuf 4x4)", f"{res_ms:.3f} ms · resident VRAM 64x64 float4 (err 0.0)", res_gf, None, True),
    ]

    panels = [
        ("GEMM Double Precision f64 — 512 x 512 x 512 (Plus haut = meilleur)", left_rows, 30, 108, 68),
        ("GEMM Simple Precision f32 — 1024 x 1024 x 1024 (Plus haut = meilleur)", right_rows, 634, 90, 58),
    ]

    pw, ph = 576, 438
    py0 = 108
    for title, rows, px0, step_y, start_off in panels:
        img = rounded_rect(
            img,
            [px(px0), px(py0), px(px0 + pw), px(py0 + ph)],
            radius=px(14),
            fill=(15, 23, 40, 220),
            outline=(148, 163, 184, 60),
            width=px(1),
        )
        img = blend_layer(img, lambda d, t=title, x=px0: (
            d.text((px(x + 20), px(py0 + 14)), t, font=FONTS["h2"], fill=(248, 250, 252, 255)),
            d.line([px(x + 20), px(py0 + 44), px(x + pw - 20), px(py0 + 44)], fill=(148, 163, 184, 42), width=px(1)),
        ))

        gmax = max(r[2] for r in rows) * 1.12
        ry = py0 + start_off
        for label, sub, gf, col, hi in rows:
            if hi:
                img = rounded_rect(
                    img,
                    [px(px0 + 14), px(ry - 6), px(px0 + pw - 14), px(ry + 73)],
                    radius=px(10),
                    fill=(16, 185, 129, 34),
                    outline=(52, 211, 153, 145),
                    width=px(1.3),
                )
            bx0 = px(px0 + 24)
            bw_max = px(380)
            img = rounded_rect(img, [bx0, px(ry + 42), bx0 + bw_max, px(ry + 61)], radius=px(6), fill=(26, 36, 56, 200))
            fw = max(px(8), int(bw_max * (gf / gmax)))
            if hi:
                img = gradient_bar(img, bx0, px(ry + 42), fw, px(19), (79, 70, 229, 255), (16, 185, 129, 255), radius=6)
            else:
                img = rounded_rect(img, [bx0, px(ry + 42), bx0 + fw, px(ry + 61)], radius=px(6), fill=col)

            gf_txt = f"{gf:7.2f} GFLOP/s"
            img = blend_layer(img, lambda d, l=label, s=sub, gt=gf_txt, h=hi, r=ry, x=px0: (
                d.text((px(x + 24), px(r)), l, font=FONTS["label"], fill=(255, 255, 255, 255) if h else (226, 232, 240, 245)),
                d.text((px(x + 24), px(r + 20)), s, font=FONTS["label_sm"], fill=(167, 243, 208, 215) if h else (148, 163, 184, 225)),
                d.text((bx0 + bw_max + px(12), px(r + 43)), gt, font=FONTS["mono_b"], fill=(110, 231, 183, 255) if h else (226, 232, 240, 245)),
            ))
            ry += step_y

    img = rounded_rect(
        img,
        [px(30), px(564), px(W - 30), px(606)],
        radius=px(8),
        fill=(15, 23, 42, 215),
        outline=(148, 163, 184, 48),
        width=px(1),
    )
    fnote = (
        f"std/hw opt-in: f64 BLIS = {f64_vx_sp}x vs Vortex 1T ({f64_c_sp}x vs C -O3) | "
        f"GPU VRAM HwBuf = {res_gf / 1000.0:.2f} TFLOP/s | MatMul+AXPY+ReLU+sync = {pipe_ms:.2f} ms | OCL Cache hits={cache_hits}"
    )
    img = blend_layer(img, lambda d: d.text((px(44), px(577)), fnote, font=FONTS["mono_sm"], fill=(148, 163, 184, 235)))

    out = ASSETS_DIR / "bench_silicon_hw.png"
    img.resize((W, H), Image.Resampling.LANCZOS).save(out, "PNG")
    return out


def main():
    data = json.loads(RESULTS_PATH.read_text(encoding="utf-8"))
    outs = [
        build_total_chart(data),
        build_breakdown_chart(data),
        build_silicon_chart(data),
    ]
    for o in outs:
        im = Image.open(o)
        corner_alpha = im.getpixel((0, 0))[3]
        center_alpha = im.getpixel((im.width // 2, im.height // 2))[3]
        print(f"Generated {o.name}: {im.size} {im.mode} corner_alpha={corner_alpha} center_alpha={center_alpha}")


if __name__ == "__main__":
    main()
