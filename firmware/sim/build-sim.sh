#!/bin/bash
# sim build env loader (WSL no-sudo SDL2 prefix); run sourcable steps then build
grep -qxF 'sim/local_sdl2.env' /home/barbara/src/ClawdDeck/firmware/.gitignore || echo 'sim/local_sdl2.env' >> /home/barbara/src/ClawdDeck/firmware/.gitignore
export SDL2_PREFIX_CFLAGS="$(grep '^SDL2_PREFIX_CFLAGS' /home/barbara/src/ClawdDeck/firmware/sim/local_sdl2.env | cut -d= -f2-)"
export PATH="$HOME/opt/sdl2-pref/bin:$PATH"
cd /home/barbara/src/ClawdDeck/firmware
pio run -e sim "$@"