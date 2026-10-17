#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
AERA_TREE="${AERA_TREE:?Set AERA_TREE to the AERA Android source checkout}"
REGISTRY="${AERA_PLUGIN_REGISTRY:-$ROOT/../registry}"
export AERA_TREE
if [[ ! -x "$ROOT/vendor/fio/fio" ]]; then
  (
    cd "$ROOT/vendor/fio"
    CC="$ROOT/tools/cc-aarch64" ./configure --cpu=aarch64 --build-static \
      --disable-native --disable-numa --disable-rdma --disable-rados \
      --disable-rbd --disable-http --disable-gfapi --disable-libnfs \
      --disable-lex --disable-pmem --disable-xnvme --disable-isal \
      --disable-isal64 --disable-libblkio --disable-libzbc \
      --disable-tcmalloc --disable-dfs --disable-shm
    make -j16 fio
  )
fi
mkdir -p "$ROOT/stage/usr/bin" "$ROOT/stage/usr/share/licenses" "$ROOT/build"
"$ROOT/tools/cc-aarch64" -std=c11 -Os -Wall -Wextra -Werror -Wno-misleading-indentation \
  -ffunction-sections -fdata-sections -pthread \
  "$ROOT/source/main.c" "$ROOT/source/bench.c" "$ROOT/source/settings.c" \
  -Wl,--gc-sections -o "$ROOT/stage/usr/bin/aera-plugin"
install -m 0755 "$ROOT/vendor/fio/fio" "$ROOT/stage/usr/bin/fio"
"$AERA_TREE/prebuilts/clang/host/linux-x86/clang-r530567/bin/llvm-strip" \
  "$ROOT/stage/usr/bin/fio" "$ROOT/stage/usr/bin/aera-plugin"
install -m 0644 "$ROOT/vendor/fio/COPYING" "$ROOT/stage/usr/share/licenses/fio-GPL-2.0.txt"
install -m 0644 "$ROOT/vendor/jsmn/LICENSE" "$ROOT/stage/usr/share/licenses/jsmn-MIT.txt"
python3 "$ROOT/source/pack.py" "$ROOT/stage" "$ROOT/build"
python3 - "$ROOT" <<'PY'
import json, sys
from pathlib import Path
root = Path(sys.argv[1])
manifest = json.loads((root / 'plugin.json').read_text())
meta = json.loads((root / 'build/metadata.json').read_text())
manifest.update(payload_size=meta['compressed'], payload_sha256=meta['sha256'],
                expanded_size=meta['expanded'], expanded_sha256=meta['expanded_sha256'],
                member_count=meta['files'])
(root / 'build/plugin.json').write_text(json.dumps(manifest, indent=2) + '\n')
PY
VERSION=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$ROOT/plugin.json")
python3 "$REGISTRY/scripts/package_aerap.py" \
  --manifest "$ROOT/build/plugin.json" --payload "$ROOT/build/runtime.xz" \
  --output "$ROOT/build/AERA-Storage-Benchmark-$VERSION.aerap"
file "$ROOT/stage/usr/bin/aera-plugin" "$ROOT/stage/usr/bin/fio"
