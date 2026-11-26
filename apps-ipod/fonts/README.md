# Fonts the application ships

Run `bundle-theme.sh` after `make zip` to populate `/.rockbox/fonts/`.
The script copies `24x24-icons.fnt`, `24-spun-badges.fnt` and
`24-seven-fifteen.fnt` from this directory. It generates PBF1 text fonts
with `tools/fonts/compile.py` and ships them with the font licences.

Scrim and Light share the bundled fonts. The text font filenames retain the
`noto` basenames used by settings. Classic and Plastic use the `helvetica`
fonts, built from the stock firmware glyphs in `stock/`. See
[the native theme format](../../docs/podbox/native-theme-format.md) for font
roles and custom themes.
