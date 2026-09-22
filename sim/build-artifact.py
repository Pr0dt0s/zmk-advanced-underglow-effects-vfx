#!/usr/bin/env python3
"""Fold the simulator into one self-contained HTML file.

Claude Artifacts serve pages under a CSP that blocks fetch, and wrap whatever
they are given in their own document skeleton. So this inlines the stylesheet,
the script, the key geometry and the WebAssembly module (base64, decoded in
the page) and emits only the body content, leaving nothing to load over the
network.

Run after sim/build.sh, which produces docs/sim.
"""

import base64
import json
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parent.parent
src = root / "docs" / "sim"
out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / "docs" / "vfx-simulator.html"

html = (src / "index.html").read_text()
css = (src / "style.css").read_text()
js = (src / "app.js").read_text()
host_js = (src / "host.js").read_text()
keys = json.loads((src / "lily58-keys.json").read_text())
wasm = base64.b64encode((src / "vfx.wasm").read_bytes()).decode("ascii")

# The artifact supplies <!doctype>, <html>, <head> and <body>, so hand it only
# what goes inside the body.
body = re.search(r"<body>(.*)</body>", html, re.S).group(1)
body = re.sub(r'\s*<script type="module" src="app\.js"></script>', "", body)
body = re.sub(r'\s*<script type="module" src="host\.js"></script>', "", body)

title = re.search(r"<title>(.*?)</title>", html).group(1)

# Both fetches become constants that are already in the page.
js = js.replace(
    """  const [wasmBytes, keyJson] = await Promise.all([
    fetch('vfx.wasm').then(r => r.arrayBuffer()),
    fetch('lily58-keys.json').then(r => r.json()),
  ]);""",
    """  /* Inlined rather than fetched: the artifact CSP blocks network requests,
     and a self-contained page also means the simulator keeps working from a
     saved copy of the file. */
  const wasmBytes = Uint8Array.from(atob(VFX_WASM_BASE64), c => c.charCodeAt(0));
  const keyJson = LILY58_KEYS;""",
)

out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(
    f"<title>{title}</title>\n"
    f"<style>\n{css}</style>\n"
    f"{body}\n"
    f'<script type="module">\n'
    f"const LILY58_KEYS = {json.dumps(keys, separators=(',', ':'))};\n"
    f'const VFX_WASM_BASE64 = "{wasm}";\n\n'
    f"{js}</script>\n"
    # A separate inline module, not concatenated into the one above: both
    # files declare their own top-level `$`, and only two distinct module
    # scopes let that not collide.
    f'<script type="module">\n{host_js}</script>\n'
)

print(f"wrote {out} ({out.stat().st_size / 1024:.0f} KB, wasm inlined as {len(wasm) / 1024:.0f} KB base64)")
