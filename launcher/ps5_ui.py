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

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageTk

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
    launcher has no timer running at all. A tween that is expensive per frame (the full-screen
    background blend) can ask for a slower `interval` of its own: the cheap tweens beside it keep
    the 16 ms cadence and only that one is sampled less often."""

    def __init__(self, root, interval=16):
        self.root = root
        self.interval = interval
        self.items = []
        self.job = None
        self.enabled = True

    def add(self, duration, step, done=None, ease=ease_out_cubic, interval=0):
        if not self.enabled or duration <= 0:
            try:
                step(1.0)
            except tk.TclError:
                return
            if done:
                done()
            return
        self.items.append({"start": time.perf_counter(), "duration": duration / 1000.0,
                           "step": step, "done": done, "ease": ease,
                           "interval": interval / 1000.0, "next": 0.0})
        self.start()

    def start(self):
        if self.job is None and self.items:
            self.job = self.root.after(self.interval, self.tick)

    def tick(self):
        self.job = None
        now = time.perf_counter()
        live = []
        for tween in self.items:
            if now < tween["next"]:
                live.append(tween)
                continue
            tween["next"] = now + tween["interval"]
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


# -- liquid glass -------------------------------------------------------------------------------


def rounded_image(image, radius, rim=None, rim_alpha=64):
    """`image` with rounded corners, as RGBA: a soft-edged mask and an optional 1 px rim."""
    image = image.convert("RGBA")
    width, height = image.size
    mask = Image.new("L", (width * 2, height * 2), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, width * 2 - 1, height * 2 - 1),
                                           radius=max(0, int(radius * 2)), fill=255)
    result = image.copy()
    result.putalpha(mask.resize((width, height), Image.LANCZOS))
    if rim is not None:
        line = Image.new("RGBA", (width * 2, height * 2), (0, 0, 0, 0))
        ImageDraw.Draw(line).rounded_rectangle((0, 0, width * 2 - 1, height * 2 - 1),
                                               radius=max(0, int(radius * 2)),
                                               outline=rim + (rim_alpha,), width=2)
        result.alpha_composite(line.resize((width, height), Image.LANCZOS))
    return result


class Glass:
    """Liquid glass, cheaply.

    One blurred backdrop per (art, size, brightness); each panel is then a cached crop of it with
    a tint, a 1 px light rim and a rounded mask. Nothing is blurred while the window is idle: a
    resize rebuilds a handful of small images once, and the panels are reused from then on. With
    animations off the blur is skipped too (flat glass), so a weak integrated GPU can opt out."""

    def __init__(self, art):
        self.art = art
        self.enabled = True
        self.backdrops = {}
        self.surfaces = {}

    def backdrop(self, key, size, dim=1.0):
        if key is None:
            return Image.new("RGB", (max(1, size[0]), max(1, size[1])), BG)
        cache_key = (key, tuple(size), dim)
        if cache_key in self.backdrops:
            return self.backdrops[cache_key]
        image = self.art.dim(key, size, dim) if dim < 1 else self.art.background(key, size)
        if image is None:
            image = Image.new("RGB", (max(1, size[0]), max(1, size[1])), BG)
        if self.enabled:
            small = (max(1, size[0] // 6), max(1, size[1] // 6))
            image = (image.resize(small, Image.BILINEAR)
                     .filter(ImageFilter.GaussianBlur(4)).resize(tuple(size), Image.BILINEAR))
        if len(self.backdrops) > 4:
            self.backdrops.clear()
        self.backdrops[cache_key] = image
        return image

    def surface(self, key, size, box, radius, dim=1.0, tint=(9, 11, 16, 168), rim=64):
        """A rounded glass panel over the artwork at `box` (x1, y1, x2, y2)."""
        x1, y1, x2, y2 = (int(round(value)) for value in box)
        width, height = max(1, x2 - x1), max(1, y2 - y1)
        cache_key = (key, tuple(size), (x1, y1, x2, y2), int(radius), dim, tint, rim)
        if cache_key in self.surfaces:
            return self.surfaces[cache_key]
        scale = 2  # drawn twice the size, so the rounded edge and the rim stay smooth
        if key is None or not self.enabled:
            base = Image.new("RGBA", (width * scale, height * scale), tint)
        else:
            back = self.backdrop(key, size, dim)
            crop = back.crop((max(0, x1), max(0, y1), min(back.width, x2), min(back.height, y2)))
            base = crop.convert("RGBA").resize((width * scale, height * scale), Image.BILINEAR)
            base = Image.alpha_composite(base, Image.new("RGBA", base.size, tint))
        highlight = Image.new("RGBA", base.size, (0, 0, 0, 0))
        draw = ImageDraw.Draw(highlight)
        span = max(1, int(height * scale * 0.45))
        for y in range(span):
            draw.line((0, y, base.width, y), fill=(255, 255, 255, int(22 * (1 - y / span))))
        base = Image.alpha_composite(base, highlight)
        mask = Image.new("L", base.size, 0)
        ImageDraw.Draw(mask).rounded_rectangle((0, 0, base.width - 1, base.height - 1),
                                               radius=max(0, int(radius * scale)), fill=255)
        base.putalpha(mask)
        line = Image.new("RGBA", base.size, (0, 0, 0, 0))
        ImageDraw.Draw(line).rounded_rectangle((0, 0, base.width - 1, base.height - 1),
                                               radius=max(0, int(radius * scale)),
                                               outline=(255, 255, 255, rim), width=2 * scale)
        base = Image.alpha_composite(base, line)
        result = base.resize((width, height), Image.LANCZOS)
        if len(self.surfaces) > 96:
            self.surfaces.clear()
        self.surfaces[cache_key] = result
        return result


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


# -- the floating top strip ---------------------------------------------------------------------


class TopBar(tk.Canvas):
    """The floating strip over the artwork: the wordmark, the three capsules and the two buttons.

    It draws its own strip of the same background the screen below uses, so it reads as glass over
    the art rather than a bar on top of it. Everything is painted once per change: no timer runs
    while the strip is at rest."""

    TABS = (("games", "Games"), ("gallery", "Gallery"), ("display", "Display"))
    STOPS = ("games", "gallery", "display", "settings", "power")

    def __init__(self, host, parent, glass):
        self.host, self.glass, self.parent = host, glass, parent
        self.scale = host.s
        self.height_px = int(84 * host.s)
        super().__init__(parent, height=self.height_px, bg=BG, highlightthickness=0, bd=0, takefocus=0)
        self.tab = "games"
        self.key = None
        self.dim = 1.0
        self.focus = None
        self.size = (0, 0)
        self.photos = {}
        self.caps = {}
        self.buttons = {}
        self.base_item = None
        self.frame = None
        self._scrim = None
        self._scrim_size = (0, 0)
        self.bind("<Configure>", self._on_configure)
        self.bind("<Button-1>", self._click)
        self.bind("<Motion>", self._motion)
        self.bind("<Leave>", lambda event: self.config(cursor=""))

    # -- state ----------------------------------------------------------------------------------

    def set_art(self, key, dim=1.0, frame=None):
        """`frame` is a whole screen of art the caller is already showing: the strip then paints
        its own slice of that frame instead of the artwork, so a background fade carries the strip
        with it and no seam shows at the strip's edge."""
        if (key, dim) == (self.key, self.dim) and frame is None:
            return
        self.key, self.dim = key, dim
        if frame is not None:
            self.set_frame(frame)
        else:
            self.frame = None
            self.draw()

    def set_frame(self, image):
        """Repaints only the strip's background from `image`; the capsules and buttons stay put."""
        self.frame = image
        if self.base_item is None:
            return
        photo = ImageTk.PhotoImage(self._strip(image))
        self.photos["base"] = photo
        self.itemconfig(self.base_item, image=photo)

    def set_tab(self, name):
        if name != self.tab:
            self.tab = name
            self.draw()

    def set_focus(self, name):
        if name != self.focus:
            self.focus = name
            self.draw()

    def move_focus(self, dx):
        stops = list(self.STOPS)
        current = self.focus if self.focus in stops else self.tab if self.tab in stops else "games"
        self.set_focus(stops[(stops.index(current) + dx) % len(stops)])

    def activate(self):
        name = self.focus or self.tab
        self.set_focus(None)
        if name == "settings":
            self.host.open_settings()
        elif name == "power":
            self.host.ask_quit()
        else:
            self.host.show_hub_tab(name)

    # -- drawing --------------------------------------------------------------------------------

    def _on_configure(self, event):
        if event.width < 40 or event.height < 10:
            return
        self.size = (event.width, event.height)
        self.after_idle(self.draw)

    def _art(self, width, height):
        """The artwork for the strip's slice, at the screen's size so the pixels line up with the
        screen behind it."""
        key = self.key
        if key is None:
            return None
        full = (width, max(height, self.parent.winfo_height()))
        if self.frame is not None and self.frame.size == full:
            return self.frame
        return self.glass.art.dim(key, full, self.dim) if self.dim < 1 \
            else self.glass.art.background(key, full)

    def _strip(self, image):
        """`image` cropped to the strip, under a dark gradient for legibility."""
        width, height = self.winfo_width(), self.winfo_height()
        if width <= 1 or height <= 1:
            return None
        if image is None:
            image = Image.new("RGB", (width, height), BG)
        else:
            image = image.crop((0, 0, min(width, image.width), min(height, image.height)))
            if image.size != (width, height):
                padded = Image.new("RGB", (width, height), BG)
                padded.paste(image, (0, 0))
                image = padded
        scrim = self._scrim_for(width, height)
        return Image.alpha_composite(image.convert("RGBA"), scrim).convert("RGB")

    def _scrim_for(self, width, height):
        """The gradient is the same for every frame of a fade: built once per strip size."""
        if self._scrim is None or self._scrim_size != (width, height):
            scrim = Image.new("RGBA", (width, height), (0, 0, 0, 0))
            draw = ImageDraw.Draw(scrim)
            for y in range(height):
                alpha = int(148 * (1 - y / max(1, height - 1)) ** 1.4)
                draw.line((0, y, width, y), fill=(0, 0, 0, alpha))
            self._scrim, self._scrim_size = scrim, (width, height)
        return self._scrim

    def _image(self, surface, box, tag):
        photo = ImageTk.PhotoImage(surface)
        self.photos[tag] = photo
        self.create_image(box[0], box[1], image=photo, anchor="nw")

    def draw(self):
        self.delete("all")
        self.photos = {}
        self.caps, self.buttons = {}, {}
        self.base_item = None
        width, height = self.winfo_width(), self.winfo_height()
        if width <= 1 or height <= 1:
            return
        s = self.scale
        full = (width, max(height, self.parent.winfo_height()))
        photo = ImageTk.PhotoImage(self._strip(self._art(width, height)))
        self.photos["base"] = photo
        self.base_item = self.create_image(0, 0, image=photo, anchor="nw")

        pad = int(34 * s)
        word = self.create_text(pad, int(29 * s), text=spaced("UNCHARTED"), anchor="w",
                                font=self.host.fonts.f_h2, fill=WHITE)
        word_sub = self.create_text(pad, int(50 * s), text=spaced("THE NATHAN DRAKE COLLECTION"),
                                    anchor="w", font=self.host.fonts.f_caps, fill=MUTED)

        # The capsules sit in the room between the wordmark and the two buttons.
        font = self.host.fonts.f_body_bold
        gap = int(10 * s)
        cap_h = int(38 * s)
        size = int(38 * s)
        sized = []
        total = 0
        for name, label in self.TABS:
            item = self.create_text(0, 0, text=label, font=font, anchor="w")
            box = self.bbox(item)
            self.delete(item)
            cap_w = box[2] - box[0] + int(38 * s)
            sized.append((name, label, cap_w))
            total += cap_w
        total += gap * (len(sized) - 1)
        word_right = max(self.bbox(word)[2], self.bbox(word_sub)[2])
        space_left = word_right + int(30 * s)
        space_right = width - pad - (2 * size + int(10 * s)) - int(30 * s)
        x = space_left + (space_right - space_left - total) / 2
        x = max(space_left, x)
        y = (height - cap_h) / 2
        for name, label, cap_w in sized:
            box = (x, y, x + cap_w, y + cap_h)
            selected = self.tab == name
            focused = self.focus == name
            tint = (255, 255, 255, 46) if selected else (9, 11, 16, 156)
            rim = 235 if focused else (176 if selected else 54)
            self._image(self.glass.surface(self.key, full, box, cap_h / 2, self.dim, tint, rim),
                        box, f"cap-{name}")
            self.create_text(x + cap_w / 2, y + cap_h / 2, text=label, font=font,
                             fill=INK if selected else TEXT)
            self.caps[name] = box
            x += cap_w + gap

        bx = width - pad - size
        by = (height - size) / 2
        for name in ("power", "settings"):
            box = (bx, by, bx + size, by + size)
            focused = self.focus == name
            self._image(self.glass.surface(self.key, full, box, size / 2, self.dim,
                                           (9, 11, 16, 168), 235 if focused else 62),
                        box, f"btn-{name}")
            cx, cy = bx + size / 2, by + size / 2
            if name == "power":
                r = size * 0.21
                self.create_arc(cx - r, cy - r, cx + r, cy + r, start=42, extent=276, style="arc",
                                outline="#ffd7d7", width=max(2, int(2 * s)))
                self.create_line(cx, cy - r - int(3 * s), cx, cy, fill="#ffd7d7",
                                 width=max(2, int(2 * s)))
            else:
                self.create_text(cx, cy + 1, text="⚙", font=(self.host.fonts.text, int(17 * s)),
                                 fill=TEXT)
            self.buttons[name] = box
            bx -= size + int(10 * s)

    # -- interaction ----------------------------------------------------------------------------

    def _hit(self, x, y):
        for name, (x1, y1, x2, y2) in list(self.caps.items()) + list(self.buttons.items()):
            if x1 <= x <= x2 and y1 <= y <= y2:
                return name
        return None

    def _motion(self, event):
        self.config(cursor="hand2" if self._hit(event.x, event.y) else "")

    def _click(self, event):
        name = self._hit(event.x, event.y)
        if name is None:
            return
        if name in self.caps:
            self.host.show_hub_tab(name)
        elif name == "settings":
            self.host.open_settings()
        else:
            self.host.ask_quit()


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
        self.folder_ready = True
        self.top_pad = int(84 * host.s)
        self.status_text = ""
        self.status_detail = ""
        self.size = (0, 0)
        self.photos = {}
        self.tiles = {}
        self.geom = {}
        self.anim_geom = {}
        self.metrics = None
        self.bg_key = None
        self.bg_frame = None
        self.bg_blend = None
        self.fade = 0
        self.motion = 0
        self.frame_hook = None
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
        self.anim_geom.clear()
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

    def set_folder_ready(self, ready):
        ready = bool(ready)
        if ready != self.folder_ready:
            self.folder_ready = ready
            if getattr(self, "pill_top", None) is not None:
                self._layout_pills(self.pill_top)
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
        self.anim_geom.clear()
        self.photos.clear()
        width, height = self.size
        s = self.host.s
        metrics = self._metrics()
        self.metrics = metrics

        self.bg_items = [canvas.create_image(0, 0, anchor="nw", tags="bg"),
                         canvas.create_image(0, 0, anchor="nw", tags="bg")]
        self.bg_blend = None
        self.bg_frame = None
        self.bg_key = None
        self.fade += 1  # any fade still running belongs to the old canvas
        self._set_background(self.key or self.games[0]["key"], animate=False)

        self.logo_height = max(int(70 * s), min(int(height * 0.125), int(146 * s)))
        self.logo_item = canvas.create_image(0, 0, anchor="sw")
        self.subtitle_item = canvas.create_text(0, 0, anchor="w", font=self.host.fonts.f_small, fill=MUTED,
                                                text="")

        self.pills = {}
        for tag in ("play",):
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
        block = self.size[1] * 0.42
        self.logo_pos = (margin, block)
        self.subtitle_pos = (margin, block + int(20 * s))
        self.canvas.coords(self.subtitle_item, *self.subtitle_pos)
        self.canvas.coords(self.status_item, self.size[0] - margin, self.top_pad + int(24 * s))
        self.canvas.coords(self.status_shadow, self.size[0] - margin + 2 * s,
                           self.top_pad + int(26 * s))
        self._layout_pills(block + int(54 * s))

    def _play_text(self):
        if self.running:
            return "Stop"
        return "Play" if self.folder_ready else "Select game folder"

    def _layout_pills(self, top):
        if not getattr(self, "metrics", None) or not getattr(self, "pills", None):
            return
        s = self.host.s
        height = int(42 * s)
        x = self.metrics["margin"]
        self.pill_top = top
        for tag in ("play",):
            pill = self.pills[tag]
            text = self._play_text()
            self.canvas.itemconfig(pill["label"], text=text)
            x1, _, x2, _ = self.canvas.bbox(pill["label"])
            width = (x2 - x1) + int(46 * s)
            pill["box"] = (x, top, width, height)
            self.canvas.coords(pill["label"], x + width / 2, top + height / 2)
            self.canvas.coords(pill["body"], *round_points(x, top, x + width, top + height, 12 * s))
            x += width + int(12 * s)

    def _set_background(self, key, animate=True):
        """The art behind the hub, cross-dissolving from whatever is on screen.

        A slide of two whole images reads as a cut when the tiles move with it; a dissolve keeps
        the change calm and hides that the art is a different photograph. One Tk photo is reused for
        every frame (`PhotoImage.paste`), so no image is handed to Tk per frame, and the strip is
        handed the same frame so the art runs edge to edge through it."""
        size = self.size
        image = self.art.background(key, size)
        if image is None:
            image = Image.new("RGB", size, BG)
        photo = ImageTk.PhotoImage(image)
        self.photos[f"bg-{key}"] = photo
        base, top = self.bg_items
        old = self.bg_frame
        self.fade += 1
        fade = self.fade

        if not animate or old is None or old.size != size:
            self.bg_frame = image
            self.canvas.itemconfig(base, image=photo)
            self.canvas.coords(base, 0, 0)
            self.canvas.itemconfig(top, image="")
            self.bg_key = key
            return

        if self.bg_blend is None or self.bg_blend.width() != size[0] \
                or self.bg_blend.height() != size[1]:
            self.bg_blend = ImageTk.PhotoImage(old)
        blend = self.bg_blend
        blend.paste(old)
        self.canvas.itemconfig(top, image=blend)
        self.canvas.coords(top, 0, 0)
        self.canvas.tag_lower("bg")
        self.canvas.tag_raise(top, base)

        def step(k):
            if fade != self.fade:
                return
            frame = Image.blend(old, image, k)
            self.bg_frame = frame
            blend.paste(frame)
            if self.frame_hook is not None:
                self.frame_hook(frame, key)

        def done():
            if fade != self.fade:
                return
            self.bg_frame = image
            self.canvas.itemconfig(base, image=photo)
            self.canvas.coords(base, 0, 0)
            self.canvas.itemconfig(top, image="")
            if self.frame_hook is not None:
                self.frame_hook(image, key)

        self.anim.add(460, step, done, ease_in_out, interval=40)
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
        self.canvas.itemconfig(self.subtitle_item, text=f"{game['name']}  ·  {game.get('year', '')}".strip(" ·"))

        self.motion += 1
        motion = self.motion
        if animate:
            # The logo and the caption rise into place from wherever they are: holding a direction
            # repeats every 110 ms, and starting each move from the last frame stops them from
            # dropping back down while you flick through the games.
            for item, (x, y) in ((self.logo_item, self.logo_pos),
                                 (self.subtitle_item, self.subtitle_pos)):
                current = self.canvas.coords(item)[1]
                start = min(y + 18 * s, max(y, current)) if current else y
                self.canvas.coords(item, x, start)

                def rise(k, i=item, px=x, py=y, sy=start):
                    if motion != self.motion:
                        return
                    self.canvas.coords(i, px, py + (sy - py) * (1 - k))

                self.anim.add(280, rise)
            self._set_background(self.key, animate=True)
        else:
            self.canvas.coords(self.logo_item, *self.logo_pos)
            self.canvas.coords(self.subtitle_item, *self.subtitle_pos)

        old = dict(self.anim_geom)
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
        motion = self.motion

        def step(k):
            if motion != self.motion:
                return
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
        self.anim_geom[key] = (x, width)
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
        if not getattr(self, "pills", None):
            return
        s = self.host.s
        for tag, pill in self.pills.items():
            if tag == "play":
                text = self._play_text()
                fill = STOP_RED if self.running else WHITE
                ink = WHITE if self.running else INK
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
            if not self.running and not self.folder_ready:
                self.host.choose_game_dir()
                return
            self.host.play_or_stop()
        elif tag == "quit":
            self.host.ask_quit()
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
        pill = self._pill_at(event.x, event.y)
        if pill:
            self._pill_click(pill)
            return
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


