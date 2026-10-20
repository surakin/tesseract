#!/usr/bin/env python3
"""Run one shell's smoke test end to end.

  run.py --name qt6 --ca CA --secret-file F --workdir D [--isolate xdg] -- APP...

Seeds a fresh user, a plain room and an encrypted room on the running Synapse,
then launches the app (built with TESSERACT_ENABLE_SMOKE_MODE) three times on
the same isolated profile, with the TESSERACT_SMOKE_* environment and
`--smoke-test=<result-PHASE.json>` appended:

  fresh       password sign-in, sync, receive the PING, send the PONG in the
              plain room, then send an E2EE PONG in the encrypted room. The
              server must have the PONG in plaintext, the E2EE one only as
              m.room.encrypted, and the user must have gained one device.
  restore     relaunch without credentials: the stored session must bring the
              app back signed in. Receives a new PING, sends a new PONG, then
              signs out; the server must have dropped the app's device.
  signed-out  relaunch again: the login view shows and no session comes back.

Stops at the first failing phase. Exits non-zero on failure.

--isolate picks how the app is kept off a real profile: `xdg` and `windows`
point its config/data/cache variables into the work dir. `profile` (macOS)
runs it with `--profile=smoke-NAME-NONCE` instead, because relocating HOME
leaves macOS without a login keychain and the first keychain write blocks on
a dialog; that profile's folders under ~/Library are removed afterwards, and
the sign-out phase removes its Keychain entry.
"""

import argparse
import json
import os
import secrets
import shutil
import signal
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from seed import seed  # noqa: E402
from smokelib import Server  # noqa: E402
from seed import post_ping  # noqa: E402
from verify import app_devices, encrypted_pong_sent, pong_delivered  # noqa: E402


def isolated_env(mode, root: Path):
    """Own config/data/cache dirs so the run never touches a real profile."""
    env = {}
    if mode == "xdg":
        for var, sub in (
            ("XDG_CONFIG_HOME", "config"),
            ("XDG_DATA_HOME", "data"),
            ("XDG_CACHE_HOME", "cache"),
        ):
            (root / sub).mkdir(parents=True, exist_ok=True)
            env[var] = str(root / sub)
    elif mode == "windows":
        for var, sub in (("APPDATA", "appdata"), ("LOCALAPPDATA", "localappdata")):
            (root / sub).mkdir(parents=True, exist_ok=True)
            env[var] = str(root / sub)
    return env


