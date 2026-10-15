#!/usr/bin/env python3
"""Find and download GDTF files from GDTF Share (https://gdtf-share.com) with your own account.

  tools/gdtf_share/gdtf_share.py search mac ultra
  tools/gdtf_share/gdtf_share.py get 75541 "robe megapointe"

Uses the site's public API (login.php, getList.php, downloadFile.php):
https://github.com/mvrdevelopment/tools/blob/main/GDTF_Share_API/GDTF%20Share%20API.md

The user name and password are asked for when they are needed, or read from GDTF_SHARE_USER and
GDTF_SHARE_PASSWORD. They are sent only to gdtf-share.com over HTTPS. Neither they nor the session
cookie are written to disk.

The fixture list is kept in reference/gdtf-share/list.json, so searching again needs no login.
Files are saved next to it; git ignores the whole folder.
"""
from __future__ import annotations

import argparse
import getpass
import http.client
import http.cookiejar
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_URL = "https://gdtf-share.com/apis/public/"
DEFAULT_OUT = os.path.join(REPO, "reference", "gdtf-share")
def _built(name):
    """A program the core's build made. Visual Studio puts it a folder further down, and ends its name in .exe."""
    folder = os.path.join(REPO, "core", "build")
    for candidate in (os.path.join(folder, name), os.path.join(folder, name + ".exe"), os.path.join(folder, "Release", name + ".exe"),
                      os.path.join(folder, "RelWithDebInfo", name + ".exe"), os.path.join(folder, "Debug", name + ".exe")):
        if os.path.isfile(candidate):
            return candidate
    return os.path.join(folder, name)


INSPECTOR = _built("vizcore_inspect")
USER_AGENT = "ue5-visualizer-gdtf-share/1.0"

MAX_LIST_BYTES = 256 << 20
MAX_FILE_BYTES = 512 << 20    # the core refuses larger archive entries too
LOCAL_HOSTS = ("localhost", "127.0.0.1", "::1")


class ShareError(Exception):
    """Something the user should read, without a traceback. `status` is the HTTP status, if any."""

    def __init__(self, message, status=None):
        super().__init__(message)
        self.status = status


# ---------------------------------------------------------------------------------------------
# What the server sends is untrusted: names are uploaded by users, numbers may arrive as strings.

def _text(value, limit=200):
    """One line of printable text, so a name cannot move the cursor or forge a line of output."""
    if value is None:
        return ""
    cleaned = "".join(ch if ch.isprintable() else " " for ch in str(value))
    return " ".join(cleaned.split())[:limit]


def _int(value):
    try:
        return int(float(value))
    except (TypeError, ValueError, OverflowError):
        return None


def _float(value):
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if number == number and abs(number) != float("inf") else None


def normalise(raw):
    """A list entry with known keys and types, or None if it has no usable revision id."""
    if not isinstance(raw, dict):
        return None
    rid = _int(raw.get("rid"))
    if rid is None or rid < 0:
        return None
    modes = []
    for mode in raw.get("modes") or []:
        if isinstance(mode, dict):
            modes.append({"name": _text(mode.get("name")), "dmxfootprint": _int(mode.get("dmxfootprint")) or 0})
    return {
        "rid": rid,
        "manufacturer": _text(raw.get("manufacturer")),
        "fixture": _text(raw.get("fixture")),
        "revision": _text(raw.get("revision")),
        "uploader": _text(raw.get("uploader")),
        "creator": _text(raw.get("creator")),
        "rating": _float(raw.get("rating")),
        "version": _text(raw.get("version")),
        "uuid": _text(raw.get("uuid")),
        "filesize": max(_int(raw.get("filesize")) or 0, 0),
        "creationDate": _int(raw.get("creationDate")) or 0,
        "lastModified": _int(raw.get("lastModified")) or 0,
        "modes": modes,
    }


def _server_message(body):
    """The `error` text of a JSON answer, or the start of whatever came instead."""
    text = body.decode("utf-8", "replace") if isinstance(body, bytes) else str(body)
    try:
        answer = json.loads(text)
    except ValueError:
        answer = None
    if isinstance(answer, dict):
        return _text(answer.get("error") or answer.get("notice") or "no reason given")
    return _text(text, 120) or "an empty answer"


