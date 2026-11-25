# The level generator's own textures (2026-11-15; user: "all our random
# generated maps should use custom textures"). Painted here, tileable both
# ways, 64x64 in 16 colours (the largest colour texture the game's loader
# takes: 2 KB of indices), and written in the game's own texture format (zlib
# path: header byte 0x41, format RGBA16_CI4, palette, size, Rare zip 1173 of
# the packed 4-bit indices) into port/include/levelgentex.h, which the port
# serves under numbers from LGTEX_FIRST up (mod.c, modTextureLoad), and
# src/include/game/levelgentexnums.h, the numbers by name for the writer.
#
# A texture's number is its place in TEXTURES: add new ones at the end, so
# that the numbers already used by levels stay.
#
# Usage: python3 tools/levelgen/lgtexgen.py [preview-dir]
import sys, os, zlib
import numpy as np
from PIL import Image

N = 64
FIRST = 0xe00

# ---------------------------------------------------------------- noise

def pnoise(seed, power, shape=(N, N)):
    """Periodic noise: a random spectrum falling off as 1/f^power, so it tiles."""
    rng = np.random.default_rng(seed)
    fy = np.fft.fftfreq(shape[0])[:, None] * shape[0]
    fx = np.fft.fftfreq(shape[1])[None, :] * shape[1]
    f = np.sqrt(fx * fx + fy * fy)
    f[0, 0] = 1
    spec = (rng.normal(size=shape) + 1j * rng.normal(size=shape)) / f ** power
    spec[0, 0] = 0
    n = np.real(np.fft.ifft2(spec))
    n -= n.min(); n /= n.max()
    return n

def pnoise_streak(seed, power, stretch):
    """Periodic noise streaked along x: the spectrum falls off `stretch` times
    faster across (y) than along (x), so features run along the rows."""
    rng = np.random.default_rng(seed)
    fy = np.fft.fftfreq(N)[:, None] * N
    fx = np.fft.fftfreq(N)[None, :] * N
    f = np.sqrt(fx * fx + (fy * stretch) ** 2)
    f[0, 0] = 1
    spec = (rng.normal(size=(N, N)) + 1j * rng.normal(size=(N, N))) / f ** power
    spec[0, 0] = 0
    n = np.real(np.fft.ifft2(spec))
    n -= n.min(); n /= n.max()
    return n

def pnoise1(seed, power, n=N):
    rng = np.random.default_rng(seed)
    f = np.abs(np.fft.fftfreq(n) * n); f[0] = 1
    spec = (rng.normal(size=n) + 1j * rng.normal(size=n)) / f ** power
    spec[0] = 0
    v = np.real(np.fft.ifft(spec)); v -= v.min(); v /= v.max()
    return v

def ramp(t, stops):
    """Colour along t (0..1) through (pos, (r, g, b)) stops."""
    t = np.clip(t, 0, 1)
    out = np.zeros(t.shape + (3,))
    for i in range(len(stops) - 1):
        p0, c0 = stops[i]; p1, c1 = stops[i + 1]
        m = (t >= p0) & (t <= p1)
        k = ((t - p0) / max(p1 - p0, 1e-6))[m][:, None]
        out[m] = np.array(c0, float) * (1 - k) + np.array(c1, float) * k
    return out

def shade(col, f):
    return col * np.asarray(f)[..., None]

def solid(c):
    return np.ones((N, N, 3)) * np.array(c, float)

def grit(seed, amount):
    return 1 + amount * (pnoise(seed, 0.0) - 0.5)

def coords():
    y, x = np.mgrid[0:N, 0:N]
    return x.astype(float), y.astype(float)

# ---------------------------------------------------------------- natural

def strata(seed, stops, warp, grain, pits):
    """Sandstone in horizontal layers: an uneven layer profile down the
    texture, barely warped sideways, fine grain, a few darker seams, a little
    weathering. No sharp features, so the repeat doesn't catch the eye."""
    x, y = coords()
    w = pnoise(seed, 2.6) - 0.5
    yy = (y + warp * w * N / 16) % N
    prof = pnoise1(seed + 1, 1.9, N * 4)
    layer = np.interp(yy / N * len(prof), np.arange(len(prof)), prof, period=len(prof))
    t = layer * (1 - grain) + grain * pnoise(seed + 3, 0.5)
    col = ramp(t, stops)
    seams = pnoise1(seed + 6, 0.8, N * 4)
    sv = np.interp(yy / N * len(seams), np.arange(len(seams)), seams, period=len(seams))
    line = np.clip((sv - 0.80) * 6, 0, 1) * np.clip((pnoise(seed + 7, 1.6) - 0.25) * 3, 0, 1)
    col = shade(col, 1 - 0.30 * line)
    col = shade(col, grit(seed + 8, 0.16))
    wea = pnoise(seed + 4, 1.3)
    col = shade(col, 1 - pits * np.clip((wea - 0.6) * 2.5, 0, 1))
    return col

def gravel(seed, stops, pebble=0.78, gritamt=0.22):
    """Even ground: gravel over gentle variation, the odd darker pebble."""
    t = 0.35 * pnoise(seed, 2.4) + 0.65 * pnoise(seed + 1, 0.3)
    col = ramp(t, stops)
    col = shade(col, grit(seed + 3, gritamt))
    pebbles = pnoise(seed + 2, 0.7)
    col[pebbles > 0.80] *= pebble
    return col

def concrete(seed, base, var=0.10, gritamt=0.12, stains=0.10, seam=0):
    """Poured concrete: soft mottling, grit, faint stains; seam > 0: a
    formwork joint every seam pixels down the texture."""
    t = pnoise(seed, 1.8)
    col = solid(base) * (1 + var * (t - 0.5))[..., None]
    col = shade(col, grit(seed + 1, gritamt))
    st = pnoise(seed + 2, 2.2)
    col = shade(col, 1 - stains * np.clip((st - 0.55) * 3, 0, 1))
    x, y = coords()
    if seam:
        col[(y % seam) == 0] *= 0.80
        col[(y % seam) == 1] *= 1.06
    return col

def asphalt(seed):
    t = 0.5 * pnoise(seed, 1.6) + 0.5 * pnoise(seed + 1, 0.1)
    col = ramp(t, [(0, (40, 40, 44)), (0.5, (62, 62, 66)), (0.85, (84, 83, 86)), (1, (112, 110, 108))])
    st = pnoise(seed + 2, 2.4)
    col = shade(col, 1 - 0.12 * np.clip((st - 0.6) * 3, 0, 1))
    return col

# ---------------------------------------------------------------- made