# -- gallery ------------------------------------------------------------------------------------


class GalleryView(tk.Frame):
    """The collection's photos: a glass grid, filters per game, and a full-screen viewer.

    Photos come from the launcher (it scans the folder); thumbnails are made once and kept as
    small JPEGs in the launcher's cache, so opening the tab again is just a few file reads. The
    grid draws only the rows in view and has no timer of its own."""

    def __init__(self, host, art, animator, glass, parent):
        super().__init__(parent, bg=BG)
        self.host, self.art, self.anim = host, art, animator
        self.glass = glass
        self.canvas = tk.Canvas(self, bg=BG, highlightthickness=0, bd=0, takefocus=0)
        self.canvas.pack(fill="both", expand=True)
        self.entries = []
        self.filtered = []
        self.filter = None
        self.index = 0
        self.first = 0
        self.viewer = None
        self.fit = True
        self.key = None
        self.size = (0, 0)
        self.metrics = None
        self.cols = 4
        self.photos = {}
        self.originals = {}
        self.chips = {}
        self.buttons = {}
        self._pending = None
        self.canvas.bind("<Configure>", self._on_configure)
        self.canvas.bind("<Button-1>", self._on_click)
        self.canvas.bind("<Motion>", self._on_motion)
        self.canvas.bind("<Leave>", lambda event: self.canvas.config(cursor=""))
        self.canvas.bind("<MouseWheel>", self._on_wheel)

    # -- public ---------------------------------------------------------------------------------

    def set_art(self, key):
        self.key = key

    def set_entries(self, entries):
        self.entries = list(entries)
        self._apply_filter(redraw=False)
        if self.size != (0, 0):
            self._build()

    def refresh(self):
        if self.size != (0, 0):
            self._build()

    def set_filter(self, game):
        self.filter = game
        self.index = 0
        self.first = 0
        self._apply_filter(redraw=False)
        self._build()
        self.host.set_hints_for_hub()

    def focus_first(self):
        self.index = min(self.index, max(0, len(self.filtered) - 1))

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
        margin = max(int(44 * s), int(width * 0.042))
        top = int(104 * s)
        gap = int(18 * s)
        cols = 5 if width >= int(1680 * s) else 4
        cell_w = int((width - 2 * margin - (cols - 1) * gap) / cols)
        cell_h = int(cell_w * 9 / 16) + int(46 * s)
        grid_top = top + int(92 * s)
        rows = max(1, (height - grid_top - int(24 * s)) // (cell_h + gap))
        return {"margin": margin, "top": top, "gap": gap, "cols": cols, "cell_w": cell_w,
                "cell_h": cell_h, "grid_top": grid_top, "rows": rows}

    def _apply_filter(self, redraw=True):
        self.filtered = [entry for entry in self.entries
                         if self.filter is None or entry.get("game") == self.filter]
        self.index = max(0, min(self.index, len(self.filtered) - 1))
        if redraw:
            self._build()

    # -- drawing --------------------------------------------------------------------------------

    def _build(self):
        self._pending = None
        if self.size == (0, 0):
            return
        canvas = self.canvas
        canvas.delete("all")
        self.photos = {}
        self.chips, self.buttons = {}, {}
        width, height = self.size
        s = self.host.s
        metrics = self._metrics()
        self.metrics = metrics
        self.cols = metrics["cols"]
        self._draw_background()
        self._draw_header(metrics)

        if self.viewer is not None and self.filtered:
            self._draw_viewer()
            return
        if not self.filtered:
            self._draw_empty(metrics)
            return
        rows = metrics["rows"]
        row = self.index // self.cols
        if row < self.first:
            self.first = row
        if row >= self.first + rows:
            self.first = row - rows + 1
        self.first = max(0, self.first)
        total_rows = (len(self.filtered) + self.cols - 1) // self.cols
        if self.first > max(0, total_rows - rows):
            self.first = max(0, total_rows - rows)
        for slot in range(rows * self.cols):
            position = self.first * self.cols + slot
            if position >= len(self.filtered):
                break
            column, line = slot % self.cols, slot // self.cols
            x = metrics["margin"] + column * (metrics["cell_w"] + metrics["gap"])
            y = metrics["grid_top"] + line * (metrics["cell_h"] + metrics["gap"])
            self._draw_card(position, x, y, metrics)
        if total_rows > rows:
            self._draw_page_dots(metrics, total_rows, rows)

    def _draw_background(self):
        width, height = self.size
        image = None
        if self.key is not None:
            image = self.art.dim(self.key, self.size, 0.34)
        if image is None:
            image = Image.new("RGB", self.size, BG)
        photo = ImageTk.PhotoImage(image)
        self.photos["bg"] = photo
        self.canvas.create_image(0, 0, image=photo, anchor="nw")

    def _draw_header(self, metrics):
        canvas, s = self.canvas, self.host.s
        x, top = metrics["margin"], metrics["top"]
        count = len(self.entries)
        canvas.create_text(x, top, text="GALLERY", anchor="w", font=self.host.fonts.f_hero, fill=TEXT)
        path = self._ellipsis(str(self.host.gallery_dir()), self.host.fonts.f_small,
                              int(self.size[0] * 0.44))
        canvas.create_text(x, top + int(34 * s), anchor="w", font=self.host.fonts.f_small, fill=MUTED,
                           text=f"{count} photo{'s' if count != 1 else ''}  ·  {path}")

        # Filter chips on the right: All, then one per game.
        chips = [(None, "All")] + [(str(n), str(n)) for n in (1, 2, 3)]
        font = self.host.fonts.f_body_bold
        cap_h = int(32 * s)
        gap = int(8 * s)
        right = self.size[0] - metrics["margin"]
        y = top + int(2 * s)
        widths = []
        for value, label in chips:
            item = canvas.create_text(0, 0, text=label, font=font, anchor="w")
            box = canvas.bbox(item)
            canvas.delete(item)
            widths.append(box[2] - box[0] + int(30 * s))
        x0 = right - sum(widths) - gap * (len(chips) - 1)
        for (value, label), chip_w in zip(chips, widths):
            box = (x0, y, x0 + chip_w, y + cap_h)
            selected = value == self.filter
            surface = self.glass.surface(self.key, self.size, box, cap_h / 2, 0.34,
                                         (255, 255, 255, 46) if selected else (9, 11, 16, 158),
                                         170 if selected else 54)
            photo = ImageTk.PhotoImage(surface)
            self.photos[f"chip-{value}"] = photo
            canvas.create_image(box[0], box[1], image=photo, anchor="nw")
            canvas.create_text(x0 + chip_w / 2, y + cap_h / 2, text=label, font=font,
                               fill=INK if selected else TEXT)
            self.chips[value] = box
            x0 += chip_w + gap

        # Open folder / Change folder, under the chips.
        for name, label in (("open", "Open folder"), ("change", "Change folder")):
            item = canvas.create_text(0, 0, text=label, font=self.host.fonts.f_small, anchor="w")
            box = canvas.bbox(item)
            canvas.delete(item)
            button_w = box[2] - box[0] + int(28 * s)
            bx = right - button_w
            by = y + cap_h + int(10 * s)
            pill = (bx, by, bx + button_w, by + int(28 * s))
            surface = self.glass.surface(self.key, self.size, pill, int(14 * s), 0.34,
                                         (9, 11, 16, 150), 50)
            photo = ImageTk.PhotoImage(surface)
            self.photos[f"button-{name}"] = photo
            canvas.create_image(pill[0], pill[1], image=photo, anchor="nw")
            canvas.create_text(bx + button_w / 2, by + int(14 * s), text=label,
                               font=self.host.fonts.f_small, fill=MUTED)
            self.buttons[name] = pill
            right = bx - int(8 * s)

    def _draw_card(self, position, x, y, metrics):
        canvas, s = self.canvas, self.host.s
        entry = self.filtered[position]
        cell_w, thumb_h = metrics["cell_w"], int(metrics["cell_w"] * 9 / 16)
        thumb = self._thumb(entry, (cell_w, thumb_h))
        if thumb is not None:
            canvas.create_image(x, y, image=thumb, anchor="nw")
        else:
            canvas.create_rectangle(x, y, x + cell_w, y + thumb_h, fill=PANEL, outline=LINE)
            canvas.create_text(x + cell_w / 2, y + thumb_h / 2, text="?", fill=DIM,
                               font=self.host.fonts.f_title)
        selected = position == self.index
        if selected:
            round_rect(canvas, x, y, x + cell_w, y + thumb_h, max(2, int(10 * s)),
                       fill="", outline=WHITE, width=3)
        name = self._ellipsis(entry["name"], self.host.fonts.f_small, cell_w - int(4 * s))
        canvas.create_text(x, y + thumb_h + int(9 * s), text=name, anchor="nw",
                           font=self.host.fonts.f_small, fill=TEXT if selected else MUTED)
        game = entry.get("game") or ""
        label = entry.get("game_name") or "Uncharted"
        canvas.create_text(x, y + thumb_h + int(27 * s), text=label, anchor="nw",
                           font=self.host.fonts.f_tiny, fill=DIM)

    def _draw_page_dots(self, metrics, total_rows, rows):
        canvas, s = self.canvas, self.host.s
        pages = (total_rows + rows - 1) // rows
        page = min(pages - 1, self.first // max(1, rows))
        cx = self.size[0] / 2 - (pages - 1) * int(9 * s) / 2
        y = self.size[1] - int(16 * s)
        for index in range(pages):
            colour = WHITE if index == page else LINE
            canvas.create_oval(cx - 3 * s, y - 3 * s, cx + 3 * s, y + 3 * s, fill=colour, outline=colour)
            cx += int(9 * s)

    def _draw_empty(self, metrics):
        canvas, s = self.canvas, self.host.s
        width, height = self.size
        cx, cy = width / 2, metrics["grid_top"] + int(90 * s)
        round_rect(canvas, cx - int(210 * s), cy - int(76 * s), cx + int(210 * s), cy + int(96 * s),
                   int(18 * s), fill=PANEL, outline=LINE)
        canvas.create_text(cx, cy - int(28 * s), text="No photos yet", font=self.host.fonts.f_title,
                           fill=TEXT)
        canvas.create_text(cx, cy + int(8 * s), font=self.host.fonts.f_small, fill=MUTED,
                           justify="center", width=int(360 * s),
                           text="Put your Uncharted pictures in the folder below (or in its game "
                                "subfolders) and they appear here.")
        canvas.create_text(cx, cy + int(52 * s), font=self.host.fonts.f_small, fill=DIM,
                           text=str(self.host.gallery_dir()))
        self.buttons["open"] = (cx - int(150 * s), cy + int(66 * s), cx - int(10 * s),
                                cy + int(94 * s))
        self.buttons["change"] = (cx + int(10 * s), cy + int(66 * s), cx + int(150 * s),
                                  cy + int(94 * s))
        for name, label in (("open", "Open folder"), ("change", "Change folder")):
            box = self.buttons[name]
            round_rect(canvas, *box, int(12 * s), fill=PANEL_HI, outline=LINE)
            canvas.create_text((box[0] + box[2]) / 2, (box[1] + box[3]) / 2, text=label,
                               font=self.host.fonts.f_small, fill=TEXT)

    # -- thumbnails -----------------------------------------------------------------------------

    def _thumb(self, entry, size):
        key = (entry["path"], entry["stamp"], size)
        if key in self.photos:
            return self.photos[key]
        image = None
        target = None
        cache = cache_dir()
        if cache is not None:
            import hashlib
            digest = hashlib.sha1(f"{entry['path']}|{entry['stamp']}".encode("utf-8", "replace"))
            folder = cache / "gallery"
            try:
                folder.mkdir(parents=True, exist_ok=True)
            except OSError:
                folder = None
            if folder is not None:
                target = folder / f"{digest.hexdigest()[:20]}.jpg"
                try:
                    image = Image.open(target)
                    image.load()
                    image = image.convert("RGB")
                except (OSError, ValueError):
                    image = None
        if image is None:
            try:
                image = Image.open(entry["path"])
                image.load()
                image = cover(image.convert("RGB"), 480, 270)
            except (OSError, ValueError):
                return None
            if target is not None:
                try:
                    image.save(target, "JPEG", quality=82)
                except (OSError, ValueError):
                    pass
        image = cover(image, size[0], size[1])
        photo = ImageTk.PhotoImage(rounded_image(image, max(2, int(size[0] / 42)),
                                                 rim=(255, 255, 255), rim_alpha=34))
        if len(self.photos) > 120:
            self.photos.clear()
        self.photos[key] = photo
        return photo

    def _original(self, entry):
        path = entry["path"]
        if path not in self.originals:
            try:
                image = Image.open(path)
                image.load()
                image = image.convert("RGB")
            except (OSError, ValueError):
                return None
            if len(self.originals) > 3:
                self.originals.clear()
            self.originals[path] = image
        return self.originals[path]

    # -- viewer ---------------------------------------------------------------------------------

    def _draw_viewer(self):
        canvas, s = self.canvas, self.host.s
        width, height = self.size
        entry = self.filtered[self.index]
        canvas.create_rectangle(0, 0, width, height, fill="#030406", outline="")
        top = int(84 * s) + int(24 * s)
        bottom = height - int(34 * s)
        image = self._original(entry)
        if image is not None:
            area_w, area_h = width, bottom - top
            if self.fit:
                scale = min(area_w / image.width, area_h / image.height)
                size = (max(1, int(image.width * scale)), max(1, int(image.height * scale)))
                shown = image.resize(size, Image.LANCZOS)
            else:
                shown = cover(image, area_w, area_h)
            photo = ImageTk.PhotoImage(shown)
            self.photos["viewer"] = photo
            canvas.create_image(width / 2, top + (bottom - top) / 2, image=photo, anchor="center")
        canvas.create_text(int(44 * s), top, anchor="nw", text=entry["name"],
                           font=self.host.fonts.f_body_bold, fill=TEXT)
        detail = entry.get("game_name") or "Uncharted"
        if entry.get("when"):
            detail += f"  ·  {entry['when']}"
        canvas.create_text(int(44 * s), top + int(22 * s), anchor="nw", text=detail,
                           font=self.host.fonts.f_small, fill=MUTED)
        canvas.create_text(width - int(44 * s), top + int(2 * s), anchor="ne",
                           font=self.host.fonts.f_small, fill=MUTED,
                           text=f"{self.index + 1} / {len(self.filtered)}")
        for side in (-1, 1):
            cx = width / 2 + side * (width / 2 - int(34 * s))
            cy = top + (bottom - top) / 2
            canvas.create_text(cx, cy, text="‹" if side < 0 else "›",
                               font=(self.host.fonts.text, int(30 * s)), fill=MUTED)

    def _viewer_step(self, step):
        if not self.filtered:
            return
        self.index = (self.index + step) % len(self.filtered)
        self._build()

    # -- interaction ----------------------------------------------------------------------------

    def move(self, dx, dy):
        if self.viewer is not None:
            if dx:
                self._viewer_step(dx)
            return True
        if not self.filtered:
            return False
        if dx:
            self.index = (self.index + dx) % len(self.filtered)
            self._build()
            return True
        if dy:
            target = self.index + dy * self.cols
            if target < 0:
                return False
            self.index = min(target, len(self.filtered) - 1)
            self._build()
            return True
        return False

    def activate(self):
        if self.viewer is not None:
            self.fit = not self.fit
            self._build()
            self.host.set_hints_for_hub()
            return
        if self.filtered:
            self.viewer = self.index
            self.fit = True
            self._build()
            self.host.set_hints_for_hub()
        else:
            self.host.open_gallery_folder()

    def back(self):
        if self.viewer is not None:
            self.viewer = None
            self._build()
            return True
        return False

    def _card_at(self, x, y):
        metrics = self.metrics
        if metrics is None or not self.filtered:
            return None
        for slot in range(metrics["rows"] * self.cols):
            position = self.first * self.cols + slot
            if position >= len(self.filtered):
                break
            column, line = slot % self.cols, slot // self.cols
            cx = metrics["margin"] + column * (metrics["cell_w"] + metrics["gap"])
            cy = metrics["grid_top"] + line * (metrics["cell_h"] + metrics["gap"])
            if cx <= x <= cx + metrics["cell_w"] and cy <= y <= cy + metrics["cell_h"]:
                return position
        return None

    def _on_motion(self, event):
        if self.viewer is not None:
            self.canvas.config(cursor="hand2")
            return
        self.canvas.config(cursor="hand2" if self._card_at(event.x, event.y) is not None else "")

    def _on_click(self, event):
        if self.viewer is not None:
            third = self.size[0] / 3
            if event.x < third:
                self._viewer_step(-1)
            elif event.x > third * 2:
                self._viewer_step(1)
            else:
                self.fit = not self.fit
                self._build()
            return
        for value, box in self.chips.items():
            if box[0] <= event.x <= box[2] and box[1] <= event.y <= box[3]:
                self.set_filter(value)
                return
        for name, box in self.buttons.items():
            if box[0] <= event.x <= box[2] and box[1] <= event.y <= box[3]:
                if name == "change":
                    self.host.change_gallery_folder()
                else:
                    self.host.open_gallery_folder()
                return
        position = self._card_at(event.x, event.y)
        if position is not None:
            self.index = position
            self.activate()

    def _on_wheel(self, event):
        if self.viewer is not None:
            self._viewer_step(-1 if event.delta > 0 else 1)
            return "break"
        self.move(0, -1 if event.delta > 0 else 1)
        return "break"

    # -- text -----------------------------------------------------------------------------------

    def _ellipsis(self, text, font, width):
        item = self.canvas.create_text(-4000, -4000, text=text, font=font, anchor="w")
        try:
            if self.canvas.bbox(item)[2] - self.canvas.bbox(item)[0] <= width:
                return text
            while text:
                text = text[:-1]
                self.canvas.itemconfig(item, text=text + "…")
                if self.canvas.bbox(item)[2] - self.canvas.bbox(item)[0] <= width:
                    break
            return text + "…"
        finally:
            self.canvas.delete(item)


# -- display panel ------------------------------------------------------------------------------


class DisplayView(tk.Frame):
    """The display capsule: the handful of settings that change the picture, as a glass panel.

    Each row is a value with a pair of arrows: Left/Right on the controller changes it, the mouse
    clicks the arrows, and everything goes through the launcher's existing settings handlers (so
    the Settings pages and the config always agree). The panel is drawn on demand, never on a
    timer."""

    def __init__(self, host, art, animator, glass, parent):
        super().__init__(parent, bg=BG)
        self.host, self.art, self.anim = host, art, animator
        self.glass = glass
        self.canvas = tk.Canvas(self, bg=BG, highlightthickness=0, bd=0, takefocus=0)
        self.canvas.pack(fill="both", expand=True)
        self.rows = []
        self.focus = 0
        self.key = None
        self.size = (0, 0)
        self.metrics = None
        self.photos = {}
        self.hits = []
        self.all_box = None
        self._pending = None
        self.canvas.bind("<Configure>", self._on_configure)
        self.canvas.bind("<Button-1>", self._on_click)
        self.canvas.bind("<Motion>", self._on_motion)
        self.canvas.bind("<Leave>", lambda event: self.canvas.config(cursor=""))

    # -- public ---------------------------------------------------------------------------------

    def set_art(self, key):
        self.key = key

    def refresh(self):
        self.rows = self.host.display_model()
        self.focus = max(0, min(self.focus, len(self.rows)))
        if self.size != (0, 0):
            self._build()

    def move(self, dx, dy):
        if dy:
            target = self.focus + dy
            if target < 0:
                return False
            self.focus = min(target, len(self.rows))
            self._build()
            return True
        if dx:
            if self.focus < len(self.rows):
                self._step(self.focus, dx)
            return True
        return False

    def activate(self):
        if self.focus >= len(self.rows):
            self.host.open_settings("display")
        elif self.rows:
            self._step(self.focus, 1)

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
        margin = max(int(44 * s), int(width * 0.042))
        top = int(100 * s)
        panel_w = min(width - 2 * margin, int(1120 * s))
        panel_x = (width - panel_w) / 2
        panel_y = top + int(64 * s)
        rows = max(1, len(self.rows))
        room = height - panel_y - int(66 * s)
        row_h = max(int(44 * s), min(int(60 * s), int((room - int(24 * s)) / rows)))
        return {"margin": margin, "top": top, "panel_w": panel_w, "panel_x": panel_x,
                "panel_y": panel_y, "row_h": row_h, "rows": rows}

    # -- drawing --------------------------------------------------------------------------------

    def _build(self):
        self._pending = None
        if self.size == (0, 0):
            return
        canvas = self.canvas
        canvas.delete("all")
        self.photos = {}
        self.hits, self.all_box = [], None
        width, height = self.size
        s = self.host.s
        metrics = self._metrics()
        self.metrics = metrics
        self._draw_background()
        self._draw_header(metrics)

        panel_x, panel_y = metrics["panel_x"], metrics["panel_y"]
        panel_w = metrics["panel_w"]
        row_h = metrics["row_h"]
        panel_h = row_h * len(self.rows) + int(24 * s)
        box = (panel_x, panel_y, panel_x + panel_w, panel_y + panel_h)
        surface = self.glass.surface(self.key, self.size, box, int(18 * s), 0.34,
                                     (9, 11, 16, 196), 58)
        photo = ImageTk.PhotoImage(surface)
        self.photos["panel"] = photo
        canvas.create_image(panel_x, panel_y, image=photo, anchor="nw")

        for index, entry in enumerate(self.rows):
            self._draw_row(index, entry, metrics, panel_y + int(12 * s) + index * row_h)
        self._draw_all_settings(metrics, panel_y + panel_h + int(16 * s))

    def _draw_background(self):
        image = None
        if self.key is not None:
            image = self.art.dim(self.key, self.size, 0.34)
        if image is None:
            image = Image.new("RGB", self.size, BG)
        photo = ImageTk.PhotoImage(image)
        self.photos["bg"] = photo
        self.canvas.create_image(0, 0, image=photo, anchor="nw")

    def _draw_header(self, metrics):
        canvas, s = self.canvas, self.host.s
        x, top = metrics["margin"], metrics["top"]
        canvas.create_text(x, top, text="DISPLAY", anchor="w", font=self.host.fonts.f_hero, fill=TEXT)
        canvas.create_text(x, top + int(34 * s), anchor="w", font=self.host.fonts.f_small, fill=MUTED,
                           text="Picture and performance. Changes save now and apply when a game starts.")

    def _draw_row(self, index, entry, metrics, y):
        canvas, s = self.canvas, self.host.s
        x1 = metrics["panel_x"] + int(18 * s)
        x2 = metrics["panel_x"] + metrics["panel_w"] - int(18 * s)
        row_h = metrics["row_h"]
        focused = index == self.focus
        if focused:
            round_rect(canvas, x1, y, x2, y + row_h - int(6 * s), int(10 * s),
                       fill="", outline=WHITE, width=2)
        canvas.create_text(x1 + int(16 * s), y + int(14 * s), anchor="nw", text=entry["title"],
                           font=self.host.fonts.f_body_bold, fill=TEXT)
        hint_w = int(metrics["panel_w"] * 0.46)
        hint = self._ellipsis(entry.get("hint", ""), self.host.fonts.f_small, hint_w)
        canvas.create_text(x1 + int(16 * s), y + int(34 * s), anchor="nw", text=hint,
                           font=self.host.fonts.f_small, fill=MUTED)

        control_w = min(int(330 * s), int(metrics["panel_w"] * 0.36))
        control_x = x2 - int(14 * s) - control_w
        mid = y + (row_h - int(6 * s)) / 2
        arrow = int(26 * s)
        left_box = (control_x, mid - arrow / 2, control_x + arrow, mid + arrow / 2)
        right_box = (control_x + control_w - arrow, mid - arrow / 2, control_x + control_w,
                     mid + arrow / 2)
        value_box = (left_box[2] + int(6 * s), mid - int(15 * s), right_box[0] - int(6 * s),
                     mid + int(15 * s))
        fill = WHITE if focused else PANEL_HI
        ink = INK if focused else TEXT
        round_rect(canvas, *value_box, int(10 * s), fill=fill, outline=fill)
        canvas.create_text((value_box[0] + value_box[2]) / 2, mid, text=entry.get("label", ""),
                           font=self.host.fonts.f_body_bold, fill=ink)
        for delta, box in ((-1, left_box), (1, right_box)):
            canvas.create_text((box[0] + box[2]) / 2, mid, text="‹" if delta < 0 else "›",
                               font=self.host.fonts.f_h2, fill=TEXT if focused else MUTED)
            self.hits.append((index, delta, box))
        self.hits.append((index, 1, value_box))

    def _draw_all_settings(self, metrics, y):
        canvas, s = self.canvas, self.host.s
        label = "All settings"
        item = canvas.create_text(0, 0, text=label, font=self.host.fonts.f_body_bold, anchor="w")
        box = canvas.bbox(item)
        canvas.delete(item)
        width = box[2] - box[0] + int(52 * s)
        x2 = metrics["panel_x"] + metrics["panel_w"]
        self.all_box = (x2 - width, y, x2, y + int(38 * s))
        focused = self.focus >= len(self.rows)
        surface = self.glass.surface(self.key, self.size, self.all_box, int(19 * s), 0.34,
                                     (255, 255, 255, 46) if focused else (9, 11, 16, 170),
                                     235 if focused else 62)
        photo = ImageTk.PhotoImage(surface)
        self.photos["all"] = photo
        canvas.create_image(self.all_box[0], self.all_box[1], image=photo, anchor="nw")
        canvas.create_text((self.all_box[0] + self.all_box[2]) / 2,
                           (self.all_box[1] + self.all_box[3]) / 2, text=label,
                           font=self.host.fonts.f_body_bold, fill=TEXT)

    # -- interaction ----------------------------------------------------------------------------

    def _step(self, index, delta):
        if not (0 <= index < len(self.rows)):
            return
        entry = self.rows[index]
        values = [value for value, _ in entry["options"]]
        if not values:
            return
        try:
            position = values.index(entry["value"])
        except ValueError:
            position = 0
        self.host.display_change(entry["key"], values[(position + delta) % len(values)])

    def _on_motion(self, event):
        over = self.all_box is not None and self.all_box[0] <= event.x <= self.all_box[2] \
            and self.all_box[1] <= event.y <= self.all_box[3]
        over = over or any(box[0] <= event.x <= box[2] and box[1] <= event.y <= box[3]
                           for _, _, box in self.hits)
        self.canvas.config(cursor="hand2" if over else "")

    def _on_click(self, event):
        if self.all_box is not None and self.all_box[0] <= event.x <= self.all_box[2] \
                and self.all_box[1] <= event.y <= self.all_box[3]:
            self.host.open_settings("display")
            return
        for index, delta, box in self.hits:
            if box[0] <= event.x <= box[2] and box[1] <= event.y <= box[3]:
                self.focus = index
                self._step(index, delta)
                return

    # -- text -----------------------------------------------------------------------------------

    def _ellipsis(self, text, font, width):
        item = self.canvas.create_text(-4000, -4000, text=text, font=font, anchor="w")
        try:
            if self.canvas.bbox(item)[2] - self.canvas.bbox(item)[0] <= width:
                return text
            while text:
                text = text[:-1]
                self.canvas.itemconfig(item, text=text + "…")
                if self.canvas.bbox(item)[2] - self.canvas.bbox(item)[0] <= width:
                    break
            return text + "…"
        finally:
            self.canvas.delete(item)


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
