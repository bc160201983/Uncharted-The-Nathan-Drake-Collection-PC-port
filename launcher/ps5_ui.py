"""PlayStation 5 style shell for the Uncharted launcher.

Everything here is Tkinter + Pillow, the two libraries the launcher already ships with. The
home screen is drawn on one canvas whose items are only *moved* while animating: no image is
resized or re-composited per frame, which keeps the whole thing smooth on an integrated GPU.
Artwork is decoded and cropped once, then kept in a disk cache next to the launcher's config.
"""
from __future__ import annotations

import os
import time
import tkinter as tk
import tkinter.font as tkfont
from pathlib import Path

from PIL import Image, ImageEnhance, ImageTk

# -- palette ------------------------------------------------------------------------------------
# PS5 system software: near-black ground, white type, one blue accent, translucent glass panels.
BG = "#08090c"
PANEL = "#14161b"
PANEL_HI = "#1e2128"
LINE = "#2c3038"
TEXT = "#f5f6f8"
MUTED = "#a2a8b4"
DIM = "#666d79"
GOLD = "#2f7cf6"          # the accent (the launcher's old gold); blue now
GOLD_HI = "#4f95ff"
ACCENT = GOLD
ACCENT_HI = GOLD_HI
INK = "#0a0c10"
STOP_RED = "#e5484d"
WHITE = "#ffffff"

# -- motion -------------------------------------------------------------------------------------


def ease_out_cubic(t):
    return 1 - (1 - t) ** 3


def ease_in_out(t):
    return 4 * t ** 3 if t < 0.5 else 1 - (-2 * t + 2) ** 3 / 2


class Animator:
    """One Tk timer for every running tween, stopped as soon as nothing is left to animate.

    Each tween gets `ease(0..1)` every `interval` ms and is dropped when it finishes, so an idle
    launcher has no timer running at all."""

    def __init__(self, root, interval=16):
        self.root = root
        self.interval = interval
        self.items = []
        self.job = None
        self.enabled = True

    def add(self, duration, step, done=None, ease=ease_out_cubic):
        if not self.enabled or duration <= 0:
            try:
                step(1.0)
            except tk.TclError:
                return
            if done:
                done()
            return
        self.items.append({"start": time.perf_counter(), "duration": duration / 1000.0,
                           "step": step, "done": done, "ease": ease})
        self.start()

    def start(self):
        if self.job is None and self.items:
            self.job = self.root.after(self.interval, self.tick)

    def tick(self):
        self.job = None
        now = time.perf_counter()
        live = []
        for tween in self.items:
            k = min(1.0, (now - tween["start"]) / tween["duration"])
            try:
                tween["step"](tween["ease"](k))
            except tk.TclError:  # the widget went away mid-tween
                continue
            if k < 1.0:
                live.append(tween)
            elif tween["done"]:
                try:
                    tween["done"]()
                except tk.TclError:
                    pass
        self.items = live
        if live:
            self.job = self.root.after(self.interval, self.tick)

    def cancel(self):
        self.items = []
        if self.job is not None:
            try:
                self.root.after_cancel(self.job)
            except tk.TclError:
                pass
            self.job = None


# -- fonts --------------------------------------------------------------------------------------


def pick_font(families, *candidates):
    """The first of `candidates` the system has (Sony's SST is not on Windows; Segoe is closest)."""
    lowered = {name.lower() for name in families}
    for name in candidates:
        if name.lower() in lowered:
            return name
    return candidates[-1]


class Fonts:
    """The launcher's type scale, in the PS5's voice."""

    def __init__(self, root, scale):
        families = set(tkfont.families(root))
        self.display = pick_font(families, "Segoe UI Variable Display", "Segoe UI Semibold", "Segoe UI")
        self.text = pick_font(families, "Segoe UI Variable Text", "Segoe UI", "Tahoma")
        s = scale
        self.f_hero = (self.display, int(30 * s), "bold")
        self.f_title = (self.display, int(22 * s), "bold")
        self.f_h2 = (self.display, int(17 * s), "bold")
        self.f_body = (self.text, int(10 * s))
        self.f_body_bold = (self.display, int(10 * s), "bold")
        self.f_small = (self.text, int(9 * s))
        self.f_tiny = (self.display, int(8 * s), "bold")
        self.f_caps = (self.display, int(8 * s), "bold")
        self.f_tab = (self.display, int(11 * s), "bold")
        self.f_play = (self.display, int(11 * s), "bold")


