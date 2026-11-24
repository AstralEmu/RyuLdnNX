#!/usr/bin/env bash
# Local CodeQL run, same inputs as .github/workflows/codeql.yml: C++ database
# traced over the host test build and the cross-compiled sysmodule, analysed
# with the queries and paths of .github/codeql-config.yml. Any result fails.
# Needs the CodeQL bundle mounted at $CODEQL_HOME (see the codeql compose
# service); nothing here depends on a version or a host path.
set -euo pipefail

CODEQL="${CODEQL_HOME:?CODEQL_HOME must point to the unpacked CodeQL bundle}/codeql"
[ -x "$CODEQL" ] || { echo "codeql: no CodeQL bundle at $CODEQL (set CODEQL_HOME on the host)" >&2; exit 2; }
WORK="${CODEQL_WORK:-/workspace/build-logs/codeql}"
DB="$WORK/db"
SARIF="$WORK/results.sarif"
mkdir -p "$WORK"
rm -rf "$DB"

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
export PATH="$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH"

LIB_DIR=/workspace/sysmodule/Atmosphere-libs/libstratosphere/lib/nintendo_nx_arm64_armv8a/release
PREBUILT=/opt/ryu_ldn_nx/libstratosphere/lib/nintendo_nx_arm64_armv8a/release/libstratosphere.a
if [ ! -f "$LIB_DIR/libstratosphere.a" ]; then
    mkdir -p "$LIB_DIR"
    ln -s "$PREBUILT" "$LIB_DIR/libstratosphere.a"
fi

# The .npdm is generated before tracing: the tracer pollutes errno, which
# breaks npdmtool (same reason as in the workflow).
APP_JSON="$WORK/app.json"
cp /workspace/sysmodule/res/app.json "$APP_JSON"
npdmtool "$APP_JSON" /workspace/sysmodule/ryu_ldn_nx.npdm

BUILD="$WORK/build.sh"
cat > "$BUILD" <<EOS
#!/usr/bin/env bash
set -euo pipefail
make -C /workspace/tests clean
make -C /workspace/tests COVERAGE=1 test
make -C /workspace/sysmodule -j"\$(nproc)" all APP_JSON="$APP_JSON"
EOS
chmod +x "$BUILD"

"$CODEQL" database create "$DB" --language=cpp --source-root=/workspace \
    --codescanning-config=/workspace/.github/codeql-config.yml \
    --command="$BUILD"

"$CODEQL" database analyze "$DB" --format=sarifv2.1.0 --output="$SARIF" \
    --threads=0 --sarif-category=/language:cpp

python3 - "$SARIF" <<'EOP'
import fnmatch, json, re, sys
# The CLI ignores `paths-ignore`; apply the config's list like the workflow.
config = open("/workspace/.github/codeql-config.yml").read()
section = config.split("paths-ignore:", 1)[1].split("queries:", 1)[0]
ignored = re.findall(r"^\s*-\s*(\S+)\s*(?:#.*)?$", section, re.M)
def kept(uri):
    return not any(fnmatch.fnmatch(uri, p) or
                   uri.startswith(p.rstrip("*").rstrip("/") + "/")
                   for p in ignored)
runs = json.load(open(sys.argv[1]))["runs"]
results = [r for run in runs for r in run["results"]
           if kept(r["locations"][0]["physicalLocation"]["artifactLocation"]["uri"])]
for r in results:
    loc = r["locations"][0]["physicalLocation"]
    print(f'{loc["artifactLocation"]["uri"]}:{loc["region"]["startLine"]}: '
          f'{r["ruleId"]}: {r["message"]["text"]}')
print(f"codeql: {len(results)} alert(s)")
sys.exit(1 if results else 0)
EOP
