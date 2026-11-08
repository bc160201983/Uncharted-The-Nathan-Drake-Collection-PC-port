# Controller shell artwork

The controller (PlayStation 5 style) launcher shell can show a background, a tile and a logo for each
game. The port does not ship that artwork: it is Sony's and Naughty Dog's, and not covered by this
project's license. Without it the shell draws plain dark panels and works the same.

To add your own, put these files in this folder (any size; they are cropped to fit):

| File | Used as |
|---|---|
| `bg-u1.jpg`, `bg-u2.jpg`, `bg-u3.jpg` | the full-screen background of each game |
| `tile-u1.jpg`, `tile-u2.jpg`, `tile-u3.jpg` | the game's tile on the carousel |
| `logo-u1.png`, `logo-u2.png`, `logo-u3.png` | the game's logo (transparent PNG) |

Your own game dump has a background you can use: `sce_sys/pic1.png` in the game folder.
