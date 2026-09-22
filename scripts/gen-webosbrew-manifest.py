#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path
from urllib.parse import urlparse

APP_ID = "io.github.sysdvrwebos.client"
VERSION = "1.0.0"
TITLE = "SysDVR"
DESCRIPTION = "Native SysDVR client for LG webOS TVs"

parser = argparse.ArgumentParser(
    description="Generate a webOS Homebrew manifest for a GitHub release."
)
parser.add_argument("--source-url", required=True,
                    help="GitHub repository URL, e.g. https://github.com/user/repo")
parser.add_argument("--tag", default="v1.0.0")
parser.add_argument("--ipk", help="Path to built arm IPK; defaults to dist/*_arm.ipk")
parser.add_argument("--icon-url", help="Public icon URL; defaults to raw GitHub main/webos/icon.png")
parser.add_argument("--output", default=f"dist/{APP_ID}.manifest.json")
args = parser.parse_args()

root = Path(__file__).resolve().parents[1]
if args.ipk:
    ipk = Path(args.ipk)
else:
    candidates = sorted((root / "dist").glob("*_arm.ipk"))
    if not candidates:
        raise SystemExit("No dist/*_arm.ipk found. Build the app first.")
    ipk = candidates[-1]

if not ipk.is_file():
    raise SystemExit(f"IPK not found: {ipk}")

source = args.source_url.rstrip("/")
parsed = urlparse(source)
parts = [p for p in parsed.path.split("/") if p]
if parsed.netloc.lower() != "github.com" or len(parts) < 2:
    raise SystemExit("--source-url must look like https://github.com/OWNER/REPO")
owner, repo = parts[0], parts[1]

icon_url = args.icon_url or (
    f"https://raw.githubusercontent.com/{owner}/{repo}/main/webos/icon.png"
)
release_base = f"https://github.com/{owner}/{repo}/releases/download/{args.tag}"
ipk_url = f"{release_base}/{ipk.name}"

sha256 = hashlib.sha256(ipk.read_bytes()).hexdigest()
manifest = {
    "id": APP_ID,
    "version": VERSION,
    "type": "native",
    "title": TITLE,
    "appDescription": DESCRIPTION,
    "iconUri": icon_url,
    "sourceUrl": source,
    "rootRequired": False,
    "ipkUrl": ipk_url,
    "ipkHash": {"sha256": sha256},
    "ipkSize": ipk.stat().st_size,
}

output = root / args.output
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(output)
print(f"sha256={sha256}")
print(f"ipkSize={ipk.stat().st_size}")
