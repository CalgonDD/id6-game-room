#!/bin/sh
# Apply the Game Room QNX changes to a PPSSPP v1.17.1 source tree (with its submodules).
# Usage: apply_ppsspp_patch.sh <ppsspp-source-dir>
# The patch was made with line endings ignored, so the files it touches are first converted to LF.
set -eu
src=$1
patch_file=$(cd "$(dirname "$0")" && pwd)/ppsspp-1.17.1-qnx.diff
cd "$src"
grep '^+++ b/' "$patch_file" | sed 's#^+++ b/##; s#\t.*##' | while IFS= read -r f; do
    if [ -f "$f" ]; then sed -i 's/\r$//' "$f"; fi
done
patch -p1 --forward < "$patch_file"
echo "PPSSPP patched for QNX (Game Room)."
