#!/usr/bin/env python3
"""Write a throwaway Synapse config and start it for the smoke run.

  synapse_up.py --dir DATA --certs CERTS          start detached, wait until ready
  synapse_up.py --dir DATA --certs CERTS --foreground   exec in the foreground

The homeserver is `localhost` on https://localhost:8448 (TLS from make_ca.py),
SQLite, no federation, rate limits raised. The registration shared secret is
written to DATA/registration_secret for seed.py. Needs matrix-synapse
(requirements.txt) in the running interpreter.
"""

import argparse
import os
import secrets
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smokelib import Server  # noqa: E402

PORT = 8448

LIMIT = "{ per_second: 1000, burst_count: 100000 }"


def write_config(data: Path, certs: Path, bind: str) -> Path:
    data.mkdir(parents=True, exist_ok=True)
    (data / "media").mkdir(exist_ok=True)
    secret = secrets.token_urlsafe(32)
    (data / "registration_secret").write_text(secret)

    # Plain INFO to a file so a failed run can upload the log.
    (data / "log.config").write_text(
        "version: 1\n"
        "formatters:\n"
        "  plain:\n"
        "    format: '%(asctime)s - %(name)s - %(levelname)s - %(message)s'\n"
        "handlers:\n"
        "  file:\n"
        "    class: logging.FileHandler\n"
        "    formatter: plain\n"
        f"    filename: '{(data / 'synapse.log').as_posix()}'\n"
        "root:\n"
        "  level: INFO\n"
        "  handlers: [file]\n"
        "disable_existing_loggers: false\n"
    )

    cfg = data / "homeserver.yaml"
    cfg.write_text(
        'server_name: "localhost"\n'
        f"pid_file: '{(data / 'homeserver.pid').as_posix()}'\n"
        "listeners:\n"
        f"  - port: {PORT}\n"
        f"    bind_addresses: ['{bind}']\n"
        "    type: http\n"
        "    tls: true\n"
        "    resources:\n"
        "      - names: [client]\n"
        "        compress: false\n"
        f"tls_certificate_path: '{(certs / 'server.crt').as_posix()}'\n"
        f"tls_private_key_path: '{(certs / 'server.key').as_posix()}'\n"
        "database:\n"
        "  name: sqlite3\n"
        "  args:\n"
        f"    database: '{(data / 'homeserver.db').as_posix()}'\n"
        f"media_store_path: '{(data / 'media').as_posix()}'\n"
        f"signing_key_path: '{(data / 'localhost.signing.key').as_posix()}'\n"
        f"log_config: '{(data / 'log.config').as_posix()}'\n"
        f'registration_shared_secret: "{secret}"\n'
        "enable_registration: false\n"
        "report_stats: false\n"
        "suppress_key_server_warning: true\n"
        "trusted_key_servers: []\n"
        "federation_domain_whitelist: []\n"
        f"rc_message: {LIMIT}\n"
        f"rc_registration: {LIMIT}\n"
        f"rc_joins: {{ local: {LIMIT}, remote: {LIMIT} }}\n"
        f"rc_invites: {{ per_room: {LIMIT}, per_user: {LIMIT}, per_issuer: {LIMIT} }}\n"
        "rc_login:\n"
        f"  address: {LIMIT}\n"
        f"  account: {LIMIT}\n"
        f"  failed_attempts: {LIMIT}\n"
    )
    return cfg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--certs", required=True)
    ap.add_argument("--foreground", action="store_true")
    # WSL2 only forwards a distro's ports to Windows reliably when they are
    # not bound to the distro's own loopback.
    ap.add_argument("--bind", default="127.0.0.1")
    args = ap.parse_args()

    data = Path(args.dir).resolve()
    certs = Path(args.certs).resolve()
    cfg = write_config(data, certs, args.bind)

    base = [sys.executable, "-m", "synapse.app.homeserver", "--config-path", str(cfg)]
    subprocess.run(base + ["--generate-keys"], check=True)

    if args.foreground:
        os.execv(base[0], base)

    log = open(data / "synapse.stdout.log", "ab")
    kwargs = {}
    if os.name == "nt":
        kwargs["creationflags"] = 0x00000008 | 0x00000200  # DETACHED | NEW_GROUP
    else:
        kwargs["start_new_session"] = True
    subprocess.Popen(base, stdout=log, stderr=log, stdin=subprocess.DEVNULL, **kwargs)

    Server(f"https://localhost:{PORT}", str(certs / "ca.crt")).wait_ready(120)
    print(f"synapse ready on https://localhost:{PORT}")


if __name__ == "__main__":
    main()