# -- artwork ------------------------------------------------------------------------------------


def cache_dir():
    base = os.environ.get("LOCALAPPDATA") or str(Path.home())
    path = Path(base) / "Uncharted Launcher" / "ui-cache"
    try:
        path.mkdir(parents=True, exist_ok=True)
    except OSError:
        return None
    return path


def cover(image, width, height):
    """Scale-and-crop to exactly width x height (the artwork keeps its aspect, nothing stretches)."""
    width, height = max(1, int(width)), max(1, int(height))
    if image.size == (width, height):
        return image
    scale = max(width / image.width, height / image.height)
    size = (max(width, round(image.width * scale)), max(height, round(image.height * scale)))
    image = image.resize(size, Image.LANCZOS)
    left = (image.width - width) // 2
    top = (image.height - height) // 2
    return image.crop((left, top, left + width, top + height))


class Art:
    """Decodes the launcher/assets/ps5 artwork, cropped to the size asked for and cached on disk.

    A missing or unreadable file is never fatal: the caller gets None and draws a plain dark
    panel instead, so a stripped-down install still opens."""

    def __init__(self, folder):
        self.folder = Path(folder)
        self.cache = cache_dir()
        self.sources = {}
        self.memo = {}
        self.prune()

    def _source(self, name):
        if name not in self.sources:
            path = self.folder / name
            try:
                stat = path.stat()
                stamp = f"{stat.st_mtime_ns:x}{stat.st_size:x}"
                image = Image.open(path)
                image.load()
            except (OSError, ValueError):
                image, stamp = None, "0"
            self.sources[name] = (image, stamp)
        return self.sources[name]

    def prune(self):
        """Keeps the cache to what this launcher will ask for: the backgrounds at the sizes it has
        used, with the current artwork. Tiles are resized in memory now, so their files (written by
        an earlier version) go."""
        if not self.cache:
            return
        per_stem = {}
        for path in self.cache.glob("*.png"):
            parts = path.name.rsplit("-", 2)
            if len(parts) != 3:
                continue
            stem, stamp, _ = parts
            keep = stem.startswith("bg-") and self._source(f"{stem}.jpg")[1] == stamp
            if keep:
                per_stem.setdefault(stem, []).append(path)
            else:
                try:
                    path.unlink()
                except OSError:
                    pass
        for paths in per_stem.values():  # a handful of window sizes, newest kept
            paths.sort(key=lambda p: p.stat().st_mtime, reverse=True)
            for path in paths[8:]:
                try:
                    path.unlink()
                except OSError:
                    pass

    def _resized(self, name, size):
        """The source resized to `size`, through ui-cache/<name>-<stamp>-<w>x<h>.png."""
        image, stamp = self._source(name)
        if image is None:
            return None
        width, height = size
        key = (name, width, height, stamp)
        if key in self.memo:
            return self.memo[key]
        result = None
        target = None
        if self.cache:
            target = self.cache / f"{Path(name).stem}-{stamp}-{width}x{height}.png"
            try:
                result = Image.open(target)
                result.load()
                result = result.convert("RGB")
            except (OSError, ValueError):
                result = None
        if result is None:
            result = cover(image.convert("RGB"), width, height)
            if target is not None:
                try:
                    result.save(target, format="PNG", compress_level=1)
                except (OSError, ValueError):
                    pass
        self.memo[key] = result
        return result

    def background(self, key, size):
        return self._resized(f"bg-{key}.jpg", size)

    def tile(self, key, size):
        """A tile is small (a few milliseconds to resize): done in memory for the window's own
        size, so resizing the window does not write a file per size to the cache."""
        name = f"tile-{key}.jpg"
        image, _ = self._source(name)
        if image is None:
            return None
        cache_key = ("tile", name)
        cached = self.memo.get(cache_key)
        if cached is not None and cached.size == (int(size[0]), int(size[1])):
            return cached
        result = cover(image, size[0], size[1])
        self.memo[cache_key] = result
        return result

    def logo(self, key, height):
        """The transparent game logo, scaled to `height` (RGBA, keeps its own aspect)."""
        name = f"logo-{key}.png"
        cache_key = ("logo", name, height)
        if cache_key in self.memo:
            return self.memo[cache_key]
        image, _ = self._source(name)
        if image is None:
            return None
        image = image.convert("RGBA")
        scale = height / image.height
        result = image.resize((max(1, round(image.width * scale)), int(height)), Image.LANCZOS)
        self.memo[cache_key] = result
        return result

    def dim(self, key, size, factor=0.22):
        """A very dark copy of the artwork, for the settings screen's backdrop."""
        image = self.background(key, size)
        if image is None:
            return None
        cache_key = ("dim", key, size, factor)
        if cache_key not in self.memo:
            self.memo[cache_key] = ImageEnhance.Brightness(image).enhance(factor)
        return self.memo[cache_key]


