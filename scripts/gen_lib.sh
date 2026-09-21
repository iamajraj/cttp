#!/bin/sh
# Regenerate include/cttp.h from the annotated teaching sources in
# internals/ after editing them.
# Usage: sh scripts/gen_lib.sh
set -e
cd "$(dirname "$0")/.."
python3 - <<'EOF'
import re, pathlib
root = pathlib.Path("internals")
header = (root/"cttp.h").read_text()
# the public header must not #include a private one — buf_t is defined inline
modules = ["buf", "log", "api", "http", "router", "static", "server"]  # main.c = demo, not lib
parts = []
for m in modules:
    code = (root/f"{m}.c").read_text()
    code = re.sub(r'#include "buf\.h"|#include "cttp\.h"', '', code)
    parts.append(f"/* ==== from internals/{m}.c ==== */\n" + code)
impl = "#ifdef CTTP_IMPLEMENTATION\n\n#include <stdarg.h>\n#include <sys/stat.h>\n#include <poll.h>\n"
impl += "\n\n".join(parts) + "\n#endif /* CTTP_IMPLEMENTATION */\n"
pathlib.Path("include/cttp.h").write_text(
    header + "\n/* ================= one-file IMPLEMENTATION ================= */\n" + impl)
print("include/cttp.h regenerated from internals/")
EOF
