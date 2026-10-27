"""Signs and notarizes Openviz's Mac disk image, on GitHub's Mac machines (.github/workflows/sign-mac.yml).

publish/publish.py makes the .dmg with an ad hoc signature and puts it on a draft release. This takes
it apart, signs every program in the app and then the app itself with the Developer ID Application
certificate from the repository's secrets (hardened runtime, secure timestamp), makes the disk image
again, signs it, has Apple notarize it with the App Store Connect API key, staples the ticket to it,
and checks that Gatekeeper accepts the disk image and the app in it. Openviz's updater installs only an
app signed by the same team and accepted by Gatekeeper (launcher/updater.py).

    python3 sign_mac_dmg.py <in.dmg> <out.dmg> --identity <name> [--keychain <path>]
                            --key <AuthKey.p8> --key-id <id> --issuer <id>        (an App Store Connect API key)
                            or --apple-id <email> --team-id <id>, the app-specific password in NOTARY_APP_PASSWORD
    python3 sign_mac_dmg.py <in.dmg> <out.dmg> --identity - --no-notarize     (to try it without secrets)

Standard library only.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

MACH_O = {b"\xfe\xed\xfa\xce", b"\xfe\xed\xfa\xcf", b"\xce\xfa\xed\xfe", b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"}


def run(command):
    done = subprocess.run(command, capture_output=True, text=True)
    if done.returncode != 0:
        raise SystemExit(f"{os.path.basename(command[0])} {command[1] if len(command) > 1 else ''} failed:\n{(done.stdout + done.stderr)[-3000:]}")
    return done.stdout + done.stderr


def programs_in(app):
    """The Mach-O files an app carries besides its own executable (the core's inspector), innermost first."""
    found = []
    main = os.path.join(app, "Contents", "MacOS")
    for folder, _, names in os.walk(app):
        if os.path.abspath(folder) == os.path.abspath(main):
            continue
        for name in names:
            path = os.path.join(folder, name)
            if os.path.islink(path) or not os.path.isfile(path):
                continue
            with open(path, "rb") as handle:
                if handle.read(4) in MACH_O:
                    found.append(path)
    return sorted(found, key=lambda path: -path.count(os.sep))


def sign(path, identity, keychain="", program=True):
    command = ["codesign", "--force", "--sign", identity]
    if identity != "-":
        command += ["--timestamp"] + (["--options", "runtime"] if program else [])
    if keychain:
        command += ["--keychain", keychain]
    run(command + [path])


def notarize(disk, credentials):
    """`credentials`: notarytool's own options, an API key's or an Apple ID's."""
    print("Notarizing with Apple (a few minutes)...", flush=True)
    done = subprocess.run(["xcrun", "notarytool", "submit", disk, *credentials, "--wait", "--output-format", "json"], capture_output=True, text=True)
    try:
        answer = json.loads(done.stdout or "{}")
    except ValueError:
        answer = {}
    if done.returncode != 0 or answer.get("status") != "Accepted":
        why = subprocess.run(["xcrun", "notarytool", "log", answer["id"], *credentials], capture_output=True, text=True).stdout if answer.get("id") else ""
        raise SystemExit(f"Apple did not notarize it ({answer.get('status') or done.stderr.strip()}).\n{why[-3000:]}")
    run(["xcrun", "stapler", "staple", disk])


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("source")
    p.add_argument("target")
    p.add_argument("--identity", required=True, help='the certificate\'s name, "Developer ID Application: ...", or - for ad hoc')
    p.add_argument("--keychain", default="")
    p.add_argument("--key", help="the App Store Connect API key (.p8)")
    p.add_argument("--key-id")
    p.add_argument("--issuer")
    p.add_argument("--apple-id", help="the developer account's Apple ID, with --team-id and the app-specific password in NOTARY_APP_PASSWORD")
    p.add_argument("--team-id")
    p.add_argument("--no-notarize", action="store_true")
    args = p.parse_args(argv)
    password = os.environ.get("NOTARY_APP_PASSWORD", "")
    if args.key and args.key_id and args.issuer:
        credentials = ["--key", args.key, "--key-id", args.key_id, "--issuer", args.issuer]
    elif args.apple_id and args.team_id and password:
        credentials = ["--apple-id", args.apple_id, "--team-id", args.team_id, "--password", password]
    elif not args.no_notarize:
        p.error("notarizing needs --key, --key-id and --issuer, or --apple-id and --team-id with NOTARY_APP_PASSWORD set (or --no-notarize)")

    work = tempfile.mkdtemp(prefix="openviz-sign-")
    try:
        mount = os.path.join(work, "mounted")
        os.makedirs(mount)
        run(["hdiutil", "attach", "-nobrowse", "-readonly", "-noautoopen", "-mountpoint", mount, args.source])
        disk = os.path.join(work, "disk")
        os.makedirs(disk)
        try:
            run(["ditto", os.path.join(mount, "Openviz.app"), os.path.join(disk, "Openviz.app")])
        finally:
            run(["hdiutil", "detach", "-quiet", mount])
        app = os.path.join(disk, "Openviz.app")
        with open(os.path.join(app, "Contents", "Resources", "Openviz", "openviz-version.txt"), encoding="utf-8") as handle:
            version = handle.read().strip()

        # From the inside out: what the app carries, then the app, which seals all of it.
        for program in programs_in(app):
            sign(program, args.identity, args.keychain)
            print(f"signed {os.path.relpath(program, disk)}", flush=True)
        sign(app, args.identity, args.keychain)
        run(["codesign", "--verify", "--strict", "--deep", app])
        os.symlink("/Applications", os.path.join(disk, "Applications"))
        if os.path.exists(args.target):
            os.remove(args.target)
        run(["hdiutil", "create", "-quiet", "-volname", "Openviz " + version.replace("-", " "), "-srcfolder", disk, "-format", "UDZO", args.target])
        sign(args.target, args.identity, args.keychain, program=False)

        if not args.no_notarize:
            notarize(args.target, credentials)
            run(["spctl", "--assess", "--type", "open", "--context", "context:primary-signature", args.target])
            run(["hdiutil", "attach", "-nobrowse", "-readonly", "-noautoopen", "-mountpoint", mount, args.target])
            try:
                run(["spctl", "--assess", "--type", "execute", os.path.join(mount, "Openviz.app")])
            finally:
                run(["hdiutil", "detach", "-quiet", mount])
        said = run(["codesign", "-dv", "--verbose=2", app])
        team = next((line.split("=", 1)[1] for line in said.splitlines() if line.startswith("TeamIdentifier=")), "not set")
        print(f"{os.path.basename(args.target)}: Openviz {version}, team {team}, {'notarized and stapled' if not args.no_notarize else 'not notarized'}", flush=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