# ---------------------------------------------------------------------------------------------
# The API

class _SameOrigin(urllib.request.HTTPRedirectHandler):
    """The session cookie has no Secure flag, so it must not follow a redirect off HTTPS or off the host."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        old = urllib.parse.urlsplit(req.full_url)
        new = urllib.parse.urlsplit(newurl)
        if (old.scheme, old.hostname, old.port) != (new.scheme, new.hostname, new.port):
            raise ShareError(f"the server redirected to {new.scheme}://{new.hostname}, which is not followed")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


class Share:
    """One logged-in session. Nothing about it outlives the object."""

    def __init__(self, base_url=DEFAULT_URL, timeout=60.0):
        parts = urllib.parse.urlsplit(base_url)
        if parts.scheme != "https" and not (parts.scheme == "http" and parts.hostname in LOCAL_HOSTS):
            raise ShareError(f"{base_url} is not an HTTPS address; a password is not sent to it")
        self.base_url = base_url if base_url.endswith("/") else base_url + "/"
        self.host = parts.hostname or base_url
        self._timeout = timeout
        cookies = http.cookiejar.CookieJar()
        self._opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cookies), _SameOrigin())

    def _open(self, path, data=None, content_type=None):
        headers = {"User-Agent": USER_AGENT}
        if content_type:
            headers["Content-Type"] = content_type
        request = urllib.request.Request(self.base_url + path, data=data, headers=headers)
        try:
            return self._opener.open(request, timeout=self._timeout)
        except urllib.error.HTTPError as error:
            with error:
                body = error.read(4096)
            raise ShareError(_server_message(body), status=error.code) from None
        except urllib.error.URLError as error:
            raise ShareError(f"cannot reach {self.host}: {error.reason}") from None
        except (OSError, http.client.HTTPException) as error:
            raise ShareError(f"cannot reach {self.host}: {error}") from None

    def login(self, user, password):
        body = json.dumps({"user": user, "password": password}).encode("utf-8")
        try:
            with self._open("login.php", data=body, content_type="application/json") as response:
                raw = response.read(65536)
            try:
                answer = json.loads(raw.decode("utf-8", "replace"))
            except ValueError:
                answer = None
            if not isinstance(answer, dict) or answer.get("result") is not True:
                raise ShareError(_server_message(raw), status=401)
        except ShareError as error:
            # Whatever the server answered, the password is not repeated on the terminal.
            said = str(error).replace(password, "***") if password else str(error)
            if error.status == 401:
                raise ShareError(f"GDTF Share did not accept that user name and password ({said})", status=401) from None
            raise ShareError(f"login failed: {said}", status=error.status) from None

    def fetch_list(self):
        """Every revision on GDTF Share, normalised. Returns (entries, server timestamp)."""
        try:
            with self._open("getList.php") as response:
                raw = response.read(MAX_LIST_BYTES + 1)
        except ShareError as error:
            raise _explain_session(error, "the fixture list") from None
        if len(raw) > MAX_LIST_BYTES:
            raise ShareError("the fixture list is larger than this tool accepts")
        try:
            answer = json.loads(raw.decode("utf-8", "replace"))
        except ValueError:
            answer = None
        if not isinstance(answer, dict) or answer.get("result") is not True or not isinstance(answer.get("list"), list):
            raise ShareError(f"GDTF Share did not send a fixture list: {_server_message(raw)}")
        entries = [entry for entry in map(normalise, answer["list"]) if entry]
        return entries, _int(answer.get("timestamp")) or 0

    def download(self, rid, destination, limit=MAX_FILE_BYTES):
        """Saves revision `rid` as `destination` and returns its size. Leaves nothing behind on failure."""
        part = destination + ".part"
        try:
            try:
                response = self._open(f"downloadFile.php?rid={int(rid)}")
            except ShareError as error:
                raise _explain_session(error, f"revision {rid}") from None
            try:
                with response, open(part, "wb") as out:
                    head = response.read(4)
                    if head != b"PK\x03\x04":
                        # Errors arrive as JSON, sometimes with status 200 and always as text/html.
                        said = _server_message(head + response.read(4096))
                        raise ShareError(f"GDTF Share sent no file for revision {rid}: {said}")
                    out.write(head)
                    size = len(head)
                    while True:
                        chunk = response.read(1 << 16)
                        if not chunk:
                            break
                        size += len(chunk)
                        if size > limit:
                            raise ShareError(f"revision {rid} is larger than {limit >> 20} MB; stopped")
                        out.write(chunk)
            except (OSError, http.client.HTTPException) as error:
                raise ShareError(f"the download of revision {rid} broke off: {error}") from None
            check_gdtf(part, f"revision {rid}")
            os.replace(part, destination)
            return size
        finally:
            if os.path.exists(part):
                os.remove(part)


def _explain_session(error, what):
    if error.status == 401:
        return ShareError(f"GDTF Share refused {what}: not logged in, or the session ran out ({error})", status=401)
    return ShareError(f"GDTF Share refused {what}: {error}", status=error.status)


def check_gdtf(path, what):
    """A GDTF is a zip archive with a description.xml at its top level. Nothing is extracted."""
    try:
        with zipfile.ZipFile(path) as archive:
            names = archive.namelist()
    except (zipfile.BadZipFile, OSError) as error:
        raise ShareError(f"{what} is not a readable archive: {error}") from None
    if "description.xml" not in names:
        raise ShareError(f"{what} has no description.xml, so it is not a GDTF")


# ---------------------------------------------------------------------------------------------
# The list: cache, grouping, searching

def list_path(out):
    return os.path.join(out, "list.json")


def save_list(out, entries, timestamp, source):
    os.makedirs(out, exist_ok=True)
    path = list_path(out)
    with open(path + ".part", "w", encoding="utf-8") as handle:
        json.dump({"fetched": int(time.time()), "source": source, "timestamp": timestamp, "list": entries}, handle)
    os.replace(path + ".part", path)


def load_list(out):
    """(entries, time fetched), or (None, 0) when there is no usable cache."""
    try:
        with open(list_path(out), encoding="utf-8") as handle:
            cached = json.load(handle)
        entries = [entry for entry in map(normalise, cached["list"]) if entry]
        return entries, _int(cached.get("fetched")) or 0
    except (OSError, ValueError, KeyError, TypeError):
        return None, 0


def by_manufacturer(entry):
    return entry["uploader"].casefold().startswith("manuf")


def rank(entry):
    """Higher is the better revision of a fixture: the manufacturer's own upload, then the newest.

    Newest by creation date. lastModified also moves when a revision is rated or re-saved, and on
    the real list it names an older revision as the latest for about one fixture in nine.
    """
    return (by_manufacturer(entry), entry["creationDate"], entry["rid"])


def group(entries):
    """Revisions grouped into fixtures: a list of revision lists, best first, sorted by name."""
    groups = {}
    for entry in entries:
        groups.setdefault((entry["manufacturer"].casefold(), entry["fixture"].casefold()), []).append(entry)
    return [sorted(groups[key], key=rank, reverse=True) for key in sorted(groups)]


def matches(entry, terms, manufacturer=""):
    name = f"{entry['manufacturer']} {entry['fixture']}".casefold()
    if manufacturer and manufacturer.casefold() not in entry["manufacturer"].casefold():
        return False
    return all(term.casefold() in name for term in terms)


def resolve(targets, entries):
    """Turns revision ids and fixture names into list entries.

    Returns (entries to fetch, problems). A name must come down to one fixture; its best revision
    is taken. One fixture means: the only match, or the only one with exactly that name, or the
    only one its manufacturer uploaded. Anything else is a problem, listed with its candidates.
    """
    by_rid = {entry["rid"]: entry for entry in entries}
    fixtures = group(entries)
    chosen, problems = [], []
    for target in targets:
        if re.fullmatch(r"\d+", target.strip()):
            entry = by_rid.get(int(target))
            if entry:
                chosen.append(entry)
            else:
                problems.append(f"no revision {int(target)} in the list")
            continue
        terms = target.split()
        found = [revisions for revisions in fixtures if terms and matches(revisions[0], terms)]
        wanted = " ".join(terms).casefold()
        exact = [revisions for revisions in found
                 if wanted in (revisions[0]["fixture"].casefold(),
                               f"{revisions[0]['manufacturer']} {revisions[0]['fixture']}".casefold())]
        if len(exact) == 1:
            found = exact
        official = [revisions for revisions in found if by_manufacturer(revisions[0])]
        if len(found) > 1 and len(official) == 1:
            found = official    # the others are users' own versions, filed under their own names
        if len(found) == 1:
            chosen.append(found[0][0])
        elif not found:
            problems.append(f'nothing in the list matches "{target}"')
        else:
            names = [f"    {r[0]['rid']:>7}  {r[0]['manufacturer']}  {r[0]['fixture']}" for r in found[:12]]
            more = [f"    and {len(found) - 12} more"] if len(found) > 12 else []
            problems.append("\n".join([f'"{target}" matches {len(found)} fixtures; name one, or use its number:'] + names + more))
    unique = []
    for entry in chosen:
        if all(entry["rid"] != other["rid"] for other in unique):
            unique.append(entry)
    return unique, problems


# ---------------------------------------------------------------------------------------------
# File names

def _safe(text, limit=60):
    cleaned = re.sub(r'[\\/:*?"<>|@\x00-\x1f\x7f]', "_", text).strip(" .")
    return cleaned[:limit] or "_"


def file_name(entry):
    """Manufacturer@Fixture@Revision@rid<N>.gdtf. The revision id keeps two uploads of one name apart."""
    parts = [_safe(entry["manufacturer"] or "Unknown"), _safe(entry["fixture"] or "Fixture")]
    if entry["revision"]:
        parts.append(_safe(entry["revision"]))
    stem = "@".join(parts).encode("utf-8")[:200].decode("utf-8", "ignore")
    return f"{stem}@rid{entry['rid']}.gdtf"


def already_have(path, entry):
    if not os.path.isfile(path):
        return False
    if entry["filesize"] and os.path.getsize(path) != entry["filesize"]:
        return False
    try:
        check_gdtf(path, path)
    except ShareError:
        return False
    return True


# ---------------------------------------------------------------------------------------------
# Output

def _size(count):
    if not count:
        return "-"
    return f"{count / 1048576:.1f} MB" if count >= 1048576 else f"{max(count // 1024, 1)} KB"


def _date(timestamp):
    return time.strftime("%Y-%m-%d", time.gmtime(timestamp)) if timestamp > 0 else "-"


def _age(fetched):
    days = int(max(time.time() - fetched, 0) // 86400)
    return "today" if days == 0 else "yesterday" if days == 1 else f"{days} days ago"


def _shown(path):
    relative = os.path.relpath(path)
    return path if relative.startswith("..") else relative


ROW = "{:>7}  {:<24.24}  {:<34.34}  {:<16.16}  {:<6}  {:>4}  {:<10}  {:>8}"


def _row(entry):
    rating = f"{entry['rating']:.1f}" if entry["rating"] is not None else "-"
    return ROW.format(entry["rid"], entry["manufacturer"], entry["fixture"], entry["revision"],
                      "manuf." if by_manufacturer(entry) else "user", rating, _date(entry["creationDate"]),
                      _size(entry["filesize"]))


def _describe(entry):
    revision = f', revision "{entry["revision"]}"' if entry["revision"] else ""
    return f"{entry['manufacturer']} {entry['fixture']}{revision} (number {entry['rid']}, {_size(entry['filesize'])})"


def core_check(path, entry=None, inspector=INSPECTOR):
    """Whether the project's own parser reads the file, and whether it counts each mode's slots as
    GDTF Share's list does. None when the core has not been built."""
    if not os.path.exists(inspector):
        return None
    try:
        done = subprocess.run([inspector, path], capture_output=True, text=True, errors="replace", timeout=120)
    except (OSError, subprocess.TimeoutExpired) as error:
        return f"core: the check did not run ({error})"
    lines = [line.strip() for line in done.stdout.splitlines()]
    warnings = sum(line.startswith("warning:") for line in lines)
    errors = sum(line.startswith("ERROR:") for line in lines)
    if done.returncode != 0:
        return f"core: cannot read it ({errors} error{'s' if errors != 1 else ''}, {warnings} warning{'s' if warnings != 1 else ''})"
    verdict = "core: reads it, " + (f"{warnings} warning{'s' if warnings != 1 else ''}" if warnings else "no warnings")

    # "mode 'Name': 12 channels, 10 slots on break 1, 6 slots on break 2". The list gives one
    # footprint per mode, taken here to be the slots of all breaks together.
    ours = {}
    for line in lines:
        found = re.fullmatch(r"mode '(.*)': \d+ channels(.*)", line)
        if found:
            ours[" ".join(found.group(1).split())] = sum(int(n) for n in re.findall(r"(\d+) slots on break", found.group(2)))
    listed = [(mode["name"], mode["dmxfootprint"]) for mode in (entry or {}).get("modes", []) if mode["dmxfootprint"]]
    if not listed:
        return verdict
    differ = [f"'{name}' is {ours.get(name, 'missing')} here, {footprint} listed"
              for name, footprint in listed if ours.get(name) != footprint]
    if not differ:
        return f"{verdict}; {'all ' if len(listed) > 1 else ''}{len(listed)} mode footprint{'s' if len(listed) != 1 else ''} match GDTF Share's"
    return f"{verdict}; {len(differ)} of {len(listed)} mode footprints DIFFER from GDTF Share's: " + "; ".join(differ[:6])


