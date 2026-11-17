#!/usr/bin/env bash
# Oracle comparison of md::pu (engine/src/particles/pu_system.cpp) against ParticleUniverse fa74677 on Ogre 14.6: same script, same random sequence, N steps.
#   run.sh <script.pu> <steps> <dt> <seed>        (the first technique of the script only; the renderer is replaced by a no-op one)
# The ParticleUniverse sources are NOT part of the repo: PU_SRC = a checkout of OGRECave/particleuniverse (default ~/rdga1prj/refs/particleuniverse). Nothing of it is copied here.
# Objects are built once into $PU_BUILD (default /tmp/pu_oracle_build); ForceField / BeamRenderer / Root do not compile on Ogre 14 and are not needed.
set -eu
PU_SRC=${PU_SRC:-$HOME/rdga1prj/refs/particleuniverse}; PU_BUILD=${PU_BUILD:-/tmp/pu_oracle_build}
HERE=$(cd "$(dirname "$0")" && pwd); ENGINE=$HERE/../../../engine
P=$PU_SRC/Plugins/ParticleUniverse
mkdir -p "$PU_BUILD/o" /tmp/pu_oracle_scripts
find "$P/src" -name '*.cpp' | grep -v 'ForceField\|BeamRenderer\|ParticleUniverseRoot' | xargs -P2 -I{} sh -c \
  'o='"$PU_BUILD"'/o/$(basename {} .cpp).o; [ -f $o ] || g++ -std=c++17 -O1 -w -I'"$P"'/include -I/usr/include/OGRE -c {} -o $o'
g++ -std=c++17 -O1 -w -I"$P/include" -I/usr/include/OGRE -I"$ENGINE/include" "$HERE/oracle.cpp" "$ENGINE/src/particles/pu_system.cpp" "$ENGINE/src/particles/pu_script.cpp" "$PU_BUILD"/o/*.o -lOgreMain -o "$PU_BUILD/oracle"
(cd "$PU_BUILD" && ./oracle "$@")
