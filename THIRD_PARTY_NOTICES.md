# Third-party notices

The project's `LICENSE` applies only to original materials created for this
project. It does not replace or restrict licenses that apply to third-party
components distributed with or fetched by this project.

## Arduino_GFX

The vendored display driver under `lib/GFX_Library_for_Arduino/` is Arduino_GFX
by Moon On Our Nation and contributors. Upstream repository:
<https://github.com/moononournation/Arduino_GFX>

Arduino_GFX uses the BSD license reproduced in
`lib/GFX_Library_for_Arduino/license.txt`. Its bundled Unifont-derived font
headers identify their own font license and notices in each source header;
check those notices when redistributing or changing those font assets.

## U8g2

The firmware obtains U8g2 through PlatformIO (`olikraus/U8g2`). Upstream source
and license: <https://github.com/olikraus/u8g2> and
<https://github.com/olikraus/u8g2/blob/master/LICENSE>. The U8g2 library code
uses the two-clause BSD license. Fonts may have separate terms; see the U8g2
font license table and each font's own header.

## Other tools and references

- PlatformIO: <https://platformio.org/> (tooling; fetched by the developer's
  local PlatformIO installation).
- kk_ui: <https://gitee.com/keysking/kk_ui> (animation design reference only;
  no kk_ui source code is included in this repository).
