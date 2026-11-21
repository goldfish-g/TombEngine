# Touch control images

The images of the on-screen touch controls (`TombEngine/Specific/Input/TouchControls.h`).
The engine has them in `TombEngine/Renderer/TouchIcons.h`, which `make_touch_icons.mjs` writes
from the files here:

    node make_touch_icons.mjs ../../TombEngine/Renderer/TouchIcons.h

- `svg/<icon>.svg`: the icons (512 x 512), white with a black outline. Each shape is in the
  file twice: first black and widened by the outline, then white. The engine draws them with
  nanosvg (`Libs/nanosvg`) at the size they show on screen, so keep to what nanosvg reads:
  paths and basic shapes, fills and strokes, transforms; no `<use>`, filters or CSS.
- `svg/button.svg`, `ring.svg`, `thumb.svg`: the circles (100 x 100): a button and the D-pad's
  base, the D-pad's deadzone and its thumb, in the colours of a pressed button. The engine sets
  the width of the shape with the id `ring` to the controls' border width, keeping its outer edge.
- `touch_glyphs.png`: the low-res icons, TRX's touch glyphs (`data/common/glyphs` of TRX), shown
  as solid squares with the Touch Icons setting on Low Res.

The icons other than roll and bullet are Font Awesome Free 6.7.2 icons by Fonticons, Inc.
(https://fontawesome.com), licensed CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/):
turn-up (jump), turn-down (crouch), person-walking (walk), person-running (run, with speed
lines added), gun (draw), eye (look), hand (action), bars (menu), pause (pause) and
person-swimming (swim, mirrored). Roll and bullet are drawn in their style.
