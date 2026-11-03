# Lua 5.4.7

From https://www.lua.org/ftp/lua-5.4.7.tar.gz (sha256
9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30), MIT
licensed (LICENSE). Only `src/`, without the standalone interpreter and
compiler (lua.c, luac.c) and the libraries a script mod must not have: io, os,
package (loadlib.c), debug, and linit.c (src/izuna/izuna_scripts.cpp opens
the libraries it allows itself). Unmodified.