# -- canvas helpers -----------------------------------------------------------------------------


def round_points(x1, y1, x2, y2, r):
    r = max(0, min(r, (x2 - x1) / 2, (y2 - y1) / 2))
    return [x1 + r, y1, x2 - r, y1, x2, y1, x2, y1 + r, x2, y2 - r, x2, y2, x2 - r, y2,
            x1 + r, y2, x1, y2, x1, y2 - r, x1, y1 + r, x1, y1]


def round_rect(canvas, x1, y1, x2, y2, r, **kw):
    return canvas.create_polygon(round_points(x1, y1, x2, y2, r), smooth=True, splinesteps=12, **kw)


def spaced(text):
    """Loose letter-spacing, the way the Collection sets its small caps."""
    return " ".join(text)


# -- the home screen ----------------------------------------------------------------------------


class HomeView(tk.Frame):
    """The game hub: full-bleed art per game, the game's logo, and a carousel of the three."""

    SCALES = (1.00, 1.02, 1.04, 1.06, 1.08, 1.10, 1.12)

    def __init__(self, host, art, animator, parent):
        super().__init__(parent, bg=BG)
        self.host, self.art, self.anim = host, art, animator
        self.canvas = tk.Canvas(self, bg=BG, highlightthickness=0, bd=0, takefocus=0)
        self.canvas.pack(fill="both", expand=True)
        self.games = []
        self.key = None
        self.running = False
        self.status_text = ""
        self.status_detail = ""
        self.size = (0, 0)
        self.photos = {}
        self.tiles = {}
        self.geom = {}
        self.metrics = None
        self.bg_key = None
        self.hover = None
        self._pending = None
        self.canvas.bind("<Configure>", self._on_configure)
        self.canvas.bind("<Button-1>", self._on_click)
        self.canvas.bind("<Motion>", self._on_motion)
        self.canvas.bind("<Leave>", lambda e: self._set_hover(None))

    # -- public ---------------------------------------------------------------------------------

    def set_games(self, games):
        self.games = list(games)
        self.photos.clear()
        self.tiles.clear()
        self.geom.clear()
        self.bg_key = None
        self.key = self.key or self.games[0]["key"]
        if self.size != (0, 0):
            self._build()

    def select(self, key, animate=True):
        keys = [g["key"] for g in self.games]
        if key not in keys:
            return
        if key == self.key and self.geom:
            return
        self.key = key
        if self.size == (0, 0) or not self.tiles:
            return
        self._draw_selection(animate=animate and bool(self.geom))

    def set_running(self, running):
        if running != self.running:
            self.running = running
            self._draw_pills()

    def set_status(self, text, detail=""):
        self.status_text, self.status_detail = text, detail
        self._draw_status()

    # -- geometry -------------------------------------------------------------------------------

    def _on_configure(self, event):
        if (event.width, event.height) == self.size or event.width < 80 or event.height < 80:
            return
        self.size = (event.width, event.height)
        if self._pending is not None:
            try:
                self.after_cancel(self._pending)
            except tk.TclError:
                pass
        self._pending = self.after_idle(self._build)

    def _metrics(self):
        width, height = self.size
        s = self.host.s
        base = max(int(190 * s), min(int(width * 0.142), int(320 * s)))
        base -= base % 2
        gap = max(int(14 * s), int(width * 0.011))
        sel_w = base * self.SCALES[-1]
        row_w = sel_w + 2 * base + 2 * gap
        return {"base": base, "gap": gap, "sel_w": sel_w, "row_w": row_w,
                "left": (width - row_w) / 2,
                "bottom": height - max(int(20 * s), int(height * 0.032)),
                "margin": max(int(38 * s), int(width * 0.055))}

    def _slot(self, metrics, index, selected_index):
        """The left edge and width of tile `index` while `selected_index` is enlarged."""
        base, gap = metrics["base"], metrics["gap"]
        x = metrics["left"]
        for i in range(len(self.games)):
            width = metrics["sel_w"] if i == selected_index else base
            if i == index:
                return x, width
            x += width + gap
        return x, base

    # -- drawing --------------------------------------------------------------------------------

    def _build(self):
        self._pending = None
        if self.size == (0, 0) or not self.games:
            return
        canvas = self.canvas
        canvas.delete("all")
        self.tiles.clear()
        self.geom.clear()
        self.photos.clear()
        width, height = self.size
        s = self.host.s
        metrics = self._metrics()
        self.metrics = metrics

        self.bg_items = [canvas.create_image(0, 0, anchor="nw", tags="bg"),
                         canvas.create_image(width, 0, anchor="nw", tags="bg")]
        self.bg_index = 0
        self.bg_key = None
        self._set_background(self.key or self.games[0]["key"], animate=False)

        self.logo_height = max(int(70 * s), min(int(height * 0.125), int(146 * s)))
        self.logo_item = canvas.create_image(0, 0, anchor="sw")
        self.eyebrow_item = canvas.create_text(0, 0, anchor="w", font=self.host.fonts.f_caps, fill=MUTED,
                                               text=spaced("THE NATHAN DRAKE COLLECTION"))
        self.subtitle_item = canvas.create_text(0, 0, anchor="w", font=self.host.fonts.f_small, fill=MUTED,
                                                text="")

        self.pills = {}
        for tag in ("play", "settings", "quit"):
            body = round_rect(canvas, 0, 0, 0, 0, 12 * s, fill=WHITE, outline=WHITE)
            label = canvas.create_text(0, 0, text="", font=self.host.fonts.f_body_bold, fill=INK)
            for item in (body, label):
                canvas.tag_bind(item, "<Button-1>", lambda e, t=tag: self._pill_click(t))
            self.pills[tag] = {"body": body, "label": label, "box": (0, 0, 0, 0)}

        for game in self.games:
            image = canvas.create_image(0, 0, anchor="nw")
            ring = round_rect(canvas, 0, 0, 0, 0, 10 * s, fill="", outline=LINE, width=1)
            number = canvas.create_text(0, 0, anchor="sw", font=self.host.fonts.f_h2, fill=WHITE)
            name = canvas.create_text(0, 0, anchor="sw", font=self.host.fonts.f_tiny, fill=MUTED)
            for item in (image, ring, number, name):
                canvas.tag_bind(item, "<Button-1>", lambda e, k=game["key"]: self.host.select_game(k))
                canvas.tag_bind(item, "<Double-Button-1>",
                                lambda e, k=game["key"]: self.host.play_game(k))
            self.tiles[game["key"]] = {"image": image, "ring": ring, "number": number, "name": name}

        self.status_shadow = canvas.create_text(width - metrics["margin"] + 2 * s, metrics["margin"] + 2 * s,
                                                anchor="ne", font=self.host.fonts.f_small, fill="#000000",
                                                text="", justify="right")
        self.status_item = canvas.create_text(width - metrics["margin"], metrics["margin"], anchor="ne",
                                              font=self.host.fonts.f_small, fill=WHITE, text="", justify="right")
        canvas.tag_lower("bg")
        self._layout(metrics)
        self._draw_selection(animate=False)

    def _layout(self, metrics):
        s = self.host.s
        margin = metrics["margin"]
        logo_h = self.logo_height
        block = self.size[1] * 0.42
        self.eyebrow_pos = (margin, block - logo_h - int(24 * s))
        self.logo_pos = (margin, block)
        self.subtitle_pos = (margin, block + int(20 * s))
        self.canvas.coords(self.eyebrow_item, *self.eyebrow_pos)
        self.canvas.coords(self.subtitle_item, *self.subtitle_pos)
        self.canvas.coords(self.status_item, self.size[0] - margin, margin)
        self.canvas.coords(self.status_shadow, self.size[0] - margin + 2 * s, margin + 2 * s)
        self._layout_pills(block + int(54 * s))

    def _layout_pills(self, top):
        s = self.host.s
        height = int(42 * s)
        x = self.metrics["margin"]
        for tag in ("play", "settings", "quit"):
            pill = self.pills[tag]
            text = {"play": "Play", "settings": "Settings", "quit": "Quit"}[tag]
            self.canvas.itemconfig(pill["label"], text=text)
            x1, _, x2, _ = self.canvas.bbox(pill["label"])
            width = (x2 - x1) + int(46 * s)
            pill["box"] = (x, top, width, height)
            self.canvas.coords(pill["label"], x + width / 2, top + height / 2)
            self.canvas.coords(pill["body"], *round_points(x, top, x + width, top + height, 12 * s))
            x += width + int(12 * s)

    def _set_background(self, key, animate=True):
        width, _ = self.size
        image = self.art.background(key, self.size)
        photo = ImageTk.PhotoImage(image if image is not None else Image.new("RGB", self.size, BG))
        self.photos[f"bg-{key}"] = photo
        if animate and self.bg_key is not None:
            outgoing = self.bg_items[self.bg_index]
            incoming = self.bg_items[1 - self.bg_index]
            self.canvas.itemconfig(incoming, image=photo)
            self.canvas.coords(outgoing, 0, 0)
            self.canvas.coords(incoming, width, 0)
            self.canvas.tag_lower("bg")

            def step(k):
                self.canvas.coords(outgoing, -width * k, 0)
                self.canvas.coords(incoming, width - width * k, 0)

            def done():
                self.canvas.coords(incoming, 0, 0)
                self.canvas.coords(outgoing, width, 0)
                self.canvas.itemconfig(outgoing, image="")
                self.bg_index = 1 - self.bg_index

            self.anim.add(340, step, done, ease_in_out)
        else:
            current = self.bg_items[self.bg_index]
            other = self.bg_items[1 - self.bg_index]
            self.canvas.itemconfig(current, image=photo)
            self.canvas.coords(current, 0, 0)
            self.canvas.coords(other, width, 0)
        self.bg_key = key

    def _draw_selection(self, animate=True):
        if self.key not in self.tiles:
            return
        s = self.host.s
        metrics = self.metrics
        keys = [g["key"] for g in self.games]
        index = keys.index(self.key)
        game = self.games[index]

        logo = self.art.logo(self.key, self.logo_height)
        if logo is not None:
            photo = ImageTk.PhotoImage(logo)
            self.photos["logo"] = photo
            self.canvas.itemconfig(self.logo_item, image=photo)
        self.canvas.coords(self.logo_item, *self.logo_pos)
        self.canvas.itemconfig(self.subtitle_item, text=f"{game['name']}  ·  {game.get('year', '')}".strip(" ·"))

        if animate:
            for item, (x, y) in ((self.logo_item, self.logo_pos), (self.subtitle_item, self.subtitle_pos)):
                self.anim.add(280, lambda k, i=item, px=x, py=y: self.canvas.coords(i, px, py + (1 - k) * 18 * s))
            self._set_background(self.key, animate=True)

        old = dict(self.geom)
        self.geom = {}
        for i, key in enumerate(keys):
            x, width = self._slot(metrics, i, index)
            self.geom[key] = (x, width)
        if animate and old:
            self._animate_tiles(old)
        else:
            for key, (x, width) in self.geom.items():
                self._apply_tile(key, x, width)

        self._draw_pills()
        self._draw_status()

    def _animate_tiles(self, old):
        s = self.host.s

        def step(k):
            for key, (nx, nw) in self.geom.items():
                ox, ow = old.get(key, (nx, nw))
                self._apply_tile(key, ox + (nx - ox) * k, ow + (nw - ow) * k)

        self.anim.add(300, step)

    def _tile_photo(self, key, width):
        base = self.metrics["base"]
        best = min(range(len(self.SCALES)), key=lambda i: abs(base * self.SCALES[i] - width))
        scale = self.SCALES[best]
        pixel_w = int(base * scale) - int(base * scale) % 2
        pixel_h = int(pixel_w * 9 / 16)
        cache_key = (key, pixel_w)
        if cache_key not in self.photos:
            image = self.art.tile(key, (pixel_w, pixel_h))
            if image is None:
                return None
            self.photos[cache_key] = ImageTk.PhotoImage(image)
        return self.photos[cache_key]

    def _apply_tile(self, key, x, width):
        s = self.host.s
        items = self.tiles[key]
        height = width * 9 / 16
        y = self.metrics["bottom"] - height
        photo = self._tile_photo(key, width)
        if photo is not None:
            self.canvas.itemconfig(items["image"], image=photo)
        self.canvas.coords(items["image"], x, y)
        self.canvas.coords(items["ring"], *round_points(x, y, x + width, y + height, 10 * s))
        chosen = key == self.key
        hovered = key == self.hover
        self.canvas.itemconfig(items["ring"], outline=WHITE if chosen else (GOLD_HI if hovered else LINE),
                               width=3 if chosen else 1)
        game = next(g for g in self.games if g["key"] == key)
        self.canvas.itemconfig(items["number"], text=game["number"], fill=WHITE if chosen else DIM,
                               font=self.host.fonts.f_title if chosen else self.host.fonts.f_h2)
        self.canvas.coords(items["number"], x + 12 * s, y - 8 * s)
        self.canvas.itemconfig(items["name"], text=game["title"], fill=TEXT if chosen else DIM,
                               state="normal" if chosen else "hidden")
        offset = int((30 if chosen else 24) * s)
        self.canvas.coords(items["name"], x + 12 * s + offset, y - 8 * s)

    def _draw_pills(self):
        s = self.host.s
        for tag, pill in self.pills.items():
            if tag == "play":
                text = "Stop" if self.running else "Play"
                fill = STOP_RED if self.running else WHITE
                ink = WHITE if self.running else INK
            elif tag == "quit":
                text, fill, ink = "Quit", PANEL_HI, TEXT
            else:
                text, fill, ink = "Settings", PANEL_HI, TEXT
            self.canvas.itemconfig(pill["label"], text=text, fill=ink)
            x, y, w, h = pill["box"]
            self.canvas.coords(pill["body"], *round_points(x, y, x + w, y + h, 12 * s))
            self.canvas.itemconfig(pill["body"], fill=fill, outline=fill)

    def _draw_status(self):
        if not hasattr(self, "status_item"):
            return
        text = self.status_text
        if self.status_detail:
            text = f"{text}\n{self.status_detail}"
        self.canvas.itemconfig(self.status_item, text=text)
        self.canvas.itemconfig(self.status_shadow, text=text)

    # -- interaction ----------------------------------------------------------------------------

    def _pill_click(self, tag):
        if tag == "play":
            self.host.play_or_stop()
        elif tag == "quit":
            self.host.close()
        else:
            self.host.open_settings()

    def _pill_at(self, x, y):
        for tag, pill in self.pills.items():
            px, py, pw, ph = pill["box"]
            if px <= x <= px + pw and py <= y <= py + ph:
                return tag
        return None

    def _tile_at(self, x, y):
        keys = [g["key"] for g in self.games]
        index = keys.index(self.key) if self.key in keys else 0
        for i, key in enumerate(keys):
            tx, tw = self._slot(self.metrics, i, index)
            th = tw * 9 / 16
            ty = self.metrics["bottom"] - th
            if tx <= x <= tx + tw and ty <= y <= ty + th:
                return key
        return None

    def _on_click(self, event):
        key = self._tile_at(event.x, event.y)
        if key:
            self.host.select_game(key)

    def _on_motion(self, event):
        self._set_hover(self._tile_at(event.x, event.y))

    def _set_hover(self, key):
        if key == self.hover:
            return
        self.hover = key
        if not self.geom:
            return
        for tile_key, (x, width) in self.geom.items():
            if tile_key != self.key:
                self._apply_tile(tile_key, x, width)