# ---------------------------------------------------------------------------------------------
# Commands

def ask_login(environ):
    user = environ.get("GDTF_SHARE_USER", "")
    password = environ.get("GDTF_SHARE_PASSWORD", "")
    if user and password:
        return user, password
    if not sys.stdin.isatty():
        raise ShareError("a GDTF Share login is needed: run this in a terminal, "
                         "or set GDTF_SHARE_USER and GDTF_SHARE_PASSWORD")
    if not user:
        sys.stderr.write("GDTF Share user name: ")
        sys.stderr.flush()
        user = sys.stdin.readline().strip()
    if not password:
        password = getpass.getpass("GDTF Share password (not shown, not stored): ")
    if not user or not password:
        raise ShareError("no user name or password given")
    return user, password


def connect(args, environ):
    share = Share(args.base_url)
    user, password = ask_login(environ)
    share.login(user, password)
    return share


def refresh(share, args):
    entries, timestamp = share.fetch_list()
    save_list(args.out, entries, timestamp, share.base_url)
    print(f"fetched the list: {len(entries)} revisions, saved to {_shown(list_path(args.out))}", file=sys.stderr)
    return entries


def cmd_search(args, environ):
    entries, fetched = (None, 0) if args.refresh else load_list(args.out)
    if entries is None:
        entries = refresh(connect(args, environ), args)
        fetched = time.time()

    if args.all_revisions:
        rows = sorted((e for e in entries if matches(e, args.terms, args.manufacturer)),
                      key=lambda e: (e["manufacturer"].casefold(), e["fixture"].casefold(), tuple(-x for x in rank(e))))
        extra = [""] * len(rows)
        unit = "revision"
    else:
        found = [revisions for revisions in group(entries) if matches(revisions[0], args.terms, args.manufacturer)]
        rows = [revisions[0] for revisions in found]
        extra = [f"  +{len(revisions) - 1} more" if len(revisions) > 1 else "" for revisions in found]
        unit = "fixture"

    if rows:
        print(ROW.format("number", "manufacturer", "fixture", "revision", "by", "rate", "created", "size"))
    for entry, note in list(zip(rows, extra))[:args.limit]:
        print(_row(entry) + note)
        if args.modes and entry["modes"]:
            print("         modes: " + ", ".join(f"{mode['name']} ({mode['dmxfootprint']})" for mode in entry["modes"]))
    hidden = len(rows) - min(len(rows), args.limit)
    summary = f"{len(rows)} {unit}{'s' if len(rows) != 1 else ''} match"
    if hidden:
        summary += f"; {hidden} not shown (narrow the search or raise --limit)"
    print(f"{summary}. List of {len(entries)} revisions fetched {_age(fetched)}.", file=sys.stderr)
    return 0 if rows else 1


