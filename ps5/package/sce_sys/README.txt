Home-screen art for the app:
  icon0.png  512x512 PNG
  pic0.dds   3840x2160 background, BC7_UNORM DX10 DDS, no mipmaps
  pic1.dds   the same picture, the launch splash (the launcher wants both or
             neither)
The background shows at launch; on the home screen, while the app is only
highlighted, it does not yet (PNG copies and a copy into /user/appmeta were
tried in build 16 and did not help).
param.json is written by ps5/tools/ps5_package.sh.

These are edited from Ninja Gaiden II promotional artwork the owner chose
(the cover crop for the icon, with a "PS5 RECOMP" band; a wallpaper for the
background, its watermark lines cropped off and the left side darkened for
the home screen's text). The artwork is Koei Tecmo's: replace these with
original art before publishing a release if that matters for where it goes.
pic0/pic1 are made with ps5/tools/make_bc7_dds.py.