# -- hint bar -----------------------------------------------------------------------------------


class HintBar(tk.Canvas):
    """The controller legend along the bottom: icon + label pairs, and the L3+R3 hold ring."""

    def __init__(self, host, height):
        self.host = host
        self.scale = host.s
        super().__init__(host.root, height=height, bg=BG, highlightthickness=0, bd=0, takefocus=0)
        self.items = []
        self.photos = {}
        self.progress = None
        self.bind("<Configure>", lambda e: self.draw())

    def set_items(self, items):
        self.items = list(items)
        self.draw()

    def set_progress(self, fraction):
        if fraction is None and self.progress is None:
            return
        self.progress = fraction
        self.draw()

    def icon(self, button):
        height = int(19 * self.scale)
        style = self.host.pad_style()
        key = (button, height, style)
        if key not in self.photos:
            try:
                self.photos[key] = ImageTk.PhotoImage(self.host.pad_icon(button, style, height))
            except Exception:
                return None
        return self.photos[key]

    def draw(self):
        self.delete("all")
        s = self.scale
        width, height = self.winfo_width(), self.winfo_height()
        if width <= 1:
            return
        x, mid = int(34 * s), height / 2
        for button, label in self.items:
            photo = self.icon(button)
            if photo is not None:
                self.create_image(x, mid, image=photo, anchor="w")
                x += photo.width() + int(8 * s)
            else:
                item = self.create_text(x, mid, text=button.upper(), font=self.host.fonts.f_tiny, fill=MUTED,
                                        anchor="w")
                x = self.bbox(item)[2] + int(8 * s)
            item = self.create_text(x, mid, text=label, font=self.host.fonts.f_small, fill=MUTED, anchor="w")
            x = self.bbox(item)[2] + int(26 * s)
        if self.progress is not None:
            self._draw_ring(width - int(40 * s), mid, self.progress)

    def _draw_ring(self, cx, cy, fraction):
        s = self.scale
        r = int(11 * s)
        width = max(2, int(2 * s))
        self.create_oval(cx - r, cy - r, cx + r, cy + r, outline=LINE, width=width)
        if fraction > 0:
            self.create_arc(cx - r, cy - r, cx + r, cy + r, start=90, extent=-359.9 * fraction,
                            style="arc", outline=WHITE, width=width)


