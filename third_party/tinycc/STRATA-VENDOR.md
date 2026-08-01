# TinyCC vendoring notes

This directory was copied from:

`/home/sear/rb/snow/tools/riblang/tinycc`

The source was introduced to Snow by commits `0cedafb9` (tinycc-mob source)
and `57412f37` (Riblang embedded-build configuration). It reports tinycc-mob
revision `a338258d309c888bde96b2d1f206299231a54ddf` and version `0.9.28rc`.

Strata initially copied the complete, clean 540-file source tree. The local
`config.h` is the Riblang minimal static configuration and is intentionally
different from an upstream configure-generated file. Strata-specific build
changes should remain limited to that configuration and clearly identified
integration files so the tree can be refreshed by replacing it from the Snow
copy and reapplying the documented deltas.

TinyCC is distributed under the GNU Lesser General Public License 2.1. Keep
`COPYING`, `RELICENSING`, the source, and this provenance note with redistributed
Strata packages. Static-link distribution obligations must be considered by
downstream distributors.
