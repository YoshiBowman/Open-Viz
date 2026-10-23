"""Openviz updating itself from its GitHub releases (owner, 2026-10-22: "getting the software signed so
we can push ota updates").

The newest release (not a draft; pre-releases too, as the installers take them) whose version is
newer than this copy's `openviz-version.txt` is offered. On a Mac an update is that release's
.dmg, and it is installed only if, after its download matches the checksum GitHub gives:
  - the Openviz.app in it has a valid signature from the same Apple developer team as the app it
    replaces (so an app that is not signed, as releases before signing were, never updates by
    itself: the first signed one is installed from its .dmg by hand), and
  - Gatekeeper accepts it (it is notarized by Apple), and it says it is the version offered.
Then it takes the installed app's place, and Openviz is opened again: the new app puts its project
in ~/Openviz as it always does, keeping the library and what was built on this Mac. On Windows the
installer is not signed yet, so an update is offered as its download page. A copy made with Git, or
one run without the app, is never updated in place.

Everything that goes on the network or runs a program goes through what the caller passes in, so
the tests never do either. Standard library only.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import urllib.request

REPOSITORY = "YoshiBowman/Open-Viz"
RELEASES = f"https://api.github.com/repos/{REPOSITORY}/releases"
STAGES = {"alpha": 0, "beta": 1, "rc": 2}


class UpdateRefused(RuntimeError):
    """An update that is not installed, and why, said plainly."""


def version_key(text):
    """A version such as 0.3-beta as something to compare: numbers first, then alpha < beta < rc < none."""
    text = (text or "").strip().lower().lstrip("v")
    numbers = [int(part) for part in re.findall(r"\d+", text.split("-")[0])] + [0, 0, 0]
    stage = next((rank for word, rank in STAGES.items() if word in text), 3)
    return tuple(numbers[:3]) + (stage,)


def fetch_json(url, timeout=20):
    request = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json", "User-Agent": "Openviz-updater"})
    with urllib.request.urlopen(request, timeout=timeout) as answer:
        return json.loads(answer.read().decode("utf-8"))


def newest_release(releases):
    """The newest release that carries Openviz (an Openviz-*.zip), as GitHub lists them newest first:
    {"version", "tag", "page", "notes", "assets": {name: {"url", "digest", "size"}}}, or None."""
    for release in releases or []:
        if release.get("draft"):
            continue
        assets = {asset["name"]: {"url": asset.get("browser_download_url", ""), "digest": asset.get("digest") or "", "size": asset.get("size", 0)}
                  for asset in release.get("assets", []) if isinstance(asset, dict) and asset.get("name")}
        if any(re.fullmatch(r"Openviz-.*\.zip", name) for name in assets):
            tag = release.get("tag_name", "")
            return {"version": tag.lstrip("v"), "tag": tag, "page": release.get("html_url", ""), "notes": release.get("body") or "", "assets": assets}
    return None


def offer(current, releases):
    """What the System page says: this version, the newest, and whether it is newer."""
    newest = newest_release(releases)
    return {"current": current, "latest": newest["version"] if newest else "", "available": bool(newest) and version_key(newest["version"]) > version_key(current),
            "page": newest["page"] if newest else "", "notes": newest["notes"] if newest else "", "release": newest}


def mac_asset(release):
    name = f"Openviz-{release['version']}-Mac.dmg"
    asset = release["assets"].get(name)
    if not asset:
        raise UpdateRefused(f"The release {release['tag']} carries no {name}.")
    digest = asset["digest"]
    if not digest.startswith("sha256:"):
        raise UpdateRefused("GitHub gives no checksum for the download, so it cannot be checked: install it from its page instead.")
    return name, asset["url"], digest[len("sha256:"):]


def team_of(app, run):
    """The Apple developer team an app is signed by, or "" if it is not signed by one."""
    code, said = run(["codesign", "-dv", "--verbose=2", app])
    found = re.search(r"^TeamIdentifier=([A-Z0-9]{10})$", said or "", re.M)
    return found.group(1) if code == 0 and found else ""


def why_not_here(installed_app, repo):
    """Why this copy is not updated in place, or "" if it can be."""
    if os.path.isdir(os.path.join(repo, ".git")):
        return "This copy of Openviz was made with Git: update it with git pull."
    if not installed_app:
        return "Openviz was not opened from its app, which is what an update replaces: install the update from its page."
    return ""


def install_mac(release, installed_app, work, run, download):
    """Downloads the release's .dmg into `work`, checks it, and puts its app in the installed app's
    place. `run(command)` gives (exit code, output); `download(url, path)` fetches a file. Raises
    UpdateRefused, leaving the installed app as it was, whenever a check fails."""
    team = team_of(installed_app, run)
    if not team:
        raise UpdateRefused("The Openviz app here is not signed by a developer team, so no update can be checked against it: "
                            "install the newest release from its .dmg once, and later ones then install themselves.")
    name, url, expected = mac_asset(release)
    disk = os.path.join(work, name)
    download(url, disk)
    with open(disk, "rb") as handle:
        got = hashlib.sha256(handle.read()).hexdigest()
    if got != expected:
        raise UpdateRefused("The download is not what GitHub says it should be (its checksum differs): nothing was changed. Try again.")
    mount = os.path.join(work, "disk")
    os.makedirs(mount, exist_ok=True)
    code, said = run(["hdiutil", "attach", "-nobrowse", "-readonly", "-noautoopen", "-mountpoint", mount, disk])
    if code != 0:
        raise UpdateRefused(f"The download could not be opened: {said[-300:]}")
    try:
        new_app = os.path.join(mount, "Openviz.app")
        if not os.path.isdir(new_app):
            raise UpdateRefused("The download holds no Openviz.app.")
        code, said = run(["codesign", "--verify", "--deep", "--strict", new_app])
        if code != 0:
            raise UpdateRefused(f"The new app's signature does not hold: {said[-300:]}")
        if team_of(new_app, run) != team:
            raise UpdateRefused("The new app is not signed by the same developer team as this one: it is not installed.")
        code, said = run(["spctl", "--assess", "--type", "execute", new_app])
        if code != 0:
            raise UpdateRefused(f"Gatekeeper does not accept the new app (is it notarized?): {said[-300:]}")
        try:
            with open(os.path.join(new_app, "Contents", "Resources", "Openviz", "openviz-version.txt"), encoding="utf-8") as handle:
                carried = handle.read().strip()
        except OSError:
            carried = ""
        if carried != release["version"]:
            raise UpdateRefused(f"The new app says it is {carried or 'no version'}, not {release['version']}.")
        # Copied beside the installed app first, then swapped in, so that a copy cut short leaves the old one whole.
        staged = installed_app.rstrip("/") + ".update"
        old = installed_app.rstrip("/") + ".old"
        for leftover in (staged, old):
            shutil.rmtree(leftover, ignore_errors=True)
        code, said = run(["ditto", new_app, staged])
        if code != 0:
            shutil.rmtree(staged, ignore_errors=True)
            raise UpdateRefused(f"The new app could not be copied next to {installed_app}: {said[-300:]}")
        os.rename(installed_app, old)
        try:
            os.rename(staged, installed_app)
        except OSError:
            os.rename(old, installed_app)
            raise
        shutil.rmtree(old, ignore_errors=True)
    finally:
        run(["hdiutil", "detach", "-quiet", mount])
    return installed_app


def download_file(url, path, timeout=120):
    request = urllib.request.Request(url, headers={"User-Agent": "Openviz-updater"})
    with urllib.request.urlopen(request, timeout=timeout) as answer, open(path, "wb") as out:
        shutil.copyfileobj(answer, out)


def run_program(command, timeout=600):
    try:
        done = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as error:
        return None, str(error)
    return done.returncode, (done.stdout + done.stderr).strip()


def reopen_after(pid, app):
    """Opens the app again once the process `pid` (the app now running) has ended."""
    script = f"while kill -0 {int(pid)} 2>/dev/null; do sleep 0.3; done; sleep 0.5; open {_quoted(app)}"
    subprocess.Popen(["/bin/sh", "-c", script], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)


def _quoted(text):
    return "'" + text.replace("'", "'\\''") + "'"


def work_folder():
    return tempfile.mkdtemp(prefix="openviz-update-")