# -- settings sidebar ---------------------------------------------------------------------------


class Sidebar(tk.Canvas):
    """The category list: a white pill marks the page in view, PS5 style."""

    def __init__(self, host, parent, width, items, command):
        self.host = host
        self.scale = host.s
        self.items = list(items)
        self.command = command
        self.current = None
        self.hover = None
        self.rows = []
        super().__init__(parent, width=width, bg=BG, highlightthickness=0, bd=0, takefocus=0)
        self.bind("<Configure>", lambda e: self.draw())
        self.bind("<Motion>", self._motion)
        self.bind("<Leave>", lambda e: self._hover(None))
        self.bind("<Button-1>", self._click)

    def select(self, name):
        if name != self.current:
            self.current = name
            self.draw()

    def draw(self):
        self.delete("all")
        self.rows = []
        s = self.scale
        width, height = self.winfo_width(), self.winfo_height()
        if width <= 1:
            return
        self.create_text(int(32 * s), int(78 * s), text="Settings", font=self.host.fonts.f_hero,
                         fill=TEXT, anchor="w")
        top = int(150 * s)
        row_h = int(46 * s)
        for index, (name, label) in enumerate(self.items):
            y = top + index * row_h
            if y + row_h > height:
                break
            selected = name == self.current
            hovered = name == self.hover and not selected
            body = round_rect(self, int(22 * s), y + int(4 * s), width - int(20 * s), y + row_h - int(4 * s),
                              int(10 * s), fill=WHITE if selected else (PANEL_HI if hovered else BG),
                              outline=WHITE if selected else BG)
            text = self.create_text(int(38 * s), y + row_h / 2, text=label, anchor="w",
                                    font=self.host.fonts.f_tab if selected else self.host.fonts.f_body,
                                    fill=INK if selected else (TEXT if hovered else MUTED))
            self.rows.append((name, y, row_h))
            for item in (body, text):
                self.tag_bind(item, "<Button-1>", lambda e, n=name: self.command(n))

    def _row_at(self, y):
        for name, top, height in self.rows:
            if top <= y <= top + height:
                return name
        return None

    def _motion(self, event):
        self._hover(self._row_at(event.y))

    def _hover(self, name):
        if name != self.hover:
            self.hover = name
            self.draw()

    def _click(self, event):
        name = self._row_at(event.y)
        if name:
            self.command(name)


