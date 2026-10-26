# PS5 shell artwork

These images are the launcher's home screen: a full-screen background, a carousel tile and a
transparent logo for each of the three games.

| file | size | used for |
| --- | --- | --- |
| `bg-u1.jpg`, `bg-u2.jpg`, `bg-u3.jpg` | 1920 x 1080 | the hub's full-bleed background |
| `tile-u1.jpg`, `tile-u2.jpg`, `tile-u3.jpg` | 720 x 405 | the game tiles along the bottom |
| `logo-u1.png`, `logo-u2.png`, `logo-u3.png` | 150 px tall, transparent | the game logo on the left |

## Where they come from

The key art and logos are Uncharted promotional artwork. Uncharted: The Nathan Drake Collection,
its artwork and its logos are (c) Sony Interactive Entertainment and Naughty Dog. They are here so
the launcher can look like the PlayStation 5's own home screen, for the people who already own the
games.

They are **not** covered by this project's license. If you redistribute the launcher, or build it
into something of your own, replace them with artwork you have the right to ship (or delete the
folder: without these files the launcher still works, drawing plain dark backgrounds).

## Replacing them

Drop files with the same names and roughly the same shape into this folder. The launcher scales and
crops each one to the screen, so any size larger than the table above works; JPEG for the
backgrounds and tiles, PNG with transparency for the logos. The first start after a change rebuilds
the cached copies in `%LOCALAPPDATA%\Uncharted Launcher\ui-cache` by itself.

The backgrounds already carry the dark gradient the shell expects: heavy at the bottom (where the
tiles and the hint bar sit) and on the left (where the logo and the game's name go), light on the
right. A plain screenshot of the game works, but the text can be harder to read over it.