def cmd_get(args, environ):
    share = None
    entries, fetched = (None, 0) if args.refresh else load_list(args.out)
    if entries is None:
        if args.dry_run:
            raise ShareError("there is no saved list yet, and --dry-run does not log in; run a search first")
        share = connect(args, environ)
        entries = refresh(share, args)
        fetched = time.time()

    plan, problems = resolve(args.targets, entries)
    if problems:
        for problem in problems:
            print(problem, file=sys.stderr)
        print(f"Nothing was downloaded. The list was fetched {_age(fetched)}; --refresh fetches it again.", file=sys.stderr)
        return 1
    if len(plan) > args.max:
        raise ShareError(f"that is {len(plan)} files and --max is {args.max}; raise --max if you mean it")

    os.makedirs(args.out, exist_ok=True)
    saved, failed, requests = [], 0, 0
    for index, entry in enumerate(plan):
        path = os.path.join(args.out, file_name(entry))
        print(_describe(entry))
        if not args.force and already_have(path, entry):
            print(f"  already here: {_shown(path)}")
            saved.append((path, entry))
            continue
        if args.dry_run:
            print(f"  would save {_shown(path)}")
            continue
        try:
            if share is None:
                share = connect(args, environ)
            if requests:
                time.sleep(args.delay)
            requests += 1
            size = share.download(entry["rid"], path)
        except ShareError as error:
            print(f"  FAILED: {error}", file=sys.stderr)
            failed += 1
            if error.status == 401 or share is None:
                print(f"Stopped with {len(plan) - index} of {len(plan)} not downloaded.", file=sys.stderr)
                return 1
            continue
        print(f"  saved {_shown(path)} ({_size(size)})")
        saved.append((path, entry))

    if not args.dry_run and not args.no_check:
        if os.path.exists(INSPECTOR):
            for path, entry in saved:
                print(f"{os.path.basename(path)}\n  {core_check(path, entry)}")
        elif saved:
            print("The core is not built, so the files were not run through its parser "
                  "(cmake -S core -B core/build && cmake --build core/build -j).", file=sys.stderr)
    return 1 if failed else 0