# -- controller navigation ----------------------------------------------------------------------


class FocusNav:
    """D-pad / stick navigation over a page's rows.

    A focusable is any widget carrying `_activate` (called by Cross) and optionally `_adjust(delta)`
    (Left/Right) or `_grid = (row, column)` (2D movement). Rows built by Launcher.row get one
    automatically; the preset cards and the Controls rows register themselves."""

    def __init__(self, host):
        self.host = host
        self.targets = []
        self.index = -1

    def bind(self, targets):
        self.clear()
        self.targets = [w for w in targets if w.winfo_exists()]
        if self.targets:
            # The page opens at its top: focusing its first row must not scroll it.
            self.focus(0, scroll=False)

    def clear(self):
        for widget in self.targets:
            self._highlight(widget, False)
        self.targets = []
        self.index = -1

    def focus(self, index, scroll=True):
        if not self.targets:
            return
        index = max(0, min(len(self.targets) - 1, index))
        if index == self.index:
            return
        if 0 <= self.index < len(self.targets):
            self._highlight(self.targets[self.index], False)
        self.index = index
        widget = self.targets[index]
        self._highlight(widget, True)
        if scroll:
            self.host.ensure_visible(widget)

    def _highlight(self, widget, on):
        setter = getattr(widget, "_focus_set", None)
        if setter:
            try:
                setter(on)
            except tk.TclError:
                pass
            return
        try:
            widget.config(highlightbackground=WHITE if on else BG, highlightcolor=WHITE if on else BG)
        except tk.TclError:
            pass

    def move(self, dx, dy):
        if not self.targets:
            return False
        widget = self.targets[self.index]
        adjust = getattr(widget, "_adjust", None)
        if dx and adjust is not None:
            adjust(dx)
            return True
        if getattr(widget, "_grid", None) is not None:
            return self._move_grid(dx, dy)
        if dy:
            self.focus(self.index + dy)
            return True
        return False

    def _move_grid(self, dx, dy):
        row, col = self.targets[self.index]._grid
        for i, widget in enumerate(self.targets):
            r, c = widget._grid
            if dx and r == row and c == col + dx:
                self.focus(i)
                return True
            if dy and c == col and r == row + dy:
                self.focus(i)
                return True
        if dx or dy:
            # Nothing that way: wrap along the line.
            line = [i for i, w in enumerate(self.targets)
                    if (w._grid[0] == row if dx else w._grid[1] == col)]
            if line:
                self.focus(line[0] if (dx or dy) > 0 else line[-1])
                return True
        return False

    def activate(self):
        if 0 <= self.index < len(self.targets):
            self.targets[self.index]._activate()
            return True
        return False


# -- L3 + R3 ------------------------------------------------------------------------------------


class StickWatcher:
    """L3 + R3 held together closes the running game. Armed only while a game runs and for a few
    seconds after it starts, so a controller resting on a desk cannot close anything."""

    ARM_DELAY = 5.0

    def __init__(self, host):
        self.host = host
        self.since = None
        self.fired = False

    def reset(self):
        if self.since is not None:
            self.since = None
            self.host.hintbar.set_progress(None)
        self.fired = False

    def update(self, held, now):
        host = self.host
        if not host.config.get("pad_quit", True) or not host.process:
            self.reset()
            return
        if now - getattr(host, "started_at", 0) < self.ARM_DELAY:
            self.reset()
            return
        if "leftstick" not in held or "rightstick" not in held:
            self.reset()
            return
        hold = float(host.config.get("pad_quit_hold", "2.0") or 2.0)
        if self.since is None:
            self.since = now
            return
        if self.fired:
            return
        elapsed = now - self.since
        host.hintbar.set_progress(min(1.0, elapsed / hold))
        if elapsed >= hold:
            self.fired = True
            host.hintbar.set_progress(None)
            host.stop_from_pad()
