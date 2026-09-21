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
bufh = (root/"buf.h").read_text()
bufh_body = re.sub(r'^#ifndef CTTP_BUF_H$|^#define CTTP_BUF_H$|^#endif.*$', '', bufh, flags=re.M)
header = header.replace('#include "buf.h"',
                        bufh_body.replace('#include <stddef.h>','').strip('\n'))
# main.c is the demo app, not part of the lib — headers/api_intro only
modules = ["buf", "log", "http", "router", "static", "server"]
parts = []
for m in modules:
    code = (root/f"{m}.c").read_text()
    code = re.sub(r'#include "buf\.h"|#include "cttp\.h"', '', code)
    parts.append(f"/* ==== from internals/{m}.c ==== */\n" + code)
impl = "#ifdef CTTP_IMPLEMENTATION\n\n#include <stdarg.h>\n#include <sys/stat.h>\n"
impl += "\n\n".join(parts) + "\n#endif /* CTTP_IMPLEMENTATION */\n"
pathlib.Path("include/cttp.h").write_text(
    header + "\n/* ================= one-file IMPLEMENTATION ================= */\n" + impl)
print("include/cttp.h regenerated from internals/")
EOF
