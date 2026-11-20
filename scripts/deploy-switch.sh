#!/usr/bin/env bash
# One build/package/deploy transaction. Never accept a caller-picked NRO.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${NSL_BUILD_DIR:-$ROOT/build/switch}"
ARTIFACT_ROOT="${NSL_DEPLOY_DIR:-$ROOT/build/switch-deploy}"
ADDRESS=""
PREPARE_ONLY=0
while (($#)); do
    case "$1" in
        --address) ADDRESS="${2:?address required}"; shift 2 ;;
        --prepare-only) PREPARE_ONLY=1; shift ;;
        *) echo "Usage: $0 --address <device address> [--prepare-only]" >&2; exit 2 ;;
    esac
done
[[ -n "$ADDRESS" ]] || { echo 'Device address is required.' >&2; exit 2; }
[[ -f "$BUILD_DIR/CMakeCache.txt" ]] || { echo 'Configure the Switch build with make switch or make switch-diag first.' >&2; exit 2; }
cmake --build "$BUILD_DIR" --target nsteamlink_nro -j"$(nproc)"
mkdir -p "$ARTIFACT_ROOT"
RUN_DIR="$(mktemp -d "$ARTIFACT_ROOT/run-$(date +%Y%m%d-%H%M%S)-XXXXXX")"
python3 - "$BUILD_DIR" "$RUN_DIR" "$ADDRESS" "$ROOT" <<'PY'
import hashlib,json,shutil,subprocess,sys
from pathlib import Path
build,out,address,root=map(Path,sys.argv[1:])
source=build/'app/nsteamlink.nro'
elf=build/'app/nsteamlink.elf'
if source.stat().st_mtime_ns < elf.stat().st_mtime_ns:
    raise SystemExit('NRO is older than ELF after nsteamlink_nro build; refusing deployment')
with source.open('rb') as f:
    f.seek(16)
    if f.read(4)!=b'NRO0': raise SystemExit('Expected NRO0 executable')
artifact=out/'nsteamlink.nro'
shutil.copy2(source,artifact)
manifest=dict(address=str(address),source_target='nsteamlink_nro',artifact=str(artifact.resolve()),
              sha256=hashlib.sha256(artifact.read_bytes()).hexdigest(),size=artifact.stat().st_size,
              commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
              dirty=bool(subprocess.check_output(['git','status','--porcelain'],cwd=root,text=True)))
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
PY
printf 'Deployment snapshot: %s\n' "$RUN_DIR"
if ((PREPARE_ONLY)); then exit 0; fi
NX_LINK="${DEVKITPRO:-/opt/devkitpro}/tools/bin/nxlink"
"$NX_LINK" -a "$ADDRESS" -p nsteamlink/nsteamlink.nro -s "$RUN_DIR/nsteamlink.nro" 2>&1 | tee "$RUN_DIR/nxlink.log"
