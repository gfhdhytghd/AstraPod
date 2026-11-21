# AstraPod plugins

Build and package the supported upstream sources with the
[AstraPod plugin SDK guide](../../docs/podbox/rock-compat-sdk.md).

| Path | Purpose |
| --- | --- |
| `sdk/` | Source-facing header, module format and API slots |
| `rock-modules.txt` | Allowlist and installation paths |
| `rock_runtime.c`, `rock_files.c` | Foreground loader and ownership |
| `rock_browser.c` | Category picker backed by the native file browser |
| `plugin.lds` | Native code and codec layout, selected by flags |
| `credits.pl` | Generator for the native credits screen |

The zip packager reads Rockbox's category and viewer manifests from
`apps/plugins/`. The AstraPod bundle script installs its own allowlist after
the theme bundle clears the zip's stock plugin payload.