def build_parser():
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--out", default=DEFAULT_OUT, help="folder for the list and the files (default reference/gdtf-share)")
    common.add_argument("--refresh", action="store_true", help="fetch the list again instead of using the saved one")
    common.add_argument("--base-url", default=DEFAULT_URL, help=argparse.SUPPRESS)    # for the tests

    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)

    search = commands.add_parser("search", parents=[common], help="list fixtures whose name has every given word",
                                 description="Lists fixtures whose manufacturer and name contain every word given. "
                                             "One row per fixture, showing its best revision: the manufacturer's own "
                                             "upload if there is one, then the most recently created.")
    search.add_argument("terms", nargs="*", help="words to look for, in any order and any case")
    search.add_argument("--manufacturer", default="", help="only this manufacturer (part of the name is enough)")
    search.add_argument("--all-revisions", action="store_true", help="one row per revision, not per fixture")
    search.add_argument("--modes", action="store_true", help="also list each row's modes with their DMX footprints")
    search.add_argument("--limit", type=int, default=40, help="rows to print (default 40)")
    search.set_defaults(run=cmd_search)

    get = commands.add_parser("get", parents=[common], help="download revisions by number, or fixtures by name",
                              description="Downloads each target. A target is a number from the search output, or a "
                                          "name in quotes that comes down to one fixture: the only match, the only one with exactly that "
                                          "name, or the only one uploaded by its manufacturer. Its best revision is taken.")
    get.add_argument("targets", nargs="+", metavar="target", help='a number, or a name such as "robe megapointe"')
    get.add_argument("--force", action="store_true", help="download again even if the file is already here")
    get.add_argument("--dry-run", action="store_true", help="say what would be downloaded; does not log in")
    get.add_argument("--max", type=int, default=20, help="refuse to fetch more files than this in one go (default 20)")
    get.add_argument("--delay", type=float, default=1.0, help="seconds to wait between downloads (default 1)")
    get.add_argument("--no-check", action="store_true", help="do not run the downloaded files through the core's parser")
    get.set_defaults(run=cmd_get)
    return parser


def main(argv=None, environ=None):
    args = build_parser().parse_args(argv)
    args.out = os.path.abspath(args.out)
    try:
        return args.run(args, os.environ if environ is None else environ)
    except ShareError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nstopped", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
