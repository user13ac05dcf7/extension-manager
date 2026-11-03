#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes signed test catalogues for run.sh, shaped like the registry's index.json.

One made-up extension, signed with a throwaway Ed25519 key. Three variants: good,
badsig (index changed after signing) and badsha (package changed after listing).
"""
import base64, hashlib, io, json, subprocess, sys, tempfile, zipfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

UUID = "proto@exm.test"


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()


def openssl(*args):
    return subprocess.run(["openssl", *args], check=True, capture_output=True).stdout


def main(out, tier):
    out.mkdir(parents=True, exist_ok=True)
    key = out / "test-key.pem"
    if not key.exists():
        openssl("genpkey", "-algorithm", "ed25519", "-out", str(key))
    public = base64.b64encode(openssl("pkey", "-in", str(key), "-pubout", "-outform", "DER")[12:]).decode()

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("metadata.json", json.dumps({"uuid": UUID, "name": "Prototype", "description": "Install test",
                                                "shell-version": ["50", "51"], "version": 1}))
        z.writestr("extension.js",
                   "import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';\n"
                   "export default class Prototype extends Extension {\n"
                   "    enable() { console.log('PROTO-EXT enabled'); }\n"
                   "    disable() { console.log('PROTO-EXT disabled'); }\n}\n")
    package = buf.getvalue()
    digest = hashlib.sha256(package).hexdigest()

    now = datetime.now(timezone.utc).replace(microsecond=0)
    index = {
        "schema_version": 1,
        "serial": int(now.timestamp()),
        "generated_at": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "expires_at": (now + timedelta(days=14)).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "minimum_shell_version": 50,
        "extensions": [{
            "uuid": UUID, "name": "Prototype", "description": "Install test", "creator": "test",
            "homepage": "https://example.invalid/", "tier": tier,
            "origin": {"site": "https://example.invalid"},
            "releases": [{"version": 1, "shell_versions": ["50", "51"], "sha256": digest,
                          "size": len(package), "download": f"downloads/{digest}.zip",
                          "license": "MIT", "license_source": "spdx-header", "capabilities": None}],
        }],
    }
    raw = canonical(index)
    with tempfile.NamedTemporaryFile() as message:
        message.write(raw)
        message.flush()
        signature = openssl("pkeyutl", "-sign", "-rawin", "-inkey", str(key), "-in", message.name)

    for variant in ("good", "badsig", "badsha"):
        site = out / variant
        (site / "downloads").mkdir(parents=True, exist_ok=True)
        (site / "index.json").write_bytes(raw.replace(b"Install test", b"Install TEST", 1)
                                          if variant == "badsig" else raw)
        (site / "index.json.sig").write_text(base64.b64encode(signature).decode())
        (site / f"downloads/{digest}.zip").write_bytes(
            package[:-1] + bytes([package[-1] ^ 1]) if variant == "badsha" else package)
    (out / "key.pub").write_text(public)


if __name__ == "__main__":
    main(Path(sys.argv[1]), sys.argv[2] if len(sys.argv) > 2 else "approved")
