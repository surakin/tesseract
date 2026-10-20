#!/usr/bin/env python3
"""Create a throwaway CA and a localhost server certificate for the smoke run.

Writes ca.crt, server.crt and server.key into the output directory. The app
only accepts https:// homeservers, so CI trusts ca.crt in the OS store and
Synapse serves server.crt. Needs the `openssl` CLI (set $OPENSSL to override).
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path


def run(openssl, *args):
    subprocess.run([openssl, *args], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    args = ap.parse_args()

    openssl = os.environ.get("OPENSSL", "openssl")
    out = Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)

    # A self-contained config: LibreSSL (macOS) and Git-for-Windows openssl
    # ship different default configs, so don't rely on -addext/-extensions.
    ca_cnf = out / "ca.cnf"
    ca_cnf.write_text(
        "[req]\n"
        "distinguished_name=dn\n"
        "x509_extensions=v3_ca\n"
        "prompt=no\n"
        "[dn]\n"
        "CN=Tesseract smoke CA\n"
        "[v3_ca]\n"
        "basicConstraints=critical,CA:TRUE\n"
        "keyUsage=critical,keyCertSign,cRLSign\n"
        "subjectKeyIdentifier=hash\n"
    )
    srv_ext = out / "server.ext"
    srv_ext.write_text(
        "basicConstraints=CA:FALSE\n"
        "keyUsage=critical,digitalSignature,keyEncipherment\n"
        "extendedKeyUsage=serverAuth\n"
        "subjectAltName=DNS:localhost,IP:127.0.0.1\n"
    )

    run(openssl, "genrsa", "-out", str(out / "ca.key"), "2048")
    run(openssl, "req", "-x509", "-new", "-key", str(out / "ca.key"),
        "-sha256", "-days", "2", "-config", str(ca_cnf),
        "-out", str(out / "ca.crt"))
    run(openssl, "genrsa", "-out", str(out / "server.key"), "2048")
    run(openssl, "req", "-new", "-key", str(out / "server.key"),
        "-subj", "/CN=localhost", "-out", str(out / "server.csr"))
    run(openssl, "x509", "-req", "-in", str(out / "server.csr"),
        "-CA", str(out / "ca.crt"), "-CAkey", str(out / "ca.key"),
        "-CAcreateserial", "-days", "2", "-sha256",
        "-extfile", str(srv_ext), "-out", str(out / "server.crt"))
    print(f"wrote {out / 'ca.crt'}, {out / 'server.crt'}, {out / 'server.key'}")


if __name__ == "__main__":
    sys.exit(main())
