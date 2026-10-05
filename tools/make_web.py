"""Write the browser patcher (docs/index.html, served by GitHub Pages) from the release patcher in releases/.

  python3 tools/make_web.py        # after make_patcher.py and copying its output to releases/

The page carries the same data as the Python patcher (only the mod's changed bytes) and does the same thing in the
browser: the user picks their own stock firmware file, the page checks its SHA-256, patches it, checks the result and
hands back BLUEEURO.BIN. Nothing is uploaded anywhere.
"""
import glob, html, os, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
rel = sorted(glob.glob(os.path.join(ROOT, "releases", "bluebox-mod-v*-patcher.py")))
assert len(rel) == 1, f"expected one patcher in releases/, found {rel}"
src = open(rel[0]).read()
version = re.search(r"bluebox-mod-(v[\d.]+)-patcher", rel[0]).group(1)
stock = re.search(r'STOCK_SHA256 = "([0-9a-f]{64})"', src).group(1)
mod = re.search(r'MOD_SHA256 = "([0-9a-f]{64})"', src).group(1)
patch = "".join(re.search(r'PATCH = """(.*?)"""', src, re.S).group(1).split())

page = open(os.path.join(ROOT, "tools", "web_template.html")).read()
for k, v in {"VERSION": version, "STOCK_SHA256": stock, "MOD_SHA256": mod, "PATCH": patch}.items():
    page = page.replace("{{" + k + "}}", html.escape(v))
os.makedirs(os.path.join(ROOT, "docs"), exist_ok=True)
open(os.path.join(ROOT, "docs", "index.html"), "w").write(page)
print(f"docs/index.html: {version}, {len(page)} bytes")