def bricks(seed, colours, mortar, courses=8, per_row=4, mortar_px=1, var=0.10, gritamt=0.18):
    """Brick courses with offset joints: each brick its own tint, grit, a
    darker lower edge; mortar lines between."""
    rng = np.random.default_rng(seed)
    x, y = coords()
    ch = N // courses
    bw = N // per_row
    col = np.zeros((N, N, 3))
    row = (y // ch).astype(int)
    off = (row % 2) * (bw // 2)
    bx = ((x + off) // bw).astype(int) % per_row
    tints = rng.integers(0, len(colours), size=(courses, per_row))
    bright = 1 + var * (rng.random((courses, per_row)) - 0.5) * 2
    for r in range(courses):
        for b in range(per_row):
            m = (row == r) & (bx == b)
            col[m] = np.array(colours[tints[r, b]], float) * bright[r, b]
    col = shade(col, grit(seed + 1, gritamt))
    col = shade(col, 1 + 0.10 * (pnoise(seed + 2, 1.2) - 0.5))
    iy = y % ch
    ix = (x + off) % bw
    col[iy == ch - 1 - mortar_px] *= 0.85
    mort = (iy >= ch - mortar_px) | (ix < mortar_px)
    col[mort] = np.array(mortar, float)[None, :] * grit(seed + 3, 0.2)[mort][:, None]
    return col

def tiles(seed, colours, grout, n=4, var=0.06, checker=False, bevel=0.10, gritamt=0.06):
    """Square tiles n to a side with grout lines; a slight bevel; each tile
    its own shade (or a checker of the colours)."""
    rng = np.random.default_rng(seed)
    x, y = coords()
    s = N // n
    tx, ty = (x // s).astype(int), (y // s).astype(int)
    col = np.zeros((N, N, 3))
    for j in range(n):
        for i in range(n):
            c = colours[(i + j) % len(colours)] if checker else colours[rng.integers(0, len(colours))]
            m = (tx == i) & (ty == j)
            col[m] = np.array(c, float) * (1 + var * (rng.random() - 0.5) * 2)
    ix, iy = x % s, y % s
    col[(ix == 1) | (iy == 1)] *= 1 + bevel
    col[(ix == s - 1) | (iy == s - 1)] *= 1 - bevel
    col = shade(col, grit(seed + 1, gritamt))
    col = shade(col, 1 + 0.06 * (pnoise(seed + 2, 1.6) - 0.5))
    g = (ix == 0) | (iy == 0)
    col[g] = np.array(grout, float)
    return col

def planks(seed, stops, boards=4, along_x=True, gap=0.45, joints=2, gritamt=0.10):
    """Wooden boards: grain stretched along the board, each board its own
    shade, a dark gap between boards, butt joints staggered."""
    rng = np.random.default_rng(seed)
    x, y = coords()
    # grain: noise squashed across the board, so it streaks along it
    g2 = pnoise_streak(seed, 1.1, 0.12)
    fine = pnoise_streak(seed + 1, 0.5, 0.2)
    t = 0.6 * g2 + 0.4 * fine
    col = ramp(t, stops)
    bh = N // boards
    b = (y // bh).astype(int)
    tone = 1 + 0.18 * (rng.random(boards) - 0.5)
    col = shade(col, tone[b])
    iy = y % bh
    col[iy == 0] *= gap
    col[iy == 1] *= 1.08
    for k in range(boards):
        for j in range(joints):
            jx = int(rng.integers(0, N))
            col[(b == k) & (x == jx)] *= gap
    col = shade(col, grit(seed + 2, gritamt))
    if not along_x:
        col = np.transpose(col, (1, 0, 2))
    return col

def panels(seed, base, n=2, rivets=True, bevel=0.16, grime=0.10, seam=0.55, gritamt=0.08):
    """Metal plates n to a side: a bevelled edge, dark seams, rivets in the
    corners, grime."""
    x, y = coords()
    s = N // n
    ix, iy = x % s, y % s
    col = solid(base) * (1 + 0.08 * (pnoise(seed, 1.5) - 0.5))[..., None]
    col = shade(col, grit(seed + 1, gritamt))
    gr = pnoise(seed + 2, 1.8)
    col = shade(col, 1 - grime * np.clip((gr - 0.5) * 2.5, 0, 1))
    col[(ix == 1) | (iy == 1)] *= 1 + bevel
    col[(ix == s - 1) | (iy == s - 1)] *= 1 - bevel
    col[(ix == 0) | (iy == 0)] *= seam
    if rivets:
        for cx in (3, s - 4):
            for cy in (3, s - 4):
                col[((ix - cx) ** 2 + (iy - cy) ** 2) <= 1.2] *= 1.25
                col[((ix - cx - 1) ** 2 + (iy - cy - 1) ** 2) <= 0.6] *= 0.6
    return col

def corrugated(seed, base, ribs=8, rust=0.0, streaks=0.15, depth=0.30):
    """Corrugated sheet, ribs running up the wall: shading across each rib,
    streaks running down, rust patches."""
    x, y = coords()
    prof = np.sin((x / N * ribs) % 1.0 * 2 * np.pi)
    col = solid(base) * (1 + depth * prof)[..., None]
    s1 = pnoise1(seed, 1.0, N)[None, :].repeat(N, 0)
    v = pnoise(seed + 1, 2.0)
    col = shade(col, 1 - streaks * np.clip((s1 * 0.7 + v * 0.3 - 0.5) * 2, 0, 1))
    if rust:
        r = pnoise(seed + 2, 1.7)
        m = np.clip((r - (1 - rust)) * 4, 0, 1)
        rc = np.array((128, 66, 34), float)
        col = col * (1 - m[..., None]) + (rc * (1 + depth * 0.6 * prof)[..., None]) * m[..., None]
    col = shade(col, grit(seed + 3, 0.10))
    return col

def hazard(seed, a=(214, 170, 30), b=(30, 28, 26), stripes=4):
    x, y = coords()
    s = ((x + y) / N * stripes) % 1.0
    col = np.where((s < 0.5)[..., None], np.array(a, float), np.array(b, float))
    col = shade(col, grit(seed, 0.14))
    w = pnoise(seed + 1, 1.6)
    col = shade(col, 1 - 0.25 * np.clip((w - 0.6) * 3, 0, 1))
    return col

def diamond(seed, base):
    """Steel tread plate: raised bars in a staggered herringbone."""
    x, y = coords()
    col = solid(base) * (1 + 0.08 * (pnoise(seed, 1.6) - 0.5))[..., None]
    col = shade(col, grit(seed + 1, 0.10))
    s = 8
    cx = x % s; cy = y % s
    flip = ((x // s + y // s) % 2) == 0
    u = np.where(flip, cx - cy, cx + cy - s)
    bar = (np.abs(u) <= 1) & (cx >= 1) & (cx <= s - 2) & (cy >= 1) & (cy <= s - 2)
    col[bar] *= 1.28
    edge = (np.abs(u - 2) <= 0.5) & (cx >= 1) & (cx <= s - 2) & (cy >= 1) & (cy <= s - 2)
    col[edge] *= 0.72
    return col

def ceiling_tiles(seed, base, n=2, speck=0.18):
    """Acoustic ceiling tiles in a metal grid."""
    x, y = coords()
    s = N // n
    col = solid(base)
    col = shade(col, 1 - speck * (pnoise(seed, 0.0) > 0.78))
    col = shade(col, 1 + 0.05 * (pnoise(seed + 1, 1.5) - 0.5))
    ix, iy = x % s, y % s
    col[(ix <= 1) | (iy <= 1)] = np.array((200, 200, 196), float)
    col[(ix == 2) | (iy == 2)] *= 0.88
    return col

def plaster(seed, base, var=0.06, stripes=0, stripe_amt=0.05):
    x, y = coords()
    col = solid(base) * (1 + var * (pnoise(seed, 1.8) - 0.5))[..., None]
    col = shade(col, grit(seed + 1, 0.06))
    if stripes:
        col = shade(col, 1 + stripe_amt * np.sign(np.sin(x / N * 2 * np.pi * stripes)))
    return col

def wall_band(seed, top, stripe, bottom, at=(40, 46)):
    """A plain wall with a coloured stripe band across it."""
    x, y = coords()
    col = solid(top)
    col[(y >= at[0]) & (y < at[1])] = np.array(stripe, float)
    col[y >= at[1]] = np.array(bottom, float)
    col = shade(col, grit(seed, 0.06))
    col = shade(col, 1 + 0.05 * (pnoise(seed + 1, 1.6) - 0.5))
    return col

def lab_panels(seed, base, stripe, n=2):
    """Clean wall panels, a coloured stripe across the middle, thin seams."""
    x, y = coords()
    s = N // n
    col = solid(base) * (1 + 0.04 * (pnoise(seed, 1.6) - 0.5))[..., None]
    col = shade(col, grit(seed + 1, 0.04))
    iy = y % s
    ix = x % s
    col[(iy >= s // 2 - 2) & (iy < s // 2 + 1)] = np.array(stripe, float)
    col[ix == 0] *= 0.70
    col[iy == 0] *= 0.70
    col[ix == 1] *= 1.06
    col[iy == 1] *= 1.06
    return col

def light_panels(seed, frame, glow):
    """Ceiling light panels in a frame."""
    x, y = coords()
    s = N // 2
    ix, iy = x % s, y % s
    col = solid(frame)
    col = shade(col, grit(seed, 0.05))
    inner = (ix >= 4) & (ix < s - 4) & (iy >= 4) & (iy < s - 4)
    col[inner] = np.array(glow, float)
    col[inner & (((ix - 4) % 4) == 0)] *= 0.92
    col[(ix == 3) | (iy == 3)] *= 0.75
    return col

def shelving(seed):
    """A warehouse rack's face, one shelf a tile: an orange beam along the
    top, a blue upright at the side, boxes and crates of several sizes
    (cardboard and wood) on the shelf, dark gaps behind."""
    rng = np.random.default_rng(seed)
    x, y = coords()
    col = solid((26, 24, 22))
    beam, up = 7, 5
    cx = up + 1
    while cx < N - 2:
        w = int(rng.integers(10, 22))
        h = int(rng.integers(22, N - beam - 6))
        if cx + w > N - 1:
            w = N - 1 - cx
        if w < 6:
            break
        top = N - 2 - h
        kind = int(rng.integers(0, 3))
        c = [(168, 128, 82), (150, 112, 70), (120, 96, 66)][kind]
        m = (x >= cx) & (x < cx + w) & (y >= top) & (y < N - 2)
        col[m] = np.array(c, float) * (1 + 0.08 * (rng.random() - 0.5))
        col[m & (x == cx)] *= 1.12
        col[m & (x == cx + w - 1)] *= 0.78
        col[m & (y == top)] *= 1.15
        if kind < 2:
            col[m & (np.abs(x - (cx + w // 2)) < 1)] *= 0.86
        else:
            col[m & (((y - top) % 6) == 0)] *= 0.75
        cx += w + int(rng.integers(1, 4))
    col = shade(col, grit(seed + 1, 0.10))
    col[y < beam] = np.array((214, 112, 28), float)
    col[y == 0] *= 1.2
    col[y == beam - 1] *= 0.6
    col[y >= N - 2] = np.array((120, 104, 84), float)
    upm = x < up
    col[upm] = np.array((40, 76, 142), float)
    col[upm & (x == 0)] *= 1.25
    col[upm & (x == up - 1)] *= 0.65
    col[upm & ((y % 6) == 3) & (x >= 1) & (x <= 3)] *= 0.45
    return col


def roof_tiles(seed, colours, rows=8, barrels=8):
    """Clay barrel roof tiles: rows of half-round tiles, each overlapping the
    row below; light along the crown, shadow in the troughs and under each
    row's lip; each tile its own shade."""
    rng = np.random.default_rng(seed)
    x, y = coords()
    rh = N // rows
    bw = N // barrels
    row = (y // rh).astype(int)
    off = (row % 2) * (bw // 2)
    bx = (((x + off) // bw).astype(int)) % barrels
    tint = rng.integers(0, len(colours), size=(rows, barrels))
    bright = 1 + 0.12 * (rng.random((rows, barrels)) - 0.5) * 2
    col = np.zeros((N, N, 3))
    for r in range(rows):
        for b in range(barrels):
            m = (row == r) & (bx == b)
            col[m] = np.array(colours[tint[r, b]], float) * bright[r, b]
    u = ((x + off) % bw) / bw
    col = shade(col, 0.72 + 0.42 * np.sin(u * np.pi))
    v = (y % rh) / rh
    col = shade(col, 1 - 0.35 * np.clip((v - 0.72) * 4, 0, 1))
    col = shade(col, grit(seed + 1, 0.10))
    col = shade(col, 1 + 0.10 * (pnoise(seed + 2, 1.8) - 0.5))
    return col

def marble(seed, base, vein, n=2, grout=(150, 144, 136)):
    """Polished marble slabs: soft clouding and thin dark veins."""
    x, y = coords()
    col = solid(base) * (1 + 0.08 * (pnoise(seed, 1.6) - 0.5))[..., None]
    w = pnoise(seed + 1, 1.9)
    vv = np.abs(np.sin((x * 0.06 + y * 0.11 + 6 * w) * np.pi))
    m = np.clip((0.12 - vv) * 8, 0, 1)
    col = col * (1 - m[..., None]) + np.array(vein, float) * m[..., None]
    col = shade(col, grit(seed + 2, 0.04))
    s = N // n
    col[((x % s) == 0) | ((y % s) == 0)] = np.array(grout, float)
    return col

def water(seed):
    """Deep water: blue-green, wind ripples, a few glints."""
    x, y = coords()
    r = pnoise_streak(seed, 1.3, 0.35)
    t = 0.6 * pnoise(seed + 1, 1.8) + 0.4 * r
    col = ramp(t, [(0, (20, 52, 66)), (0.45, (30, 78, 92)), (0.8, (52, 108, 118)), (1, (86, 144, 150))])
    g = pnoise_streak(seed + 2, 0.8, 0.3)
    col[g > 0.86] = col[g > 0.86] * 1.25 + 20
    return col

def foliage(seed, stops, flowers=None, amount=0.0):
    """Leaves: clumpy greens with dark gaps between; flowers: specks of colour."""
    t = 0.55 * pnoise(seed, 0.9) + 0.45 * pnoise(seed + 1, 1.8)
    col = ramp(t, stops)
    gaps = pnoise(seed + 2, 0.6)
    col[gaps < 0.22] *= 0.62
    col = shade(col, grit(seed + 3, 0.14))
    if flowers:
        f = pnoise(seed + 4, 0.35)
        pick = pnoise(seed + 5, 1.2)
        for i, c in enumerate(flowers):
            m = (f > 1 - amount) & (pick >= i / len(flowers)) & (pick < (i + 1) / len(flowers))
            col[m] = c
    return col

def pool_water(seed):
    """A pool: clear turquoise over pale tiles, light ripples."""
    x, y = coords()
    t = 0.5 * pnoise(seed, 1.6) + 0.5 * pnoise_streak(seed + 1, 1.2, 0.5)
    col = ramp(t, [(0, (40, 132, 160)), (0.5, (62, 166, 186)), (0.85, (104, 196, 206)), (1, (170, 226, 228))])
    grid = ((x % 16) < 1) | ((y % 16) < 1)
    col[grid] *= 0.90
    return col

def container(seed, base, stripe=None, rust=0.25, door=False):
    """A shipping container's side: upright ribs, a rail top and bottom, a
    painted stripe with marks on it, rust creeping up from the bottom."""
    x, y = coords()
    rib = 0.5 + 0.5 * np.cos(x / N * 2 * np.pi * 16)
    col = solid(base)
    col = shade(col, 0.80 + 0.26 * rib)
    if stripe is not None:
        band = (y >= 20) & (y < 30)
        col[band] = np.array(stripe, float) * (0.86 + 0.18 * rib[band])[:, None]
        marks = band & (y >= 23) & (y < 27) & (((x // 3) % 5) < 2) & (x > 8) & (x < 44)
        col[marks] = col[marks] * 0.45
    rail = (y < 3) | (y >= N - 3)
    col[rail] = col[rail] * 0.62
    if door:
        bars = ((x % 32) >= 14) & ((x % 32) < 16)
        col[bars & ~rail] *= 0.60
    r = pnoise(seed, 1.4) * (0.35 + 0.65 * (y / N)) + 0.25 * pnoise_streak(seed + 1, 1.0, 0.2)
    m = np.clip((r - (1 - rust)) * 4, 0, 1)[..., None]
    col = col * (1 - m) + np.array((112, 62, 38), float) * m
    return shade(col, grit(seed + 2, 0.10))

def dock_concrete(seed):
    """A terminal's ground: worn concrete slabs, oil, a yellow lane line."""
    x, y = coords()
    col = concrete(seed, (112, 112, 110), var=0.14, gritamt=0.14, stains=0.30, seam=32)
    line = (x >= 29) & (x < 33) & ((pnoise(seed + 9, 0.9) > 0.30))
    col[line] = col[line] * 0.35 + np.array((196, 160, 40), float) * 0.65
    return col

def harbour_water(seed):
    """A harbour at night: near-black water, long oily highlights."""
    r = pnoise_streak(seed, 1.2, 0.25)
    t = 0.55 * pnoise(seed + 1, 1.9) + 0.45 * r
    col = ramp(t, [(0, (6, 14, 22)), (0.5, (12, 28, 40)), (0.85, (26, 52, 66)), (1, (70, 104, 112))])
    g = pnoise_streak(seed + 2, 0.7, 0.2)
    col[g > 0.90] = col[g > 0.90] * 1.2 + np.array((40, 34, 10), float)
    return col

def quay_wall(seed):
    """A quay's face: board-marked concrete, a dark tide line, weed below it."""
    x, y = coords()
    col = concrete(seed, (126, 124, 118), var=0.12, gritamt=0.16, stains=0.22)
    col = shade(col, 1 - 0.10 * (((y // 8) % 2) == 0))
    wet = np.clip((y - 40) / 10.0, 0, 1)[..., None]
    col = col * (1 - 0.55 * wet) + np.array((20, 44, 30), float) * (0.25 * wet)
    return col

def warehouse_wall(seed, base, band):
    """A shed's wall: pale ribbed sheet over a painted dado, a stencilled bay mark."""
    x, y = coords()
    rib = 0.5 + 0.5 * np.cos(x / N * 2 * np.pi * 8)
    col = shade(solid(base), 0.88 + 0.16 * rib)
    lower = y >= 44
    col[lower] = np.array(band, float) * (0.90 + 0.12 * pnoise(seed, 1.5)[lower])[:, None]
    edge = (y >= 42) & (y < 44)
    col[edge] = (36, 36, 38)
    col = shade(col, grit(seed + 1, 0.10))
    s = pnoise(seed + 2, 1.2)
    col[s > 0.82] *= 0.86
    return col

def bay_floor(seed):
    """A warehouse floor: sealed concrete, hazard-striped bay edges."""
    x, y = coords()
    col = concrete(seed, (96, 100, 102), var=0.10, gritamt=0.08, stains=0.20)
    edge = (y < 4) | (x < 4)
    stripe = ((x + y) // 4) % 2 == 0
    col[edge & stripe] = (198, 162, 36)
    col[edge & ~stripe] = (34, 32, 30)
    return col

def helipad(seed):
    """A landing pad's deck: dark non-slip plate, a white marking grid."""
    x, y = coords()
    col = concrete(seed, (58, 62, 66), var=0.08, gritamt=0.20, stains=0.10)
    grid = (x < 3) | (y < 3)
    col[grid] = col[grid] * 0.3 + np.array((214, 214, 206), float) * 0.7
    dots = ((x % 8) == 5) & ((y % 8) == 5)
    col[dots] *= 0.7
    return col

def tree_column(seed, crown=0.70):
    """A wall of pines: dark trunks and gaps below, a dense crown above (the
    image's top is up), drawn as one tile from the ground to the treetops."""
    x, y = coords()
    col = foliage(seed, [(0, (14, 34, 22)), (0.45, (26, 58, 32)), (0.8, (44, 82, 40)), (1, (70, 104, 54))])
    # the crown's lower edge: ragged, hanging lower in places
    edge = crown * N + 6 * (pnoise1(seed + 7, 1.2) - 0.5)[None, :] * np.ones((N, 1))
    below = y >= edge
    # trunks: a few dark bark columns in deep shade below the crown
    trunks = np.zeros((N, N), bool)
    rng = np.random.RandomState(seed)
    for cx in rng.choice(N, 6, replace=False):
        w = rng.randint(2, 4)
        trunks |= (np.abs(((x - cx + N // 2) % N) - N // 2) < w)
    shadow = np.array((10, 18, 14), float) * (0.8 + 0.4 * pnoise(seed + 3, 1.5))[..., None]
    bark = np.array((62, 46, 34), float) * (0.75 + 0.5 * pnoise_streak(seed + 4, 1.0, 0.2))[..., None]
    col[below] = shadow[below]
    col[below & trunks] = bark[below & trunks]
    # undergrowth along the foot
    fern = (y >= N - 7) & (pnoise(seed + 5, 1.6) > 0.35)
    col[fern] = np.array((34, 70, 30), float) * (0.8 + 0.4 * grit(seed + 6, 1.0)[fern])[:, None]
    return col

def clapboard(seed, base, boards=8):
    """Painted weatherboard: overlapping boards, a shadow under each."""
    x, y = coords()
    col = shade(solid(base), 0.94 + 0.08 * pnoise(seed, 1.2))
    t = (y % (N // boards)) / (N // boards)
    col = shade(col, 0.82 + 0.22 * t)
    col[(y % (N // boards)) == 0] *= 0.62
    col = shade(col, grit(seed + 1, 0.05))
    return col

def wallpaper(seed, base, motif, dado):
    """A suburban wallpaper: a small repeating sprig over a pale ground, a wood dado."""
    x, y = coords()
    col = shade(solid(base), 0.96 + 0.06 * pnoise(seed, 1.4))
    dots = (((x + 4 * ((y // 8) % 2)) % 8) == 3) & ((y % 8) >= 2) & ((y % 8) <= 4)
    col[dots] = motif
    lower = y >= 46
    col[lower] = np.array(dado, float) * (0.86 + 0.18 * pnoise_streak(seed + 1, 1.0, 0.2)[lower])[:, None]
    col[(y == 45) | (y == 46)] = (236, 230, 214)
    return col

def lawn(seed):
    """Mown grass: stripes from the mower, clover, a few bare patches."""
    x, y = coords()
    col = ramp(0.5 * pnoise(seed, 1.8) + 0.5 * pnoise(seed + 1, 2.4), [(0, (52, 96, 36)), (0.5, (76, 128, 48)), (1, (110, 152, 62))])
    col = shade(col, 0.92 + 0.10 * ((x // 16) % 2))
    bare = pnoise(seed + 2, 0.8) > 0.86
    col[bare] = col[bare] * 0.5 + np.array((120, 100, 64), float) * 0.5
    return shade(col, grit(seed + 3, 0.12))

def forest_floor(seed):
    """A trail through pines: packed dirt, needles, roots, moss at the sides."""
    x, y = coords()
    col = ramp(pnoise(seed, 1.5), [(0, (64, 48, 34)), (0.5, (92, 70, 48)), (1, (120, 96, 66))])
    needles = pnoise_streak(seed + 1, 2.2, 0.3) > 0.70
    col[needles] = col[needles] * 0.6 + np.array((132, 92, 50), float) * 0.4
    moss = pnoise(seed + 2, 0.9) > 0.72
    col[moss] = col[moss] * 0.4 + np.array((56, 84, 38), float) * 0.6
    return shade(col, grit(seed + 3, 0.16))

def hesco(seed, base):
    """Filled barrier bags: square fabric cells in a wire mesh frame, sagging."""
    x, y = coords()
    col = shade(solid(base), 0.84 + 0.22 * pnoise(seed, 1.6))
    cell = 32
    u, v = (x % cell) / cell, (y % cell) / cell
    bulge = 1 - 0.9 * ((u - 0.5) ** 2 + (v - 0.45) ** 2)
    col = shade(col, 0.7 + 0.35 * bulge)
    wire = ((x % cell) < 2) | ((y % cell) < 2) | (((x + y) % 8) == 0) & (((x % cell) < 4) | ((y % cell) < 4))
    col[wire] = (78, 80, 72)
    return shade(col, grit(seed + 1, 0.12))

def bunker(seed, base, stripe):
    """Painted military concrete: shuttering lines, a stencilled band, chips."""
    x, y = coords()
    col = concrete(seed, base, var=0.10, gritamt=0.12, stains=0.25, seam=32)
    band = (y >= 40) & (y < 46)
    col[band] = stripe
    mark = band & (((x // 2) % 6) < 2) & (x > 6) & (x < 30)
    col[mark] = (226, 222, 200)
    chips = pnoise(seed + 5, 0.6) > 0.88
    col[chips] = col[chips] * 0.6 + 70
    return col

def ruin_stone(seed, mossy=0.3):
    """Old dressed stone, worn round, moss in the joints and creeping up."""
    x, y = coords()
    col = stone_wall(seed, [(150, 144, 128), (132, 126, 112), (168, 158, 138)], (88, 84, 74), courses=4)
    moss = (pnoise(seed + 9, 1.0) * (0.5 + 0.6 * (y / N))) > (1 - mossy)
    col[moss] = col[moss] * 0.35 + np.array((62, 92, 44), float) * 0.65
    return col

def flagstones(seed):
    """A ruin's floor: big cracked slabs, grass coming up between them."""
    x, y = coords()
    col = tiles(seed, [(146, 140, 126), (130, 124, 110), (158, 150, 132)], (64, 82, 40), n=2, var=0.10, bevel=0.12, gritamt=0.14)
    crack = pnoise_streak(seed + 1, 2.0, 0.5) > 0.86
    col[crack] *= 0.55
    weeds = (pnoise(seed + 2, 1.0) > 0.78)
    col[weeds] = col[weeds] * 0.3 + np.array((70, 104, 46), float) * 0.7
    return col

def stone_wall(seed, colours, mortar, courses=5):
    """Rough stone blocks of uneven length, deep mortar joints."""
    rng = np.random.default_rng(seed)
    x, y = coords()
    ch = N // courses
    col = np.zeros((N, N, 3))
    row = (y // ch).astype(int)
    joint = np.zeros((N, N), bool)
    for r in range(courses):
        cuts = sorted(set([0] + list(rng.integers(6, N - 6, size=3))))
        cuts = [c for i, c in enumerate(cuts) if i == 0 or c - cuts[i - 1] >= 8]
        edges = cuts + [N]
        for i in range(len(cuts)):
            m = (row == r) & (x >= edges[i]) & (x < edges[i + 1])
            col[m] = np.array(colours[rng.integers(0, len(colours))], float) * (1 + 0.14 * (rng.random() - 0.5))
            joint |= (row == r) & (x == edges[i])
    col = shade(col, 1 + 0.25 * (pnoise(seed + 1, 1.2) - 0.5))
    col = shade(col, grit(seed + 2, 0.22))
    iy = y % ch
    col[iy == ch - 2] *= 0.82
    joint |= iy == ch - 1
    col[joint] = np.array(mortar, float)
    return col

def facade(seed, wall, glass, frame, cols=2, rows=2, lit=0.3, mullion=True):
    """A tall building's face (the rooftops' streets, 2026-11-16): a grid of
    windows in the wall, cols x rows to the tile, each glass pane dark blue or
    lit warm, a frame round it, a sill's shadow under it."""
    rng = np.random.default_rng(seed)
    x, y = coords()
    col = solid(wall) * (1 + 0.08 * (pnoise(seed, 1.4) - 0.5))[..., None]
    col = shade(col, grit(seed + 1, 0.14))
    cw, ch = N // cols, N // rows
    for r in range(rows):
        for c in range(cols):
            x0, y0 = c * cw + cw // 6, r * ch + ch // 5
            x1, y1 = (c + 1) * cw - cw // 6, (r + 1) * ch - ch // 4
            on = rng.random() < lit
            g = np.array((214, 186, 110) if on else glass, float)
            m = (x >= x0) & (x < x1) & (y >= y0) & (y < y1)
            fr = m & ((x == x0) | (x == x1 - 1) | (y == y0) | (y == y1 - 1))
            pane = m & ~fr
            t = pnoise(seed + 5 + r * 7 + c, 1.0)
            col[pane] = (g * (0.85 + 0.3 * t[..., None]))[pane]
            if mullion:
                col[pane & (x == (x0 + x1) // 2)] = np.array(frame, float)
            col[fr] = np.array(frame, float)
            sill = (x >= x0 - 1) & (x < x1 + 1) & (y == y1)
            col[sill] = np.array(frame, float) * 1.15
            col[(x >= x0) & (x < x1) & (y == y1 + 1)] *= 0.72
    return col

def roof_membrane(seed, base):
    """A flat roof: tarred felt laid in strips, the seams lapped, grit."""
    x, y = coords()
    col = solid(base) * (1 + 0.10 * (pnoise(seed, 1.6) - 0.5))[..., None]
    col = shade(col, grit(seed + 1, 0.26))
    col[(y % 16) == 0] *= 0.70
    col[(y % 16) == 1] *= 1.12
    st = pnoise(seed + 2, 2.0)
    col = shade(col, 1 - 0.12 * np.clip((st - 0.6) * 3, 0, 1))
    return col

# ---------------------------------------------------------------- output

def coffer(seed, frame, panel, glow):
    """A coffered ceiling: a deep square panel in a moulded frame, a soft cove
    light round its edge (the penthouse, 2026-11-17)."""
    x, y = coords()
    col = solid(frame) * (1 + 0.05 * (pnoise(seed, 1.6) - 0.5))[..., None]
    m = (x % 32 >= 4) & (x % 32 < 28) & (y % 32 >= 4) & (y % 32 < 28)
    cove = m & ~((x % 32 >= 6) & (x % 32 < 26) & (y % 32 >= 6) & (y % 32 < 26))
    col[m] = np.array(panel, float)
    col[cove] = np.array(glow, float)
    edge = (x % 32 == 3) | (y % 32 == 3)
    col[edge] *= 0.7
    return shade(col, grit(seed + 1, 0.04))

def silk_wall(seed, base, stripe):
    """Silk wallpaper, a faint damask stripe."""
    x, y = coords()
    col = solid(base) * (1 + 0.05 * (pnoise(seed, 1.4) - 0.5))[..., None]
    band = (x % 16 >= 6) & (x % 16 < 10)
    col[band] = (col * 0.6 + np.array(stripe, float) * 0.4)[band]
    return shade(col, grit(seed + 1, 0.04))

def brushed_steel(seed, base, n=2):
    """Brushed steel panels, a lift shaft's lining: streaks along y, seams."""
    x, y = coords()
    st = pnoise_streak(seed, 1.2, 8)
    col = solid(base) * (0.9 + 0.2 * st)[..., None]
    seam = (y % (N // n) == 0) | (x % (N // n) == 0)
    col[seam] *= 0.6
    return shade(col, grit(seed + 1, 0.05))

def girder(seed, base):
    """A steel I-beam's web: red oxide primer, a flange top and bottom, bolts."""
    x, y = coords()
    col = solid(base) * (1 + 0.12 * (pnoise(seed, 1.8) - 0.5))[..., None]
    fl = (y % 32 < 5) | (y % 32 >= 27)
    col[fl] *= 1.18
    col[(y % 32 == 5) | (y % 32 == 26)] *= 0.55
    bolts = ((y % 32 == 2) | (y % 32 == 29)) & (x % 8 == 3)
    col[bolts] = np.array((60, 40, 32), float)
    return shade(col, grit(seed + 1, 0.16))

def formwork(seed, base):
    """Ply formwork panels, the shutter marks a slab's underside keeps."""
    x, y = coords()
    col = solid(base) * (1 + 0.10 * (pnoise(seed, 2.0) - 0.5))[..., None]
    col[(x % 32 == 0) | (y % 16 == 0)] *= 0.62
    col[((x % 32 == 2) | (x % 32 == 29)) & ((y % 16 == 3) | (y % 16 == 12))] *= 0.45
    return shade(col, grit(seed + 1, 0.10))

def mesh(seed, wire, back):
    """A hoist's cage: a diagonal wire mesh over the dark behind it, a frame."""
    x, y = coords()
    col = solid(back) * (1 + 0.2 * (pnoise(seed, 1.5) - 0.5))[..., None]
    w = ((x + y) % 8 == 0) | ((x - y) % 8 == 0)
    col[w] = np.array(wire, float) * 0.9
    fr = (x % 64 < 3) | (y % 64 < 3)
    col[fr] = np.array(wire, float)
    return shade(col, grit(seed + 1, 0.08))

# ---------------------------------------------------------------- the market town (2026-11-18)

def fan_cobbles(seed, colours, joint):
    """A square's fan paving: setts laid in overlapping arcs (each 32-texel
    cell a fan), each sett its own shade, dark joints."""
    rng = np.random.default_rng(seed)
    x, y = coords()
    cx = (x % 32) - 16.0
    cy = (y % 32) + 0.0
    rr = np.sqrt(cx * cx + cy * cy)
    ang = np.arctan2(cy, cx)
    ring = (rr // 4).astype(int)
    seg = ((ang + np.pi) * (2 + ring) / 1.6).astype(int)
    key = (ring * 97 + seg + ((x // 32) * 13 + (y // 32) * 7)).astype(int)
    pal = np.array(colours, float)
    tint = 1 + 0.16 * (np.sin(key * 12.9898) * 0.5)
    col = pal[(key % len(colours))] * tint[..., None]
    jr = (rr % 4) < 0.9
    js = ((ang + np.pi) * (2 + ring) / 1.6) % 1.0 < 0.16
    col[jr | js] = np.array(joint, float)
    col = shade(col, 1 + 0.2 * (pnoise(seed + 1, 1.4) - 0.5))
    return shade(col, grit(seed + 2, 0.16))

def setts(seed, colours, joint):
    """An alley's granite setts: small rounded blocks in courses, the crowns lit."""
    col = bricks(seed, colours, joint, courses=8, per_row=6, mortar_px=1, var=0.16, gritamt=0.2)
    x, y = coords()
    iy = (y % 8) / 8.0
    col = col * (1.08 - 0.22 * np.abs(iy - 0.4))[..., None]
    return col

def half_timber(seed, plaster_col, beam_col):
    """A timber-framed wall: limewashed panels between dark oak posts, a rail
    and braces."""
    x, y = coords()
    col = solid(plaster_col) * (1 + 0.07 * (pnoise(seed, 1.8) - 0.5))[..., None]
    beam = (x % 32 < 4) | (y % 64 < 4) | (np.abs((y % 32) - (x % 32)) < 2.2) & (x % 32 >= 4)
    wood = ramp(0.6 * pnoise_streak(seed + 1, 1.1, 0.12) + 0.4, [(0, tuple(int(c * 0.6) for c in beam_col)), (1, beam_col)])
    col[beam] = wood[beam]
    return shade(col, grit(seed + 2, 0.12))

def hex_tiles(seed, colours, grout):
    """Hexagonal terracotta floor tiles."""
    x, y = coords()
    s = 8.0
    qx = x / (s * 1.5)
    col_i = np.floor(qx).astype(int)
    off = (col_i % 2) * s * 0.866
    row_i = np.floor((y + off) / (s * 1.732)).astype(int)
    key = (col_i * 31 + row_i * 17).astype(int)
    pal = np.array(colours, float)
    col = pal[key % len(colours)] * (1 + 0.12 * np.sin(key * 7.13))[..., None]
    fx = (x - col_i * s * 1.5) / (s * 1.5)
    fy = ((y + off) - row_i * s * 1.732) / (s * 1.732)
    edge = (fx < 0.1) | (fy < 0.1) | (np.abs(fx - 0.5 * fy) < 0.05)
    col[edge] = np.array(grout, float)
    col = shade(col, 1 + 0.18 * (pnoise(seed, 1.5) - 0.5))
    return shade(col, grit(seed + 1, 0.12))

def ashlar(seed, colours, joint, courses=4):
    """Dressed limestone: even courses, long blocks, fine joints, a chiselled face."""
    col = bricks(seed, colours, joint, courses=courses, per_row=2, mortar_px=1, var=0.07, gritamt=0.12)
    x, y = coords()
    tool = (pnoise_streak(seed + 3, 0.6, 0.25) - 0.5) * 0.10
    return shade(col, 1 + tool)

def shutters(seed, paint, frame):
    """A shopfront's louvred shutters: painted slats, a frame, a hinge."""
    x, y = coords()
    col = solid(paint) * (1 + 0.08 * (pnoise(seed, 1.6) - 0.5))[..., None]
    col[(y % 4) == 3] *= 0.62
    col[(y % 4) == 0] *= 1.12
    fr = (x % 32 < 3) | (x % 32 >= 29) | (y % 64 < 3)
    col[fr] = np.array(frame, float)
    col[((x % 32 == 4) | (x % 32 == 27)) & ((y % 32 > 10) & (y % 32 < 14))] = (40, 38, 36)
    return shade(col, grit(seed + 1, 0.14))

def fountain_water(seed):
    """A fountain's basin: shallow teal over a pale floor, rings of ripples."""
    x, y = coords()
    cx, cy = (x % 32) - 16.0, (y % 32) - 16.0
    rr = np.sqrt(cx * cx + cy * cy)
    rip = 0.5 + 0.5 * np.sin(rr * 0.9 + 6.0 * pnoise(seed, 1.2))
    col = ramp(0.55 * rip + 0.45 * pnoise(seed + 1, 1.5), [(0, (28, 98, 104)), (0.6, (54, 150, 150)), (1, (150, 214, 206))])
    return shade(col, grit(seed + 2, 0.04))

# ---------------------------------------------------------------- the mall (2026-11-19): an 80s shopping mall

def terrazzo(seed, base, chips, density=0.22):
    """Terrazzo: a pale cement with chips of marble in pastels, polished."""
    rng = np.random.default_rng(seed)
    col = solid(base) * (1 + 0.05 * (pnoise(seed, 1.6) - 0.5))[..., None]
    x, y = coords()
    pal = np.array(chips, float)
    n = int(N * N * density / 6)
    for _ in range(n):
        cx, cy = rng.integers(0, N, 2)
        r = rng.uniform(0.8, 2.4)
        c = pal[rng.integers(0, len(chips))] * rng.uniform(0.85, 1.1)
        dx = (x - cx + N / 2) % N - N / 2
        dy = (y - cy + N / 2) % N - N / 2
        m = dx * dx + (dy * rng.uniform(0.6, 1.4)) ** 2 < r * r
        col[m] = c
    return shade(col, grit(seed + 1, 0.04))

def neon_tile_wall(seed, tile, grout, band1, band2):
    """Glossy white wall tiles, a magenta and a teal stripe across them."""
    col = tiles(seed, [tile], grout, n=8, var=0.03, bevel=0.06, gritamt=0.03)
    x, y = coords()
    col[(y >= 36) & (y < 41)] = np.array(band1, float)
    col[(y >= 43) & (y < 46)] = np.array(band2, float)
    col[(y == 35) | (y == 41) | (y == 42) | (y == 46)] *= 0.85
    return col

def memphis_carpet(seed, base, colours):
    """A loud carpet: squiggles, triangles and dots in neon on a dark ground."""
    rng = np.random.default_rng(seed)
    x, y = coords()
    col = solid(base) * (1 + 0.10 * (pnoise(seed, 0.8) - 0.5))[..., None]
    pal = np.array(colours, float)
    # squiggles: a sine line, wrapped
    for k in range(3):
        ph, amp, yo = rng.uniform(0, 6.28), rng.uniform(2, 4), rng.uniform(0, N)
        yy = (yo + amp * np.sin(x * 2 * np.pi / 16 + ph)) % N
        m = np.minimum(np.abs(y - yy), N - np.abs(y - yy)) < 1.1
        m &= ((x + k * 11) % 32) < 22
        col[m] = pal[k % len(colours)]
    # triangles
    for k in range(5):
        cx, cy, sz = rng.integers(0, N), rng.integers(0, N), rng.integers(4, 7)
        dx = (x - cx) % N
        dy = (y - cy) % N
        m = (dx < sz) & (dy < sz) & (dy >= dx * 0.5) & (dy <= sz - dx * 0.5) & (dx + dy >= 0)
        col[m] = pal[(k + 1) % len(colours)]
    # dots
    for k in range(14):
        cx, cy = rng.integers(0, N, 2)
        dx = (x - cx + N / 2) % N - N / 2
        dy = (y - cy + N / 2) % N - N / 2
        col[dx * dx + dy * dy < 1.6] = pal[(k + 2) % len(colours)]
    return shade(col, grit(seed + 1, 0.22))

def confetti_carpet(seed, base, colours):
    """A plum carpet strewn with confetti dashes."""
    rng = np.random.default_rng(seed)
    x, y = coords()
    col = solid(base) * (1 + 0.08 * (pnoise(seed, 1.0) - 0.5))[..., None]
    pal = np.array(colours, float)
    for k in range(60):
        cx, cy = rng.integers(0, N, 2)
        horiz = rng.integers(0, 2)
        dx = (x - cx) % N
        dy = (y - cy) % N
        m = ((dx < 3) & (dy < 1)) if horiz else ((dx < 1) & (dy < 3))
        col[m] = pal[k % len(colours)]
    return shade(col, grit(seed + 1, 0.24))

def grid_panel(seed, base, line, glow):
    """A pastel panel ruled in a bright grid, the lines glowing."""
    x, y = coords()
    col = solid(base) * (1 + 0.06 * (pnoise(seed, 1.4) - 0.5))[..., None]
    g = (x % 16 == 0) | (y % 16 == 0)
    h = ((x % 16 == 1) | (x % 16 == 15) | (y % 16 == 1) | (y % 16 == 15)) & ~g
    col[h] = (col[h] * 0.5 + np.array(glow, float) * 0.5)
    col[g] = np.array(line, float)
    return shade(col, grit(seed + 1, 0.04))

def glass_block(seed, glass, mortar):
    """Glass blocks: square panes with a bright rim and a darker, wavy centre."""
    x, y = coords()
    s = 16
    fx, fy = (x % s) / s, (y % s) / s
    d = np.maximum(np.abs(fx - 0.5), np.abs(fy - 0.5))
    wav = 0.5 + 0.5 * np.sin((fx + fy) * 12.0 + pnoise(seed, 1.2) * 3)
    col = ramp(0.35 + 0.4 * d + 0.25 * wav, [(0, tuple(int(c * 0.55) for c in glass)), (0.6, glass), (1, (236, 250, 250))])
    col[(x % s == 0) | (y % s == 0)] = np.array(mortar, float)
    return shade(col, grit(seed + 1, 0.03))

def skylight(seed, frame, sky):
    """A skylight from below: a white frame grid, pale sky through the glass."""
    x, y = coords()
    t = (y / N) * 0.4 + 0.6 * pnoise(seed, 2.2)
    col = ramp(t, [(0, sky), (1, tuple(min(255, int(c * 1.18)) for c in sky))])
    fr = (x % 32 < 2) | (y % 32 < 2) | (x % 16 == 0) & (y % 32 >= 2)
    col[fr] = np.array(frame, float)
    return shade(col, grit(seed + 1, 0.02))

def escalator_treads(seed, metal, edge):
    """An escalator's steps: grooved aluminium, a yellow edge on each step."""
    x, y = coords()
    col = solid(metal) * (0.92 + 0.12 * pnoise_streak(seed, 1.0, 6))[..., None]
    col[(x % 3) == 0] *= 0.62
    st = (y % 16)
    col[st < 2] = np.array(edge, float)
    col[st == 2] *= 0.5
    return shade(col, grit(seed + 1, 0.05))

def chrome(seed, base):
    """Chrome: bright vertical streaks of reflection over a cool grey."""
    x, y = coords()
    sweep = np.sin(x * 2 * np.pi / N * 2 + pnoise(seed, 1.0) * 2.0) * 0.5 + 0.5
    col = ramp(0.25 + 0.6 * sweep + 0.15 * pnoise_streak(seed + 1, 1.0, 8), [(0, (70, 76, 86)), (0.5, base), (1, (250, 252, 255))])
    col[(y % 32) == 0] *= 0.6
    return col

def vct(seed, colours, grout):
    """Vinyl tile: a back corridor's speckled squares, some a shade off."""
    rng = np.random.default_rng(seed)
    x, y = coords()
    s = 16
    key = ((x // s) * 7 + (y // s) * 13).astype(int)
    pal = np.array(colours, float)
    col = pal[key % len(colours)]
    sp = rng.random((N, N))
    col[sp < 0.08] *= 0.7
    col[sp > 0.95] *= 1.15
    col[(x % s == 0) | (y % s == 0)] = np.array(grout, float)
    return shade(col, grit(seed + 1, 0.06))

def acoustic(seed, base, frame):
    """A dropped ceiling: fissured tiles in a white grid, one a light."""
    rng = np.random.default_rng(seed)
    x, y = coords()
    col = solid(base) * (1 + 0.06 * (pnoise(seed, 1.2) - 0.5))[..., None]
    sp = rng.random((N, N))
    col[sp < 0.12] *= 0.8
    worm = (np.abs(np.sin(x * 0.9 + 3 * pnoise(seed + 2, 1.0)) * np.cos(y * 0.7)) < 0.06)
    col[worm] *= 0.78
    s = N // 2
    lit = ((x // s) + (y // s)) % 2 == 0
    inner = (x % s > 4) & (x % s < s - 4) & (y % s > 8) & (y % s < s - 8) & lit
    col[inner] = np.array((250, 250, 242), float) * (0.96 + 0.04 * ((x % 4) == 0))[inner][..., None]
    col[(x % s < 2) | (y % s < 2)] = np.array(frame, float)
    return col

def parking(seed):
    """A car park: asphalt with white bay lines."""
    col = concrete(seed, (64, 64, 66), var=0.14, gritamt=0.30, stains=0.15)
    x, y = coords()
    col[((x % 32) < 2) & ((y % 64) < 44)] = (220, 220, 210)
    col[((y % 64) >= 44) & ((y % 64) < 46)] = (200, 190, 60)
    return col

def mall_facade(seed, brick, band1, band2):
    """The mall's outside: tan split-face block, a pink and a teal stripe."""
    col = bricks(seed, [brick, tuple(int(c * 0.92) for c in brick), tuple(min(255, int(c * 1.05)) for c in brick)], (150, 140, 124), courses=8, per_row=2, var=0.06)
    x, y = coords()
    col[(y >= 24) & (y < 28)] = np.array(band1, float)
    col[(y >= 30) & (y < 33)] = np.array(band2, float)
    return col

def dept_wallpaper(seed, base, stripe, dado, rail):
    """A department store's wall: pale wallpaper with fine pinstripes, a
    chrome rail over a coloured dado."""
    x, y = coords()
    col = solid(base) * (1 + 0.04 * (pnoise(seed, 2.0) - 0.5))[..., None]
    col[(x % 8) == 3] = np.array(stripe, float)
    col[((x % 16) == 11) & (y < 44)] = np.array(stripe, float) * 0.92 + np.array(base, float) * 0.08
    col[y >= 46] = np.array(dado, float)
    col[(y >= 46) & ((x % 16) < 1)] = np.array(dado, float) * 0.82
    col[(y >= 43) & (y < 46)] = np.array(rail, float)
    col[y == 43] = np.array(rail, float) * 1.12
    col[y == 45] = np.array(rail, float) * 0.70
    col = shade(col, grit(seed + 1, 0.04))
    return col

# ---------------------------------------------------------------- the pipeworks (2026-11-19)

def pw_grate(seed, bar, holes_every=4):
    """Steel bar grating seen from above: bearing bars one way, cross bars
    the other, the gaps between see-through (returns the holes too)."""
    x, y = coords()
    col = solid(bar) * (1 + 0.10 * (pnoise(seed, 1.4) - 0.5))[..., None]
    col = shade(col, grit(seed + 1, 0.10))
    bear = (x % holes_every) == 0
    cross = (y % (holes_every * 2)) == 0
    solidpx = bear | cross
    col[bear] *= 1.12
    col[cross & ~bear] *= 0.86
    rust = pnoise(seed + 2, 1.6)
    col = shade(col, 1 - 0.25 * np.clip((rust - 0.62) * 3, 0, 1))
    return col, ~solidpx

def pw_pipe(seed, paint, band, seam_every=32):
    """A big pipe's skin (s along it, t round it): chipped paint, a welded
    seam and a flange band every so often, rust weeping from the bands."""
    x, y = coords()
    col = solid(paint) * (1 + 0.06 * (pnoise(seed, 1.8) - 0.5))[..., None]
    col = shade(col, grit(seed + 1, 0.08))
    chips = pnoise(seed + 2, 1.2)
    col[chips > 0.78] = col[chips > 0.78] * 0.55 + np.array((120, 96, 80), float) * 0.45
    bx = x % seam_every
    col[(bx >= 0) & (bx < 4)] = np.array(band, float) * (1 + 0.06 * (pnoise(seed + 3, 1.0)[(bx >= 0) & (bx < 4)] - 0.5))[..., None]
    col[bx == 4] *= 0.7
    col[bx == 63 % seam_every] *= 0.7
    for k in range(0, N, 8):
        col[(bx == 2) & ((y % 8) == 4)] *= 1.3     # bolt heads
    weep = pnoise_streak(seed + 4, 1.6, 4.0)
    col = shade(col, 1 - 0.18 * np.clip((weep - 0.55) * 3, 0, 1) * (bx < 16))
    shine = 0.86 + 0.28 * np.sin(np.pi * y / N)        # lit along the top
    return shade(col, shine)

def pw_sludge(seed):
    """Toxic sludge: green-yellow muck, darker swirls, pale foam and bubbles."""
    x, y = coords()
    w = pnoise(seed, 2.4)
    t = 0.65 * w + 0.35 * pnoise(seed + 1, 1.2)
    col = ramp(t, [(0, (34, 62, 18)), (0.45, (70, 120, 24)), (0.75, (128, 176, 40)), (1, (186, 214, 84))])
    sw = np.sin((w * 9 + pnoise(seed + 2, 2.0) * 3) * np.pi)
    col = shade(col, 1 - 0.18 * np.clip(sw, 0, 1))
    foam = pnoise(seed + 3, 0.9)
    col[foam > 0.86] = np.array((210, 228, 150), float)
    rng = np.random.default_rng(seed + 4)
    for _ in range(14):
        cx, cy, r = rng.integers(0, N), rng.integers(0, N), rng.integers(1, 3)
        d = ((x - cx + N / 2) % N - N / 2) ** 2 + ((y - cy + N / 2) % N - N / 2) ** 2
        col[d <= r * r] = np.array((196, 222, 110), float)
        col[(d <= r * r) & (d >= (r - 0.7) ** 2)] *= 0.75
    return col

def pw_tread(seed, base):
    """Diamond tread plate: raised diamonds in rows, worn bright on top."""
    x, y = coords()
    col = solid(base) * (1 + 0.08 * (pnoise(seed, 1.6) - 0.5))[..., None]
    col = shade(col, grit(seed + 1, 0.10))
    u = (x + (y // 8 % 2) * 4) % 8
    v = y % 8
    dia = (np.abs(u - 3.5) + np.abs(v - 3.5) * 2.4) < 3.2
    col[dia] *= 1.22
    col[(np.abs(u - 3.5) + np.abs(v - 4.5) * 2.4 < 3.2) & ~dia] *= 0.78
    wear = pnoise(seed + 2, 1.4)
    col = shade(col, 1 + 0.14 * np.clip((wear - 0.6) * 3, 0, 1))
    return col

def pw_hazard(seed, yellow, black, stripe=8):
    """Yellow and black chevrons, scuffed."""
    x, y = coords()
    on = ((x + y) // stripe) % 2 == 0
    col = np.where(on[..., None], solid(yellow), solid(black))
    col = shade(col, grit(seed, 0.14))
    scuff = pnoise(seed + 1, 1.0)
    col[scuff > 0.82] = col[scuff > 0.82] * 0.6 + np.array((90, 86, 80), float) * 0.4
    return col

def pw_deck(seed, base, edge):
    """A concrete deck slab: stains, cracks, a painted safety line."""
    col = concrete(seed, base, var=0.12, gritamt=0.14, stains=0.18, seam=32)
    x, y = coords()
    col[(y >= 58) & (y < 62)] = np.array(edge, float) * (1 + 0.1 * (pnoise(seed + 5, 1.0)[(y >= 58) & (y < 62)] - 0.5))[..., None]
    return col

def pw_brick(seed):
    """Sooty yellow-brown works brick, black-streaked."""
    col = bricks(seed, [(132, 96, 60), (118, 84, 52), (146, 108, 70), (104, 76, 50)], (70, 64, 58), courses=8, per_row=4, var=0.12, gritamt=0.20)
    soot = pnoise_streak(seed + 9, 1.4, 0.25)
    return shade(col, 1 - 0.45 * np.clip((soot - 0.45) * 2.5, 0, 1))

def pw_tile(seed, glaze):
    """Glazed green tiles in a control room, a dark band at dado height."""
    col = tiles(seed, [glaze, tuple(int(c * 0.94) for c in glaze), tuple(min(255, int(c * 1.04)) for c in glaze)], (60, 70, 62), n=8, var=0.04, bevel=0.08)
    x, y = coords()
    col[(y >= 40) & (y < 48)] *= 0.62
    return col

def pw_truss(seed):
    """The roof seen from below: dark sheeting and a steel truss's members."""
    x, y = coords()
    col = solid((52, 54, 58)) * (1 + 0.12 * (pnoise(seed, 1.5) - 0.5))[..., None]
    col[(x % 32) < 3] = np.array((96, 84, 60), float)
    col[(y % 32) < 3] = np.array((96, 84, 60), float)
    d = (x - y) % 32
    col[(d < 2) | (d > 30)] = np.array((86, 76, 56), float)
    col = shade(col, grit(seed + 1, 0.12))
    return col

def pw_stairs(seed, base):
    """Steel stair treads: a ribbed tread and a dark gap, a yellow nosing."""
    x, y = coords()
    col = solid(base) * (1 + 0.08 * (pnoise(seed, 1.4) - 0.5))[..., None]
    v = y % 16
    col[(v % 3) == 0] *= 0.8
    col[v >= 14] = np.array((30, 30, 32), float)
    col[(v >= 0) & (v < 2)] = np.array((206, 170, 40), float)
    return shade(col, grit(seed + 1, 0.10))

def pw_ladder(seed, paint):
    """Painted steel for the ladders' rails and rungs."""
    col = solid(paint) * (1 + 0.10 * (pnoise(seed, 1.5) - 0.5))[..., None]
    chips = pnoise(seed + 1, 1.0)
    col[chips > 0.8] = np.array((80, 76, 70), float)
    return shade(col, grit(seed + 2, 0.12))

def pw_boiler(seed, base):
    """Boiler plate: big riveted sheets, heat-blued and soot-stained."""
    col = panels(seed, base, n=2, rivets=True, bevel=0.12, grime=0.30, seam=0.45, gritamt=0.12)
    heat = pnoise(seed + 7, 2.0)
    blue = np.clip((heat - 0.55) * 2.5, 0, 1)[..., None]
    return col * (1 - 0.35 * blue) + np.array((60, 70, 110), float) * 0.35 * blue

def pw_lino(seed, base):
    """A control room's speckled lino."""
    col = solid(base) * (1 + 0.05 * (pnoise(seed, 1.6) - 0.5))[..., None]
    sp = pnoise(seed + 1, 0.0)
    col[sp > 0.80] *= 0.72
    col[sp < 0.12] *= 1.18
    return col

def quantize(rgb):
    """16 colours; a painter that gives (rgb, holes) gets 15 and the 16th
    see-through (its palette entry's alpha bit clear)"""
    holes = None
    if isinstance(rgb, tuple):
        rgb, holes = rgb
    im = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8), 'RGB')
    q = im.quantize(colors=15 if holes is not None else 16, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = q.getpalette()[:48]
    while len(pal) < 48:
        pal += [0, 0, 0]
    idx = np.array(q, dtype=np.uint8)
    pal = [tuple(pal[3 * i:3 * i + 3]) for i in range(16)]
    prev = q.convert('RGB')
    if holes is not None:
        idx = np.where(holes, 15, idx).astype(np.uint8)
        pal[15] = (0, 0, 0, 0)
        pa = np.array(prev)
        pa[holes] = (255, 0, 255)
        prev = Image.fromarray(pa, 'RGB')
    return idx, pal, prev

def encode(idx, pal):
    """The game's zlib texture: 0x41 (zlib, one level), then bits: format 8
    (RGBA16_CI4 = 0x0a), colours - 1, the palette (RGBA5551), width, height,
    then 11 73 + 24-bit length + raw deflate of the packed indices."""
    out = bytearray([0x41, 0x0a, 15])
    for c in pal:
        r, g, b = c[:3]
        v = (r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (0 if len(c) == 4 else 1)
        out += bytes([v >> 8, v & 255])
    out += bytes([N, N])
    packed = bytearray()
    for y in range(N):
        for x in range(0, N, 2):
            packed.append(int(idx[y, x]) << 4 | int(idx[y, x + 1]))
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    comp = co.compress(bytes(packed)) + co.flush()
    out += bytes([0x11, 0x73, len(packed) >> 16, (len(packed) >> 8) & 255, len(packed) & 255]) + comp
    assert len(out) <= 4096
    return bytes(out)

SAND = [(0, (126, 82, 48)), (0.4, (170, 116, 70)), (0.75, (196, 144, 92)), (1, (214, 168, 114))]
OAK = [(0, (92, 56, 30)), (0.45, (138, 90, 50)), (0.8, (170, 118, 68)), (1, (190, 140, 86))]
DARKWOOD = [(0, (46, 26, 16)), (0.5, (78, 46, 26)), (0.85, (104, 64, 36)), (1, (122, 80, 46))]
GREYWOOD = [(0, (82, 72, 62)), (0.5, (122, 108, 92)), (0.85, (150, 136, 116)), (1, (168, 154, 134))]

# name, what it is, painter. ADD NEW ONES AT THE END (the number is the place).
TEXTURES = [
    # the canyon (ravine)
    ("CLIFF_BASE", "dark red-brown sandstone", lambda: strata(11, [(0, (82, 40, 26)), (0.45, (122, 62, 38)), (0.8, (150, 84, 52)), (1, (168, 102, 64))], 1.0, 0.30, 0.20)),
    ("CLIFF_MID", "orange sandstone", lambda: strata(23, [(0, (140, 74, 40)), (0.4, (180, 106, 60)), (0.8, (204, 134, 80)), (1, (218, 156, 98))], 1.0, 0.28, 0.16)),
    ("CLIFF_TOP", "pale weathered sandstone", lambda: strata(37, [(0, (170, 122, 82)), (0.45, (204, 158, 112)), (0.8, (222, 184, 138)), (1, (234, 202, 160))], 1.2, 0.32, 0.18)),
    ("DESERT", "rocky desert floor", lambda: gravel(51, SAND)),
    ("TIN_RUST", "corrugated tin, rusting", lambda: corrugated(61, (150, 146, 136), ribs=8, rust=0.45, streaks=0.18)),
    ("BOARDS_WEATHERED", "weathered grey boards", lambda: planks(62, GREYWOOD, boards=4)),
    ("STEEL_RUST", "riveted steel plates, rust", lambda: panels(63, (112, 92, 76), n=2, grime=0.22)),
    # the city (street)
    ("ASPHALT", "asphalt", lambda: asphalt(71)),
    ("BRICK_RED", "red brick", lambda: bricks(72, [(150, 62, 44), (132, 54, 40), (164, 76, 54), (120, 50, 38)], (176, 168, 156))),
    ("BRICK_PAINTED", "painted brick, cream", lambda: bricks(73, [(206, 196, 172), (198, 188, 164), (212, 202, 180)], (160, 152, 138), var=0.04)),
    ("OAK_FLOOR", "oak floorboards", lambda: planks(74, OAK, boards=4)),
    ("PLASTER_BEIGE", "beige plaster, faint stripe", lambda: plaster(75, (196, 182, 150), stripes=8, stripe_amt=0.03)),
    ("CEILING_TILES", "acoustic ceiling tiles", lambda: ceiling_tiles(76, (214, 210, 200))),
    ("CONCRETE", "concrete", lambda: concrete(77, (150, 148, 142), seam=32)),
    ("CHECKER_TILES", "black and white floor tiles", lambda: tiles(78, [(208, 206, 198), (54, 52, 50)], (120, 118, 112), n=4, checker=True)),
    ("WALL_TILES", "white wall tiles", lambda: tiles(79, [(214, 216, 210), (206, 210, 204)], (150, 152, 148), n=8, var=0.03, bevel=0.06)),
    ("DARK_PLANKS", "dark wood", lambda: planks(80, DARKWOOD, boards=4)),
    ("WOOD_PANELLING", "dark wood panelling", lambda: planks(81, DARKWOOD, boards=4, along_x=False, joints=0)),
    ("PAVING", "concrete paving slabs", lambda: tiles(82, [(158, 154, 146), (150, 146, 138), (164, 160, 150)], (96, 92, 88), n=2, var=0.05, bevel=0.06, gritamt=0.16)),
    # the depot (warehouse)
    ("FLOOR_CONCRETE", "smooth warehouse concrete", lambda: concrete(91, (138, 136, 130), var=0.12, gritamt=0.08, stains=0.18)),
    ("SHEET_BLUE", "blue-grey corrugated sheet", lambda: corrugated(92, (92, 108, 124), ribs=8, streaks=0.10, depth=0.22)),
    ("ROOF_STEEL", "steel roof decking", lambda: corrugated(93, (104, 104, 104), ribs=4, streaks=0.05, depth=0.28)),
    ("SHELVES", "rack shelf with boxes", lambda: shelving(94)),
    ("BLOCKWORK", "grey concrete blocks", lambda: bricks(95, [(142, 140, 134), (134, 132, 126), (150, 148, 140)], (110, 108, 102), courses=4, per_row=2, var=0.04, gritamt=0.20)),
    ("TREAD_PLATE", "steel tread plate", lambda: diamond(96, (120, 122, 124))),
    ("HAZARD", "hazard stripes", lambda: hazard(97)),
    ("OFFICE_GREEN", "pale green wall, darker band", lambda: wall_band(98, (176, 190, 170), (92, 110, 90), (130, 146, 124))),
    ("FLOOR_TILES_GREY", "grey vinyl floor tiles", lambda: tiles(99, [(130, 132, 134), (122, 124, 126), (138, 140, 140)], (104, 104, 106), n=2, var=0.04, bevel=0.04)),
    # the facility (hub) and the lab
    ("PANEL_GREY", "grey-blue wall panels", lambda: panels(111, (108, 118, 130), n=2)),
    ("PANEL_DARK", "dark steel floor plates", lambda: panels(112, (70, 74, 80), n=2, grime=0.16)),
    ("PANEL_OLIVE", "olive wall panels", lambda: panels(113, (112, 116, 88), n=2, grime=0.14)),
    ("LAB_WHITE_TEAL", "white lab panels, teal stripe", lambda: lab_panels(114, (214, 218, 216), (40, 150, 150))),
    ("LAB_GREY_BLUE", "light grey panels, blue stripe", lambda: lab_panels(115, (176, 182, 188), (50, 90, 170))),
    ("LAB_FLOOR", "white lab floor tiles", lambda: tiles(116, [(200, 204, 204), (192, 196, 198)], (150, 154, 156), n=2, var=0.03, bevel=0.05)),
    ("LIGHT_PANELS", "ceiling light panels", lambda: light_panels(117, (150, 154, 158), (232, 236, 230))),
    ("CEIL_METAL", "metal ceiling panels", lambda: panels(118, (132, 136, 140), n=2, rivets=False, grime=0.06)),
    ("PANEL_RED", "red-brown wall panels", lambda: panels(119, (120, 70, 58), n=2, grime=0.12)),
    ("CARPET_BLUE", "blue-grey carpet", lambda: concrete(120, (76, 84, 104), var=0.08, gritamt=0.22, stains=0.06)),
    # the villa (Carrington-like, 2026-11-15)
    ("ROOF_TILES", "terracotta barrel roof tiles", lambda: roof_tiles(131, [(172, 76, 44), (158, 66, 40), (184, 92, 54), (146, 62, 38)])),
    ("STUCCO", "cream stucco", lambda: plaster(132, (214, 196, 160), var=0.10)),
    ("TERRACOTTA", "terracotta floor tiles", lambda: tiles(133, [(176, 92, 56), (166, 84, 50), (184, 100, 62)], (204, 184, 150), n=4, var=0.06, bevel=0.06, gritamt=0.10)),
    ("MARBLE", "cream marble slabs", lambda: marble(134, (222, 214, 198), (132, 124, 116))),
    ("FLAGSTONE", "warm flagstone paving", lambda: bricks(135, [(186, 172, 146), (170, 156, 132), (196, 182, 156), (160, 148, 126)], (112, 102, 86), courses=4, per_row=2, var=0.06, gritamt=0.16)),
    ("WATER", "deep water", lambda: water(136)),
    ("STONE_WALL", "rough stone walling", lambda: stone_wall(137, [(150, 138, 118), (136, 124, 106), (164, 150, 128), (126, 116, 100)], (90, 82, 70), courses=4)),
    ("BEAMS", "dark wooden ceiling beams", lambda: planks(138, DARKWOOD, boards=2, joints=0, gap=0.30)),
    ("STUCCO_OCHRE", "ochre stucco", lambda: plaster(139, (204, 160, 96), var=0.12)),
    # the rooftops (2026-11-16)
    ("FACADE_OFFICE", "office block face, blue glass windows", lambda: facade(151, (150, 148, 142), (44, 66, 96), (76, 78, 82), cols=2, rows=2, lit=0.25)),
    ("FACADE_BRICK", "brick block face, sash windows", lambda: facade(152, (132, 64, 46), (40, 50, 64), (196, 188, 170), cols=2, rows=2, lit=0.35)),
    ("ROOF_FELT", "flat roof, tarred felt", lambda: roof_membrane(153, (70, 70, 74))),
    ("ROOF_GRAVEL", "flat roof, gravel", lambda: gravel(154, [(0, (96, 92, 86)), (0.5, (128, 124, 116)), (0.85, (150, 146, 138)), (1, (170, 166, 158))])),
    # the villa's gardens (2026-11-16)
    ("HEDGE", "clipped hedge", lambda: foliage(161, [(0, (22, 54, 24)), (0.45, (38, 84, 34)), (0.8, (58, 110, 44)), (1, (84, 136, 58))])),
    ("FLOWER_BED", "flower bed", lambda: foliage(162, [(0, (36, 70, 30)), (0.5, (60, 104, 44)), (1, (96, 140, 62))], flowers=[(214, 60, 70), (236, 200, 60), (226, 226, 232), (188, 90, 190)], amount=0.20)),
    ("POOL_WATER", "pool water", lambda: pool_water(163)),
    # Claude Missions: Pier 17 (2026-11-16)
    ("CONTAINER_BLUE", "shipping container, blue, white stripe", lambda: np.roll(container(171, (38, 78, 122), (214, 214, 206), rust=0.22), 24, axis=0)),
    ("CONTAINER_RED", "shipping container, oxide red", lambda: np.roll(container(172, (140, 52, 36), (206, 170, 60), rust=0.30, door=True), 24, axis=0)),
    ("CONTAINER_TOP", "container roof, ribbed and pooled", lambda: corrugated(173, (70, 96, 124), ribs=16, rust=0.25, streaks=0.05, depth=0.20)),
    ("DOCK_CONCRETE", "terminal concrete, yellow lane line", lambda: dock_concrete(174)),
    ("HARBOUR_WATER", "harbour water at night", lambda: harbour_water(175)),
    ("QUAY_WALL", "quay face, tide line", lambda: quay_wall(176)),
    ("SHED_WALL", "warehouse wall, cream sheet over a blue dado", lambda: warehouse_wall(177, (196, 190, 170), (44, 70, 104))),
    ("BAY_FLOOR", "warehouse floor, hazard edge", lambda: bay_floor(178)),
    ("PAD_DECK", "landing pad deck", lambda: helipad(179)),
    ("CONTAINER_GREEN", "shipping container, green", lambda: np.roll(container(180, (44, 104, 80), (220, 220, 210), rust=0.28), 24, axis=0)),
    ("OFFICE_NAVY", "office wall, grey over navy", lambda: wall_band(181, (168, 172, 174), (30, 34, 40), (40, 58, 88))),
    # Claude Missions: Hollow Creek (2026-11-17)
    ("TREE_WALL", "pines: trunks below, crown above, ground to treetop", lambda: np.roll(tree_column(191, 0.62), 48, axis=0)),
    ("TREE_TOP", "pine crowns from above", lambda: foliage(192, [(0, (12, 30, 18)), (0.5, (30, 64, 34)), (1, (60, 96, 48))])),
    ("LAWN", "mown lawn", lambda: lawn(193)),
    ("FOREST_FLOOR", "trail dirt and needles", lambda: forest_floor(194)),
    ("SIDING_CREAM", "cream weatherboard", lambda: clapboard(195, (214, 206, 182))),
    ("WALLPAPER", "sprig wallpaper over a wood dado", lambda: wallpaper(196, (222, 214, 190), (150, 92, 86), (118, 82, 52))),
    ("HESCO", "barrier bags in wire mesh", lambda: hesco(197, (176, 156, 116))),
    ("BUNKER_OLIVE", "olive military concrete", lambda: bunker(198, (104, 108, 82), (52, 58, 40))),
    ("GRAVEL_YARD", "base gravel, tyre tracks", lambda: gravel(199, [(0, (92, 88, 78)), (0.5, (120, 116, 104)), (1, (150, 146, 132))])),
    ("RUIN_STONE", "old dressed stone, mossy", lambda: ruin_stone(200)),
    ("FLAGSTONES", "ruin flagstones, grass between", lambda: flagstones(201)),
    ("CRYPT_STONE", "crypt stone, dark and damp", lambda: shade(ruin_stone(202, 0.15), 0.62)),
    # the penthouse (2026-11-17)
    ("MARBLE_BLACK", "black marble, white veins", lambda: marble(211, (42, 40, 44), (176, 172, 168), grout=(24, 22, 24))),
    ("WALNUT", "walnut wall panelling", lambda: planks(212, [(0, (58, 34, 22)), (0.5, (92, 56, 34)), (0.85, (118, 76, 46)), (1, (136, 92, 58))], boards=2, along_x=False, joints=1, gap=0.30)),
    ("CARPET_CHARCOAL", "charcoal carpet", lambda: concrete(213, (62, 62, 68), var=0.06, gritamt=0.26, stains=0.0)),
    ("SILK_WALL", "silk wallpaper, damask stripe", lambda: silk_wall(214, (206, 198, 178), (176, 160, 120))),
    ("LIFT_STEEL", "brushed steel lining", lambda: brushed_steel(215, (150, 154, 160))),
    ("TEAK_DECK", "teak decking", lambda: planks(216, [(0, (106, 70, 40)), (0.5, (146, 102, 62)), (0.85, (172, 126, 80)), (1, (188, 142, 94))], boards=8, gap=0.55)),
    ("FACADE_GLASS", "glass curtain wall", lambda: facade(217, (60, 66, 74), (54, 84, 120), (40, 44, 50), cols=2, rows=2, lit=0.15)),
    ("CEIL_COFFER", "coffered ceiling, cove lit", lambda: coffer(218, (214, 206, 190), (198, 190, 174), (248, 236, 200))),
    # the building site (2026-11-17)
    ("SLAB_FORM", "fresh slab concrete, trowel marks", lambda: concrete(221, (156, 154, 148), var=0.14, gritamt=0.14, stains=0.16, seam=32)),
    ("FORMWORK", "ply formwork, shutter marks", lambda: formwork(222, (170, 140, 96))),
    ("GIRDER", "steel beam, red oxide primer", lambda: girder(223, (150, 64, 42))),
    ("SCAFFOLD_BOARDS", "scaffold boards, hooped ends", lambda: planks(224, [(0, (120, 100, 70)), (0.5, (156, 132, 92)), (1, (182, 158, 116))], boards=4, joints=1)),
    ("HOIST_MESH", "hoist cage, yellow mesh", lambda: mesh(225, (214, 176, 40), (40, 40, 42))),

    ("FREIGHT_ENAMEL", "cream siding over freight-orange enamel", lambda: warehouse_wall(241, (208, 202, 170), (174, 77, 34))),
    ("FREIGHT_IRON", "blue-grey dock plates", lambda: panels(242, (79, 101, 111), n=4)),
    ("CANAL_TILE", "glazed sea-green pump house tiles", lambda: tiles(243, [(48, 112, 107), (73, 134, 128), (82, 144, 132)], (38, 73, 71), n=8)),
    ("CANAL_SILT", "dark mineral-stained canal bed", lambda: concrete(244, (90, 99, 79), var=0.28, stains=0.46, seam=32)),
    ("MARKET_AWNING", "striped saffron and wine market canvas", lambda: market_awning(245)),
    ("MARKET_PAVERS", "night market plum and clay pavers", lambda: bricks(246, [(119, 78, 75), (149, 98, 78), (103, 77, 83)], (66, 61, 66))),
    ("STUDIO_FOAM", "alternating graphite acoustic wedges", lambda: acoustic_foam(247)),
    ("STUDIO_BAND", "deep blue cladding with a red tally band", lambda: wall_band(248, (58, 78, 114), (222, 67, 60), (40, 49, 68))),
    ("RELAY_BASALT", "weathered basalt ridge", lambda: gravel(249, [(0, (47, 51, 65)), (0.5, (97, 102, 109)), (1, (159, 150, 133))])),
    ("RELAY_CLADDING", "ribbed signal-orange weather cladding", lambda: corrugated(250, (168, 93, 51), ribs=8, rust=0.15)),
    ("STUDIO_MONITOR", "recessed control-room screen with colour bars", lambda: studio_monitor(251)),
    ("CANAL_GAUGE", "painted sluice gate with a depth gauge", lambda: canal_gate(252)),
    ("MARKET_STALL", "paper lantern on dark timber stall slats", lambda: lantern_stall(253)),
    ("RELAY_STRATA", "exposed layers of fractured ridge stone", lambda: ridge_stone(254)),
    # the market town (2026-11-18): its square, alleys, shops, fountain and walls
    ("PLAZA_FAN", "a square's fan-laid cobbles", lambda: fan_cobbles(261, [(150, 140, 126), (128, 120, 108), (166, 154, 136), (112, 106, 98)], (70, 64, 58))),
    ("ALLEY_SETTS", "an alley's granite setts", lambda: setts(262, [(108, 106, 104), (92, 90, 90), (122, 118, 112)], (52, 50, 48))),
    ("STUCCO_ROSE", "rose stucco, the plaza's houses", lambda: plaster(263, (196, 138, 124), var=0.14)),
    ("STUCCO_SAFFRON", "saffron stucco, the alleys' houses", lambda: plaster(264, (206, 164, 84), var=0.16)),
    ("HALF_TIMBER", "timber-framed limewash", lambda: half_timber(265, (222, 214, 196), (86, 58, 38))),
    ("LIMEWASH", "a whitewashed room", lambda: plaster(266, (226, 222, 210), var=0.10)),
    ("HEX_TERRACOTTA", "hexagonal terracotta tiles", lambda: hex_tiles(267, [(168, 82, 52), (152, 72, 46), (182, 96, 62)], (110, 86, 70))),
    ("ASHLAR_LIME", "dressed limestone", lambda: ashlar(268, [(214, 204, 178), (198, 188, 164), (226, 216, 192)], (150, 140, 120))),
    ("BASIN_STONE", "a fountain basin's wet stone", lambda: ashlar(269, [(134, 140, 136), (118, 126, 124), (146, 150, 144)], (82, 88, 86), courses=2)),
    ("FOUNTAIN_TEAL", "a fountain's rippling water", lambda: fountain_water(270)),
    ("TOWN_WALL", "the town wall's rubble stone", lambda: stone_wall(271, [(132, 122, 104), (116, 108, 94), (146, 134, 114), (104, 98, 88)], (70, 64, 56), courses=4)),
    ("TIMBER_JOISTS", "a beamed ceiling", lambda: planks(272, [(0, (70, 46, 30)), (0.6, (108, 74, 48)), (1, (132, 94, 62))], boards=4, along_x=False, gap=0.8, joints=1)),
    ("SHUTTERS_BLUE", "a shopfront's blue shutters", lambda: shutters(273, (64, 104, 140), (210, 202, 182))),
    ("CLAY_ROOF_DEEP", "deep red barrel roof tiles", lambda: roof_tiles(274, [(150, 62, 40), (132, 54, 36), (166, 76, 48)])),
    # the mall (2026-11-19): pastel terrazzo, neon stripes, loud carpets, chrome, glass block, skylights
    ("TERRAZZO_80S", "cream terrazzo, pastel chips", lambda: terrazzo(281, (226, 220, 206), [(232, 150, 176), (110, 196, 196), (176, 156, 214), (150, 150, 150), (246, 214, 150)])),
    ("TERRAZZO_ROSE", "rose terrazzo, the atrium's floor", lambda: terrazzo(282, (220, 176, 176), [(250, 236, 226), (90, 170, 176), (120, 100, 150), (240, 200, 120)], density=0.28)),
    ("MALL_WALL_NEON", "white tiles, magenta and teal stripes", lambda: neon_tile_wall(283, (238, 238, 232), (190, 190, 186), (236, 64, 158), (40, 190, 190))),
    ("CARPET_MEMPHIS", "a loud squiggle carpet", lambda: memphis_carpet(284, (34, 30, 64), [(250, 80, 170), (60, 220, 220), (250, 220, 70), (170, 110, 250)])),
    ("CARPET_CONFETTI", "a plum confetti carpet", lambda: confetti_carpet(285, (84, 40, 80), [(250, 120, 190), (90, 220, 210), (250, 230, 120), (200, 200, 250)])),
    ("CHECKER_BW", "black and white checker floor", lambda: tiles(286, [(232, 230, 224), (34, 34, 38)], (120, 120, 120), n=8, checker=True, var=0.03, bevel=0.04, gritamt=0.05)),
    ("NEON_GRID", "lilac panels, a pink neon grid", lambda: grid_panel(287, (170, 150, 214), (255, 90, 190), (255, 190, 230))),
    ("CORAL_STUCCO", "coral stucco", lambda: plaster(288, (236, 128, 110), var=0.10)),
    ("GLASS_BLOCK", "teal glass blocks", lambda: glass_block(289, (120, 200, 200), (214, 214, 206))),
    ("SKYLIGHT_GRID", "a skylight's frames and sky", lambda: skylight(290, (236, 236, 236), (140, 196, 236))),
    ("ESCALATOR_STEPS", "an escalator's grooved steps", lambda: escalator_treads(291, (176, 180, 184), (236, 200, 40))),
    ("CHROME_PANEL", "polished chrome", lambda: chrome(292, (176, 184, 196))),
    ("BATH_TILE", "mint and pink washroom tiles", lambda: tiles(293, [(160, 222, 196), (240, 176, 196)], (236, 236, 230), n=8, checker=True, var=0.03, bevel=0.05, gritamt=0.04)),
    ("VCT_SPECKLE", "a back corridor's vinyl tiles", lambda: vct(294, [(206, 196, 176), (196, 186, 168), (214, 206, 190)], (150, 144, 130))),
    ("ACOUSTIC_80S", "a dropped ceiling, lights in it", lambda: acoustic(295, (226, 224, 216), (246, 246, 246))),
    ("PARKING_LOT", "a car park's bays", lambda: parking(296)),
    ("MALL_FACADE", "tan block, pink and teal stripes", lambda: mall_facade(297, (198, 172, 136), (236, 96, 150), (40, 176, 180))),
    ("DEPT_WALLPAPER", "a department store's pinstripe wallpaper, a chrome rail", lambda: dept_wallpaper(298, (236, 226, 236), (186, 150, 214), (52, 164, 170), (204, 210, 220))),
    # the pipeworks (2026-11-19)
    ("PW_GRATE", "steel bar grating, see-through between the bars", lambda: pw_grate(301, (150, 146, 132))),
    ("PW_PIPE", "a big pipe's green paint, flange bands", lambda: pw_pipe(302, (74, 104, 82), (150, 140, 120))),
    ("PW_SLUDGE", "toxic green sludge, foam and bubbles", lambda: pw_sludge(303)),
    ("PW_TREAD", "diamond tread plate", lambda: pw_tread(304, (120, 122, 124))),
    ("PW_HAZARD", "yellow and black chevrons", lambda: pw_hazard(305, (222, 182, 30), (30, 28, 26))),
    ("PW_DECK", "a concrete deck, a yellow safety line", lambda: pw_deck(306, (138, 134, 124), (214, 176, 36))),
    ("PW_BRICK", "sooty works brick", lambda: pw_brick(307)),
    ("PW_TILE", "glazed green tiles, a dark band", lambda: pw_tile(308, (96, 150, 120))),
    ("PW_TRUSS", "dark roof sheeting, a steel truss", lambda: pw_truss(309)),
    ("PW_STAIRS", "steel stair treads, yellow nosings", lambda: pw_stairs(310, (112, 114, 118))),
    ("PW_LADDER", "yellow painted steel", lambda: pw_ladder(311, (214, 172, 34))),
    ("PW_BOILER", "riveted boiler plate, heat-blued", lambda: pw_boiler(312, (104, 92, 82))),
    ("PW_LINO", "speckled grey-blue lino", lambda: pw_lino(313, (118, 128, 140))),
    ("PW_RIVET", "riveted steel wall plates, grimy", lambda: panels(314, (98, 108, 104), n=2, rivets=True, bevel=0.14, grime=0.32, seam=0.5, gritamt=0.12)),

]

def market_awning(seed):
    x, y = coords()
    cloth = np.where(((x // 8) % 2)[..., None], (173, 61, 66), (236, 181, 87))
    return shade(cloth, (0.95 + 0.05 * ((x + y) % 2)) * grit(seed, 0.08))

def acoustic_foam(seed):
    x, y = coords()
    axis = np.where((x // 16 + y // 16) % 2, x, y)
    wedges = 0.65 + 0.40 * (1 - np.abs(axis % 8 - 3.5) / 3.5)
    return shade(np.full((N, N, 3), (64, 71, 86)), wedges * grit(seed, 0.12))

def studio_monitor(seed):
    x, y = coords()
    wall = panels(seed, (50, 60, 73), n=1)
    wall[12:47, 6:58] = (25, 27, 33)
    bars = np.array([(200, 193, 171), (182, 162, 65), (78, 174, 168),
                     (102, 161, 90), (158, 91, 146), (167, 78, 74), (71, 92, 149)])
    wall[16:38, 11:53] = bars[np.arange(42) // 6]
    wall[49:51, 45:50] = (219, 80, 62)
    return shade(wall, 0.96 + 0.04 * (y % 2))

def canal_gate(seed):
    x, y = coords()
    wall = corrugated(seed, (75, 107, 104), ribs=4, rust=0.24)
    wall[:, 9:18] = (197, 187, 153)
    wall[(x >= 10) & (x <= np.where(y % 16 < 2, 17, 14)) & (y % 4 < 2)] = (62, 73, 72)
    return wall

def lantern_stall(seed):
    x, y = coords()
    wall = corrugated(seed, (91, 58, 53), ribs=8, depth=0.17)
    lantern = ((x - 32) / 9) ** 2 + ((y - 26) / 13) ** 2 < 1
    wall[lantern] = (226, 112, 66)
    wall[lantern & (y % 5 == 0)] = (167, 68, 52)
    wall[9:14, 30:34] = wall[39:45, 31:33] = (218, 168, 79)
    return wall

def ridge_stone(seed):
    x, y = coords()
    warp = 5 * np.sin(x * 2 * np.pi / N) + 5 * pnoise(seed, 1.5)
    band = (y + warp) % 16
    return shade(concrete(seed, (120, 122, 127), var=0.26, gritamt=0.24),
                 0.68 + 0.32 * np.minimum(band / 4, 1))

def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    prev = sys.argv[1] if len(sys.argv) > 1 else None
    data_lines = [
        "/* The level generator's own textures: made by tools/levelgen/lgtexgen.py (do",
        " * not edit). Served by modTextureLoad (mod.c) under LGTEX_FIRST + i, in the",
        " * game's own texture format (zlib, RGBA16_CI4, 64x64). */",
        "#ifndef LEVELGENTEX_H",
        "#define LEVELGENTEX_H",
        "",
        "#include \"game/levelgentexnums.h\"",
        "",
    ]
    num_lines = [
        "/* The level generator's own textures by name: made by tools/levelgen/lgtexgen.py",
        " * (do not edit). Their data is in port/include/levelgentex.h. */",
        "#ifndef LEVELGENTEXNUMS_H",
        "#define LEVELGENTEXNUMS_H",
        "",
        "#define LGTEX_FIRST 0x%03x" % FIRST,
        "#define LGTEX_COUNT %d" % len(TEXTURES),
        "",
    ]
    sizes = []
    for i, (name, what, make) in enumerate(TEXTURES):
        idx, pal, img = quantize(make())
        data = encode(idx, pal)
        sizes.append(len(data))
        if prev:
            os.makedirs(prev, exist_ok=True)
            img.save(os.path.join(prev, "%03x.png" % (FIRST + i)))
        num_lines.append("#define LGTEX_%-18s 0x%03x   /* %s */" % (name, FIRST + i, what))
        data_lines.append("/* 0x%03x LGTEX_%s: %s */" % (FIRST + i, name, what))
        data_lines.append("static const unsigned char g_LgTex%d[%d] = {" % (i, len(data)))
        for k in range(0, len(data), 16):
            data_lines.append("\t" + ", ".join("0x%02x" % b for b in data[k:k + 16]) + ",")
        data_lines.append("};")
        data_lines.append("")
    data_lines.append("static const unsigned char *const g_LgTexData[LGTEX_COUNT] = {")
    for i in range(0, len(TEXTURES), 8):
        data_lines.append("\t" + ", ".join("g_LgTex%d" % k for k in range(i, min(i + 8, len(TEXTURES)))) + ",")
    data_lines.append("};")
    data_lines.append("static const unsigned short g_LgTexSize[LGTEX_COUNT] = { %s };" % ", ".join(str(s) for s in sizes))
    data_lines.append("")
    data_lines.append("#endif")
    num_lines += ["", "#endif"]
    open(os.path.join(root, "port", "include", "levelgentex.h"), "w").write("\n".join(data_lines) + "\n")
    open(os.path.join(root, "src", "include", "game", "levelgentexnums.h"), "w").write("\n".join(num_lines) + "\n")
    print("wrote %d textures, %d bytes in all" % (len(TEXTURES), sum(sizes)))

if __name__ == "__main__":
    main()