def kill_tree(proc):
    try:
        if os.name == "nt":
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                           capture_output=True)
        else:
            os.killpg(proc.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


def summary(text):
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if path:
        with open(path, "a") as f:
            f.write(text + "\n")


# What session_store.cpp leaves in session.json once the session itself is in
# the OS secret store; anything else there is a plaintext session.
SECRET_STORE_SENTINEL = '{"v":2}'


def plaintext_sessions(root: Path):
    """Returns (session.json files found, those holding a plaintext session)."""
    found = sorted(root.rglob("accounts/*/session.json"))
    plain = [p for p in found if p.read_text(errors="replace").strip() != SECRET_STORE_SENTINEL]
    return found, plain


def run_app(args, cmd, root, phase, env):
    """Launches the app once; returns (ok, reason, parsed result.json or None)."""
    result_path = root / f"result-{phase}.json"
    app_log = root / f"app-{phase}.log"
    result_path.unlink(missing_ok=True)
    full = cmd + [f"--smoke-test={result_path}", "--log-level=debug"]
    print(f"[{args.name}] {phase}: running: {' '.join(full)}", flush=True)
    kwargs = {} if os.name == "nt" else {"start_new_session": True}
    ok = True
    reason = ""
    with open(app_log, "wb") as log:
        proc = subprocess.Popen(full, env=env, stdout=log, stderr=subprocess.STDOUT,
                                **kwargs)
        try:
            rc = proc.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            kill_tree(proc)
            proc.wait()
            rc = None
            ok, reason = False, f"app still running after {args.timeout}s"

    result = None
    if result_path.exists():
        try:
            result = json.loads(result_path.read_text())
        except ValueError:
            pass
    if ok and rc != 0:
        ok, reason = False, f"app exited {rc}"
    if ok and not (result and result.get("ok")):
        ok = False
        reason = "no result file" if result is None else (
            f"step '{result.get('failed_step')}' failed: {result.get('error')}")
    return ok, reason, result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--base", default="https://localhost:8448")
    ap.add_argument("--ca", required=True)
    ap.add_argument("--secret-file", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--isolate", choices=["xdg", "windows", "profile", "none"],
                    default="none")
    ap.add_argument("--timeout", type=int, default=330)
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    args = ap.parse_args()

    cmd = args.cmd[1:] if args.cmd[:1] == ["--"] else args.cmd
    if not cmd:
        ap.error("missing app command after --")

    root = Path(args.workdir).resolve() / args.name
    root.mkdir(parents=True, exist_ok=True)
    state_path = root / "state.json"
    for stale in root.glob("result-*.json"):
        stale.unlink()

    server = Server(args.base, args.ca)
    server.wait_ready()
    state = seed(server, Path(args.secret_file).read_text().strip(), args.name)
    state_path.write_text(json.dumps(state))

    base_env = os.environ.copy()
    base_env.update(isolated_env(args.isolate, root))

    # Where the app keeps account data, for the plaintext-session check.
    session_root = root
    profile_dirs = []
    if args.isolate == "profile":
        profile = f"smoke-{args.name}-{secrets.token_hex(3)}"
        cmd = cmd + [f"--profile={profile}"]
        library = Path.home() / "Library"
        session_root = library / "Application Support" / f"Tesseract-{profile}"
        profile_dirs = [session_root, library / "Caches" / f"Tesseract-{profile}"]
    try:
        return run_phases(args, cmd, root, server, state, state_path, base_env, session_root)
    finally:
        for d in profile_dirs:
            shutil.rmtree(d, ignore_errors=True)


def run_phases(args, cmd, root, server, state, state_path, base_env, session_root):
    """Runs the three launches; returns the process exit status."""
    def launch(phase, extra):
        env = dict(base_env, TESSERACT_SMOKE_PHASE=phase, **extra)
        return run_app(args, cmd, root, phase, env)

    def one_device(phase_label):
        extra = app_devices(server, state) - {state["app_reg_device"]}
        if len(extra) != 1:
            return f"expected one app device after {phase_label}, found {len(extra)}"
        return ""

    def fresh_checks():
        if not pong_delivered(server, state):
            return "app reported success but the server never saw the PONG"
        if not encrypted_pong_sent(server, state):
            return "the E2EE PONG is missing, or was sent in plaintext"
        found, plain = plaintext_sessions(session_root)
        if not found:
            return "no session.json in the isolated profile after sign-in"
        if plain:
            return f"session stored in plaintext, not the OS secret store: {plain[0]}"
        return one_device("sign-in")

    def restore_checks():
        if not pong_delivered(server, state):
            return "app reported success but the server never saw the new PONG"
        left = app_devices(server, state) - {state["app_reg_device"]}
        if left:
            return f"sign-out left the app's device on the server: {sorted(left)}"
        return ""

    fresh_env = dict(
        TESSERACT_SMOKE_HOMESERVER=state["base"],
        TESSERACT_SMOKE_USER=state["app_user"],
        TESSERACT_SMOKE_PASSWORD=state["app_password"],
        TESSERACT_SMOKE_ROOM_ID=state["room_id"],
        TESSERACT_SMOKE_PING=state["ping"],
        TESSERACT_SMOKE_PONG=state["pong"],
        TESSERACT_SMOKE_E2EE_ROOM_ID=state["e2ee_room_id"],
        TESSERACT_SMOKE_E2EE_PONG=state["e2ee_pong"],
    )

    def phases():
        yield "fresh", lambda: fresh_env, fresh_checks
        # A new ping so the restore phase proves it receives live, not stale.
        def restore_env():
            ping, pong = post_ping(server, state["bot_token"], state["room_id"])
            state["ping"], state["pong"] = ping, pong
            state_path.write_text(json.dumps(state))
            return dict(
                TESSERACT_SMOKE_ROOM_ID=state["room_id"],
                TESSERACT_SMOKE_PING=ping,
                TESSERACT_SMOKE_PONG=pong,
            )

        yield "restore", restore_env, restore_checks
        yield "signed-out", dict, lambda: ""

    for phase, make_env, checks in phases():
        ok, reason, result = launch(phase, make_env())
        if ok:
            reason = checks()
            ok = not reason
        if result:
            print(f"[{args.name}] {phase} result: {json.dumps(result)}")
        if ok:
            print(f"[{args.name}] {phase}: PASS")
            summary(f"- **{args.name}** {phase}: pass")
            continue
        print(f"[{args.name}] {phase}: FAIL: {reason}", file=sys.stderr)
        tail = (root / f"app-{phase}.log").read_text(errors="replace").splitlines()[-40:]
        print(f"[{args.name}] last app log lines:\n" + "\n".join(tail), file=sys.stderr)
        summary(f"- **{args.name}** {phase}: FAIL, {reason}")
        return 1

    print(f"[{args.name}] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
