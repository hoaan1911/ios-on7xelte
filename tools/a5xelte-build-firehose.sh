#!/usr/bin/env bash
set -euo pipefail

# Rebuild the libdispatch firehose archive for Exynos7580's ARMv8.0 A53.
# Run after darwin-xnu-build has staged libdispatch and the kernel fakeroot.
if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: $0 DXB_DIRECTORY [OUTPUT_DIRECTORY]" >&2
    exit 2
fi

dxb=$(cd "$1" && pwd -P)
source_dir="$dxb/libdispatch"
fakeroot="$dxb/fakeroot"
archive="$fakeroot/usr/local/lib/kernel/libfirehose_kernel.a"
[[ -d "$source_dir" && -f "$archive" ]] || {
    echo "libdispatch or staged firehose archive missing in $dxb" >&2
    exit 1
}

if [[ $# -eq 2 ]]; then
    output_dir="$2"
    mkdir -p "$output_dir"
else
    output_dir=$(mktemp -d /tmp/a5xelte-firehose.XXXXXX)
fi
output_dir=$(cd "$output_dir" && pwd -P)

(
    cd "$source_dir"
    env LD="$(xcrun -find clang)" LDPLUSPLUS="$(xcrun -find clang++)" \
        xcodebuild install -target libfirehose_kernel -sdk macosx \
        ARCHS=arm64 VALID_ARCHS=arm64 \
        OBJROOT="$output_dir/obj" SYMROOT="$output_dir/sym" \
        DSTROOT="$output_dir/root" FAKEROOT_DIR="$fakeroot" \
        OTHER_CFLAGS='-mkernel -nostdinc -Wno-packed -mcpu=cortex-a53 -march=armv8-a+crypto'
)

built="$output_dir/root/usr/local/lib/kernel/liblibfirehose_kernel.a"
[[ -f "$built" ]] || { echo "A53 firehose archive was not produced" >&2; exit 1; }
xcrun llvm-objdump -d "$built" > "$output_dir/firehose.disasm"
python3 - "$output_dir/firehose.disasm" <<'PY'
import re
import sys

lse = re.compile(
    r"\t(?:cas|ldadd|ldclr|ldeor|ldset|ldsmax|ldsmin|ldumax|ldumin|"
    r"stadd|stclr|steor|stset|stsmax|stsmin|stumax|stumin|swp)[a-z]*\t"
)
with open(sys.argv[1], encoding="utf-8") as stream:
    bad = [line.strip() for line in stream if lse.search(line)]
if bad:
    raise SystemExit("A53 firehose still contains LSE instructions:\n" + "\n".join(bad[:8]))
PY

install -m 0644 "$built" "$archive"
echo "A53 firehose installed at $archive"
echo "Build evidence: $output_dir/firehose.disasm"
