"use strict";
// Openviz's window. All it knows comes from launcher.py over /api; names from files and from GDTF
// Share are only ever put on the page as text, never as markup.

// The token arrives after the # of the address, so it is never sent to the server in a URL or
// kept in history. A reload within the same window finds it in sessionStorage.
let token = location.hash.slice(1);
try {
  if (token) sessionStorage.setItem("token", token);
  else token = sessionStorage.getItem("token") || "";
} catch (error) { /* storage is off: the token lives for this page load only */ }
if (location.hash) history.replaceState(null, "", location.pathname);

const $ = (id) => document.getElementById(id);

function el(tag, props, ...kids) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(props || {})) {
    if (value == null || value === false) continue;
    if (key === "class") node.className = value;
    else if (key.startsWith("on")) node.addEventListener(key.slice(2), value);
    else if (key === "value" || key === "disabled" || key === "selected" || key === "hidden" || key === "open" || key === "checked") node[key] = value;
    else node.setAttribute(key, value === true ? "" : value);
  }
  for (const kid of kids.flat(Infinity)) if (kid != null && kid !== false) node.append(kid);
  return node;
}

// Icons: simple line drawings on a 24-unit grid, built as SVG so that the page's content policy allows them.
const ICONS = {
  layers: [["polygon", { points: "12 3 21 8 12 13 3 8" }], ["path", { d: "M3 12.5 12 17.5 21 12.5" }], ["path", { d: "M3 16.5 12 21.5 21 16.5" }]],
  box: [["path", { d: "M21 8 12 3 3 8v8l9 5 9-5z" }], ["path", { d: "M3 8l9 5 9-5" }], ["path", { d: "M12 13v8" }]],
  activity: [["path", { d: "M3 12h4l3-8 4 16 3-8h4" }]],
  record: [["circle", { cx: 12, cy: 12, r: 9 }], ["circle", { cx: 12, cy: 12, r: 3.5, fill: "currentColor" }]],
  network: [["circle", { cx: 6, cy: 12, r: 2.5 }], ["circle", { cx: 18, cy: 6, r: 2.5 }], ["circle", { cx: 18, cy: 18, r: 2.5 }], ["path", { d: "M8.3 10.9 15.7 7.1M8.3 13.1 15.7 16.9" }]],
  eye: [["path", { d: "M2 12s3.6-7 10-7 10 7 10 7-3.6 7-10 7S2 12 2 12z" }], ["circle", { cx: 12, cy: 12, r: 3 }]],
  sliders: [["path", { d: "M4 7h9M17 7h3M4 12h3M11 12h9M4 17h11M19 17h1" }], ["circle", { cx: 15, cy: 7, r: 2 }], ["circle", { cx: 9, cy: 12, r: 2 }], ["circle", { cx: 17, cy: 17, r: 2 }]],
  chart: [["path", { d: "M4 20V11M10 20V5M16 20v-6M3 20h18" }]],
  play: [["polygon", { points: "7 4.5 19.5 12 7 19.5", fill: "currentColor" }]],
  stop: [["rect", { x: 6, y: 6, width: 12, height: 12, rx: 1.5, fill: "currentColor" }]],
  edit: [["path", { d: "M4 20h4L19 9l-4-4L4 16z" }], ["path", { d: "M13.5 6.5l4 4" }]],
  more: [["circle", { cx: 5, cy: 12, r: 1.6, fill: "currentColor" }], ["circle", { cx: 12, cy: 12, r: 1.6, fill: "currentColor" }], ["circle", { cx: 19, cy: 12, r: 1.6, fill: "currentColor" }]],
  undo: [["path", { d: "M9 14 4 9l5-5" }], ["path", { d: "M4 9h10.5a5.5 5.5 0 0 1 0 11H11" }]],
  redo: [["path", { d: "M15 14l5-5-5-5" }], ["path", { d: "M20 9H9.5a5.5 5.5 0 0 0 0 11H13" }]],
  plus: [["path", { d: "M12 5v14M5 12h14" }]],
  download: [["path", { d: "M12 4v11M7 10l5 5 5-5M5 20h14" }]],
  upload: [["path", { d: "M12 16V5M7 10l5-5 5 5M5 20h14" }]],
  folder: [["path", { d: "M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" }]],
  chevron: [["path", { d: "M9 6l6 6-6 6" }]],
  up: [["path", { d: "M6 15l6-6 6 6" }]],
  down: [["path", { d: "M6 9l6 6 6-6" }]],
  back: [["path", { d: "M15 6l-6 6 6 6" }]],
  check: [["path", { d: "M5 12.5l4.5 4.5L19 7" }]],
  alert: [["path", { d: "M12 3.5 2.5 20h19z" }], ["path", { d: "M12 10v4.5" }], ["circle", { cx: 12, cy: 17.3, r: 0.6, fill: "currentColor" }]],
  info: [["circle", { cx: 12, cy: 12, r: 9 }], ["path", { d: "M12 11v6" }], ["circle", { cx: 12, cy: 7.6, r: 0.6, fill: "currentColor" }]],
  close: [["path", { d: "M6 6l12 12M18 6 6 18" }]],
  search: [["circle", { cx: 11, cy: 11, r: 7 }], ["path", { d: "m20 20-3.5-3.5" }]],
  file: [["path", { d: "M14 3H6v18h12V7z" }], ["path", { d: "M14 3v4h4" }]],
  pointer: [["path", { d: "M6 3l13 8-5.5 1.6L11 18z" }]],
  camera: [["path", { d: "M4 8h3l2-3h6l2 3h3v11H4z" }], ["circle", { cx: 12, cy: 13, r: 3.5 }]],
  export: [["path", { d: "M14 4h6v6M20 4l-9 9" }], ["path", { d: "M18 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h5" }]],
  trash: [["path", { d: "M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3" }]],
};
function icon(name) {
  const ns = "http://www.w3.org/2000/svg";
  const node = document.createElementNS(ns, "svg");
  node.setAttribute("viewBox", "0 0 24 24");
  node.setAttribute("fill", "none");
  node.setAttribute("stroke", "currentColor");
  node.setAttribute("stroke-width", "1.8");
  node.setAttribute("stroke-linecap", "round");
  node.setAttribute("stroke-linejoin", "round");
  node.setAttribute("aria-hidden", "true");
  for (const [tag, attributes] of ICONS[name] || []) {
    const part = document.createElementNS(ns, tag);
    for (const [key, value] of Object.entries(attributes)) part.setAttribute(key, String(value));
    if (attributes.fill) part.setAttribute("stroke", "none");
    node.append(part);
  }
  return node;
}

// A bar filled to `percent`. Set through the style object, which the page's content policy allows
// where a style attribute in markup would not be.
function fill(className, percent) {
  const node = el("i", { class: className });
  node.style.width = `${Math.max(0, Math.min(100, percent))}%`;
  return node;
}

// replaceChildren, but arrays are spread and nothing is made of null or false.
function put(node, ...kids) {
  node.replaceChildren(...kids.flat(Infinity).filter((kid) => kid != null && kid !== false));
}

async function api(path, options = {}) {
  const init = { method: options.method || "GET", headers: { "X-Launcher-Token": token } };
  if (options.json !== undefined) {
    init.method = "POST";
    init.headers["Content-Type"] = "application/json";
    init.body = JSON.stringify(options.json);
  } else if (options.body !== undefined) {
    init.method = "POST";
    init.body = options.body;
  }
  let response;
  try {
    response = await fetch(path, init);
  } catch (error) {
    throw Object.assign(new Error("Openviz is not responding. It may have been closed."), { code: "offline" });
  }
  let answer = {};
  try { answer = await response.json(); } catch (error) { /* not JSON */ }
  if (!response.ok) throw Object.assign(new Error(answer.error || `Something went wrong (${response.status}).`), { code: answer.code });
  return answer;
}

const size = (bytes) => bytes >= 1048576 ? `${(bytes / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(bytes / 1024))} KB`;
const plural = (count, word, many) => `${count.toLocaleString()} ${count === 1 ? word : (many || `${word}s`)}`;
const NOTICE_ICONS = { ok: "check", error: "alert", info: "info" };
const notice = (text, kind, ...more) => el("div", { class: `notice ${kind || ""}`, role: kind === "error" ? "alert" : "status" },
  icon(NOTICE_ICONS[kind] || "alert"), el("div", { class: "text" }, text, ...more));
const bare = (name) => name.replace(/\.(mvr|gdtf)$/i, "");
// "Martin@MAC Viper@rev 3" as people say it: "Martin MAC Viper".
const typeName = (spec) => bare(spec || "").split("@").slice(0, 2).join(" ").trim();
// Runs `work` with the button switched off, and says what went wrong in `said` (or above the page).
const busy = (button, work, said) => async (event) => {
  const target = button || event.currentTarget;
  target.disabled = true;
  try { await work(event); }
  catch (error) { (said || pageNotice()).replaceChildren(notice(error.message, "error")); }
  if (target.isConnected) target.disabled = false;
};

let state = null;

// Words that depend on the system Openviz runs on (state.system is Python's sys.platform).
const onMac = () => !state || state.system === "darwin";
const onWindows = () => !!state && state.system === "win32";
const thisComputer = () => (onMac() ? "this Mac" : "this computer");
const passwordStore = () => (onMac() ? "the macOS Keychain" : onWindows() ? "Windows Credential Manager" : "the system's password store");
const fileManager = () => (state && state.file_manager) || "Finder";
const showInFolder = () => `Show in ${fileManager()}`;
const reveal = (what) => api("/api/reveal", { json: { what } }).catch(() => {});
const revealButton = (what, label) => el("button", { class: "link", onclick: () => reveal(what) }, label || showInFolder());

// ------------------------------------------------------------------------------------- pages

const NAV = [
  ["Workspace", [["shows", "Shows", "layers"], ["library", "Fixture Library", "box"]]],
  ["Live", [["dmx", "DMX Input", "activity"], ["recordings", "Recordings", "record"], ["connections", "Connections", "network"]]],
  ["Settings", [["visualizer", "Visualizer", "eye"], ["system", "System", "sliders"]]],
  [null, [["parity", "Parity", "chart"]]],
];
const VIEWS = ["shows", "show", "library", "dmx", "recordings", "connections", "visualizer", "system", "parity"];
// Where the window is: the page, and on a show's page which show and which tab.
let route = { view: "shows", show: null, tab: "patch" };

function buildNav() {
  const nav = $("nav");
  for (const [label, items] of NAV) {
    if (label) nav.append(el("div", { class: "nav-label" }, label));
    else nav.append(el("div", { class: "spacer" }));
    for (const [view, title, mark] of items) {
      nav.append(el("button", { class: "tab", "data-view": view, onclick: () => go(view) }, icon(mark), el("span", {}, title),
        view === "system" ? el("span", { id: "engine-dot", class: "dot" }) : null));
    }
  }
}

function go(view, extra) {
  if (!VIEWS.includes(view)) view = "shows";
  const was = route;
  route = Object.assign({}, route, { view }, extra || {});
  // What was said on one page, or one tab of a show, is not carried to the next.
  if (was.view !== route.view || was.show !== route.show || was.tab !== route.tab) {
    for (const holder of document.querySelectorAll(".page-notice")) holder.replaceChildren();
  }
  if (view === "show" && !route.show) route.view = view = "shows";
  for (const name of VIEWS) $(`view-${name}`).hidden = name !== view;
  for (const button of document.querySelectorAll("button.tab")) button.classList.toggle("on", button.dataset.view === (view === "show" ? "shows" : view));
  try { sessionStorage.setItem("route", JSON.stringify(route)); } catch (error) { /* fine */ }
  document.querySelector("main").scrollTop = 0;
  if (view === "shows") { loadShows(); loadXchange(); }
  if (view === "show") loadShows();
  if (view === "library") { loadLibrary(); renderShare(); }
  if (view === "dmx") renderDmx();
  if (view === "recordings") renderRecordings();
  if (view === "connections") renderConnections();
  if (view === "visualizer") renderVisualizer();
  if (view === "system") renderSystem();
  if (view === "parity") loadParity();
}

// The notice area at the top of whichever page is showing.
function pageNotice() {
  const page = document.querySelector("main section:not([hidden]) .page");
  let holder = page && page.querySelector(":scope > .page-notice");
  if (!holder && page) {
    holder = el("div", { class: "page-notice" });
    const head = page.querySelector(":scope > .page-head, :scope > .show-top");
    if (head) head.after(holder); else page.prepend(holder);
  }
  return holder || el("div");
}

function pageHead(title, subtitle, ...actions) {
  return el("div", { class: "page-head" },
    el("div", { class: "lead" }, el("h1", {}, title), subtitle ? el("div", { class: "subtitle" }, subtitle) : null),
    actions.length ? el("div", { class: "actions" }, actions) : null);
}

function ready() {
  if (!state.engine_found) return "Unreal Engine was not found. Set its folder under System.";
  if (!state.project_built) return "The visualizer has not been built yet. See System.";
  return "";
}

// A menu of actions under a button; it closes on a choice, a click elsewhere or Escape.
function menuButton(label, items, options = {}) {
  const wrap = el("span", { class: "menu-wrap" });
  const button = el("button", { class: options.class || "", "aria-haspopup": "menu", "aria-expanded": "false", title: options.title || "" }, label);
  let menu = null;
  const close = () => {
    if (!menu) return;
    menu.remove();
    menu = null;
    button.setAttribute("aria-expanded", "false");
    document.removeEventListener("mousedown", outside, true);
    document.removeEventListener("keydown", escape, true);
  };
  const outside = (event) => { if (!wrap.contains(event.target)) close(); };
  const escape = (event) => { if (event.key === "Escape") { close(); button.focus(); } };
  button.addEventListener("click", () => {
    if (menu) return close();
    menu = el("div", { class: "menu", role: "menu" }, items.filter(Boolean).map((item) => item === "-" ? el("hr") :
      el("button", { role: "menuitem", class: item.danger ? "danger" : "", disabled: !!item.disabled, title: item.title || "",
        onclick: (event) => { close(); item.run(event); } }, item.icon ? icon(item.icon) : null,
        el("span", {}, item.label, item.hint ? el("span", { class: "hint" }, item.hint) : null))));
    wrap.append(menu);
    // Opens toward the side with room: under the button's right edge, or its left edge near the page's left.
    const area = (wrap.closest("main") || document.body).getBoundingClientRect();
    if (menu.getBoundingClientRect().left < area.left + 8) { menu.style.right = "auto"; menu.style.left = "0"; }
    button.setAttribute("aria-expanded", "true");
    document.addEventListener("mousedown", outside, true);
    document.addEventListener("keydown", escape, true);
    const first = menu.querySelector("button:not(:disabled)");
    if (first) first.focus();
  });
  wrap.append(button);
  return wrap;
}

// A dialog over the page. `build(close)` gives its body and its buttons.
function dialog(title, build, options = {}) {
  const box = el("dialog", { class: options.wide ? "modal wide" : "modal" });
  const close = () => { box.close(); box.remove(); };
  const [body, buttons] = build(close);
  put(box, el("div", { class: "dialog-head" }, el("h3", {}, title),
    el("button", { class: "subtle icon-only small", "aria-label": "Close", onclick: close }, icon("close"))),
    el("div", { class: "dialog-body" }, body), el("div", { class: "dialog-foot" }, buttons));
  box.addEventListener("close", () => box.remove());
  document.body.append(box);
  box.showModal();
  return close;
}

// ------------------------------------------------------------------------------------- shows

let pendingDownload = null;     // a revision to fetch once logged in
const typesKept = {};           // per show: what GDTF Share had for its types and what was last said under each, kept while the page is drawn again
const patchKept = {};           // per show: the patch list's filter, selection, sort and what was last said, kept likewise
const paperKept = {};           // per show: the paperwork page's fixture filter
// While a page is built again, its panels fetch what they show. They are gathered here, so that the
// new page is only put on the window once they have it: the page then keeps its length, and
// whoever was half way down a patch list stays there.
let gathering = null;
const afterDraw = [];           // what panels want done once they are on the page: a list scrolled back to where it was
let building = 0;               // a page is being built off the window: nothing is on it yet to scroll
const drawn = () => { if (!building) for (const restore of afterDraw.splice(0)) restore(); };
const gather = (loading) => { if (gathering) gathering.push(loading); return loading; };

async function launch(kind, name, extra, button) {
  button.disabled = true;
  try {
    renderRun(await api("/api/launch", { json: Object.assign({ kind, name }, extra) }));
    pollRun();
  } catch (error) {
    pageNotice().replaceChildren(notice(error.message, "error"));
  }
  button.disabled = false;
}

const watchButton = (row, label) => {
  const blocked = ready() || (row.error ? "This file cannot be opened." : "");
  return el("button", { class: "primary", title: blocked, disabled: !!blocked, onclick: (event) => { event.stopPropagation(); launch("show", row.name, {}, event.currentTarget); } },
    icon("play"), label || "Watch Live");
};

// What a show's card and header say needs attention.
function showIssues(row) {
  const issues = [];
  for (const missing of row.missing || []) issues.push({ kind: "error", text: `${plural(missing.count, "fixture")} use the fixture type ${missing.name}, which is not in the MVR file. They are left out.` });
  if (row.without_type) issues.push({ kind: "error", text: `${plural(row.without_type, "fixture")} have no fixture type and are left out.` });
  if (row.unpatched) issues.push({ kind: "", text: `${plural(row.unpatched, "fixture")} have no DMX address.` });
  const placeholders = row.placeholders || [];
  if (placeholders.length) {
    const lamps = placeholders.reduce((sum, entry) => sum + entry.count, 0);
    issues.push({ kind: "", tab: "types", action: "Replace Fixture Types",
      text: `${plural(placeholders.length, "fixture type")} (${plural(lamps, "fixture")}) arrived from the console as placeholders: they dim only, with no colour, gobos, pan or tilt.` });
  }
  if (row.unplaced) {
    issues.push({ kind: "", tab: "patch", action: "Position Fixtures", filter: "no position",
      text: `${plural(row.unplaced, "fixture")} ${row.unplaced === 1 ? "has" : "have"} no position in the MVR and ${row.unplaced === 1 ? "is" : "are"} hidden.` });
  }
  return issues;
}

function showCard(row) {
  const open = () => go("show", { show: row.name, tab: route.show === row.name ? route.tab : "patch" });
  const stats = row.error || row.unchecked ? [] : [
    [row.fixtures || 0, "Fixtures"], [row.types.length, "Types"], [row.universes || 0, "Universes"], row.objects_with_shape || row.pieces ? [(row.objects_with_shape || 0) + (row.pieces || 0), "Set Pieces"] : null,
  ].filter(Boolean);
  const issues = showIssues(row);
  const badges = [];
  if (row.error) badges.push(el("span", { class: "badge bad" }, "Unreadable"));
  if (row.edited) badges.push(el("span", { class: "badge info", title: "Fixtures changed, added or removed since the MVR" }, `${row.edited} edited`));
  if ((row.placeholders || []).length) badges.push(el("span", { class: "badge warn" }, plural(row.placeholders.length, "placeholder type")));
  if (row.unplaced) badges.push(el("span", { class: "badge warn" }, `${row.unplaced.toLocaleString()} unpositioned`));
  if (!badges.length && !row.error && !row.unchecked && !issues.length) badges.push(el("span", { class: "badge good" }, "Ready"));
  const card = el("div", { class: "show-card", tabindex: "0", role: "button", "aria-label": `Open ${bare(row.name)}`,
    onclick: open, onkeydown: (event) => { if ((event.key === "Enter" || event.key === " ") && event.target === card) { event.preventDefault(); open(); } } },
    el("div", {}, el("div", { class: "title" }, bare(row.name)),
      el("div", { class: "meta" }, row.error ? el("span", { class: "bad" }, `Cannot be read: ${row.error}`) : row.unchecked ? "Not checked: the core is not built" : `MVR · ${size(row.size)}`)),
    stats.length ? el("div", { class: "stats" }, stats.map(([value, label]) => el("div", { class: "stat" }, el("b", {}, value.toLocaleString()), el("span", {}, label)))) : null,
    el("div", { class: "badges" }, badges),
    el("div", { class: "foot" }, el("button", { class: "subtle", onclick: (event) => { event.stopPropagation(); open(); } }, "Open"), watchButton(row)));
  return card;
}

// What the Shows page or a show's page draws again after anything changes: the whole page, built off the
// window with what its panels fetch gathered, then swapped in at the place it was scrolled to.
async function loadShows() {
  if (!$("view-shows").hidden) return drawShowsPage();
  if (!$("view-show").hidden) return drawShowPage();
}

async function drawShowsPage() {
  if (!$("shows-page").childElementCount) put($("shows-page"), el("p", { class: "muted" }, "Loading…"));
  let answer;
  try { answer = await api("/api/shows"); } catch (error) { return put($("shows-page"), notice(error.message, "error")); }
  const kept = $("shows-page").querySelector(":scope > .page-notice");
  const drop = el("div", { id: "drop", class: "drop", tabindex: "0", role: "button", "aria-label": "Import an MVR, GDTF or package file",
    onclick: () => $("file").click(), onkeydown: (event) => { if (event.key === "Enter" || event.key === " ") { event.preventDefault(); $("file").click(); } } },
    icon("upload"), el("div", {}, el("strong", {}, "Drop an MVR file to add a show"), el("span", {}, "or click to browse. GDTF fixture types, Openviz packages and 3D models (GLB, OBJ, 3DS) can be dropped too.")));
  const how = el("details", { class: "how" }, el("summary", {}, "Exporting an MVR from grandMA3 or Vectorworks"),
    el("p", {}, "Show files from consoles and CAD programs (.show, .vwx, .c2p) are closed formats. Export the rig as MVR, the open exchange format, which carries fixtures, positions, patch and fixture types."),
    el("p", {}, el("b", {}, "grandMA3: "), "Menu › Patch › Export, with the file type set to MVR. The file is saved to grandMA3's ", el("code", {}, "gma3_library/mvr"), " folder and listed below once it is there."),
    el("p", {}, el("b", {}, "Vectorworks Spotlight: "), "File › Export › Export MVR."));
  const found = !answer.found.length ? null : [
    el("h2", {}, "Available to Import"),
    el("div", { class: "list" }, answer.found.map((row) => el("div", { class: "row-item" },
      el("div", { class: "body" }, el("div", { class: "title" }, bare(row.name)), el("div", { class: "meta" }, `${row.application} export folder · ${size(row.size)}`)),
      el("div", { class: "actions" }, el("button", { onclick: busy(null, async () => {
        await api("/api/import", { json: { path: row.path } });
        pageNotice().replaceChildren();
        loadShows();
      }) }, icon("download"), "Import")))))];
  const offers = xchange && xchange.running ? xchange.offers.filter((offer) => !offer.mine) : [];
  const offered = !offers.length ? null : [
    el("h2", {}, "Offered over MVR-xchange"),
    el("div", { class: "list" }, offers.map((offer) => el("div", { class: "row-item" },
      el("div", { class: "body" }, el("div", { class: "title" }, offer.file || "Rig"),
        el("div", { class: "meta" }, `From ${offer.from} · ${size(offer.size)}${offer.comment ? ` · ${offer.comment}` : ""}`)),
      el("div", { class: "actions" }, el("button", { onclick: busy(null, async () => {
        pageNotice().replaceChildren(addedNotice(await api("/api/xchange/fetch", { json: { uuid: offer.uuid } })));
        loadShows();
        loadXchange();
      }) }, icon("download"), offer.have ? "Import Again" : "Import")))))];
  put($("shows-page"),
    pageHead("Shows", "Your rigs, imported as MVR. Open a show to work on its patch, fixture types, set and paperwork.",
      el("button", { onclick: () => $("file").click() }, icon("upload"), "Import…")),
    kept || el("div", { class: "page-notice" }),
    drop, how, found, offered,
    el("h2", {}, answer.shows.length ? `${plural(answer.shows.length, "Show")}` : "Shows"),
    answer.shows.length ? el("div", { class: "shows" }, answer.shows.map(showCard))
      : el("div", { class: "empty" }, el("b", {}, "No shows yet"), "Drop an MVR file above, or import one from your console's export folder."));
}

// What to say about a file that was just taken in; and, if a show of that name was here already,
// the offer to put the newer one in its place.
function addedNotice(answer) {
  if (answer.kind === "package") {
    // A show packed by Openviz on this computer or another: it came with its fixture types and media.
    const more = [answer.types_added ? `${plural(answer.types_added, "fixture type")} added to your library` : "",
      answer.media ? `${answer.media} media ${answer.media === 1 ? "file" : "files"} for its screens` : ""].filter(Boolean);
    return notice(`${bare(answer.name)} was imported from its package${answer.made ? ` (saved ${answer.made})` : ""}${answer.renamed ? " under a new name, as a show of that name exists" : ""}` +
      `${more.length ? `, with ${more.join(" and ")}` : ""}. ${(answer.warnings || []).join(" ")}`, answer.warnings && answer.warnings.length ? "" : "ok");
  }
  if (answer.kind === "model") {
    return notice(`${answer.name} ${answer.already_here ? "is already in" : "was added to"} your 3D models. Add it to a show under Set Pieces. ${(answer.warnings || []).join(" ")}`,
      answer.warnings && answer.warnings.length ? "" : "ok");
  }
  const where = answer.kind === "gdtf" ? "your fixture library" : "your shows";
  const said = notice(answer.already_here ? `${answer.name} is already in ${where}.`
    : `${answer.name} was added to ${where}${answer.renamed ? " under a new name, as a different file of that name exists" : ""}.`, "ok");
  if (answer.instead_of) {
    // A newer export of a show already here? Then it can take that show's place and keep what was done to it.
    said.querySelector(".text").append(" If this is a newer version of ", bare(answer.instead_of), ", ",
      el("button", { class: "link", onclick: busy(null, async () => {
        const done = await api("/api/show/update", { json: { name: answer.instead_of, newer: answer.name } });
        const words = [`${bare(done.name)} now uses the newer file: ${plural(done.fixtures, "fixture")}`];
        if (done.added) words.push(`${done.added} new`);
        if (done.gone) words.push(`${done.gone} removed`);
        let kept = `. Edits to ${plural(done.changes_kept, "fixture")} were kept`;
        if (done.changes_lost) kept += `; ${done.changes_lost} belonged to fixtures no longer in the file`;
        pageNotice().replaceChildren(notice(`${words.join(", ")}${done.changes_kept || done.changes_lost ? kept : ""}. The previous file was kept as ${done.kept_as}.`, "ok"));
        loadShows();
      }) }, `update ${bare(answer.instead_of)} with it`), ", keeping your edits, views and layer visibility.");
  }
  return said;
}

async function addFiles(files) {
  const drop = $("drop");
  if (drop) drop.classList.add("busy");
  const messages = [];
  for (const file of files) {
    try {
      messages.push(addedNotice(await api(`/api/files?name=${encodeURIComponent(file.name)}`, { body: file })));
    } catch (error) {
      messages.push(notice(error.message, error.code === "export_needed" ? "" : "error"));
    }
  }
  if (drop) drop.classList.remove("busy");
  pageNotice().replaceChildren(...messages);
  loadShows();
}

// ------------------------------------------------------------------------------- a show's page

const TABS = [["patch", "Patch"], ["types", "Fixture Types"], ["pieces", "Set Pieces"], ["screens", "Screens"], ["layers", "Layers"], ["paper", "Paperwork"]];

async function drawShowPage() {
  const name = route.show;
  if (!$("show-page").childElementCount || $("show-page").dataset.show !== name) put($("show-page"), el("p", { class: "muted" }, "Loading…"));
  $("show-page").dataset.show = name;
  let answer;
  try { answer = await api("/api/shows"); } catch (error) { return put($("show-page"), notice(error.message, "error")); }
  const row = answer.shows.find((entry) => entry.name === name);
  if (!row) return go("shows");
  const tab = TABS.some(([key]) => key === route.tab) && !(row.error || row.unchecked) ? route.tab : "patch";
  // Built off the window, with whatever its panel fetches gathered; put on the window when that has
  // come (or after a second and a half, whichever is first), at the place the page was scrolled to.
  gathering = [];
  building += 1;
  const body = row.error ? notice(`This file cannot be read: ${row.error}`, "error") : row.unchecked ? notice("This show cannot be listed: the core is not built. See System.", "error") : tabBody(row, tab);
  const fetched = gathering;
  gathering = null;
  await Promise.race([Promise.allSettled(fetched), new Promise((resolve) => setTimeout(resolve, 1500))]);
  await new Promise((resolve) => setTimeout(resolve, 0));    // let the panels draw what they were sent
  if (route.show !== name || $("view-show").hidden) { building -= 1; return; }    // gone elsewhere meanwhile
  const page = document.querySelector("main");
  const scrolled = page.scrollTop;
  const kept = $("show-page").querySelector(":scope > .page-notice");
  put($("show-page"), showTop(row, tab), kept || el("div", { class: "page-notice" }), body);
  page.scrollTop = scrolled;
  building -= 1;
  drawn();
}

function showTop(row, tab) {
  const facts = row.error || row.unchecked ? [size(row.size)] : [
    plural(row.fixtures || 0, "fixture"), plural(row.types.length, "fixture type"), row.universes ? `universes up to ${row.universes}` : null,
    row.objects_with_shape ? `${plural(row.objects_with_shape, "set piece")} in the MVR` : null, size(row.size)].filter(Boolean);
  const step = (which) => busy(null, async () => {
    const answer = await api(`/api/${which}`, { json: { name: row.name } });
    pageNotice().replaceChildren(notice(`${which === "undo" ? "Undone" : "Redone"}: ${answer.done}.`, "ok"));
    loadShows();
  });
  const undo = el("button", { class: "subtle icon-only", disabled: !row.undo, title: row.undo ? `Undo ${row.undo}` : "Nothing to undo", "aria-label": row.undo ? `Undo ${row.undo}` : "Undo", onclick: step("undo") }, icon("undo"));
  const redo = el("button", { class: "subtle icon-only", disabled: !row.redo, title: row.redo ? `Redo ${row.redo}` : "Nothing to redo", "aria-label": row.redo ? `Redo ${row.redo}` : "Redo", onclick: step("redo") }, icon("redo"));
  const blocked = ready() || (row.error ? "This file cannot be opened." : "");
  const more = menuButton(icon("more"), [
    { label: "Export MVR", hint: "A new MVR with your edits and set pieces written in", icon: "export", disabled: !!row.error, run: () => exportShow(row) },
    { label: "Save Package", hint: "One file with the show, its fixture types and screen media, for another computer", icon: "download", disabled: !!row.error, run: () => packageShow(row) },
    xchange && xchange.running ? { label: `Send to MVR-xchange Group “${xchange.group}”`, hint: "Offers the show, with your edits, to the group", icon: "network", run: () => sendShow(row) } : null,
    "-",
    { label: "Show in " + fileManager(), icon: "folder", run: () => reveal("shows") },
    row.edited ? { label: "Discard All Edits…", hint: `${plural(row.edited, "fixture")} back to how the MVR describes them`, icon: "trash", danger: true, run: () => discardEdits(row) } : null,
  ], { class: "icon-only", title: "More actions" });
  const issues = showIssues(row).map((issue) => notice(issue.text, issue.kind, issue.tab ? [" ", el("button", { class: "link", onclick: () => {
    if (issue.filter) (patchKept[row.name] = patchKept[row.name] || newPatchKept()).filter = issue.filter;
    go("show", { tab: issue.tab });
  } }, issue.action)] : null));
  const counts = {
    types: (row.placeholders || []).length ? [String(row.placeholders.length), "warn"] : null,
    pieces: row.pieces ? [String(row.pieces), ""] : null,
    layers: (() => {    // layers, classes and groups hidden in the visualizer
      const out = (row.hidden ? row.hidden.layers.length + row.hidden.classes.length : 0) + (row.groups || []).filter((group) => group.hidden).length;
      return out ? [`${out} hidden`, ""] : null;
    })(),
  };
  const layersOff = !((row.layer_list || []).length + (row.class_list || []).length);
  return el("div", { class: "show-top" },
    el("div", { class: "crumbs" }, el("button", { onclick: () => go("shows") }, "Shows"), icon("chevron"), el("span", {}, bare(row.name))),
    el("div", { class: "show-head" },
      el("div", {}, el("h1", {}, bare(row.name)),
        el("div", { class: "meta" }, facts.map((fact) => el("span", {}, fact)), row.edited ? el("span", { class: "badge info" }, `${row.edited.toLocaleString()} edited since the MVR`) : null)),
      el("div", { class: "actions" }, undo, redo, more,
        el("button", { title: blocked, disabled: !!blocked, onclick: (event) => launch("show", row.name, { editor: true }, event.currentTarget) }, icon("edit"), "Open in Unreal Editor"),
        watchButton(row))),
    issues.length ? el("div", { class: "issues" }, issues) : null,
    el("div", { class: "tabs", role: "tablist" }, TABS.map(([key, title]) => el("button", { role: "tab", class: key === tab ? "on" : "", "aria-selected": String(key === tab),
      disabled: (row.error || row.unchecked) && key !== "patch" || (key === "layers" && layersOff),
      title: key === "layers" && layersOff ? "This MVR has no layers or classes" : "",
      onclick: () => go("show", { tab: key }) }, title, counts[key] ? el("span", { class: `count ${counts[key][1]}` }, counts[key][0]) : null))));
}

async function exportShow(row) {
  try {
    const answer = await api("/api/export", { json: { name: row.name } });
    const pieces = answer.pieces ? ` and ${plural(answer.pieces, "set piece")}` : "";
    pageNotice().replaceChildren(notice(`Exported ${answer.file}, with edits to ${plural(answer.changed, "fixture")}${pieces}. `, "ok", revealButton("exports")));
  } catch (error) { pageNotice().replaceChildren(notice(error.message, "error")); }
}

async function packageShow(row) {
  try {
    const answer = await api("/api/package", { json: { name: row.name } });
    const extra = [plural(answer.fixtures, "fixture type"), answer.media ? `${answer.media} media ${answer.media === 1 ? "file" : "files"}` : ""].filter(Boolean).join(" and ");
    pageNotice().replaceChildren(notice(`Saved ${answer.file} (${size(answer.size)}) with ${extra}. Drop it on Openviz on another computer to open the show there. ` +
      (answer.missing.length ? `Not in your library, so not included: ${answer.missing.join(", ")}. ` : ""), answer.missing.length ? "" : "ok", revealButton("exports")));
  } catch (error) { pageNotice().replaceChildren(notice(error.message, "error")); }
}

async function sendShow(row) {
  try {
    const answer = await api("/api/xchange/send", { json: { name: row.name } });
    pageNotice().replaceChildren(notice(answer.told.length ? `${bare(row.name)} was offered to ${answer.told.join(", ")}.`
      : `${bare(row.name)} is on offer, but no other station is in the group yet. Stations that join will see it.`, answer.told.length ? "ok" : "info"));
  } catch (error) { pageNotice().replaceChildren(notice(error.message, "error")); }
}

function discardEdits(row) {
  dialog("Discard all edits?", (close) => {
    const confirm = el("button", { class: "primary", onclick: busy(null, async () => {
      const answer = await api("/api/edits/discard", { json: { name: row.name } });
      close();
      pageNotice().replaceChildren(answer.discarded ? notice(`${bare(row.name)} is back to how its MVR describes it. The edits were kept as ${answer.kept_as} in the shows folder, and Undo brings them back.`, "ok") : "");
      loadShows();
    }) }, "Discard Edits");
    return [el("p", { class: "muted" }, `The ${plural(row.edited, "fixture")} edited, added or removed in Openviz or the Unreal Editor return to how the MVR describes them. The edits are kept as a backup file, and Undo restores them.`),
      [el("button", { onclick: close }, "Cancel"), confirm]];
  });
}

function tabBody(row, tab) {
  if (tab === "types") return typesPanel(row.name);
  if (tab === "pieces") return piecesPanel(row.name);
  if (tab === "screens") return screensPanel(row.name);
  if (tab === "layers") return layersPanel(row);
  // The patch and the paperwork both read the show's fixtures.
  const holder = el("div", {}, el("p", { class: "muted" }, "Loading…"));
  gather(api(`/api/show?name=${encodeURIComponent(row.name)}`))
    .then((detail) => { put(holder, tab === "paper" ? paperPanel(detail) : patchPanel(detail)); drawn(); })
    .catch((error) => put(holder, notice(error.message, "error")));
  return holder;
}

// ------------------------------------------------------------------------------------- patch

const PROBLEMS = { overlap: "address conflict", past_end: "runs past the end of its universe", unpatched: "no DMX address" };
const NOTES = { moved: "moved", repatched: "re-patched", remoded: "mode changed", renamed: "renamed", reinverted: "pan/tilt inverted",
  notype: "fixture type missing (excluded)", nomode: "mode not found (first mode used)", added: "added in Openviz", removed: "removed from show",
  unplaced: "no position (hidden)", retyped: "fixture type replaced" };
const CHANGED = ["moved", "repatched", "remoded", "renamed", "reinverted", "retyped"];    // what "Reset to MVR" undoes
// Positions are shown and typed in metres, to the millimetre; rotations in degrees, to the hundredth.
const metres = (millimetres) => String(Math.round(millimetres) / 1000);
const degrees = (value) => String(Math.round(value * 100) / 100);
const newPatchKept = () => ({ filter: "", only: false, ticked: new Set(), note: null, sort: null, down: false });

// What each sortable column of the patch list sorts by: a key, or null for a row with nothing there.
const SORTS = {
  id: (row) => row.id === "" || row.id === undefined ? null : row.id,
  name: (row) => row.name,
  type: (row) => typeName(row.type),
  mode: (row) => row.mode,
  address: (row) => row.universe > 0 && !row.notes.includes("notype") && !row.notes.includes("removed") ? [row.universe, row.address] : null,
  slots: (row) => row.slots || null,
  // From the front of the stage back, then from stage right across, then from the floor up.
  place: (row) => row.place && !row.notes.includes("unplaced") ? [row.place[1], row.place[0], row.place[2]] : null,
  layer: (row) => row.layer || null,
};
const naturally = new Intl.Collator(undefined, { numeric: true, sensitivity: "base" });
function compareKeys(a, b) {
  if (Array.isArray(a)) {
    for (let k = 0; k < a.length; k++) if (a[k] !== b[k]) return a[k] < b[k] ? -1 : 1;
    return 0;
  }
  if (typeof a === "number" && typeof b === "number") return a - b;
  return naturally.compare(String(a), String(b));    // "Spot 2" before "Spot 10", IDs as numbers
}

const libraryTypes = async () => (await api("/api/fixtures")).fixtures.filter((row) => !row.error && (row.modes || []).length);

function patchPanel(detail) {
  const kept = patchKept[detail.name] || (patchKept[detail.name] = newPatchKept());
  const listed = new Map(detail.fixtures.filter((row) => row.uuid).map((row) => [row.uuid, row]));
  for (const uuid of [...kept.ticked]) if (!listed.has(uuid)) kept.ticked.delete(uuid);
  const said = el("div");
  if (kept.note) { said.replaceChildren(notice(kept.note[0], kept.note[1])); kept.note = null; }

  // At a glance: how full the patch is, whether it clashes, and what the rig weighs and draws.
  const adrift = detail.fixtures.filter((row) => row.problems.includes("past_end")).length;
  const totals = detail.totals;
  const onStage = detail.fixtures.filter((row) => !row.notes.includes("notype") && !row.notes.includes("removed")).length;
  const all = detail.fixtures.filter((row) => !row.notes.includes("notype")).length;
  const facts = [[onStage.toLocaleString(), onStage === all ? "Fixtures" : `of ${all.toLocaleString()} fixtures in show`], [String(detail.universes.length), "Universes"]];
  if (totals && totals.weight) facts.push([`${totals.weight >= 100 ? Math.round(totals.weight).toLocaleString() : totals.weight.toFixed(1)} kg`, "Weight"]);
  if (totals && totals.power) facts.push([totals.power >= 1000 ? `${(totals.power / 1000).toFixed(1)} kW` : `${Math.round(totals.power)} W`, "Power"]);
  const clash = detail.conflicts.length ? el("span", { class: "badge bad" }, plural(detail.conflicts.length, "address conflict"))
    : adrift ? null : el("span", { class: "badge good" }, "No address conflicts");
  const caveats = [];
  if (totals && totals.no_weight) caveats.push(`${plural(totals.no_weight, "fixture")} state no weight`);
  if (totals && totals.sources_only) caveats.push(`${totals.sources_only} state only their light sources' power`);
  const exportCsv = el("button", { onclick: busy(null, async () => {
    const answer = await api("/api/export/patch", { json: { name: detail.name } });
    said.replaceChildren(notice(`Saved ${answer.file}. `, "ok", revealButton("exports")));
  }, said) }, icon("download"), "Export CSV");
  const add = el("button", { onclick: () => addFixturesDialog(detail) }, icon("plus"), "Add Fixtures");
  const summary = el("div", { class: "summary-strip" },
    facts.map(([value, label]) => el("div", { class: "fact" }, el("b", {}, value), el("span", {}, label))),
    el("div", { class: "badges" }, clash, adrift ? el("span", { class: "badge bad" }, `${plural(adrift, "fixture")} past the end of a universe`) : null),
    el("div", { class: "grow" }), add, exportCsv);
  const universes = el("details", { class: "how", open: true }, el("summary", {}, "Universe usage"),
    el("div", { class: "universes" }, detail.universes.map((u) =>
      el("span", { class: `universe ${u.conflict ? "clash" : ""}`, title: `${plural(u.fixtures, "fixture")}, ${512 - u.used} slots free` },
        el("b", {}, `U${u.universe}`), ` ${u.used}/512`, el("span", { class: "bar" }, fill("", u.used / 5.12))))),
    caveats.length ? el("p", { class: "help" }, `Weight and power as stated in the fixture files: ${caveats.join("; ")}.`) : null);

  const body = el("tbody");
  const filter = el("input", { type: "search", placeholder: "Filter by name, type, mode, address or remark", "aria-label": "Filter fixtures", value: kept.filter });
  const problemsOnly = el("input", { type: "checkbox", checked: kept.only });
  const inspector = el("div", { class: "card inspector" });
  const count = el("span", { class: "muted small-text" });
  let shown = [];
  // Headings that sort the list: once by that column, again the other way round.
  const heads = [["ID", "id"], ["Name", "name"], ["Fixture Type", "type"], ["Mode", "mode"], ["Address", "address"], ["Slots", "slots"], ["Position (m)", "place"], ["Layer", "layer"]]
    .map(([title, key]) => {
      const arrow = el("span", { "aria-hidden": "true" });
      const cell = el("th", { class: ["id", "address", "slots", "place"].includes(key) ? "num" : "" },
        el("button", { class: "sort", title: `Sort by ${title.replace(" (m)", "").toLowerCase()}`, onclick: () => {
          if (kept.sort === key) kept.down = !kept.down; else { kept.sort = key; kept.down = false; }
          draw();
        } }, title, arrow));
      return { title, key, cell, arrow };
    });
  const remarksOf = (row) => [...row.problems.map((key) => PROBLEMS[key]), ...row.notes.map((key) => NOTES[key])].filter(Boolean).join(", ");
  const redrawInspector = () => {
    const chosen = detail.fixtures.filter((row) => kept.ticked.has(row.uuid));
    count.textContent = chosen.length ? `${plural(chosen.length, "fixture")} selected` : "";
    inspector.classList.toggle("has-selection", chosen.length > 0);
    put(inspector, chosen.length ? fixtureInspector(detail, chosen, kept)
      : el("div", { class: "card-body" }, el("div", { class: "placeholder" }, icon("pointer"),
        el("div", {}, "Select fixtures to edit them"), el("div", { class: "small-text" }, "Position, rotation, patch, mode, name and ID. Several can be edited at once."))));
  };
  const draw = () => {
    kept.filter = filter.value;
    kept.only = problemsOnly.checked;
    const terms = filter.value.toLowerCase().split(/\s+/).filter(Boolean);
    shown = detail.fixtures.filter((row) => {
      if (problemsOnly.checked && !row.problems.length && !row.notes.includes("notype")) return false;
      const text = `${row.id} ${row.name} ${row.type} ${row.mode} ${row.layer} ${row.universe}.${row.address} ${remarksOf(row)}`.toLowerCase();
      return terms.every((term) => text.includes(term));
    });
    if (kept.sort && SORTS[kept.sort]) {
      const order = SORTS[kept.sort];
      // Rows with nothing to sort by (no address, no position) stay at the foot either way round.
      shown = shown.map((row, index) => [row, index]).sort(([a, i], [b, j]) => {
        const ka = order(a), kb = order(b);
        if ((ka === null) !== (kb === null)) return ka === null ? 1 : -1;
        const c = ka === null ? 0 : compareKeys(ka, kb);
        return (kept.down ? -c : c) || i - j;
      }).map(([row]) => row);
    }
    for (const head of heads) {
      const on = head.key === kept.sort;
      head.cell.setAttribute("aria-sort", on ? (kept.down ? "descending" : "ascending") : "none");
      head.arrow.textContent = on ? (kept.down ? " ↓" : " ↑") : "";
    }
    body.replaceChildren(...shown.map((row) => {
      const gone = row.notes.includes("removed");
      const out = row.notes.includes("notype") || gone;
      const address = out ? "" : row.universe > 0 ? `${row.universe}.${row.address}` : "—";
      const place = out || !row.place ? "" : row.notes.includes("unplaced") ? "—" : `${metres(row.place[0])}, ${metres(row.place[1])}, ${metres(row.place[2])}`;
      const tick = el("input", { type: "checkbox", checked: kept.ticked.has(row.uuid), disabled: !row.uuid, "aria-label": `Select ${row.name}` });
      const line = el("tr", { class: `${row.problems.length ? "problem" : out ? "out" : ""} ${kept.ticked.has(row.uuid) ? "chosen" : ""}` },
        el("td", {}, tick), el("td", { class: "num" }, row.id), el("td", {}, row.name), el("td", { title: row.type }, typeName(row.type)), el("td", {}, row.mode),
        el("td", { class: "num" }, address), el("td", { class: "num" }, row.slots ? String(row.slots) : ""), el("td", { class: "num" }, place),
        el("td", {}, row.layer), el("td", { class: "wrap remarks" }, remarksOf(row)));
      // As in a file list: a click selects that fixture alone (again: none), Command or Control (or its box)
      // adds or takes it away, Shift selects the run from the fixture clicked before.
      const choose = (how) => {
        if (!row.uuid) return;
        if (how === "range" && kept.anchor && shown.some((other) => other.uuid === kept.anchor)) {
          const from = shown.findIndex((other) => other.uuid === kept.anchor);
          const to = shown.indexOf(row);
          for (const other of shown.slice(Math.min(from, to), Math.max(from, to) + 1)) if (other.uuid) kept.ticked.add(other.uuid);
        } else if (how === "toggle") {
          if (kept.ticked.has(row.uuid)) kept.ticked.delete(row.uuid); else kept.ticked.add(row.uuid);
          kept.anchor = row.uuid;
        } else {
          const alone = kept.ticked.size === 1 && kept.ticked.has(row.uuid);
          kept.ticked.clear();
          if (!alone) kept.ticked.add(row.uuid);
          kept.anchor = row.uuid;
        }
        draw();
        redrawInspector();
      };
      tick.addEventListener("click", (event) => { event.stopPropagation(); choose(event.shiftKey ? "range" : "toggle"); });
      line.addEventListener("click", (event) => choose(event.shiftKey ? "range" : event.metaKey || event.ctrlKey ? "toggle" : "one"));
      return line;
    }));
    if (!shown.length) body.replaceChildren(el("tr", {}, el("td", { colspan: "10", class: "muted" }, "No fixtures match the filter.")));
  };
  filter.addEventListener("input", draw);
  problemsOnly.addEventListener("change", draw);

  const select = (rows) => { kept.ticked.clear(); for (const row of rows) if (row.uuid) kept.ticked.add(row.uuid); draw(); redrawInspector(); };
  const loose = detail.fixtures.filter((row) => row.notes.includes("unplaced"));
  // Groups of one's own: selected from here, made from what is selected, hidden from the Layers tab.
  const groups = detail.groups || [];
  const inGroup = (group) => { const ids = new Set(group.uuids.map((uuid) => uuid.toUpperCase())); return detail.fixtures.filter((row) => row.uuid && ids.has(row.uuid.toUpperCase())); };
  const selectMenu = menuButton("Select", [
    { label: "All Listed", hint: "Every fixture the filter shows", run: () => select(shown) },
    loose.length ? { label: `Unpositioned (${loose.length.toLocaleString()})`, hint: "Fixtures the MVR gives no position", run: () => select(loose) } : null,
    { label: "None", run: () => select([]) },
    groups.length ? "-" : null,
    ...groups.map((group) => ({ label: `${group.name} (${group.count.toLocaleString()})`, hint: group.hidden ? "Group, hidden in the visualizer" : "Group", run: () => select(inGroup(group)) })),
    "-",
    { label: "Save Selection as Group…", get disabled() { return !kept.ticked.size; }, hint: "Name the selected fixtures, to select them again or hide them together", run: () => groupDialog(detail.name, [...kept.ticked], groups) },
  ]);
  const noPlace = !loose.length ? null : notice(`${plural(loose.length, "fixture")} ${loose.length === 1 ? "has" : "have"} no position in the MVR: the console patched ${loose.length === 1 ? "it" : "them"} but never placed ${loose.length === 1 ? "it" : "them"} in 3D. ` +
    `${loose.length === 1 ? "It is" : "They are"} hidden, since ${loose.length === 1 ? "it" : "they"} would all light up in one spot. Select ${loose.length === 1 ? "it" : "them"}, give the first a position and set the spacing for the rest, or place them in the Unreal Editor.`, "info");

  const wrap = el("div", { class: "tablewrap" }, el("table", { class: "select" },
    el("thead", {}, el("tr", {}, ["", "ID", "Name", "Fixture Type", "Mode", "Address", "Slots", "Position (m)", "Layer", "Remarks"].map((title) => {
      const head = heads.find((h) => h.title === title);
      return head ? head.cell : el("th", {}, title);
    }))), body));
  draw();
  redrawInspector();
  wrap.addEventListener("scroll", () => { kept.scroll = wrap.scrollTop; });
  if (kept.scroll) afterDraw.push(() => { wrap.scrollTop = kept.scroll; });    // once it is on the page: before that it has no height to scroll
  return el("div", {}, summary, universes, said, noPlace,
    el("div", { class: "patch-layout" },
      el("div", {},
        el("div", { class: "toolbar" }, el("div", { class: "search" }, icon("search"), filter),
          el("label", { class: "check small-text" }, problemsOnly, "Problems only"), selectMenu, count),
        wrap,
        el("p", { class: "help" }, "Addresses are universe.slot of each fixture's first DMX break. Positions are x, y, z in metres: x toward stage left (house right), y upstage, z up. " +
          (detail.open ? "This show is open in the visualizer: edits appear there within half a second." : "A show that is open in the visualizer picks up edits immediately.") +
          " The MVR file itself is never modified; Export MVR writes a new one.")),
      inspector));
}

// Naming selected fixtures as a group of the show, or adding them to one it has.
function groupDialog(show, uuids, groups) {
  dialog("Save as Group", (close) => {
    const said = el("div");
    const name = el("input", { type: "text", class: "wide", placeholder: "Group name", "aria-label": "Group name", autocomplete: "off" });
    const into = el("select", { "aria-label": "Add to an existing group" }, el("option", { value: "" }, "A new group"),
      groups.map((group) => el("option", { value: group.id }, `Add to ${group.name}`)));
    into.addEventListener("change", () => { name.disabled = !!into.value; });
    const save = el("button", { class: "primary" }, "Save Group");
    save.addEventListener("click", busy(save, async () => {
      if (into.value) await api("/api/group/set", { json: { name: show, id: into.value, changes: { add: uuids } } });
      else await api("/api/group/add", { json: { name: show, group: name.value, uuids } });
      close();
      loadShows();
    }, said));
    const field = (text, control) => el("label", { class: "field" }, el("span", {}, text), control);
    return [[el("p", { class: "muted" }, `${plural(uuids.length, "fixture")} selected. A group can be selected again from the Select menu, and hidden in the visualizer from the Layers tab.`),
      groups.length ? field("Save To", into) : null, field("Name", name), said],
      [el("button", { onclick: close }, "Cancel"), save]];
  });
}

// The inspector beside the patch list, for the fixtures selected. For one fixture every field shows
// what it has, and what is changed is applied. For several an empty field leaves them as they are;
// a position is that of the first with the others spaced out from it, a patch runs on from one to
// the next, names and IDs count up.
function fixtureInspector(detail, chosen, kept) {
  const one = chosen.length === 1 ? chosen[0] : null;
  const many = chosen.length > 1;
  const live = chosen.filter((row) => !row.notes.includes("removed"));
  const said = el("div");
  const input = (label, value) => el("input", { type: "text", "aria-label": label, value: value == null ? "" : String(value), autocomplete: "off", placeholder: many ? "—" : "" });
  const field = (text, control) => el("label", { class: "field" }, el("span", {}, text), control);

  const name = input("Name", one ? one.name : "");
  const numbered = input("Number names from", "");
  const id = input(many ? "First fixture ID" : "Fixture ID", one ? one.id : "");
  const patched = one && one.universe > 0;
  const universe = input("Universe", patched ? one.universe : "");
  const address = input("Address", patched ? one.address : "");
  // The modes every selected fixture has: one of them can be set for all.
  const shared = live.length && live.every((row) => row.modes.length) ? live[0].modes.filter((mode) => live.every((row) => row.modes.some((other) => other.name === mode.name))) : [];
  const mode = !shared.length ? null : el("select", { "aria-label": "Mode", class: "wide" },
    many ? el("option", { value: "" }, "Unchanged") : null,
    shared.map((entry) => el("option", { value: entry.name, selected: !!one && entry.name === one.mode }, `${entry.name} (${plural(entry.slots, "slot")})`)));
  const placed = one && one.place && !one.notes.includes("unplaced");
  const at = ["x", "y", "z"].map((axis, index) => input(`Position ${axis}`, placed ? metres(one.place[index]) : ""));
  const step = ["x", "y", "z"].map((axis) => input(`Spacing ${axis}`, ""));
  const by = ["x", "y", "z"].map((axis) => input(`Move by ${axis}`, ""));
  const turn = ["X", "Y", "Z"].map((axis, index) => input(`Rotation ${axis}`, one && one.place ? degrees(one.place[3 + index]) : ""));
  const mirror = (key, has) => !live.some((row) => row[has]) ? null : el("select", { "aria-label": key === "invert_pan" ? "Pan" : "Tilt", class: "wide" },
    many ? el("option", { value: "" }, "Unchanged") : null,
    el("option", { value: "0", selected: !!one && !one[key] }, "Normal"), el("option", { value: "1", selected: !!one && one[key] }, "Inverted"));
  const pan = mirror("invert_pan", "has_pan");
  const tilt = mirror("invert_tilt", "has_tilt");

  // What the form says is to change. Throws with what is wrong.
  const read = () => {
    const changes = {};
    const number = (box, what, whole) => {
      const text = box.value.trim().replace(",", ".");
      const value = Number(text);
      if (!text || !Number.isFinite(value) || (whole && !Number.isInteger(value))) throw new Error(`${what} must be ${whole ? "a whole number" : "a number"}.`);
      return value;
    };
    const was = (box, value) => one && box.value.trim() === String(value == null ? "" : value);    // one fixture, and the field as it was shown
    if (name.value.trim() && !was(name, one && one.name)) {
      changes.name = name.value.trim();
      if (many && numbered.value.trim()) changes.number = number(numbered, "The starting number", true);
    }
    if (!was(id, one && one.id) && (id.value.trim() || one)) changes.id = id.value.trim() ? number(id, "Fixture ID", true) : "";
    if (universe.value.trim() || address.value.trim()) {
      if (!(patched && was(universe, one.universe) && was(address, one.address))) {
        changes.universe = number(universe, "Universe", true);
        if (changes.universe > 0) changes.address = number(address, "Address", true);
      }
    } else if (patched) {
      changes.universe = 0;    // both emptied: unpatched
    }
    if (mode && mode.value && !(one && mode.value === one.mode)) changes.mode = mode.value;
    ["x", "y", "z"].forEach((axis, index) => {
      if (at[index].value.trim() && !(placed && was(at[index], metres(one.place[index])))) changes[axis] = number(at[index], `Position ${axis}`) * 1000;
      if (many && step[index].value.trim()) changes[`step${axis}`] = number(step[index], `Spacing ${axis}`) * 1000;
      if (by[index].value.trim()) changes[`d${axis}`] = number(by[index], `Move by ${axis}`) * 1000;
    });
    ["rx", "ry", "rz"].forEach((key, index) => {
      if (turn[index].value.trim() && !(one && one.place && was(turn[index], degrees(one.place[3 + index])))) changes[key] = number(turn[index], "Rotation");
    });
    if (pan && pan.value && !(one && (pan.value === "1") === one.invert_pan)) changes.invertpan = pan.value === "1";
    if (tilt && tilt.value && !(one && (tilt.value === "1") === one.invert_tilt)) changes.inverttilt = tilt.value === "1";
    return changes;
  };
  const title = one ? `${one.id ? `${one.id} · ` : ""}${one.name}` : plural(chosen.length, "fixture");
  const send = async (changes, what, rows) => {
    await api("/api/show/set", { json: { name: detail.name, uuids: (rows || live).map((row) => row.uuid), changes } });
    kept.note = [what, "ok"];
    loadShows();
  };
  const apply = el("button", { class: "primary" }, "Apply");
  apply.addEventListener("click", busy(apply, async () => {
    const changes = read();
    if (!Object.keys(changes).length) throw new Error("Nothing was changed.");
    if (!live.length) throw new Error("Restore removed fixtures before editing them.");
    await send(changes, `${one ? title : plural(live.length, "fixture")} updated.`);
  }, said));
  for (const box of [name, numbered, id, universe, address, ...at, ...step, ...by, ...turn]) {
    box.addEventListener("keydown", (event) => { if (event.key === "Enter") apply.click(); });
  }
  const spin = el("button", { title: "Rotates each fixture 180° about its own vertical axis; a moving head then tilts the other way" }, "Rotate 180°");
  spin.addEventListener("click", busy(spin, () => send({ spin: 180 }, `${one ? title : plural(live.length, "fixture")} rotated 180°.`), said));
  const changed = live.filter((row) => row.notes.some((note) => CHANGED.includes(note)) && !row.notes.includes("added"));
  const reset = !changed.length ? null : el("button", { title: "Position, patch, mode, name, inversion and fixture type as the MVR has them" }, "Reset to MVR");
  if (reset) reset.addEventListener("click", busy(reset, () => send({ reset: "all" }, `${plural(changed.length, "fixture")} reset to the MVR.`, changed), said));
  // Removing fixtures, or restoring those of the MVR. A fixture added in Openviz is deleted when removed.
  const gone = chosen.filter((row) => row.notes.includes("removed"));
  const remove = async (rows, restore) => {
    await api("/api/show/remove", { json: { name: detail.name, uuids: rows.map((row) => row.uuid), restore } });
    kept.note = [`${plural(rows.length, "fixture")} ${restore ? "restored" : "removed from the show"}.`, "ok"];
    loadShows();
  };
  const out = !live.length ? null : el("button", { class: "danger", title: "Removes them from the show. Fixtures from the MVR can be restored; fixtures added in Openviz are deleted." }, icon("trash"), "Remove");
  if (out) out.addEventListener("click", busy(out, () => remove(live, false), said));
  const restore = !gone.length ? null : el("button", {}, gone.length === chosen.length ? "Restore" : `Restore ${gone.length} Removed`);
  if (restore) restore.addEventListener("click", busy(restore, () => remove(gone, true), said));
  const retyped = live.filter((row) => row.notes.includes("retyped"));
  const changeType = !live.length ? null : el("button", { title: "Replace the fixture type of these fixtures with one from your library" }, "Change Type…");
  if (changeType) changeType.addEventListener("click", () => changeTypeDialog(detail, live, one ? title : plural(live.length, "fixture"), send));
  const revertType = !retyped.length ? null : el("button", { title: "The fixture type and mode the MVR specifies" }, "Revert Type");
  if (revertType) revertType.addEventListener("click", busy(revertType, () => send({ reset: "type" }, `${plural(retyped.length, "fixture")} reverted to the MVR's fixture type.`, retyped), said));

  // Aiming fixtures that do not pan or tilt: each is rotated where it hangs so that its beam points at a spot.
  const still = live.filter((row) => !row.has_pan && !row.has_tilt);
  const aimAt = ["x", "y", "z"].map((axis) => el("input", { type: "text", "aria-label": `Aim at ${axis}`, autocomplete: "off" }));
  // Somewhere to aim at: the rig file's focus points, and the show's set pieces once they are fetched.
  const targets = {};
  const focusPoints = detail.focus_points || [];
  focusPoints.forEach((point) => { targets[`focus:${point.uuid}`] = point.place; });
  const aimPick = el("select", { "aria-label": "Aim at a focus point or set piece", class: "wide" }, el("option", { value: "" }, "Choose…"),
    focusPoints.length ? el("optgroup", { label: "Focus Points" }, focusPoints.map((point) => el("option", { value: `focus:${point.uuid}` }, point.name))) : null);
  const aimPieces = el("div", { class: "grow", hidden: !focusPoints.length }, el("label", { class: "field" }, el("span", {}, "Focus Point or Set Piece"), aimPick));
  aimPick.addEventListener("change", () => {
    const place = targets[aimPick.value];
    if (place) place.forEach((value, index) => { aimAt[index].value = metres(value); });
  });
  // One fixture the rig file aims at a focus point: that point to start with.
  if (one && one.focus && targets[`focus:${one.focus}`]) {
    aimPick.value = `focus:${one.focus}`;
    targets[aimPick.value].forEach((value, index) => { aimAt[index].value = metres(value); });
  }
  const aimButton = el("button", {}, still.length === 1 ? "Aim Fixture" : "Aim Fixtures");
  const unaimed = { moves: "they pan or tilt, so the console aims them", unplaced: "they have no position yet", nobeam: "they have no beam", unreadable: "their fixture type cannot be read" };
  aimButton.addEventListener("click", busy(aimButton, async () => {
    const point = aimAt.map((box, index) => {
      const text = box.value.trim().replace(",", ".");
      const value = Number(text);
      if (!text || !Number.isFinite(value)) throw new Error(`Aim At ${"XYZ"[index]} must be a number of metres.`);
      return Math.round(value * 1000);
    });
    const answer = await api("/api/show/set", { json: { name: detail.name, uuids: still.map((row) => row.uuid), changes: { aimx: point[0], aimy: point[1], aimz: point[2] } } });
    const reasons = [...new Set(answer.not_aimed.map((entry) => unaimed[entry.why] || entry.why))];
    if (!answer.aimed.length) throw new Error(`Nothing was aimed: ${reasons.join("; ")}.`);
    kept.note = [`${plural(answer.aimed.length, "fixture")} aimed at ${point.map((value) => metres(value)).join(", ")} m.` +
      (answer.not_aimed.length ? ` ${plural(answer.not_aimed.length, "fixture")} not aimed: ${reasons.join("; ")}.` : ""), "ok"];
    loadShows();
  }, said));
  for (const box of aimAt) box.addEventListener("keydown", (event) => { if (event.key === "Enter") aimButton.click(); });

  // Hanging them on a truss of the show's set pieces: at its height, spread evenly along it, rotated with it.
  const hang = el("div");
  if (live.length) {
    api(`/api/show/pieces?name=${encodeURIComponent(detail.name)}`).then((answer) => {
      if (still.length && answer.pieces.length) {
        // A set piece's middle, half way up: where a performer's chest or a deck's top would be lit.
        answer.pieces.forEach((piece) => { targets[`piece:${piece.id}`] = [piece.x, piece.y, piece.z + piece.height / 2]; });
        aimPick.append(el("optgroup", { label: "Set Pieces" }, answer.pieces.map((piece) => el("option", { value: `piece:${piece.id}` }, piece.name))));
        aimPieces.hidden = false;
      }
      const trusses = answer.pieces.filter((piece) => ["truss", "triangle", "pipe"].includes(piece.kind));
      if (!trusses.length) return;
      const pick = el("select", { "aria-label": "Truss", class: "wide" }, trusses.map((truss) => el("option", { value: truss.id }, `${truss.name} (${metres(truss.width)} m)`)));
      const hangButton = el("button", {}, "Hang");
      hangButton.addEventListener("click", busy(hangButton, () => {
        const truss = trusses.find((piece) => piece.id === pick.value);
        const angle = truss.turn * Math.PI / 180;
        const along = [Math.cos(angle), Math.sin(angle)];
        const spacing = truss.width / live.length;
        const first = -truss.width / 2 + spacing / 2;
        return send({ x: Math.round(truss.x + along[0] * first), y: Math.round(truss.y + along[1] * first), z: Math.round(truss.z),
                      stepx: Math.round(along[0] * spacing * 1000) / 1000, stepy: Math.round(along[1] * spacing * 1000) / 1000, rz: truss.turn },
          `${one ? title : plural(live.length, "fixture")} hung on ${truss.name}.`);
      }, said));
      put(hang, el("h4", {}, "Hang on Truss or Pipe"), el("div", { class: "grid2" }, pick, hangButton),
        el("p", { class: "help" }, many ? "Spaced evenly along it, at its height." : "At its middle, at its height."));
    }).catch(() => null);
  }
  const loose = live.length && live.every((row) => row.notes.includes("unplaced"));
  return el("div", {},
    el("div", { class: "card-head" }, el("div", {}, el("h3", {}, title), many ? el("div", { class: "sub" }, "Empty fields leave each fixture unchanged.") : null),
      el("button", { class: "subtle small", onclick: () => { kept.ticked.clear(); loadShows(); } }, "Clear")),
    el("div", { class: "card-body" },
      !live.length ? el("p", { class: "muted" }, many ? "These fixtures are removed from the show. Restore them to edit them." : "This fixture is removed from the show. Restore it to edit it.") : [
        el("h4", {}, "Identity"),
        el("div", { class: "grid2" }, field("Name", name), field(many ? "IDs Starting At" : "Fixture ID", id)),
        many ? field("Number Names From", numbered) : null,
        el("h4", {}, "DMX"),
        el("div", { class: "grid2" }, field("Universe", universe), field(many ? "Start Address" : "Address", address)),
        mode ? field("Mode", mode) : null,
        el("h4", {}, many ? "Position of First (m)" : "Position (m)"),
        el("div", { class: "grid3" }, at.map((box, index) => field(["X", "Y", "Z"][index], box))),
        many ? [el("h4", {}, "Spacing (m)"), el("div", { class: "grid3" }, step.map((box, index) => field(["X", "Y", "Z"][index], box)))] : null,
        el("h4", {}, "Move By (m)"),
        el("div", { class: "grid3" }, by.map((box, index) => field(["X", "Y", "Z"][index], box))),
        el("h4", {}, "Rotation (°)"),
        el("div", { class: "grid3" }, turn.map((box, index) => field(["X", "Y", "Z"][index], box))),
        still.length ? [el("h4", {}, "Aim At (m)"),
          el("div", { class: "grid3" }, aimAt.map((box, index) => field(["X", "Y", "Z"][index], box))),
          el("div", { class: "fields tight" }, aimPieces, aimButton),
          el("p", { class: "help" }, `Rotates ${still.length === 1 ? "the fixture" : `each of the ${still.length} fixtures`} where it hangs so that its beam points at this spot. ` +
            "0, 0, 0 is the rig's origin, usually centre stage on the floor." + (still.length < live.length ? ` ${plural(live.length - still.length, "moving head")} ${live.length - still.length === 1 ? "is" : "are"} left as ${live.length - still.length === 1 ? "it is" : "they are"}: the console aims them.` : ""))] : null,
        pan || tilt ? [el("h4", {}, "Pan and Tilt"), el("div", { class: "grid2" }, pan ? field("Pan", pan) : null, tilt ? field("Tilt", tilt) : null),
          el("p", { class: "help" }, "For a fixture hung the other way round. If a whole fixture type moves the wrong way, correct it under Fixture Types.")] : null,
        loose ? el("p", { class: "help" }, many ? "These fixtures have no position yet. Give the first its X, Y and Z and a spacing (0.5 in X for a row half a metre apart); they appear once applied."
          : "This fixture has no position yet. Give it X, Y and Z; it appears once applied.") : null,
        hang],
      said,
      el("div", { class: "button-row" }, live.length ? apply : null, live.length ? spin : null, reset, changeType, revertType, restore, out)));
}

// Replacing the fixture type of selected fixtures with one from the library, for those fixtures alone.
async function changeTypeDialog(detail, live, what, send) {
  let types = [];
  try { types = await libraryTypes(); } catch (error) { pageNotice().replaceChildren(notice(error.message, "error")); return; }
  dialog("Change Fixture Type", (close) => {
    if (!types.length) return [el("p", { class: "muted" }, "Your fixture library is empty. Download fixture types under Fixture Library, or drop GDTF files on this window."), [el("button", { onclick: close }, "Close")]];
    const said = el("div");
    const type = el("select", { "aria-label": "Fixture type", class: "wide" }, types.map((row) => el("option", { value: row.file }, typeName(row.file))));
    const mode = el("select", { "aria-label": "Mode", class: "wide" });
    const fillModes = () => {
      const chosen = types.find((row) => row.file === type.value);
      mode.replaceChildren(el("option", { value: "" }, "Same channel count as now"),
        ...(chosen ? chosen.modes : []).map((entry) => el("option", { value: entry.name }, `${entry.name} (${plural(entry.footprint, "slot")})`)));
    };
    type.addEventListener("change", fillModes);
    fillModes();
    const apply = el("button", { class: "primary" }, "Change Type");
    apply.addEventListener("click", busy(apply, async () => {
      await send(Object.assign({ type: type.value }, mode.value ? { mode: mode.value } : {}), `${what}: fixture type changed to ${typeName(type.value)}.`);
      close();
    }, said));
    return [[el("p", { class: "muted" }, `Replaces the fixture type of ${what} only. The MVR is not modified, and an MVR export carries the new type. To replace a type for every fixture that uses it, use Fixture Types. The patch is kept; re-patch if the new mode has another channel count.`),
      el("div", { class: "stack" }, el("label", { class: "field" }, el("span", {}, "Fixture Type"), type), el("label", { class: "field" }, el("span", {}, "Mode"), mode)), said],
      [el("button", { onclick: close }, "Cancel"), apply]];
  });
}

// Adding fixtures of a library type to a show. They are placed in a row in front of the stage.
async function addFixturesDialog(detail) {
  let types = [];
  try { types = await libraryTypes(); } catch (error) { pageNotice().replaceChildren(notice(error.message, "error")); return; }
  dialog("Add Fixtures", (close) => {
    if (!types.length) return [el("p", { class: "muted" }, "Your fixture library is empty. Download fixture types under Fixture Library, or drop GDTF files on this window."), [el("button", { onclick: close }, "Close")]];
    const said = el("div");
    const type = el("select", { "aria-label": "Fixture type", class: "wide" }, types.map((row) => el("option", { value: row.file }, typeName(row.file))));
    const mode = el("select", { "aria-label": "Mode", class: "wide" });
    const fillModes = () => {
      const chosen = types.find((row) => row.file === type.value);
      mode.replaceChildren(...chosen.modes.map((entry) => el("option", { value: entry.name }, `${entry.name} (${plural(entry.footprint, "slot")})`)));
    };
    type.addEventListener("change", fillModes);
    fillModes();
    const number = (label, value, min, max) => el("input", { type: "number", "aria-label": label, value: String(value), min: String(min), max: String(max) });
    const count = number("Quantity", 1, 1, 512);
    const universe = number("Universe", 0, 0, 32767);
    const address = number("Start address", 1, 1, 512);
    const firstId = number("First fixture ID", 0, 0, 999999);
    const add = el("button", { class: "primary" }, icon("plus"), "Add Fixtures");
    add.addEventListener("click", busy(add, async () => {
      const answer = await api("/api/show/add", { json: { name: detail.name, fixture: type.value, mode: mode.value, count: Number(count.value),
        universe: Number(universe.value), address: Number(address.value), first_id: Number(firstId.value) } });
      close();
      pageNotice().replaceChildren(notice(`${plural(answer.added.length, "fixture")} added. They stand in a row in front of the stage: position them in the inspector or the Unreal Editor.`, "ok"));
      loadShows();
    }, said));
    const field = (text, control) => el("label", { class: "field" }, el("span", {}, text), control);
    return [[
      el("div", { class: "stack" }, field("Fixture Type", type), el("div", { class: "grid2" }, field("Mode", mode), field("Quantity", count)),
        el("div", { class: "grid3" }, field("Universe", universe), field("Start Address", address), field("First Fixture ID", firstId))),
      el("p", { class: "help" }, "Universe 0 leaves them unpatched; ID 0 leaves them unnumbered. Each fixture is patched after the previous one, moving to the next universe when one is full."),
      said], [el("button", { onclick: close }, "Cancel"), add]];
  });
}

// ------------------------------------------------------------------------------- fixture types

// A show's fixture types. A console does not always say what its fixtures can do: grandMA3 sends
// every type of its own library as a placeholder. Each can be replaced with a real fixture file,
// from the library or from GDTF Share, matched to the modes in use by their channel count.
function typesPanel(name) {
  const panel = el("div", {}, el("p", { class: "muted" }, "Loading fixture types…"));
  const said = el("div");
  // Kept outside the panel: a change draws the page again, and this panel with it.
  const kept = typesKept[name] || (typesKept[name] = { found: {}, notes: {}, open: {} });
  kept.open = kept.open || {};    // types the MVR describes whose replacing was asked for
  const found = kept.found;    // what GDTF Share has for a type, once searched
  const notes = kept.notes;    // what there is to say under a type: what was done, or why it could not be
  let asking = null;           // the download that waits for a GDTF Share sign-in: { spec, row }
  const slots = (count) => plural(count, "slot");
  const fixtureName = (row) => [row.manufacturer, row.name || row.fixture].filter(Boolean).join(" ");
  const draw = (answer) => {
    // `spec` is the type the action is about: what comes of it is said under that type, where the click was.
    const act = async (path, body, done, spec) => {
      try {
        const next = await api(path, { json: { name, ...body } });
        const text = done ? done(next) : "";
        if (spec) notes[spec] = text ? notice(text, "ok") : null;
        said.replaceChildren(!spec && text ? notice(text, "ok") : "");
        draw(next);
        loadShows();
        return true;
      } catch (error) {
        if (spec) { notes[spec] = notice(error.message, "error"); draw(answer); }
        else said.replaceChildren(notice(error.message, "error"));
        return false;
      }
    };
    // Downloads a fixture from GDTF Share and makes it the type's replacement; GDTF Share wants a sign-in for that.
    const fetchFor = async (entry, row) => {
      try {
        const next = await api("/api/types/share", { json: { name, type: entry.spec, rid: row.rid } });
        asking = null;
        notes[entry.spec] = notice(`${fixtureName(row)} was downloaded and now replaces ${typeName(entry.name)}.`, "ok");
        draw(next);
        loadShows();
      } catch (error) {
        if (error.code === "login_required") {
          asking = { spec: entry.spec, row };
          notes[entry.spec] = null;
        } else {
          asking = null;
          notes[entry.spec] = notice(error.message, "error");
        }
        draw(answer);
      }
    };
    const signIn = (entry) => {
      const user = el("input", { type: "text", autocomplete: "username", placeholder: "User name", "aria-label": "GDTF Share user name" });
      const password = el("input", { type: "password", autocomplete: "current-password", placeholder: "Password", "aria-label": "GDTF Share password" });
      const remember = el("input", { type: "checkbox" });
      const wrong = el("div");
      const submit = el("button", { class: "primary", type: "submit" }, "Sign In and Download");
      const form = el("form", { class: "fields" }, user, password, el("label", { class: "check small-text" }, remember, `Stay signed in on ${thisComputer()}`), submit,
        el("button", { class: "subtle", type: "button", onclick: () => { asking = null; draw(answer); } }, "Cancel"));
      form.addEventListener("submit", async (event) => {
        event.preventDefault();
        submit.disabled = true;
        try {
          state.share = await api("/api/share/login", { json: { user: user.value, password: password.value, remember: remember.checked } });
          password.value = "";
          await fetchFor(entry, asking.row);
        } catch (error) {
          password.value = "";
          wrong.replaceChildren(notice(error.message, "error"));
          submit.disabled = false;
        }
      });
      return el("div", { class: "section-block" }, el("div", { class: "small-text muted" }, `Sign in to GDTF Share to download ${fixtureName(asking.row)}. Accounts are free at gdtf-share.com.`), form, wrong);
    };
    const todo = answer.types.filter((entry) => entry.bare && entry.standin_state !== "ok");
    const matchable = todo.filter((entry) => entry.library.some((row) => row.fits && row.score >= 0.75));
    const block = (entry) => {
      const head = el("div", { class: "entry-head" }, el("span", { class: "name", title: entry.spec }, typeName(entry.name)));
      const lines = [head];
      if (!entry.fixtures) {
        head.append(el("span", { class: "badge" }, "Removed from show"), el("span", { class: "grow" }),
          el("button", { class: "small", onclick: () => act("/api/types/remove", { type: entry.spec, restore: true }, (next) => `${typeName(entry.name)}: ${plural(next.count, "fixture")} restored.`, entry.spec) }, "Restore"));
        lines.push(el("div", { class: "sub" }, `${plural(entry.removed, "fixture")}, excluded from the visualizer, the plot and the patch.`));
        if (notes[entry.spec]) lines.push(notes[entry.spec]);
        return el("div", { class: "entry" }, lines);
      }
      const ok = entry.standin && entry.standin_state === "ok";
      head.append(entry.bare && !ok ? el("span", { class: "badge warn" }, "Placeholder") : ok ? el("span", { class: "badge good" }, "Replaced")
        : entry.standin ? el("span", { class: "badge bad" }, "Replacement missing") : el("span", { class: "badge" }, "From MVR"));
      const inUse = entry.modes.map((mode) => `${mode.name} (${slots(mode.slots)}${entry.modes.length > 1 ? `, ${plural(mode.fixtures, "fixture")}` : ""})`).join(", ");
      lines.push(el("div", { class: "sub" }, `${plural(entry.fixtures, "fixture")} · ${entry.modes.length > 1 ? "modes" : "mode"} ${inUse}`,
        entry.removed ? [` · ${entry.removed} removed (`, el("button", { class: "link", onclick: () => act("/api/types/remove", { type: entry.spec, restore: true }, (next) => `${typeName(entry.name)}: ${plural(next.count, "fixture")} restored.`, entry.spec) }, "restore"), ")"] : null));
      if (ok) {
        const map = entry.modes.map((mode) => mode.standin ? `${mode.name} → ${mode.standin}` : `${mode.name} → no matching mode`).join("; ");
        lines.push(el("div", { class: "sub" }, el("span", { class: "good", title: bare(entry.standin) }, `Uses ${typeName(entry.standin)}. `), `Modes: ${map}. `,
          el("button", { class: "link", onclick: () => act("/api/types/clear", { type: entry.spec }, () => `${typeName(entry.name)} uses the MVR's fixture type again.`, entry.spec) }, "Revert to MVR type")));
        // The one thing a placeholder says is where its dimmer is: a replacement that has it elsewhere is in another order.
        const astray = entry.modes.filter((mode) => mode.dimmer === false).map((mode) => mode.name);
        if (astray.length) lines.push(notice(`The replacement's dimmer is not on the console's dimmer channel (${astray.join(", ")}), so its channel order differs from the console's and the result will be wrong. Try another revision or another file.`, "error"));
        else if (entry.modes.some((mode) => mode.dimmer === true)) lines.push(el("div", { class: "sub faint" }, "Dimmer channel matches the console's."));
      } else {
        if (entry.standin) lines.push(notice(`The replacement ${typeName(entry.standin)} is no longer in your fixture library.`, "error"));
        const worst = entry.modes.reduce((most, mode) => mode.silent > most.silent ? mode : most, entry.modes[0] || { silent: 0, channels: 0 });
        if (entry.bare) lines.push(el("div", { class: "sub" }, `The MVR describes ${worst.channels - worst.silent} of ${plural(worst.channels, "channel")}${worst.silent < worst.channels ? ", so these fixtures only dim" : ", so DMX has no effect on them"}.`));
      }
      if (entry.bare || entry.standin || kept.open[entry.spec]) {
        const fits = (row) => row.fits ? "matches" : `no ${entry.needs.map(slots).join(" or ")} mode`;
        const candidates = [];
        // From the library: what is called alike.
        for (const row of entry.library.filter((row) => row.file !== entry.standin || !ok)) {
          candidates.push(el("div", { class: "candidate" }, el("div", { class: "name" }, fixtureName(row), el("small", {}, `Fixture library · ${fits(row)}`)),
            el("button", { class: "small", disabled: !row.fits, onclick: () => act("/api/types/set", { type: entry.spec, file: row.file }, () => `${fixtureName(row)} now replaces ${typeName(entry.name)}.`, entry.spec) }, "Replace")));
        }
        // From GDTF Share: searched when asked.
        const share = found[entry.spec];
        if (share) {
          // Those that match, and two of the others so that a near miss can be seen for what it is.
          const offered = share.rows.filter((row) => row.fits).concat(share.rows.filter((row) => !row.fits).slice(0, 2));
          for (const row of offered) {
            candidates.push(el("div", { class: "candidate" }, el("div", { class: "name" }, fixtureName(row),
              el("small", {}, `GDTF Share${row.by_manufacturer ? " · manufacturer's file" : ""}${row.revision ? ` · ${row.revision}` : ""} · ${fits(row)}${row.have ? " · in your library" : ""}`)),
              el("button", { class: "small", disabled: !row.fits, onclick: (event) => {
                const button = event.currentTarget;
                button.disabled = true;
                button.textContent = row.have ? "Replacing…" : "Downloading…";
                fetchFor(entry, row);
              } }, row.have ? "Replace" : "Download and Replace")));
          }
          if (!share.rows.length) lines.push(el("div", { class: "sub" }, "Nothing of that name on GDTF Share. Search the Fixture Library under another name, or get a GDTF file from the manufacturer and drop it on this window."));
        }
        if (candidates.length) lines.push(el("div", { class: "candidates" }, candidates));
        const other = el("select", { "aria-label": `Replace ${entry.name} with a file from your library` },
          el("option", { value: "" }, "Choose from library…"), answer.files.map((row) => el("option", { value: row.file }, fixtureName(row) || bare(row.file))));
        other.addEventListener("change", () => { if (other.value) act("/api/types/set", { type: entry.spec, file: other.value }, () => `${bare(other.value)} now replaces ${typeName(entry.name)}.`, entry.spec); });
        lines.push(el("div", { class: "fields tight" },
          el("button", { class: "small", onclick: busy(null, async () => {
            try { found[entry.spec] = await api(`/api/types/find?name=${encodeURIComponent(name)}&type=${encodeURIComponent(entry.spec)}`); notes[entry.spec] = null; }
            catch (error) { notes[entry.spec] = notice(error.code === "login_required" ? "Openviz has no GDTF Share fixture list yet. Sign in once under Fixture Library to download it." : error.message, "error"); }
            draw(answer);
          }) }, icon("search"), share ? "Search GDTF Share Again" : "Search GDTF Share"), other,
          el("button", { class: "small", title: "Describe the fixture from its manual and save it as a GDTF fixture type, when no file exists for it",
            onclick: () => builderDialog({ show: name, type: entry.spec, file: ok && builtHere(entry.standin) ? entry.standin : "", done: (next) => {
              notes[entry.spec] = notice(`${typeName(next.file)} is saved in your fixture library and replaces the console's placeholder.`, "ok");
              draw(next.types);
              loadShows();
            } }) }, icon("edit"), ok && builtHere(entry.standin) ? "Edit in Fixture Builder" : "Build Fixture Type")));
      }
      // Which way its pan and tilt run, and from where: for every fixture of the type.
      const moves = [];
      if (entry.has_pan || entry.has_tilt || entry.invert_pan || entry.invert_tilt || entry.pan_offset || entry.tilt_offset) {
        const send = (change, done) => act("/api/types/moves", Object.assign({ type: entry.spec, invert_pan: entry.invert_pan, invert_tilt: entry.invert_tilt,
          pan_offset: entry.pan_offset, tilt_offset: entry.tilt_offset }, change), () => done, entry.spec);
        const invert = (key, axis) => {
          const box = el("input", { type: "checkbox", checked: entry[key] });
          box.addEventListener("change", () => send({ [key]: box.checked }, `${typeName(entry.name)}: ${axis} ${box.checked ? "inverted" : "normal"}.`));
          return el("label", { class: "check small-text" }, box, `Invert ${axis}`);
        };
        const offset = (key, axis) => {
          const box = el("input", { type: "text", class: "narrow", value: degrees(entry[key]), "aria-label": `${axis} offset, degrees`, autocomplete: "off" });
          box.addEventListener("change", () => {
            const value = Number(box.value.trim().replace(",", "."));
            if (!box.value.trim() || !Number.isFinite(value) || value < -360 || value > 360) { notes[entry.spec] = notice("An offset is a number of degrees from −360 to 360.", "error"); draw(answer); return; }
            send({ [key]: value }, `${typeName(entry.name)}: ${axis} offset ${degrees(value)}°.`);
          });
          return el("label", { class: "check small-text" }, `${axis[0].toUpperCase()}${axis.slice(1)} offset`, box, "°");
        };
        moves.push(entry.has_pan || entry.invert_pan ? invert("invert_pan", "pan") : null, entry.has_tilt || entry.invert_tilt ? invert("invert_tilt", "tilt") : null,
          entry.has_pan || entry.pan_offset ? offset("pan_offset", "pan") : null, entry.has_tilt || entry.tilt_offset ? offset("tilt_offset", "tilt") : null);
      }
      // How its dimmer level becomes light: as the file has it, or bent as a dimmer rack would.
      const CURVES = [["linear", "Linear"], ["square", "Square Law"], ["inverse-square", "Inverse Square"], ["s-curve", "S-Curve"]];
      const curveBox = el("select", { class: "small", "aria-label": `${typeName(entry.name)}: dimmer curve`,
        title: "How the dimmer level becomes light. Linear is as the fixture file has it; Square Law is how many dimmer racks drive incandescent lamps (half the level, a quarter of the light)." },
        CURVES.map(([value, label]) => el("option", { value, selected: (entry.dimmer_curve || "linear") === value }, label)));
      curveBox.addEventListener("change", () => act("/api/types/curve", { type: entry.spec, curve: curveBox.value },
        () => `${typeName(entry.name)}: dimmer curve ${CURVES.find(([value]) => value === curveBox.value)[1]}.`, entry.spec));
      moves.push(el("label", { class: "check small-text" }, "Dimmer curve", curveBox));
      // A type the MVR describes can be replaced too: a console's generic dimmer by a PAR or a profile, say.
      const replaceToo = !entry.bare && !entry.standin && !kept.open[entry.spec] ? el("button", { class: "small subtle",
        title: "Replace this fixture type with one from your library, from GDTF Share or built in the Fixture Builder",
        onclick: () => { kept.open[entry.spec] = true; draw(answer); } }, "Replace Type…") : null;
      lines.push(el("div", { class: "fields tight" }, moves, el("span", { class: "grow" }), replaceToo,
        el("button", { class: "small subtle", title: "Excludes every fixture of this type from the visualizer, the plot and the patch. The MVR keeps them; Restore brings them back.",
          onclick: () => act("/api/types/remove", { type: entry.spec }, (next) => `${typeName(entry.name)}: ${plural(next.count, "fixture")} removed from the show.`, entry.spec) },
          entry.fixtures === 1 ? "Remove from Show" : `Remove All ${entry.fixtures} from Show`)));
      // What came of the last click on this type, and the sign-in a download may be waiting for: here, where the click was.
      if (asking && asking.spec === entry.spec) lines.push(signIn(entry));
      if (notes[entry.spec]) lines.push(notes[entry.spec]);
      return el("div", { class: "entry" }, lines);
    };
    const gone = answer.types.filter((entry) => !entry.fixtures);
    const plain = answer.types.filter((entry) => entry.fixtures && !entry.bare && !entry.standin);
    const rest = answer.types.filter((entry) => entry.fixtures && !plain.includes(entry));
    const card = (title, sub, entries, open) => !entries.length ? null : el("div", { class: "card" },
      el("div", { class: "card-head" }, el("div", {}, el("h3", {}, title), sub ? el("div", { class: "sub" }, sub) : null)), entries.map(block));
    put(panel,
      el("div", { class: "toolbar" }, el("p", { class: "muted grow" }, todo.length
        ? "Console placeholders carry only a channel count and a dimmer. Replace each with the manufacturer's GDTF to get colour, gobos, pan and tilt; modes are matched by channel count. The MVR is not modified."
        : rest.length ? "Every placeholder in this show has been replaced with a real fixture type." : "The MVR describes every fixture type in this show."),
        matchable.length ? el("button", { class: "primary", onclick: () => act("/api/types/all", {}, (next) =>
          `Replaced from your library: ${next.done.map((entry) => typeName(entry.type)).join(", ")}.${next.left.length ? ` Still placeholders: ${next.left.map(typeName).join(", ")}.` : ""}`) },
          `Auto-Match from Library (${matchable.length})`) : null),
      said,
      el("div", { class: "stack" },
        card("Needs Attention", rest.length ? "Placeholders and replaced types" : "", rest),
        card("Described by the MVR", "", plain),
        card("Removed from Show", "", gone)),
      el("p", { class: "help" }, "A file matches when it has a mode with the same channel count as each mode in use; check the channel order on first use. " +
        "Manufacturers' files and console profiles sometimes disagree on which side of a moving head is the front: if a type points backwards, invert tilt; if it then pans the wrong way, invert pan too; if it is a fixed angle off, set an offset. " +
        "A show that is open in the visualizer picks up every change immediately."));
  };
  gather(api(`/api/show/types?name=${encodeURIComponent(name)}`)).then(draw).catch((error) => put(panel, notice(error.message, "error")));
  return panel;
}

// ----------------------------------------------------------------------------- fixture builder

// A file made by the Fixture Builder is named as GDTF Share names files, with Openviz as its revision.
const builtHere = (file) => /@Openviz\.gdtf$/i.test(file || "");

// The Fixture Builder: a fixture type described as its manual describes it, for a fixture that has
// no GDTF file anywhere. Openviz writes the file into the fixture library. Opened for a show's
// placeholder, it starts from the placeholder's modes and replaces the type when saved.
// `from`: { file } to edit one it made, { show, type } for a placeholder, `done(answer)` once saved.
// `again`: the dialog opened anew on a changed description (a template chosen), with what it had.
async function builderDialog(from, again) {
  const query = from.file ? `file=${encodeURIComponent(from.file)}` : from.show ? `name=${encodeURIComponent(from.show)}&type=${encodeURIComponent(from.type)}` : "";
  let start = again;
  if (!start) {
    try { start = await api(`/api/builder${query ? `?${query}` : ""}`); }
    catch (error) { pageNotice().replaceChildren(notice(error.message, "error")); return; }
  }
  const catalogue = start.catalogue;
  const spec = JSON.parse(JSON.stringify(start.spec));
  // What the console's placeholder takes in each mode: a replacement must take as many.
  const consoleLengths = again ? again.console : from.show && !start.file ? Object.fromEntries(spec.modes.map((mode) => [mode.name, mode.channels.length])) : {};
  const labels = {};
  for (const group of catalogue.groups) for (const entry of group.functions) labels[entry.key] = entry.label;
  const field = (text, control, help) => el("label", { class: "field" }, el("span", {}, text), control, help ? el("span", { class: "help" }, help) : null);
  const textBox = (key, label) => {
    const box = el("input", { type: "text", value: spec[key] || "", "aria-label": label, autocomplete: "off", spellcheck: "false" });
    box.addEventListener("input", () => { spec[key] = box.value; });
    return field(label, box);
  };
  // Numbers as the page shows them: `scale` turns the description's unit into the one shown (metres to millimetres).
  const numberBox = (key, label, scale = 1) => {
    const box = el("input", { type: "number", step: "any", value: String(Math.round(spec[key] * scale * 100) / 100), "aria-label": label });
    box.addEventListener("input", () => { spec[key] = box.value.trim() === "" ? NaN : Number(box.value) / scale; });
    return field(label, box);
  };
  const choiceBox = (key, label, options, then) => {
    const box = el("select", { "aria-label": label }, options.map(([value, words]) => el("option", { value: String(value), selected: String(spec[key]) === String(value) }, words)));
    box.addEventListener("change", () => { spec[key] = typeof options[0][0] === "number" ? Number(box.value) : box.value; if (then) then(); });
    return field(label, box);
  };

  dialog(start.file ? `Edit ${typeName(start.file)}` : "Build Fixture Type", (close) => {
    const said = el("div", { class: "grow" });
    const movement = el("div", { class: "stack" }, el("h4", {}, "Movement"),
      el("div", { class: "grid2" }, numberBox("pan_range", "Pan Range (°)"), numberBox("tilt_range", "Tilt Range (°)")));
    const showKind = () => { movement.hidden = spec.kind === "static"; };
    showKind();

    // The colour wheel: slot 1 is open; each colour after it is a slot.
    const colours = el("div", { class: "builder-list" });
    const drawColours = () => put(colours, spec.colors.map((colour, index) => {
      const pick = el("input", { type: "color", value: colour.hex, "aria-label": `Colour of slot ${index + 2}` });
      pick.addEventListener("input", () => { colour.hex = pick.value; });
      const named = el("input", { type: "text", value: colour.name, "aria-label": `Name of slot ${index + 2}`, autocomplete: "off" });
      named.addEventListener("input", () => { colour.name = named.value; });
      return el("div", { class: "builder-row" }, el("span", { class: "slot" }, String(index + 2)), pick, named,
        el("button", { class: "subtle icon-only small", "aria-label": `Remove ${colour.name}`, title: "Remove", onclick: () => { spec.colors.splice(index, 1); drawColours(); } }, icon("close")));
    }), el("div", { class: "button-row tight" },
      el("button", { class: "small", disabled: spec.colors.length >= 30, onclick: () => { spec.colors.push({ name: "Colour", hex: "#ffffff" }); drawColours(); } }, icon("plus"), "Add Colour"),
      spec.colors.length ? null : el("span", { class: "small-text faint" }, "No colour wheel.")));
    drawColours();

    // The DMX modes, one at a time, each a list of channels in DMX order.
    let current = 0;
    const modes = el("div", { class: "stack" });
    const functionBox = (key, label) => el("select", { "aria-label": label }, catalogue.groups.map((group) =>
      el("optgroup", { label: group.name }, group.functions.map((entry) => el("option", { value: entry.key, selected: entry.key === key }, entry.label)))));
    const drawModes = () => {
      const mode = spec.modes[current];
      const named = el("input", { type: "text", value: mode.name, "aria-label": "Mode name", autocomplete: "off" });
      named.addEventListener("input", () => { mode.name = named.value; });
      named.addEventListener("change", drawModes);
      const move = (from, to) => { const [key] = mode.channels.splice(from, 1); mode.channels.splice(to, 0, key); delete mode.pairs; drawModes(); };
      // The placeholder's 16-bit pairs, as a hint while the channels are not yet set.
      const pairs = mode.pairs || [];
      const pairNote = (slot) => {
        if (!pairs.length) return null;
        const pair = pairs.find((entry) => entry.includes(slot));
        return el("span", { class: "pair" }, pair ? el("span", { class: "badge", title: "The console's placeholder joins these two channels: a 16-bit channel and its Fine channel" },
          slot === pair[0] ? `16-bit with ${pair[1]}` : `Fine of ${pair[0]}`) : null);
      };
      const rows = mode.channels.map((key, index) => {
        const box = functionBox(key, `Channel ${index + 1}`);
        box.addEventListener("change", () => { mode.channels[index] = box.value; });
        return el("div", { class: "builder-row" }, el("span", { class: "slot" }, String(index + 1)), box, pairNote(index + 1),
          el("button", { class: "subtle icon-only small", title: "Move Up", "aria-label": `Move channel ${index + 1} up`, disabled: index === 0, onclick: () => move(index, index - 1) }, icon("up")),
          el("button", { class: "subtle icon-only small", title: "Move Down", "aria-label": `Move channel ${index + 1} down`, disabled: index === mode.channels.length - 1, onclick: () => move(index, index + 1) }, icon("down")),
          el("button", { class: "subtle icon-only small", title: "Insert Channel Below", "aria-label": `Insert a channel after ${index + 1}`, disabled: mode.channels.length >= 512,
            onclick: () => { mode.channels.splice(index + 1, 0, "unused"); delete mode.pairs; drawModes(); } }, icon("plus")),
          el("button", { class: "subtle icon-only small", title: "Remove Channel", "aria-label": `Remove channel ${index + 1}`, disabled: mode.channels.length === 1,
            onclick: () => { mode.channels.splice(index, 1); delete mode.pairs; drawModes(); } }, icon("close")));
      });
      const wanted = consoleLengths[mode.name];
      const layouts = [["beam", "Beam Moving Head"], ["spot", "Spot Moving Head"], ["wash", "Wash Moving Head"], ["par", "LED Par"]];
      put(modes,
        el("div", { class: "toolbar" }, el("div", { class: "seg" }, spec.modes.map((entry, index) =>
          el("button", { class: index === current ? "on" : "", onclick: () => { current = index; drawModes(); } }, `${entry.name || "Mode"} · ${entry.channels.length}`))),
          el("span", { class: "grow" }),
          menuButton([icon("more")], [
            { label: "Add Mode", icon: "plus", disabled: spec.modes.length >= 12, run: () => {
              spec.modes.push({ name: `Mode ${spec.modes.length + 1}`, channels: [...mode.channels] }); current = spec.modes.length - 1; drawModes(); } },
            { label: "Remove This Mode", icon: "trash", danger: true, disabled: spec.modes.length === 1, run: () => { spec.modes.splice(current, 1); current = 0; drawModes(); } },
            "-",
            ...layouts.map(([key, words]) => ({ label: `Channels of a Typical ${words}`, hint: `${catalogue.layouts[key].length} channels, replacing these`,
              run: () => { mode.channels = [...catalogue.layouts[key]]; delete mode.pairs; drawModes(); } })),
          ], { class: "small subtle icon-only", title: "Mode actions" })),
        field("Mode Name", named),
        wanted && wanted !== mode.channels.length ? notice(`The console uses ${plural(wanted, "channel")} in this mode and this has ${mode.channels.length}. ` +
          "A fixture type can only replace a placeholder whose channel count it matches.", "error") : null,
        el("div", { class: "builder-list" }, rows),
        el("div", { class: "button-row tight" }, el("button", { class: "small", disabled: mode.channels.length >= 512, onclick: () => { mode.channels.push("unused"); drawModes(); } }, icon("plus"), "Add Channel"),
          el("span", { class: "small-text faint" }, "16-bit channels are a channel and its Fine channel, as the manual lists them.")));
    };
    drawModes();

    const gobos = choiceBox("gobos", "Gobos", [[0, "None"], ...catalogue.patterns.map((pattern, index) => [index + 1, String(index + 1)])]);
    // A template's typical figures over what is here; the dialog opens again with them.
    const fromConsole = Object.keys(consoleLengths).length > 0;
    const template = el("select", { "aria-label": "Start from a template", class: "wide" }, el("option", { value: "" }, "Choose…"),
      catalogue.templates.map((entry) => el("option", { value: entry.key }, entry.label)));
    template.addEventListener("change", busy(template, async () => {
      if (!template.value) return;
      const answer = await api("/api/builder/template", { json: { template: template.value, spec, keep_modes: fromConsole } });
      close();
      builderDialog(from, { catalogue, spec: answer.spec, file: start.file, console: consoleLengths, templated: true });
    }, said));
    const save = el("button", { class: "primary" }, icon("check"), from.show ? "Save and Replace" : "Save to Library");
    save.addEventListener("click", busy(save, async () => {
      const answer = await api("/api/builder/build", { json: Object.assign({ spec }, from.show ? { name: from.show, type: from.type } : {}) });
      close();
      if (from.done) from.done(answer);
    }, said));
    return [[el("div", { class: "builder" },
      el("div", { class: "fields tight" }, el("label", { class: "field grow" }, el("span", {}, "Start From a Template"), template),
        el("p", { class: "help grow" }, fromConsole ? "Fills in the beam, size, colours, gobos and prism with typical figures for the kind; the console's modes stay. Correct the figures from the manual."
          : "Fills in everything with typical figures for the kind, channels included. Correct them from the manual.")),
      again && again.templated ? notice("Template applied. Check the figures against the fixture's manual.", "info") : null,
      from.show && !start.file && !again ? notice("Set up from the console's placeholder: its modes with their channel counts, and its dimmer channel. " +
        "Fill in the rest from the fixture's manual: what each channel does, the beam, colours, gobos and prism.", "info") : null,
      el("div", { class: "builder-columns" },
        el("div", { class: "stack" },
          el("h4", {}, "Fixture"),
          el("div", { class: "grid2" }, textBox("manufacturer", "Manufacturer"), textBox("name", "Name")),
          choiceBox("kind", "Kind", [["moving", "Moving head"], ["static", "Static fixture"]], showKind),
          el("h4", {}, "Beam"),
          el("div", { class: "grid3" }, choiceBox("lamp", "Light Source", [["LED", "LED"], ["Discharge", "Discharge lamp"], ["Tungsten", "Tungsten"]]),
            choiceBox("beam_type", "Beam Type", [["Beam", "Beam"], ["Spot", "Spot"], ["Wash", "Wash"]]), numberBox("lumens", "Output (lm)")),
          el("div", { class: "grid3" }, numberBox("beam_angle", "Beam Angle (°)"), numberBox("field_angle", "Field Angle (°)"), numberBox("color_temperature", "White (K)")),
          el("div", { class: "grid2" }, numberBox("zoom_min", "Zoom, Narrowest (°)"), numberBox("zoom_max", "Zoom, Widest (°)")),
          el("p", { class: "help" }, "Leave the zoom at 0 for a fixture without zoom. The beam angle is where the light falls to half; the field angle, to a tenth."),
          movement,
          el("h4", {}, "Size and Power"),
          el("div", { class: "grid3" }, numberBox("width", "Width (mm)", 1000), numberBox("depth", "Depth (mm)", 1000), numberBox("height", "Height (mm)", 1000)),
          el("div", { class: "grid2" }, numberBox("weight", "Weight (kg)"), numberBox("power", "Power (W)")),
          el("h4", {}, "Colour Wheel"),
          el("p", { class: "help" }, "Slot 1 is open. Add the wheel's colours in order."),
          colours,
          el("h4", {}, "Gobos and Prism"),
          el("div", { class: "grid2" }, gobos, choiceBox("prism_facets", "Prism", [[0, "None"], [3, "3-facet"], [4, "4-facet"], [5, "5-facet"], [6, "6-facet"], [8, "8-facet"]])),
          el("p", { class: "help" }, `Openviz draws simple gobo patterns in wheel order: ${catalogue.patterns.join(", ")}.`)),
        el("div", { class: "stack" }, el("h4", {}, "DMX Modes"), modes)))],
      [said, el("button", { onclick: close }, "Cancel"), save]];
  }, { wide: true });
}

// --------------------------------------------------------------------------------- set pieces

// A show's set pieces: a rig from a console often has no stage at all. Decks, drapes, trusses,
// boxes, figures and 3D models are added here; they take light and stop beams like the MVR's own
// scenery, and a show that is open shows every change at once.
function piecesPanel(name) {
  const panel = el("div", {}, el("p", { class: "muted" }, "Loading set pieces…"));
  const said = el("div");
  // A colour as the picker has it (sRGB, #rrggbb) and as the stage has it (linear, 0 to 1).
  const toHex = (colour) => "#" + colour.map((part) => {
    const lit = part <= 0.0031308 ? part * 12.92 : 1.055 * Math.pow(part, 1 / 2.4) - 0.055;
    return Math.round(Math.max(0, Math.min(1, lit)) * 255).toString(16).padStart(2, "0");
  }).join("");
  const fromHex = (hex) => [1, 3, 5].map((at) => {
    const lit = parseInt(hex.slice(at, at + 2), 16) / 255;
    return Math.round((lit <= 0.04045 ? lit / 12.92 : Math.pow((lit + 0.055) / 1.055, 2.4)) * 10000) / 10000;
  });
  const title = (text) => text.replace(/\b\w/g, (letter) => letter.toUpperCase());
  // Which of its measures a kind has, and what each is called.
  const MEASURES = {
    deck: [["width", "Width"], ["depth", "Depth"], ["height", "Height"]],
    drape: [["width", "Width"], ["height", "Height"]],
    truss: [["width", "Length"], ["depth", "Profile"]],
    box: [["width", "Width"], ["depth", "Depth"], ["height", "Height"]],
    person: [["height", "Height"]],
    circle: [["width", "Diameter"], ["depth", "Profile"]],
    corner: [["width", "Arm Length"], ["depth", "Profile"]],
    triangle: [["width", "Length"], ["depth", "Profile"]],
    pipe: [["width", "Length"], ["depth", "Diameter"]],
    boom: [["height", "Height"], ["width", "Base"]],
    crowd: [["width", "Width"], ["depth", "Depth"], ["height", "Height"]],
    stairs: [["width", "Width"], ["depth", "Run"], ["height", "Height"]],
    model: [],
  };
  const UNITS = [["", "As in file"], ["mm", "Millimetres"], ["cm", "Centimetres"], ["m", "Metres"], ["in", "Inches"], ["ft", "Feet"]];
  const UPS = [["", "As in file"], ["y", "Y up"], ["z", "Z up"]];
  const draw = (answer) => {
    const act = async (path, body) => {
      try { draw(await api(path, { json: { name, ...body } })); said.replaceChildren(); loadShows(); }
      catch (error) { said.replaceChildren(notice(error.message, "error")); }
    };
    const line = (piece) => {
      // A field that is changed is saved by itself, and the list is left as it is: whoever tabs on to the next field stays in it.
      const send = async (changes, box) => {
        try { await api("/api/piece/set", { json: { name, id: piece.id, changes } }); Object.assign(piece, changes); said.replaceChildren(); }
        catch (error) { said.replaceChildren(notice(error.message, "error")); if (box) box.focus(); }
      };
      const field = (text, control) => el("label", { class: "field" }, el("span", {}, text), control);
      const length = (key, label) => {
        const box = el("input", { type: "text", class: "narrow", value: metres(piece[key]), "aria-label": `${piece.name}: ${label}, metres`, autocomplete: "off" });
        box.addEventListener("change", () => {
          const value = Number(box.value.trim().replace(",", "."));
          if (!box.value.trim() || !Number.isFinite(value)) { said.replaceChildren(notice("Enter a number of metres.", "error")); return; }
          send({ [key]: Math.round(value * 1000) }, box);
        });
        return field(label, box);
      };
      const named = el("input", { type: "text", class: "mid", value: piece.name, "aria-label": "Name", autocomplete: "off" });
      named.addEventListener("change", () => send({ name: named.value.trim() }, named));
      const turn = el("input", { type: "text", class: "narrow", value: degrees(piece.turn), "aria-label": `${piece.name}: rotation, degrees`, autocomplete: "off" });
      turn.addEventListener("change", () => {
        const value = Number(turn.value.trim().replace(",", "."));
        if (!turn.value.trim() || !Number.isFinite(value)) { said.replaceChildren(notice("Rotation is a number of degrees.", "error")); return; }
        send({ turn: value }, turn);
      });
      const colour = el("input", { type: "color", value: toHex(piece.colour), "aria-label": `${piece.name}: colour` });
      colour.addEventListener("change", () => send({ colour: fromHex(colour.value) }));
      const kind = answer.kinds.find((entry) => entry.kind === piece.kind);
      const choose = (key, options, label) => {
        const box = el("select", { "aria-label": `${piece.name}: ${label}` }, options.map(([value, text]) => el("option", { value, selected: piece[key] === value }, text)));
        box.addEventListener("change", () => send({ [key]: box.value }, box));
        return field(label, box);
      };
      // A drape, box or deck can carry a picture on its side toward the house.
      const picture = !["drape", "box", "deck"].includes(piece.kind) ? null : field("Picture", el("span", { class: "badges" },
        el("button", { title: piece.picture || "", onclick: busy(null, async () => {
          const chosen = await api("/api/screen/choose", { json: { kind: "picture" } });
          if (chosen.path) { await send({ picture: chosen.path }); draw(await api(`/api/show/pieces?name=${encodeURIComponent(name)}`)); }
        }, said) }, piece.picture ? piece.picture.split(/[\\/]/).pop() : "Browse…"),
        piece.picture ? el("button", { class: "subtle icon-only", "aria-label": "Remove the picture", onclick: async () => { await send({ picture: "" }); draw(await api(`/api/show/pieces?name=${encodeURIComponent(name)}`)); } }, icon("close")) : null));
      // A tracker of a tracking system (PosiStageNet) it follows while a show is watched: a performer, a moving set piece.
      const tracker = el("input", { type: "text", class: "narrow", value: piece.tracker ? String(piece.tracker) : "", placeholder: "None",
        "aria-label": `${piece.name}: PosiStageNet tracker`, autocomplete: "off", inputmode: "numeric" });
      tracker.addEventListener("change", async () => {
        const text = tracker.value.trim();
        const value = Number(text);
        if (text && !(Number.isInteger(value) && value >= 1 && value <= 65535)) { said.replaceChildren(notice("A tracker is a number from 1 to 65535, or empty for none.", "error")); return; }
        await send({ tracker: text ? value : null }, tracker);
        draw(await api(`/api/show/pieces?name=${encodeURIComponent(name)}`));
      });
      const height = el("input", { type: "checkbox", checked: !!piece.follow_height, "aria-label": `${piece.name}: follow the tracker's height` });
      height.addEventListener("change", () => send({ follow_height: height.checked }));
      const follows = [field("Follows Tracker", tracker),
        piece.tracker ? el("label", { class: "check", title: "Off: the piece stays at its own Z and follows the tracker across the floor" }, height, "Follow Height") : null];
      const model = piece.kind !== "model" ? [] : [
        choose("file", [...new Set([piece.file, ...answer.models])].map((file) => [file, file]), "File"),
        choose("units", UNITS, "Units"), choose("up", UPS, "Up Axis")];
      return el("div", { class: "entry" },
        el("div", { class: "entry-head" }, el("span", { class: "badge" }, kind ? title(kind.label) : piece.kind), el("span", { class: "grow" }),
          el("button", { class: "small", title: "Adds a copy beside it", onclick: () => act("/api/piece/remove", { id: piece.id, copy: true }) }, "Duplicate"),
          el("button", { class: "small subtle danger", onclick: () => act("/api/piece/remove", { id: piece.id }) }, icon("trash"), "Remove")),
        el("div", { class: "fields" }, field("Name", named), model, MEASURES[piece.kind].map(([key, label]) => length(key, `${label} (m)`)),
          length("x", "X (m)"), length("y", "Y (m)"), length("z", "Z (m)"), field("Rotation (°)", turn),
          piece.kind === "model" ? null : field("Colour", colour), picture, follows),
        piece.trouble ? notice(piece.trouble, "error") : null);
    };
    const addModel = async () => {
      try {
        const chosen = await api("/api/model/choose", { json: {} });
        if (!chosen.name) return;
        if (chosen.warnings && chosen.warnings.length) said.replaceChildren(notice(chosen.warnings.join(" "), ""));
        await act("/api/piece/add", { kind: "model", changes: { file: chosen.name } });
      } catch (error) { said.replaceChildren(notice(error.message, "error")); }
    };
    const adds = menuButton([icon("plus"), "Add Set Piece"], [
      ...answer.kinds.filter((kind) => kind.kind !== "model").map((kind) => ({ label: title(kind.label), run: () => act("/api/piece/add", { kind: kind.kind }) })),
      "-",
      ...answer.models.map((file) => ({ label: file, hint: "3D model from your library", run: () => act("/api/piece/add", { kind: "model", changes: { file } }) })),
      { label: "3D Model File…", hint: "GLB, OBJ or 3DS: a venue or set from a CAD program", run: addModel },
    ], { class: "primary" });
    put(panel,
      el("div", { class: "toolbar" }, el("p", { class: "muted grow" }, answer.pieces.length
        ? "Everything on stage besides the rig. Sizes and positions are in metres; a position is the centre of the piece at floor level."
        : "Rigs from consoles often arrive without a stage. Add decks, drapes, truss, boxes, figures for scale, or a 3D model of the venue."), adds),
      said,
      answer.pieces.length ? el("div", { class: "card" }, answer.pieces.map(line))
        : el("div", { class: "empty" }, el("b", {}, "No set pieces"), "Use Add Set Piece to build the stage."),
      el("p", { class: "help" }, "X runs toward stage left (house right), Y upstage, Z up. Truss and pipes lie along X until rotated, with Z at their underside where fixtures hang. A truss circle lies flat around its centre; a corner's arms run along X and Y; a boom stands on its base; a crowd fills its width and depth with people about its height, facing upstage; stairs climb upstage to their height, about 18 cm a step, so set in front of a deck they lead onto it. " +
        "A drape faces the house and can carry a printed picture (a box or deck too, on its front). A model's size comes from its file (GLB in metres, 3DS in millimetres, OBJ as written): if it is far too large or lying down, set its units and up axis. " +
        "GLB files carry their colours and textures; an OBJ needs its MTL file and textures beside it, which 3D Model File… brings along. Beams land on and are cut by set pieces, which also appear on the light plot and in MVR exports. " +
        "A piece given a tracker moves with it while a show is watched, as a tracking system sends it over PosiStageNet (Connections); it stands at its own position until the tracker is heard."));
  };
  gather(api(`/api/show/pieces?name=${encodeURIComponent(name)}`)).then(draw).catch((error) => put(panel, notice(error.message, "error")));
  return panel;
}

// ------------------------------------------------------------------------------------ screens

// A show's screens: the video screens its MVR has, and screens added here. Each shows a test
// pattern, a picture, a film, an NDI stream or DMX pixels. What is chosen is kept beside the show,
// and a show that is open takes it up at once.
function screensPanel(name) {
  const panel = el("div", {}, el("p", { class: "muted" }, "Loading screens…"));
  const said = el("div");
  let streams = [];    // the NDI streams found when last searched
  // What the visualizer says about each screen while this show is open: "playing ...", "waiting for ...".
  const word = {};
  let wordTimer = null;
  const askVisualizer = async () => {
    if (!panel.isConnected) { clearInterval(wordTimer); return; }
    try {
      const run = await api("/api/run");
      const fresh = {};
      if (run.running && run.name === name) {
        for (const line of run.lines) {
          const found = /^screen (.+?): (.*)$/.exec(line.text);
          if (found) fresh[found[1]] = { text: found[2], note: line.note };
        }
      }
      for (const screenName of Object.keys({ ...word, ...fresh })) {
        const now = fresh[screenName];
        const node = panel.querySelector(`[data-screen-word="${CSS.escape(screenName)}"]`);
        if (!node) continue;
        node.textContent = now ? `Visualizer: ${now.text}` : "";
        node.className = `small-text ${now && now.note ? "bad" : "faint"}`;
      }
      Object.assign(word, fresh);
    } catch (error) { /* the next look will say */ }
  };

  const length = (mm) => (mm / 1000).toFixed(2);
  const draw = (answer) => {
    const change = async (id, changes) => {
      try { draw(await api("/api/screen/set", { json: { name, id, changes } })); said.replaceChildren(); }
      catch (error) { said.replaceChildren(notice(error.message, "error")); }
    };
    const field = (text, control) => el("label", { class: "field" }, el("span", {}, text), control);
    // The controls for what a screen shows, or one section of a split screen: `target` is the screen or the
    // section, `label` what it is called, `send` takes the changes, and `extent` is its size in millimetres.
    const content = (screen, target, label, send, extent) => {
      // DMX pixels start out a hand's width apart, from universe 1: the numbers are there to be changed.
      const firstPixels = `1.1 ${Math.max(1, Math.min(1024, Math.round(extent[0] / 100)))}x${Math.max(1, Math.min(1024, Math.round(extent[1] / 100)))}`;
      const kinds = [["pattern", "Test Pattern"], ["picture", "Picture"], ["film", "Video File"], ["ndi", "NDI Stream"], ["pixels", "DMX Pixels"], ["off", "Off"]];
      if (screen.replaces.length) kinds.unshift(["fixtures", "Fixtures (DMX as patched)"]);
      const source = el("select", { "aria-label": `Source for ${label}`, onchange: (event) =>
        send(event.target.value === "pixels" ? { source: "pixels", value: firstPixels } : { source: event.target.value, value: "" }) },
        kinds.map(([key, text]) => el("option", { value: key, selected: key === target.source }, text)));
      const controls = [field("Source", source)];
      if (target.source === "picture" || target.source === "film") {
        const what = target.source;
        const path = el("input", { type: "text", class: "mid", value: target.value, placeholder: "Full path of the file", "aria-label": `File for ${label}`,
          onchange: (event) => send({ value: event.target.value }) });
        controls.push(field("File", el("span", { class: "badges" }, path, el("button", { onclick: busy(null, async () => {
          const chosen = await api("/api/screen/choose", { json: { kind: what } });
          if (chosen.path) await send({ value: chosen.path });
        }, said) }, "Browse…"))));
      } else if (target.source === "ndi") {
        const listId = `ndi-${screen.id}-${label.length}`;
        const list = el("datalist", { id: listId }, streams.map((stream) => el("option", { value: stream })));
        const box = el("input", { type: "text", class: "mid", value: target.value, list: listId, placeholder: "COMPUTER (Stream)", "aria-label": `NDI stream for ${label}`,
          onchange: (event) => send({ value: event.target.value }) });
        controls.push(field("Stream", el("span", { class: "badges" }, box, list, el("button", { onclick: busy(null, async () => {
          streams = (await api("/api/ndi")).sources;
          list.replaceChildren(...streams.map((stream) => el("option", { value: stream })));
          said.replaceChildren(notice(streams.length ? "NDI streams on the network: " : "No NDI streams are being sent on the network right now.", streams.length ? "ok" : "info",
            ...streams.map((stream, index) => [index ? ", " : "", el("button", { class: "link", onclick: () => send({ value: stream }) }, stream)])));
          // A stream the show names that is not there, and one that is called nearly the same: every screen showing it, in one go.
          const plain = (text) => text.toLowerCase().replace(/[^a-z0-9]/g, "");
          const named = new Set(answer.screens.flatMap((one) => [one, ...one.sections]).filter((one) => one.source === "ndi" && one.value).map((one) => one.value));
          for (const missing of [...named].filter((value) => !streams.includes(value))) {
            const near = streams.filter((stream) => plain(stream).includes(plain(missing).replace(/^.*?(mix|out)/, "$1")) || plain(stream) === plain(missing));
            said.append(notice(`${missing} is not on the network. `, "info", near.length === 1
              ? el("button", { class: "link", onclick: busy(null, async () => {
                draw(await api("/api/screen/stream", { json: { name, old: missing, new: near[0] } }));
                said.replaceChildren(notice(`Every screen that showed ${missing} now shows ${near[0]}.`, "ok"));
              }, said) }, `Use ${near[0]} on every screen that shows it`) : "Is its sender on, and on this network?"));
          }
        }, said) }, "Find Streams"))));
      } else if (target.source === "pixels" && target.pixels) {
        // An LED strip or a low-resolution wall: its pixels take their levels from DMX, in order, from an address on.
        const map = target.pixels;
        const sendMap = (changes) => {
          const now = { ...map, ...changes };
          send({ value: `${now.universe}.${now.address} ${now.across}x${now.down} ${now.order} ${now.wiring}` });
        };
        const count = (key, what, max) => el("input", { type: "number", class: "narrow", min: "1", max: String(max), step: "1", value: String(map[key]), "aria-label": `${what} of ${label}`,
          onchange: (event) => sendMap({ [key]: Math.round(Number(event.target.value)) }) });
        const pick = (key, what, options) => el("select", { "aria-label": `${what} of ${label}`, onchange: (event) => sendMap({ [key]: event.target.value }) },
          options.map(([value, text]) => el("option", { value, selected: value === map[key] }, text)));
        controls.push(field("Universe", count("universe", "Universe", 32767)), field("Address", count("address", "Address", 512)),
          field("Pixels Across", count("across", "Pixels across", 1024)), field("Pixels Down", count("down", "Pixels down", 1024)),
          field("Colour Order", pick("order", "Colour order", ["RGB", "GRB", "BGR", "RBG", "GBR", "BRG", "RGBW", "GRBW"].map((order) => [order, order]))),
          field("Wiring", pick("wiring", "Wiring", [["rows", "Rows, left to right"], ["snake", "Rows, snake"], ["columns", "Columns, top to bottom"], ["snake-columns", "Columns, snake"]])));
      }
      if (["picture", "film", "ndi"].includes(target.source)) {
        // Which part of the picture: all of it, or a part, so that one picture can be spread over several screens or sections.
        const same = (one, other) => one.every((edge, index) => Math.abs(edge - other[index]) < 0.0005);
        const parts = [["whole", "Whole picture", [0, 0, 1, 1]]];
        if (target.own) parts.unshift(["own", "Its part of the screen's picture", target.own]);
        parts.push(["left", "Left half", [0, 0, 0.5, 1]], ["right", "Right half", [0.5, 0, 1, 1]], ["top", "Top half", [0, 0, 1, 0.5]], ["bottom", "Bottom half", [0, 0.5, 1, 1]]);
        // A part laid out turned in the picture, as an LED processor's map has it, is shown as such; choosing another replaces it.
        if (target.map) parts.unshift(["mapped", "Its mapped part (turned)", null]);
        const now = target.map ? parts[0] : parts.find((entry) => same(entry[2], target.part));
        const percent = (edge) => `${+(edge * 100).toFixed(1)}%`;
        controls.push(field("Crop", el("select", { "aria-label": `Which part of its picture ${label} shows`, onchange: (event) => {
          const chosen = parts.find((entry) => entry[0] === event.target.value);
          if (chosen && chosen[0] === "mapped") return;
          if (chosen) send({ part: chosen[0] === "own" || chosen[0] === "whole" ? (target.own ? chosen[0] : chosen[2]) : chosen[2] });
        } }, parts.map((entry) => el("option", { value: entry[0], selected: entry === now }, entry[1])),
          now ? null : el("option", { value: "other", selected: true }, `${percent(target.part[0])}, ${percent(target.part[1])} to ${percent(target.part[2])}, ${percent(target.part[3])}`))));
      }
      if (target.source !== "off" && target.source !== "fixtures") {
        controls.push(field("Brightness (nits)", el("input", { type: "number", class: "narrow", min: "0", max: "10000", step: "50", value: String(Math.round(target.nits)),
          "aria-label": `Brightness of ${label} in nits`, onchange: (event) => send({ nits: Number(event.target.value) }) })));
      }
      const reach = !target.pixels ? null : el("p", { class: "help" },
        `${plural(target.pixels.across * target.pixels.down, "pixel")} from the top left, on ${target.pixels.universes === 1 ? `universe ${target.pixels.universe}` :
          `universes ${target.pixels.universe}–${target.pixels.universe + target.pixels.universes - 1}`}. A pixel that does not fit in a universe starts the next, as pixel mappers count.`);
      return [el("div", { class: "fields" }, controls), reach];
    };

    const blocks = answer.screens.map((screen) => {
      const title = screen.name || "Screen";
      const split = screen.sections.length > 0;
      const where = screen.replaces.length ? `over ${plural(screen.replaces.length, "fixture")}${screen.stands_for < screen.replaces.length ? ` (${screen.replaces.length - screen.stands_for} no longer there)` : ""}`
        : screen.added ? "added in Openviz" : `from the MVR${screen.layer ? `, layer ${screen.layer}` : ""}`;
      const head = el("div", { class: "entry-head" }, el("span", { class: "name" }, title), el("span", { class: "muted small-text" }, `${length(screen.width)} × ${length(screen.height)} m · ${where}`),
        !screen.added && screen.named ? el("span", { class: "faint small-text" }, `MVR source: ${screen.named}`) : null);
      const block = el("div", { class: "entry" }, head, !split && screen.trouble ? notice(screen.trouble, "error") : null);
      if (!split) {
        block.append(...content(screen, screen, title, (changes) => change(screen.id, changes), [screen.width, screen.height]).filter(Boolean), el("div", { "data-screen-word": title }));
      } else {
        // Split into a grid, each section is a screen of its own in all but its place.
        screen.sections.forEach((section, index) => {
          const label = `${title}, section ${index + 1}`;
          const send = async (changes) => {
            try { draw(await api("/api/screen/section", { json: { name, id: screen.id, section: index, changes } })); said.replaceChildren(); }
            catch (error) { said.replaceChildren(notice(error.message, "error")); }
          };
          block.append(el("div", { class: "section-block" },
            el("div", { class: "small-text muted" }, `Section ${index + 1} · row ${Math.floor(index / screen.columns) + 1}, column ${index % screen.columns + 1}`),
            section.trouble ? notice(section.trouble, "error") : null,
            ...content(screen, section, label, send, [screen.width / screen.columns, screen.height / screen.rows]).filter(Boolean),
            el("div", { "data-screen-word": label })));
        });
      }
      // In one piece, or in a grid of sections that each show something of their own.
      const columns = el("input", { type: "number", class: "narrow", min: "1", max: String(answer.max_split), step: "1", value: String(screen.columns || 2), "aria-label": `Sections across ${title}` });
      const rows = el("input", { type: "number", class: "narrow", min: "1", max: String(answer.max_split), step: "1", value: String(screen.rows || 1), "aria-label": `Sections down ${title}` });
      const cut = async (across, down) => {
        try { draw(await api("/api/screen/split", { json: { name, id: screen.id, columns: across, rows: down } })); said.replaceChildren(); }
        catch (error) { said.replaceChildren(notice(error.message, "error")); }
      };
      block.append(el("div", { class: "fields tight" },
        field("Columns", columns), field("Rows", rows),
        el("button", { class: "small", onclick: () => cut(Math.round(Number(columns.value)), Math.round(Number(rows.value))) }, split ? "Split Again" : "Split into Sections"),
        split ? el("button", { class: "small subtle", onclick: () => cut(1, 1) }, "Merge Sections") : null));
      if (screen.added) {
        const number = (label, key, scale, step) => field(label, el("input", { type: "number", class: "narrow", step: String(step), value: String(+(screen[key] / scale).toFixed(3)), "data-key": key, "data-scale": String(scale) }));
        const named = el("input", { type: "text", class: "mid", value: screen.name, "aria-label": "Name", "data-key": "name" });
        const wall = screen.replaces.length > 0;    // as large as its fixtures and where they hang: only its name is its own
        const numbers = wall ? [] : [number("Width (m)", "width", 1000, 0.1), number("Height (m)", "height", 1000, 0.1), number("X (m)", "x", 1000, 0.1), number("Y (m)", "y", 1000, 0.1),
          number("Bottom Edge Z (m)", "z", 1000, 0.1), number("Rotation (°)", "turn", 1, 5)];
        const place = el("details", { class: "how" }, el("summary", {}, wall ? "Name" : "Name, Size and Position"),
          el("div", { class: "fields" }, field("Name", named), numbers,
            el("button", { class: "primary", onclick: () => {
              const changes = { name: named.value };
              for (const input of place.querySelectorAll("input[type=number]")) changes[input.dataset.key] = Number(input.value) * Number(input.dataset.scale);
              change(screen.id, changes);
            } }, "Apply")),
          el("p", { class: "help" }, wall ? "This screen covers the fixtures it replaces. While it shows its own content, those fixtures are hidden in the visualizer; set to Fixtures, or removed, it gives them back."
            : "Measured from the centre of the stage: X toward stage left, Y upstage, Z the bottom edge. At 0° the screen faces the house; positive rotation is counter-clockwise seen from above."),
          el("button", { class: "small subtle danger", onclick: async () => {
            try { draw(await api("/api/screen/remove", { json: { name, id: screen.id } })); said.replaceChildren(); }
            catch (error) { said.replaceChildren(notice(error.message, "error")); }
          } }, icon("trash"), wall ? "Remove Screen (Show Fixtures)" : "Remove Screen"));
        block.append(place);
      }
      return block;
    });
    // Walls of LED panels among the fixtures, which a screen can take the place of.
    const walls = !answer.walls.length ? null : el("div", { class: "card" },
      el("div", { class: "card-head" }, el("div", {}, el("h3", {}, answer.walls.length === 1 ? "LED Wall Detected" : "LED Walls Detected"),
        el("div", { class: "sub" }, "Fixtures hung edge to edge as one surface. Convert them to a screen to show a picture, video or NDI stream across them, or across some sections while the rest stay DMX-driven."))),
      answer.walls.map((wall) => el("div", { class: "entry" }, el("div", { class: "entry-head" },
        el("span", { class: "name" }, wall.count === 1 ? wall.first : `${wall.first} – ${wall.last}`),
        el("span", { class: "muted small-text" }, `${plural(wall.count, "fixture")} of ${typeName(wall.type)} · ${length(wall.width)} × ${length(wall.height)} m`),
        el("span", { class: "grow" }),
        el("button", { class: "small", onclick: busy(null, async () => {
          draw(await api("/api/screen/wall", { json: { name, fixtures: wall.uuids } }));
          said.replaceChildren();
        }, said) }, "Convert to Screen")))));
    put(panel,
      el("div", { class: "toolbar" }, el("p", { class: "muted grow" }, answer.screens.length
        ? "The content of each screen. A show that is open picks up changes immediately; videos restart, and NDI streams take about ten seconds to appear."
        : "This show has no screens yet. The MVR has no video screen with a shape."),
        el("button", { class: "primary", onclick: async () => {
          try { draw(await api("/api/screen/add", { json: { name } })); said.replaceChildren(); }
          catch (error) { said.replaceChildren(notice(error.message, "error")); }
        } }, icon("plus"), "Add Screen")),
      said,
      walls,
      blocks.length ? el("div", { class: "card" }, blocks) : null,
      el("p", { class: "help" }, `New screens are 4 m wide and stand upstage. Pictures: ${answer.pictures.join(", ").toUpperCase()}. Video: ${answer.films.join(", ").toUpperCase()} in H.264, HEVC or ProRes (HAP and DXV are not supported). Content is stretched to the screen. NDI streams are named as NDI lists them, for example Resolume's output with NDI enabled.`));
  };
  gather(api(`/api/show/screens?name=${encodeURIComponent(name)}`)).then((answer) => { draw(answer); askVisualizer(); wordTimer = setInterval(askVisualizer, 2000); })
    .catch((error) => put(panel, notice(error.message, "error")));
  return panel;
}

// ------------------------------------------------------------------------------------- layers

// The layers and classes of a show, each with a box for "visible". What is not visible is left
// out of the visualizer and the Unreal Editor alike, and a show that is open takes it up at once.
function layersPanel(row) {
  const hidden = { layers: new Set(row.hidden.layers), classes: new Set(row.hidden.classes) };
  const status = el("div");
  const send = async () => {
    try {
      await api("/api/show/hidden", { json: { name: row.name, layers: [...hidden.layers], classes: [...hidden.classes] } });
      row.hidden = { layers: [...hidden.layers], classes: [...hidden.classes] };
      status.replaceChildren(notice("Saved. A show that is open picks this up immediately.", "ok"));
    } catch (error) { status.replaceChildren(notice(error.message, "error")); }
  };
  const list = (title, key, entries) => !entries.length ? null : el("div", { class: "card" },
    el("div", { class: "card-head" }, el("h3", {}, title), el("span", { class: "sub" }, `${entries.length - entries.filter((entry) => hidden[key].has(entry.name)).length} of ${entries.length} visible`)),
    entries.map((entry) => {
      const box = el("input", { type: "checkbox", checked: !hidden[key].has(entry.name), "aria-label": `${entry.name || "Unnamed"} visible` });
      box.addEventListener("change", () => { if (box.checked) hidden[key].delete(entry.name); else hidden[key].add(entry.name); send(); });
      const holds = [entry.fixtures ? plural(entry.fixtures, "fixture") : "", entry.pieces ? plural(entry.pieces, "set piece") : ""].filter(Boolean).join(", ");
      return el("label", { class: "entry entry-head check" }, box, el("span", { class: "name" }, entry.name || "(unnamed)"), el("span", { class: "grow" }), el("span", { class: "muted small-text" }, holds));
    }));
  // Groups made in the Patch tab: shown or hidden in the visualizer, renamed, selected, removed.
  const groupAct = async (path, body, text) => {
    try { await api(path, { json: Object.assign({ name: row.name }, body) }); status.replaceChildren(text ? notice(text, "ok") : ""); loadShows(); }
    catch (error) { status.replaceChildren(notice(error.message, "error")); }
  };
  const groups = row.groups || [];
  const groupCard = el("div", { class: "card" },
    el("div", { class: "card-head" }, el("h3", {}, "Groups"), el("span", { class: "sub" }, groups.length ? `${groups.length - groups.filter((g) => g.hidden).length} of ${groups.length} visible` : "")),
    groups.length ? groups.map((group) => {
      const box = el("input", { type: "checkbox", checked: !group.hidden, "aria-label": `${group.name} visible` });
      box.addEventListener("change", () => groupAct("/api/group/set", { id: group.id, changes: { hidden: !box.checked } }, "Saved. A show that is open picks this up immediately."));
      const named = el("input", { type: "text", class: "mid", value: group.name, "aria-label": "Group name", autocomplete: "off" });
      named.addEventListener("change", () => groupAct("/api/group/set", { id: group.id, changes: { name: named.value } }));
      return el("div", { class: "entry entry-head" }, el("label", { class: "check" }, box), named, el("span", { class: "grow" }),
        el("span", { class: "muted small-text" }, plural(group.count, "fixture")),
        el("button", { class: "small", onclick: () => {
          (patchKept[row.name] = patchKept[row.name] || newPatchKept()).ticked = new Set(group.uuids);
          go("show", { show: row.name, tab: "patch" });
        } }, "Select in Patch"),
        el("button", { class: "small subtle danger", onclick: () => groupAct("/api/group/remove", { id: group.id }, `${group.name} removed; its fixtures stay in the show.`) }, icon("trash"), "Remove"));
    }) : el("p", { class: "muted entry" }, "No groups yet. In the Patch tab, select fixtures, then Select › Save Selection as Group…"));
  return el("div", {},
    el("p", { class: "muted" }, "Hide a layer, class or group to leave its contents out of the visualizer. Hidden fixtures stay patched; they are just not drawn or lit."),
    status,
    el("div", { class: "stack" }, groupCard, list("Layers", "layers", row.layer_list || []), list("Classes", "classes", row.class_list || [])));
}

// ---------------------------------------------------------------------------------- paperwork

// The light plot and paperwork of a show: saved as PDF, previewed here, and what goes on them
// (title block, sheets, lists, labels, names of types, notes about fixtures), kept with the show.
function paperPanel(detail) {
  const kept = paperKept[detail.name] || (paperKept[detail.name] = { filter: "" });
  const said = el("div");
  const change = async (changes) => {
    try { await api("/api/paper/set", { json: { name: detail.name, changes } }); said.replaceChildren(); }
    catch (error) { said.replaceChildren(notice(error.message, "error")); }
  };
  const field = (text, control) => el("label", { class: "field" }, el("span", {}, text), control);
  const paper = el("select", { "aria-label": "Paper size", onchange: (event) => save({ plot_paper: event.target.value }, () => {}) },
    state.plot.papers.map((row) => el("option", { value: row.key, selected: row.key === state.plot.paper }, row.label)));
  const measures = el("select", { "aria-label": "Units", onchange: (event) => save({ plot_units: event.target.value }, () => {}) },
    [["metric", "Metric (m)"], ["imperial", "Imperial (ft, in)"]].map(([key, label]) => el("option", { value: key, selected: key === state.plot.units }, label)));
  const made = el("div");
  const make = (what, label) => el("button", { onclick: busy(null, async () => {
    made.replaceChildren(el("p", { class: "muted" }, "Drawing…"));
    try {
      const answer = await api("/api/export/paper", { json: { name: detail.name, what, paper: paper.value, units: measures.value } });
      made.replaceChildren(notice(`Saved ${answer.file}. `, "ok",
        el("button", { class: "link", onclick: () => api("/api/export/open", { json: { file: answer.file } }).catch((error) => made.append(notice(error.message, "error"))) }, "Open"), " · ",
        revealButton("exports")));
    } catch (error) { made.replaceChildren(notice(error.message, "error")); }
  }) }, icon("download"), label);
  const preview = el("div");
  const show = (what) => busy(null, async () => {
    preview.replaceChildren(el("p", { class: "muted" }, "Drawing…"));
    try {
      const response = await fetch(`/api/export/preview?name=${encodeURIComponent(detail.name)}&what=${what}&paper=${paper.value}&units=${measures.value}`, { headers: { "X-Launcher-Token": token } });
      if (!response.ok) {
        let answer = {};
        try { answer = await response.json(); } catch (error) { /* not JSON */ }
        throw new Error(answer.error || `Something went wrong (${response.status}).`);
      }
      const url = URL.createObjectURL(await response.blob());
      put(preview, el("div", { class: "toolbar" }, el("span", { class: "muted grow" }, `${what === "plot" ? "Light plot" : "Paperwork"} preview, as it will be exported.`),
        el("button", { class: "small subtle", onclick: () => preview.replaceChildren() }, "Close Preview")),
        el("iframe", { class: "paper-preview", src: url, title: what === "plot" ? "Light plot preview" : "Paperwork preview" }));
    } catch (error) { preview.replaceChildren(notice(error.message, "error")); }
  });
  const totals = detail.totals;
  const rig = totals && (totals.weight || totals.power) ? `${plural(totals.fixtures, "fixture")} · ${totals.weight >= 100 ? Math.round(totals.weight).toLocaleString() : totals.weight.toFixed(1)} kg · ${totals.power >= 1000 ? `${(totals.power / 1000).toFixed(1)} kW` : `${Math.round(totals.power)} W`}` : "";
  const exportCard = el("div", { class: "card" },
    el("div", { class: "card-head" }, el("div", {}, el("h3", {}, "Export"), el("div", { class: "sub" }, "Light plot: plan, front and side views and a light-level map, to scale. Paperwork: summary, fixture schedule and patch list."))),
    el("div", { class: "card-body" },
      el("div", { class: "fields" }, field("Paper", paper), field("Units", measures), make("plot", "Light Plot PDF"), make("paperwork", "Paperwork PDF"),
        el("button", { class: "subtle", onclick: show("plot") }, "Preview Plot"), el("button", { class: "subtle", onclick: show("paperwork") }, "Preview Paperwork")),
      rig ? el("p", { class: "help" }, `Rig: ${rig}, as stated in the fixture files.`) : null,
      made, preview));
  const holder = el("div", { class: "stack" }, el("p", { class: "muted" }, "Loading paperwork settings…"));
  const fillIn = (answer) => {
    const e = answer.edits;
    const text = (key, label, hint, wide) => {
      const box = el("input", { type: "text", class: wide ? "wide" : "mid", value: e[key], placeholder: hint || "", "aria-label": label, autocomplete: "off" });
      box.addEventListener("change", () => change({ [key]: box.value }));
      return field(label, box);
    };
    const tick = (group, key, label, on) => {
      const box = el("input", { type: "checkbox", checked: on });
      box.addEventListener("change", () => change({ [group]: { [key]: box.checked } }));
      return el("label", { class: "check" }, box, label);
    };
    const LABELS = { id_patch: "ID and address", id: "ID", patch: "Address", purpose: "Purpose (or ID)", none: "Nothing" };
    const labels = el("select", { "aria-label": "Fixture labels" }, answer.labels.map((key) => el("option", { value: key, selected: key === e.labels }, LABELS[key])));
    labels.addEventListener("change", () => change({ labels: labels.value }));
    const notes = el("textarea", { rows: "3", class: "wide", "aria-label": "Notes", placeholder: "Notes on every sheet: trim heights, power, contacts…" }, e.notes);
    notes.addEventListener("change", () => change({ notes: notes.value }));
    const SHEETS = { plan: "Plan", front: "Front view", side: "Side view", lux: "Light levels" };
    const LISTS = { summary: "Summary", schedule: "Fixture schedule", patch: "Patch list" };

    // The types on stage, as the key and the lists name them.
    const counts = {};
    for (const row of detail.fixtures) {
      if (row.notes.includes("notype") || row.notes.includes("removed")) continue;
      const typeLabel = (row.type || "").replace(/@/g, " ").trim() || "(no type)";
      counts[typeLabel] = (counts[typeLabel] || 0) + 1;
    }
    const typeRows = Object.keys(counts).sort((a, b) => a.localeCompare(b)).map((typeLabel) => {
      const entry = e.types[typeLabel] || { name: "", hidden: false };
      const shownAs = el("input", { type: "text", value: entry.name, placeholder: typeLabel, "aria-label": `${typeLabel}: name on paper`, autocomplete: "off" });
      shownAs.addEventListener("change", () => change({ types: { [typeLabel]: { name: shownAs.value } } }));
      const out = el("input", { type: "checkbox", checked: entry.hidden, "aria-label": `Leave ${typeLabel} off paper` });
      out.addEventListener("change", () => change({ types: { [typeLabel]: { hidden: out.checked } } }));
      return el("tr", {}, el("td", { class: "wrap" }, typeLabel), el("td", { class: "num" }, String(counts[typeLabel])), el("td", {}, shownAs), el("td", {}, out));
    });

    // What is written about each fixture: in the schedule, and as a label if asked for.
    const body = el("tbody");
    const filter = el("input", { type: "search", value: kept.filter, placeholder: "Filter by ID, name or type", "aria-label": "Filter fixtures" });
    const onStage = detail.fixtures.filter((row) => row.uuid && !row.notes.includes("notype") && !row.notes.includes("removed"));
    const drawRows = () => {
      kept.filter = filter.value;
      const terms = filter.value.toLowerCase().split(/\s+/).filter(Boolean);
      const rows = onStage.filter((row) => terms.every((term) => `${row.id} ${row.name} ${row.type}`.toLowerCase().includes(term)));
      body.replaceChildren(...rows.slice(0, 150).map((row) => {
        const written = e.fixtures[row.uuid] || {};
        const cells = answer.fields.map((key) => {
          const box = el("input", { type: "text", value: written[key] || "", "aria-label": `${row.name}: ${key}`, autocomplete: "off" });
          box.addEventListener("change", () => { (e.fixtures[row.uuid] = e.fixtures[row.uuid] || {})[key] = box.value; change({ fixtures: { [row.uuid]: { [key]: box.value } } }); });
          return el("td", {}, box);
        });
        return el("tr", {}, el("td", { class: "num" }, row.id), el("td", {}, row.name), el("td", { class: "wrap" }, typeName(row.type)), ...cells);
      }));
      if (rows.length > 150) body.append(el("tr", {}, el("td", { colspan: "6", class: "muted" }, `${rows.length - 150} more: filter to find them.`)));
    };
    filter.addEventListener("input", drawRows);
    drawRows();
    put(holder,
      el("div", { class: "card" }, el("div", { class: "card-head" }, el("h3", {}, "Title Block")),
        el("div", { class: "card-body" },
          el("div", { class: "fields" }, text("title", "Title", bare(detail.name)), text("event", "Event"), text("venue", "Venue")),
          el("div", { class: "fields" }, text("designer", "Lighting Designer"), text("drawn_by", "Drawn By"), text("company", "Company"), text("contact", "Contact")),
          el("div", { class: "fields" }, text("revision", "Revision", "A"), text("revision_note", "Revision Note"), text("date", "Date", "Date of export")),
          field("Notes", notes))),
      el("div", { class: "card" }, el("div", { class: "card-head" }, el("h3", {}, "Contents")),
        el("div", { class: "card-body" },
          el("div", { class: "fields" }, el("span", { class: "small-text faint" }, "Plot sheets"), answer.sheets.map((key) => tick("sheets", key, SHEETS[key], e.sheets[key]))),
          el("div", { class: "fields" }, el("span", { class: "small-text faint" }, "Lists"), answer.lists.map((key) => tick("lists", key, LISTS[key], e.lists[key]))),
          el("div", { class: "fields" }, field("Fixture Labels on the Plan", labels)))),
      el("div", { class: "card" }, el("div", { class: "card-head" }, el("div", {}, el("h3", {}, "Fixture Types"), el("div", { class: "sub" }, "Shorter names for the key and lists, and types to leave off paper."))),
        el("div", { class: "card-body" }, el("div", { class: "tablewrap" }, el("table", {},
          el("thead", {}, el("tr", {}, el("th", {}, "Fixture Type"), el("th", { class: "num" }, "Count"), el("th", {}, "Name on Paper"), el("th", {}, "Hide"))), el("tbody", {}, typeRows))))),
      el("div", { class: "card" }, el("div", { class: "card-head" }, el("div", {}, el("h3", {}, "Fixture Notes"), el("div", { class: "sub" }, "Purpose, colour and a note per fixture appear in the schedule; the purpose can label the plan."))),
        el("div", { class: "card-body" }, el("div", { class: "toolbar" }, el("div", { class: "search" }, icon("search"), filter)),
          el("div", { class: "tablewrap" }, el("table", {}, el("thead", {}, el("tr", {}, el("th", { class: "num" }, "ID"), el("th", {}, "Name"), el("th", {}, "Fixture Type"),
            el("th", {}, "Purpose"), el("th", {}, "Colour"), el("th", {}, "Note"))), body)))),
      el("p", { class: "help" }, "Kept with this show and used by every export. Fields save when you leave them; Undo in the show's header reverts a change."));
  };
  gather(api(`/api/paper?name=${encodeURIComponent(detail.name)}`)).then(fillIn).catch((error) => put(holder, notice(error.message, "error")));
  return el("div", { class: "stack" }, said, exportCard, holder);
}

// ----------------------------------------------------------------------------- fixture library

function renderShare() {
  const share = state.share;
  const status = $("share-status");
  if (!share.has_list) status.replaceChildren("Sign in once to download the fixture list");
  else {
    const days = Math.floor((Date.now() / 1000 - share.list_fetched) / 86400);
    const listed = `${share.revisions.toLocaleString()} files · list from ${days <= 0 ? "today" : days === 1 ? "yesterday" : `${days} days ago`} · `;
    if (share.logged_in || share.remembered) {
      put(status, listed, share.remembered ? `Signed in as ${share.user}` : "Signed in until Openviz quits", " · ",
        el("button", { class: "link", onclick: refreshList }, "Refresh List"), " · ",
        el("button", { class: "link", onclick: logout }, "Sign Out"));
    } else {
      put(status, listed, el("button", { class: "link", onclick: () => askLogin("Sign in to refresh the list or download fixtures.") }, "Sign In"));
    }
  }
  if (!share.has_list && !share.logged_in) askLogin("GDTF Share is the fixture library manufacturers publish to. Signing in downloads its fixture list, which is kept on this computer for searching.");
}

function askLogin(why) {
  $("login-why").textContent = why;
  $("remember-row").hidden = !state.share.can_remember;
  $("login").hidden = false;
  $("user").focus();
}

async function logout(event) {
  event.currentTarget.disabled = true;
  try { state.share = await api("/api/share/logout", { json: {} }); }
  catch (error) { failed(error); }
  renderShare();
}

async function refreshList(event) {
  event.currentTarget.disabled = true;
  try { state.share = await api("/api/share/refresh", { json: {} }); }
  catch (error) { failed(error); }
  renderShare();
  search();
}

function failed(error) {
  if (error.code === "login_required") {
    state.share.logged_in = false;
    state.share.remembered = false;    // a saved sign-in that stopped working has been forgotten
    renderShare();
    askLogin(error.message);
  } else {
    $("fixtures-notice").replaceChildren(notice(error.message, "error"));
  }
}

async function login(event) {
  event.preventDefault();
  const button = event.submitter || $("login").querySelector("button");
  button.disabled = true;
  $("fixtures-notice").replaceChildren();
  try {
    state.share = await api("/api/share/login", { json: { user: $("user").value, password: $("password").value, remember: $("remember").checked } });
    $("login").hidden = true;
    renderShare();
    if (state.share.remember_failed) {
      $("fixtures-notice").replaceChildren(notice(`You are signed in, but the password could not be stored in ${passwordStore()}, so Openviz will ask again next time.`, ""));
    }
    if (pendingDownload) { const again = pendingDownload; pendingDownload = null; await download(again.rid, again.button); }
    search();
  } catch (error) {
    $("fixtures-notice").replaceChildren(notice(error.message, "error"));
  }
  $("password").value = "";
  button.disabled = false;
}

function modesText(modes) {
  if (!modes.length) return "";
  const slots = modes.map((mode) => mode.footprint).filter(Boolean);
  const range = !slots.length ? "" : Math.min(...slots) === Math.max(...slots) ? `, ${slots[0]} ch` : `, ${Math.min(...slots)}–${Math.max(...slots)} ch`;
  return `${plural(modes.length, "mode")}${range}`;
}

async function download(rid, button) {
  button.disabled = true;
  button.textContent = "Downloading…";
  try {
    await api("/api/share/download", { json: { rid } });
    button.replaceWith(el("span", { class: "badge good" }, "In Library"));
    loadLibrary();
  } catch (error) {
    button.disabled = false;
    button.replaceChildren(icon("download"), "Download");
    if (error.code === "login_required") pendingDownload = { rid, button };
    failed(error);
  }
}

let searchTimer = 0;
let searchCount = 0;

async function search() {
  const query = $("q").value.trim();
  const results = $("results");
  if (!state.share.has_list) return results.replaceChildren();
  if (!query) return results.replaceChildren(el("p", { class: "help" }, "Type a fixture or manufacturer name to search."));
  const mine = ++searchCount;
  let answer;
  try { answer = await api(`/api/share/search?q=${encodeURIComponent(query)}&limit=40`); }
  catch (error) { return failed(error); }
  if (mine !== searchCount) return;    // a newer search has started
  const rows = answer.rows.map((row) => el("div", { class: "candidate" },
    el("div", { class: "name" }, el("span", {}, `${row.manufacturer} ${row.fixture} `, el("span", { class: `badge ${row.by_manufacturer ? "good" : ""}` }, row.by_manufacturer ? "Manufacturer" : "Community")),
      el("small", {}, [row.revision, modesText(row.modes), size(row.size), row.more ? `${plural(row.more, "other revision")}` : ""].filter(Boolean).join(" · "))),
    row.have ? el("span", { class: "badge good" }, "In Library") : el("button", { class: "small", onclick: (event) => download(row.rid, event.currentTarget) }, icon("download"), "Download")));
  const out = [];
  if (!rows.length) out.push(el("p", { class: "help" }, `Nothing on GDTF Share matches “${query}”.`));
  else out.push(el("div", { class: "candidates" }, rows));
  if (answer.total > answer.rows.length) out.push(el("p", { class: "help" }, `Showing ${answer.rows.length} of ${answer.total.toLocaleString()}. Add a word to narrow the search.`));
  put(results, out);
}

// A fixture type's own picture, fetched once (with the page's token, so as a blob) and kept for redraws.
const thumbnails = new Map();
function thumbnail(file) {
  const holder = el("div", { class: "thumb", "aria-hidden": "true" }, icon("box"));
  let wanted = thumbnails.get(file);
  if (!wanted) {
    wanted = fetch(`/api/fixtures/thumbnail?file=${encodeURIComponent(file)}`, { headers: { "X-Launcher-Token": token } })
      .then((response) => (response.ok ? response.blob() : null)).then((blob) => (blob ? URL.createObjectURL(blob) : null)).catch(() => null);
    thumbnails.set(file, wanted);
  }
  wanted.then((url) => { if (url) holder.replaceChildren(el("img", { src: url, alt: "" })); });
  return holder;
}

function fixtureItem(row) {
  const title = row.name ? `${row.manufacturer || ""} ${row.name}`.trim() : bare(row.file);
  let verdict;
  if (row.error) verdict = el("span", { class: "badge bad" }, "Unreadable");
  else if (row.unchecked) verdict = el("span", { class: "badge" }, "Not checked");
  else if (row.footprints_differ && row.footprints_differ.length) verdict = el("span", { class: "badge warn", title: `Channel counts differ from GDTF Share's in: ${row.footprints_differ.join(", ")}` }, "Channel count differs");
  else if (row.warnings) verdict = el("span", { class: "badge warn" }, plural(row.warnings, "warning"));
  else verdict = el("span", { class: "badge good", title: row.footprints_checked ? "Channel counts match GDTF Share's" : "" }, row.footprints_checked ? "Verified" : "OK");
  const blocked = ready() || (row.error ? "This file cannot be opened." : "");
  const mode = el("select", { "aria-label": `DMX mode for ${title}` },
    (row.modes || []).map((entry) => el("option", { value: entry.name }, `${entry.name} (${entry.footprint} ch)`)));
  // A newer upload of it on GDTF Share: fetched, and put in its place in the shows that use it.
  const update = !row.update ? null : el("button", { title: `Downloads ${row.update.revision || "the newer revision"} from GDTF Share and uses it in place of this one in your shows. This file is kept.` },
    icon("download"), "Update");
  if (update) update.addEventListener("click", busy(update, async () => {
    let answer;
    try { answer = await api("/api/fixtures/update", { json: { file: row.file } }); }
    catch (error) { if (error.code === "login_required") return failed(error); throw error; }
    const shows = [...new Set(answer.replaced.map((entry) => bare(entry.show)))];
    pageNotice().replaceChildren(notice(`${typeName(answer.file)} downloaded.${shows.length ? ` It now replaces the older revision in ${shows.join(", ")}.` : ""}`, "ok",
      answer.kept.map((entry) => el("div", {}, `${bare(entry.show)}, ${typeName(entry.type)}: kept the older revision. ${entry.why}`))));
    loadLibrary();
  }));
  return el("div", { class: "row-item" }, thumbnail(row.file),
    el("div", { class: "body" }, el("div", { class: "title" }, title, " ", verdict, row.update ? [" ", el("span", { class: "badge info", title: `${row.update.revision} is on GDTF Share` }, "Update Available")] : null),
      el("div", { class: "meta" }, row.error ? `Cannot be read: ${row.error}` : [row.revision, modesText(row.modes || []), size(row.size)].filter(Boolean).join(" · "))),
    el("div", { class: "actions" }, (row.modes || []).length > 1 ? mode : null, update,
      row.built ? el("button", { title: "Opens this fixture type in the Fixture Builder", onclick: () => builderDialog({ file: row.file, done: (answer) => {
        pageNotice().replaceChildren(notice(`${typeName(answer.file)} is saved. Shows that use it pick up the change the next time they are opened.`, "ok"));
        loadLibrary();
      } }) }, icon("edit"), "Edit") : null,
      el("button", { title: blocked || "Opens a row of this fixture type in the visualizer", disabled: !!blocked, onclick: (event) => launch("fixture", row.file, { mode: mode.value }, event.currentTarget) }, icon("play"), "Preview")));
}

async function loadLibrary() {
  let answer;
  try { answer = await api("/api/fixtures"); } catch (error) { return $("library").replaceChildren(notice(error.message, "error")); }
  $("library").replaceChildren(...(answer.fixtures.length ? answer.fixtures.map(fixtureItem)
    : [el("div", { class: "empty" }, el("b", {}, "No fixture types yet"), "Search GDTF Share above, or drop GDTF files on this window.")]));
}

// ------------------------------------------------------------- settings: DMX, visualizer, system

async function save(changes, redraw) {
  try {
    await api("/api/settings", { json: changes });
    state = await api("/api/state");
  } catch (error) {
    pageNotice().replaceChildren(notice(error.message, "error"));
    return;
  }
  redraw();
  $("engine-dot").classList.toggle("off", !!ready());
}

function setting(title, help, control) {
  return el("div", { class: "setting" }, el("div", { class: "what" }, el("b", {}, title), help ? el("span", {}, help) : null),
    control ? el("div", { class: "control" }, control) : null);
}

const toggle = (key, label, redraw) => el("button", { class: "switch", role: "switch", "aria-checked": String(state.settings[key]), "aria-label": label,
  onclick: () => save({ [key]: !state.settings[key] }, redraw) });

const choice = (key, options, redraw) => el("div", { class: "seg", role: "group" }, options.map(([value, label]) =>
  el("button", { class: state.settings[key] === value ? "on" : "", "aria-pressed": String(state.settings[key] === value),
                 onclick: () => save({ [key]: value }, redraw) }, label)));

function slider(key, min, max, step, format, redraw) {
  const output = el("output", {}, format(state.settings[key]));
  return [el("input", { type: "range", min, max, step, value: state.settings[key], "aria-label": key,
    oninput: (event) => { output.textContent = format(Number(event.target.value)); },
    onchange: (event) => save({ [key]: Number(event.target.value) }, redraw) }), output];
}

let lastCheck = null;

async function checkDmx(event) {
  const button = event.currentTarget;
  button.disabled = true;
  button.textContent = "Listening…";
  try { lastCheck = { result: await api("/api/dmx/check", { json: {} }) }; }
  catch (error) { lastCheck = { error: error.message }; }
  renderDmx();
}

function checkResult() {
  if (!lastCheck) return null;
  if (lastCheck.error) return notice(lastCheck.error, "error");
  const result = lastCheck.result;
  const name = result.protocol === "artnet" ? "Art-Net" : "sACN";
  if (!result.universes.length) {
    return notice(`No ${name} arrived in ${result.seconds} seconds${result.other_packets ? `, though ${plural(result.other_packets, "other packet")} did` : ""}. Check that the console sends ${name} on the same network as this computer, and the Network Interface setting.`, "");
  }
  return el("div", {}, notice(`${name} is arriving on ${plural(result.universes.length, "universe")}.`, "ok"),
    el("div", { class: "tablewrap" }, el("table", {},
      el("thead", {}, el("tr", {}, el("th", { class: "num" }, "Universe"), el("th", { class: "num" }, "Packets/s"), el("th", { class: "num" }, "Active Slots"), el("th", {}, "Source"))),
      el("tbody", {}, result.universes.map((u) => el("tr", {}, el("td", { class: "num" }, String(u.universe)), el("td", { class: "num" }, String(u.rate)),
        el("td", { class: "num" }, String(u.slots_in_use)), el("td", {}, u.sources.join(", "))))))));
}

// What is arriving right now. Openviz receives the console's DMX itself and passes it to the
// visualizer, so it can show every universe on the way, with or without a show open.
let live = null;          // what /api/dmx/monitor last said
let liveTimer = null;
let watched = null;       // the universe whose levels are shown
const clock = (seconds) => `${Math.floor(seconds / 60)}:${String(Math.floor(seconds % 60)).padStart(2, "0")}`;

async function loadLive() {
  clearTimeout(liveTimer);
  if ($("view-dmx").hidden) return;    // nobody is looking
  try { live = await api(`/api/dmx/monitor${watched ? `?universe=${watched}` : ""}`); } catch (error) { live = { error: error.message }; }
  if ($("live")) put($("live"), livePanel());
  liveTimer = setTimeout(loadLive, 1000);
}

function livePanel() {
  if (!live) return el("p", { class: "muted" }, "Listening…");
  if (live.error) return notice(live.error, "error");
  const relay = live.relay;
  const name = relay.protocol === "artnet" ? "Art-Net" : "sACN";
  if (!relay.enabled || !relay.possible) {
    // Art-Net sent to this computer's own address: Unreal has to take it itself.
    return [
      notice(relay.possible ? "Openviz was started without its DMX receiver, so the visualizer listens directly and nothing can be monitored here."
        : "Art-Net sent to 127.0.0.1 goes straight to the visualizer and cannot be monitored. Choose this computer's lighting-network address under Network Interface to route it through Openviz.", "info"),
      el("div", { class: "group" },
        setting("Check Input", `Listens for two seconds, as the visualizer does, and lists the universes with ${name} arriving. Close Unreal first.`,
          el("button", { onclick: checkDmx }, "Check Now"))),
      checkResult()];
  }
  if (relay.error) return notice(relay.error, "error");
  const seen = live.monitor;
  if (!seen) return el("p", { class: "muted" }, `Starting to listen for ${name} on ${relay.address}…`);
  if (!seen.universes.length) {
    return notice(`No DMX is arriving. Openviz is listening for ${name} on ${relay.address}${seen.other ? `; ${plural(seen.other, "packet")} of another kind did arrive` : ""}. Check that the console sends ${name} on the same network as this computer, and the Network Interface setting.`, "");
  }
  if (watched && !seen.universes.some((row) => row.universe === watched)) watched = null;
  const rows = seen.universes.map((row) => el("tr", { class: row.universe === watched ? "chosen" : "", tabindex: "0", role: "button",
    "aria-label": `Universe ${row.universe}: show its levels`,
    onclick: () => { watched = watched === row.universe ? null : row.universe; loadLive(); },
    onkeydown: (event) => { if (event.key === "Enter" || event.key === " ") { event.preventDefault(); watched = watched === row.universe ? null : row.universe; loadLive(); } } },
    el("td", { class: "num" }, String(row.universe)), el("td", { class: "num" }, row.rate.toFixed(0)), el("td", { class: "num" }, String(row.slots_in_use)),
    el("td", { class: "num" }, row.still_for < 2 ? "changing" : `${clock(row.still_for)}`),
    el("td", {}, row.sources.map((source) => `${source.name ? `${source.name} · ` : ""}${source.address}${source.priority == null ? "" : ` · priority ${source.priority}`}`).join(", "))));
  const parts = [
    el("p", { class: "muted" }, `${name} on ${relay.address}: ${plural(seen.universes.length, "universe")}, passed on to the visualizer. Click a universe to see its levels.`),
    el("div", { class: "tablewrap" }, el("table", { class: "live" },
      el("thead", {}, el("tr", {}, el("th", { class: "num" }, "Universe"), el("th", { class: "num" }, "Packets/s"), el("th", { class: "num" }, "Active Slots"),
        el("th", { class: "num" }, "Unchanged For"), el("th", {}, "Source"))),
      el("tbody", {}, rows)))];
  if (watched && seen.watch && seen.watch.universe === watched) parts.push(levelGrid(seen.watch));
  return parts;
}

// One universe's 512 levels: a cell a slot, lit by how high it is, with the number on the lit ones.
function levelGrid(watch) {
  const grid = el("div", { class: "levels", role: "img", "aria-label": `Levels of universe ${watch.universe}` });
  for (let slot = 0; slot < 512; slot++) {
    const level = watch.levels[slot] || 0;
    const cell = el("span", { title: `Slot ${slot + 1}: ${level}` }, level ? String(level) : "");
    cell.style.backgroundColor = `rgba(242, 169, 59, ${(level / 255 * 0.9).toFixed(3)})`;    // through the style object: the page's content policy allows that
    grid.append(cell);
  }
  return el("div", {}, el("h4", {}, `Universe ${watch.universe}`), grid,
    el("p", { class: "help" }, "Slots 1–512, 32 per row, as passed to the visualizer. Hover a cell for its slot number."));
}

function renderDmx() {
  const s = state.settings;
  const addresses = state.addresses.includes(s.dmx_address) ? state.addresses : [...state.addresses, s.dmx_address];
  put($("dmx"),
    pageHead("DMX Input", "Openviz receives your console's DMX, monitors it and passes it to the visualizer."),
    el("div", { class: "page-notice" }),
    el("div", { class: "group" },
      setting("Protocol", "What your console transmits.", choice("protocol", [["artnet", "Art-Net"], ["sacn", "sACN"]], renderDmx)),
      setting("Network Interface", "The address of this computer on the lighting network. 127.0.0.1 only receives from programs on this computer.",
        el("select", { "aria-label": "Network interface", onchange: (event) => save({ dmx_address: event.target.value }, renderDmx) },
          addresses.map((address) => el("option", { value: address, selected: address === s.dmx_address },
            address === "127.0.0.1" ? "127.0.0.1 (this computer)" : address)))),
      setting("Light Fixtures Without DMX", "Shows every fixture at its default with the dimmer up until DMX arrives, so a rig is visible without a console.",
        toggle("idle_look", "Light fixtures without DMX", renderDmx))),
    el("h2", {}, "Monitor"),
    el("div", { id: "live" }, livePanel()));
  loadLive();
}

// --------------------------------------------------------------------------------- recordings

// Recording what arrives, playing a recording back to the visualizer, and videos.
let takes = null;        // what /api/recordings last said
let takesTimer = null;
let takesNotice = null;

async function loadTakes() {
  clearTimeout(takesTimer);
  try { takes = await api("/api/recordings"); } catch (error) { takes = { recordings: [], state: {}, error: error.message }; }
  if ($("takes")) put($("takes"), takesPanel());
  // While something is running the figures move: look again in a second.
  if (!$("view-recordings").hidden && (takes.state.recording || takes.state.playing)) takesTimer = setTimeout(loadTakes, 1000);
}

async function takeAction(path, body, said) {
  takesNotice = null;
  try {
    const answer = await api(path, { json: body || {} });
    if (said) takesNotice = notice(said(answer), "ok");
  } catch (error) { takesNotice = notice(error.message, "error"); }
  loadTakes();
}

// Keeping a part of a recording as a recording of its own, to render that part alone.
function trimDialog(take) {
  dialog("Trim Recording", (close) => {
    const said = el("div");
    const box = (label, value) => el("input", { type: "number", min: "0", step: "0.1", value: String(value), "aria-label": label, class: "wide" });
    const start = box("Start, seconds", 0);
    const end = box("End, seconds", take.seconds);
    const keep = el("button", { class: "primary" }, "Keep This Part");
    keep.addEventListener("click", busy(keep, async () => {
      const answer = await api("/api/recording/trim", { json: { name: take.name, start: Number(start.value), end: Number(end.value) } });
      close();
      takesNotice = notice(`Kept ${clock(answer.seconds)} as ${answer.name.replace(/\.dmxrec$/, "")}.`, "ok");
      loadTakes();
    }, said));
    const field = (text, control) => el("label", { class: "field" }, el("span", {}, text), control);
    return [[el("p", { class: "muted" }, `${take.name.replace(/\.dmxrec$/, "")} is ${clock(take.seconds)} long. The part is kept as a new recording that starts with the look as it stood at its start; this one stays as it is.`),
      el("div", { class: "grid2" }, field("Start (s)", start), field("End (s)", end)), said],
      [el("button", { onclick: close }, "Cancel"), keep]];
  });
}

function takesPanel() {
  if (!takes) return el("p", { class: "muted" }, "Loading recordings…");
  const now = takes.state;
  // Through Openviz's own receiver a recording can be made at any time; without it, Art-Net only while Unreal is closed.
  const artnet = state.settings.protocol === "artnet" && !(live && live.relay && live.relay.running);
  const record = now.recording
    ? [el("span", { class: "muted small-text num" }, `${clock(now.seconds)} · ${plural(now.universes, "universe")}`),
       el("button", { class: "primary", onclick: () => takeAction("/api/record/stop", {}, (a) => a.saved ? `Saved ${clock(a.seconds)} of DMX on ${plural(a.universes, "universe")}.` : "Recording stopped.") }, icon("stop"), "Stop Recording")]
    : [el("button", { class: "primary", onclick: () => takeAction("/api/record/start") }, icon("record"), "Record")];
  const rows = takes.recordings.map((take) => {
    const playing = now.playing === take.name;
    const facts = take.error ? take.error : `${clock(take.seconds)} · ${plural(take.universes, "universe")} · ${size(take.size)}`;
    return el("div", { class: "row-item" },
      el("div", { class: "body" }, el("div", { class: "title" }, take.name.replace(/\.dmxrec$/, "")),
        el("div", { class: "meta" }, playing ? `${facts} · playing ${clock(now.position)} of ${clock(now.length)}` : facts)),
      el("div", { class: "actions" },
        el("button", { class: "subtle", disabled: !!take.error || !take.frames, onclick: () => trimDialog(take) }, "Trim…"),
        playing
          ? el("button", { class: "primary", onclick: () => takeAction("/api/play/stop") }, icon("stop"), "Stop")
          : el("button", { disabled: !!take.error || !take.frames, onclick: () => takeAction("/api/play/start", { name: take.name, loop: true }) }, icon("play"), "Play")));
  });
  const videos = (takes.videos || []).map((video) => el("div", { class: "row-item" },
    el("div", { class: "body" }, el("div", { class: "title" }, video.name),
      el("div", { class: "meta" }, `${size(video.size)} · ${new Date(video.made * 1000).toLocaleString()}${video.converting ? " · converting to MP4" : ""}`))));
  return [
    el("div", { class: "group" },
      setting("Record DMX", `Captures what the console sends, to play back later without it. ${artnet ? "Close Unreal while recording Art-Net." : "The visualizer can stay open."} Recordings stop automatically after four hours.`, record)),
    takesNotice,
    el("h2", {}, "Recordings"),
    rows.length ? el("div", { class: "list" }, rows) : el("div", { class: "empty" }, "No recordings yet."),
    rows.length ? el("p", { class: "help" }, "Play loops a recording to the visualizer as Art-Net, as the console would. Open a show to watch it. ", revealButton("recordings")) : null,
    rows.length ? renderForm() : null,
    el("h2", {}, "Videos"),
    videos.length ? el("div", { class: "list" }, videos) : el("div", { class: "empty" }, "No videos yet. Record one while watching a show (Record in the status bar), or render a recording above."),
    videos.length ? el("p", { class: "help" }, revealButton("videos")) : null,
  ];
}

// Rendering a recording to video: a show, a recording, a frame rate and a view. The visualizer
// opens, draws every frame in full at a steady clock and closes when the recording ends.
let renderNotice = null;
const renderSound = { path: "", start: "0" };    // the sound a render's video carries, kept while the page is drawn again
function renderForm() {
  const shows = takes.shows || [];
  if (!shows.length) return null;
  const field = (text, control) => el("label", { class: "field" }, el("span", {}, text), control);
  const show = el("select", { "aria-label": "Show" }, shows.map((name) => el("option", { value: name }, bare(name))));
  const take = el("select", { "aria-label": "Recording" }, takes.recordings.filter((row) => !row.error && row.frames).map((row) => el("option", { value: row.name }, row.name.replace(/\.dmxrec$/, ""))));
  const rate = el("select", { "aria-label": "Frame rate" }, [24, 25, 30, 50, 60].map((n) => el("option", { value: String(n), selected: n === 30 }, `${n} fps`)));
  const view = el("select", { "aria-label": "Starting view" }, el("option", { value: "0" }, "Default view"),
    [1, 2, 3, 4, 5, 6, 7, 8, 9].map((n) => el("option", { value: String(n) }, `View ${n}`)));
  const then = el("input", { type: "text", class: "mid", "aria-label": "Camera moves", placeholder: "e.g. 3@8 5@15", autocomplete: "off" });
  // The song the recording was made to: chosen in the system's file window, and where in it the recording starts.
  const soundName = el("span", { class: "small-text" }, renderSound.path ? renderSound.path.split(/[\\/]/).pop() : "None");
  const chooseSound = el("button", { class: "small" }, renderSound.path ? "Change…" : "Choose…");
  chooseSound.addEventListener("click", busy(chooseSound, async () => {
    const answer = await api("/api/sound/choose", { json: {} });
    if (answer.path) { renderSound.path = answer.path; if ($("takes")) put($("takes"), takesPanel()); }
  }));
  const noSound = !renderSound.path ? null : el("button", { class: "small subtle", onclick: () => { renderSound.path = ""; if ($("takes")) put($("takes"), takesPanel()); } }, "Remove");
  const soundStart = el("input", { type: "number", class: "narrow", step: "0.01", value: renderSound.start, "aria-label": "Sound starts at, seconds",
    oninput: (event) => { renderSound.start = event.target.value; } });
  const go = el("button", { class: "primary", onclick: async () => {
    go.disabled = true;
    renderNotice = null;
    try {
      const start = Number(String(renderSound.start).replace(",", "."));
      if (renderSound.path && !Number.isFinite(start)) throw new Error("Where the sound starts is a number of seconds.");
      renderRun(await api("/api/render", { json: { name: show.value, recording: take.value, rate: Number(rate.value), view: Number(view.value), path: then.value.trim() ? `${view.value} ${then.value}` : "",
        sound: renderSound.path, sound_start: renderSound.path ? start : 0 } }));
      pollRun();
      renderNotice = notice("Rendering. The visualizer window shows the progress and closes by itself when done.", "ok");
    } catch (error) { renderNotice = notice(error.message, "error"); }
    if ($("takes")) put($("takes"), takesPanel());
  } }, "Render");
  return el("div", { class: "card" }, el("div", { class: "card-head" }, el("div", {}, el("h3", {}, "Render to Video"), el("div", { class: "sub" }, "Plays a recording through a show at a steady clock and writes every frame to a movie."))),
    el("div", { class: "card-body" },
      el("div", { class: "fields" }, field("Show", show), field("Recording", take), field("Frame Rate", rate), field("Start From", view), field("Camera Moves", then), go),
      el("div", { class: "fields tight" }, el("span", { class: "small-text faint" }, "Sound:"), soundName, chooseSound, noSound,
        renderSound.path ? [el("span", { class: "small-text faint" }, "starting"), soundStart, el("span", { class: "small-text faint" }, "seconds in")] : null),
      renderNotice,
      el("p", { class: "help" }, `The movie is the visualizer window's size (${(state.settings.resolution || "").replace("x", " × ")}, under Visualizer). Every frame is rendered in full, so a heavy look takes longer, not less smooth. ` +
        "Only the recording drives the fixtures. Camera moves glide between stored views: “3@8 5@15” reaches view 3 at 8 seconds and view 5 at 15; naming a view twice holds it. " +
        "With a sound file (the song the recording was made to), the video carries it: “starting 12.5 seconds in” if the recording began 12.5 seconds into the song, a negative number if it began before. The movie appears under Videos and is converted to MP4.")));
}

function renderRecordings() {
  put($("recordings"),
    pageHead("Recordings", "Record DMX from the console, play it back without it, and render it to video."),
    el("div", { class: "page-notice" }),
    el("div", { id: "takes" }, takesPanel()));
  loadTakes();
}

// ------------------------------------------------------------------- connections: MVR-xchange, OSC

// MVR-xchange: consoles and CAD programs on the same network offer their rigs, and take ours,
// without a file changing hands. Looked at again every few seconds while it is on.
let xchange = null;
let xchangeTimer = null;
let xchangeSaid = null;
let xchangeShown = "";

async function loadXchange() {
  let now;
  try { now = await api("/api/xchange"); } catch (error) { return; }
  const was = xchange;
  xchange = now;
  const holder = $("xchange");
  const active = document.activeElement;
  if (holder && active && holder.contains(active) && active.tagName === "INPUT") return;    // somebody is typing: leave it be
  if (holder && JSON.stringify(now) !== xchangeShown) renderXchange();
  // The shows page lists offered rigs, and a show's menu offers "Send to MVR-xchange Group": both come and go with it.
  if (was ? was.running !== now.running || was.group !== now.group || JSON.stringify(was.offers) !== JSON.stringify(now.offers) : now.running) loadShows();
  if (!xchangeTimer && now.on) xchangeTimer = setInterval(loadXchange, 4000);
}

function renderXchange() {
  const x = xchange;
  const holder = $("xchange");
  if (!x || !holder) return;
  const say = (text, kind) => { xchangeSaid = text ? notice(text, kind) : null; renderXchange(); };
  const set = async (changes) => {
    try { xchange = await api("/api/xchange/set", { json: changes }); say("", ""); }
    catch (error) { say(error.message, "error"); }
    loadShows();
  };
  const field = (text, control) => el("label", { class: "field" }, el("span", {}, text), control);
  const listId = "xchange-groups";
  const group = el("input", { type: "text", class: "mid", value: x.group, list: listId, placeholder: "Group name", "aria-label": "Group" });
  const name = el("input", { type: "text", class: "mid", value: x.name, placeholder: x.default_name, "aria-label": "Station name" });
  const groups = el("datalist", { id: listId }, Object.keys(x.groups).map((entry) => el("option", { value: entry })));
  const others = Object.entries(x.groups).filter(([entry]) => entry.toLowerCase() !== x.group.toLowerCase());
  const stations = x.stations.length
    ? `Connected with ${x.stations.map((station) => station.provider && station.provider !== station.name ? `${station.name} (${station.provider})` : station.name).join(", ")}.`
    : x.running ? "No other stations in this group yet." : "";
  const find = el("button", { class: "subtle", onclick: busy(null, async () => {
    const looked = await api("/api/xchange/groups");
    const found = looked.groups;
    const names = Object.keys(found);
    const how = "In grandMA3: Menu › In & Out › MVR, with Enable switched on.";
    if (!names.length) {
      return say(looked.mine ? `No other MVR-xchange stations are on the network. Openviz is in group ${looked.mine} and will list stations as they join. ${how}`
        : `No MVR-xchange stations are on the network. ${how} You can join a group now and wait.`, "info");
    }
    xchangeSaid = notice("Groups on the network: ", "ok", names.flatMap((entry, index) => [index ? ", " : "",
      entry === looked.mine ? `${entry} (Openviz's group, ${plural(found[entry], "other station")})`
        : [el("button", { class: "link", onclick: () => set({ on: true, group: entry, name: name.value.trim() }) }, `join ${entry}`), ` (${plural(found[entry], "station")})`]].flat()), ".");
    renderXchange();
  }) }, icon("search"), "Find Groups");
  put(holder, el("div", { class: "card" },
    el("div", { class: "card-head" }, el("div", {}, el("h3", {}, "MVR-xchange"), el("div", { class: "sub" }, "Exchange rigs with consoles and CAD programs on the same network.")),
      x.running ? el("span", { class: "badge good" }, `In group ${x.group}`) : el("span", { class: "badge" }, "Off")),
    el("div", { class: "card-body" },
      el("div", { class: "fields" }, field("Group", el("span", {}, group, groups)), field("Station Name", name),
        el("button", { onclick: () => set({ group: group.value.trim(), name: name.value.trim() }) }, "Save"),
        el("button", { class: x.on ? "" : "primary", onclick: () => set({ on: !x.on, group: group.value.trim(), name: name.value.trim() }) }, x.on ? "Leave Group" : "Join Group"),
        find),
      x.trouble ? notice(x.trouble, "error") : null,
      x.running ? el("p", { class: "muted" }, stations, others.length ? ` Other groups: ${others.map(([entry]) => entry).join(", ")}.` : "") : null,
      xchangeSaid,
      el("p", { class: "help" }, "Rigs offered by other stations appear on the Shows page, and are imported only when you choose. With a group joined, each show's menu can send it to the group."))));
  xchangeShown = JSON.stringify(x);
}

// Remote control by OSC: a console, TouchOSC or any program that sends OSC can switch views,
// take pictures and change the picture. The list of messages is redrawn on its own, so that the
// port field is not taken from under the cursor.
let oscState = null;
let oscTimer = null;
let oscNotice = null;

async function loadOsc(changes) {
  clearTimeout(oscTimer);
  oscNotice = null;
  try { oscState = await api("/api/osc", changes ? { json: changes } : {}); } catch (error) {
    if (!oscState) oscState = { addresses: [], heard: [], local: [] };
    oscNotice = notice(error.message, "error");
  }
  if ($("osc")) put($("osc"), oscPanel());
  if (!$("view-connections").hidden && oscState.running) oscTimer = setTimeout(loadHeard, 1500);
}

async function loadHeard() {
  clearTimeout(oscTimer);
  if ($("view-connections").hidden || !$("osc-heard")) return;    // nobody is looking
  try { oscState = await api("/api/osc"); } catch (error) { return; }
  put($("osc-heard"), heardPanel());
  if (oscState.running) oscTimer = setTimeout(loadHeard, 1500);
}

function heardPanel() {
  const heard = (oscState.heard || []).slice().reverse();
  if (!oscState.running) return [];
  if (!heard.length) return el("p", { class: "help" }, "No messages received yet.");
  return el("div", { class: "tablewrap" }, el("table", {},
    el("thead", {}, el("tr", {}, el("th", {}, "Time"), el("th", {}, "From"), el("th", {}, "Address"), el("th", {}, "Values"), el("th", {}, "Result"))),
    el("tbody", {}, heard.map((m) => el("tr", {}, el("td", { class: "num" }, m.at), el("td", {}, m.from), el("td", {}, el("code", {}, m.address)),
      el("td", {}, m.arguments), el("td", { class: m.result.startsWith("not") || m.result.startsWith("an address") ? "muted" : "" }, m.result))))));
}

function oscPanel() {
  if (!oscState) return el("p", { class: "muted" }, "Loading…");
  const o = oscState;
  const port = el("input", { type: "number", class: "narrow", min: "1024", max: "65535", step: "1", value: String(o.port || 8000), "aria-label": "OSC port",
    onchange: (event) => loadOsc({ osc_port: Number(event.target.value) }) });
  const where = o.local && o.local.length ? o.local.join(" or ") : "this computer's address";
  const status = !o.on ? null
    : o.running ? notice(`Listening on port ${o.port}. Send to ${where}, port ${o.port}.`, "ok")
    : o.trouble ? notice(o.trouble, "error") : notice("Not listening yet. Quit and reopen Openviz.", "");
  const sample = `SendOSC 1 "/openviz/view,i,3"`;
  return [
    el("div", { class: "group" },
      setting("Receive OSC", "Lets a console, TouchOSC or any OSC sender switch views, save pictures and adjust the picture while a show is open. Received on every network address of this computer.",
        el("button", { class: "switch", role: "switch", "aria-checked": String(!!o.on), "aria-label": "Receive OSC", onclick: () => loadOsc({ osc: !o.on }) })),
      setting("Port", "The UDP port senders send to. 8000 unless another program uses it.", port)),
    oscNotice, status,
    el("details", { class: "how" }, el("summary", {}, "OSC addresses"),
      el("div", { class: "tablewrap" }, el("table", {},
        el("thead", {}, el("tr", {}, el("th", {}, "Address"), el("th", {}, "Arguments"))),
        el("tbody", {}, (o.addresses || []).map((a) => el("tr", {}, el("td", {}, el("code", {}, a.address)), el("td", {}, a.takes)))))),
      el("p", {}, "Views, stored views and pictures need a show open in the visualizer. Picture settings change Openviz's own settings, which an open show picks up. Any prefix before /openviz/ is ignored.")),
    el("details", { class: "how" }, el("summary", {}, "Sending from grandMA3"),
      el("p", {}, "Menu › In & Out › OSC: switch on Enable Output and add a line with this computer's address as Destination IP, ",
        el("code", {}, String(o.port || 8000)), " as Port, Mode UDP and Send on. A macro or cue command can then send, for the first line:"),
      el("p", {}, el("code", {}, sample), " ", el("button", { class: "link", onclick: () => navigator.clipboard && navigator.clipboard.writeText(sample) }, "Copy")),
      el("p", {}, "This recalls stored view 3. “/openviz/view/top,i,1” shows the plan view; “/openviz/picture,i,4” saves a 4K picture; “/openviz/haze,f,0.5” sets haze to half.")),
    o.running ? el("h4", {}, "Recent Messages") : null,
    el("div", { id: "osc-heard" }, heardPanel()),
  ];
}

// PosiStageNet: a tracking system says where performers and moving scenery are; set pieces given
// a tracker follow it. Openviz's DMX receiver hears it and passes it on to the visualizer.
let psnState = null;
let psnTimer = null;
let psnNotice = null;

async function loadPsn(changes) {
  clearTimeout(psnTimer);
  psnNotice = null;
  try { psnState = await api("/api/psn", changes ? { json: changes } : {}); } catch (error) {
    if (!psnState) psnState = { trackers: [], systems: [], relay: {} };
    psnNotice = notice(error.message, "error");
  }
  const holder = $("psn");
  const active = document.activeElement;
  if (holder && !(active && holder.contains(active) && active.tagName === "INPUT")) put(holder, psnPanel());
  else if ($("psn-trackers")) put($("psn-trackers"), psnTrackers());
  if (!$("view-connections").hidden && psnState.on) psnTimer = setTimeout(loadPsn, 1000);
}

function psnTrackers() {
  const p = psnState;
  if (!p.running) return [];
  if (!p.trackers.length) return el("p", { class: "help" }, "No trackers heard yet.");
  const metre = (value) => value.toFixed(2);
  return [
    p.systems.length ? el("p", { class: "muted" }, `From ${p.systems.map((system) => `${system.name || "a tracking system"} (${system.address})`).join(", ")}.`) : null,
    el("div", { class: "tablewrap" }, el("table", {},
      el("thead", {}, el("tr", {}, el("th", {}, "Tracker"), el("th", {}, "Name"), el("th", { class: "num" }, "X (m)"), el("th", { class: "num" }, "Y (m)"),
        el("th", { class: "num" }, "Z (m)"), el("th", { class: "num" }, "Rate"), el("th", {}, "From"))),
      // Shown as the stage has it: X toward stage left, Y upstage, Z up (PSN's own order is right, up, depth).
      el("tbody", {}, p.trackers.map((t) => el("tr", {}, el("td", { class: "num" }, String(t.id)), el("td", {}, t.name || "—"),
        el("td", { class: "num" }, metre(t.position[0])), el("td", { class: "num" }, metre(t.position[2])), el("td", { class: "num" }, metre(t.position[1])),
        el("td", { class: "num" }, `${t.rate}/s`), el("td", {}, t.source))))))];
}

function psnPanel() {
  if (!psnState) return el("p", { class: "muted" }, "Loading…");
  const p = psnState;
  const group = el("input", { type: "text", class: "mid", value: p.group, placeholder: "This computer only", "aria-label": "PosiStageNet multicast group",
    onchange: (event) => loadPsn({ psn_group: event.target.value.trim() }) });
  const port = el("input", { type: "number", class: "narrow", min: "1024", max: "65535", step: "1", value: String(p.port || 56565), "aria-label": "PosiStageNet port",
    onchange: (event) => loadPsn({ psn_port: Number(event.target.value) }) });
  const relay = p.relay || {};
  const status = !p.on ? null
    : p.running ? notice(`Listening on ${p.group ? `${p.group}, ` : ""}port ${p.port}, on the DMX network (${relay.address}).`, "ok")
    : !relay.enabled ? notice("PosiStageNet comes through Openviz's DMX receiver, which is off in this copy of Openviz.", "")
    : !relay.possible ? notice("PosiStageNet comes through Openviz's DMX receiver, which does not run while Art-Net is received on 127.0.0.1. Choose the lighting network's address under DMX Input.", "")
    : relay.error ? notice(relay.error, "error") : notice("Starting…", "");
  return [
    el("div", { class: "group" },
      setting("Receive PosiStageNet", "Set pieces given a tracker number (a show's Set Pieces tab) follow where a tracking system says that tracker is, while a show is watched. Heard on the network chosen under DMX Input.",
        el("button", { class: "switch", role: "switch", "aria-checked": String(!!p.on), "aria-label": "Receive PosiStageNet", onclick: () => loadPsn({ psn: !p.on }) })),
      setting("Multicast Group", "236.10.10.10 unless the tracking system is set otherwise. Empty: only what is sent to this computer's own address.", group),
      setting("Port", "56565 unless the tracking system is set otherwise.", port)),
    psnNotice, status,
    el("div", { id: "psn-trackers" }, psnTrackers()),
  ];
}

function renderConnections() {
  put($("connections"),
    pageHead("Connections", "Exchange rigs with your console, control the visualizer remotely, and follow a tracking system."),
    el("div", { class: "page-notice" }),
    el("div", { id: "xchange" }, el("p", { class: "muted" }, "Loading…")),
    el("h2", {}, "OSC Remote Control"),
    el("div", { id: "osc" }, oscPanel()),
    el("h2", {}, "PosiStageNet Tracking"),
    el("div", { id: "psn" }, psnPanel()));
  xchangeShown = "";
  if (xchange) renderXchange();
  loadXchange();
  loadOsc();
  loadPsn();
}

// --------------------------------------------------------------------------------- visualizer

// What each quality preset sets. A preset shows as chosen when all of its settings are as it has them.
const PRESETS = {
  Performance: { beams: "cone", beam_detail: 50, beam_shadows: false, shadows: false, megalights: true },
  Balanced: { beams: "cone", beam_detail: 100, beam_shadows: true, shadows: false, megalights: true },
  Quality: { beams: "cone", beam_detail: 100, beam_shadows: true, shadows: true, megalights: true },
};

function renderVisualizer() {
  const s = state.settings;
  const resolutions = ["1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160"];
  if (!resolutions.includes(s.resolution)) resolutions.push(s.resolution);
  put($("visualizer"),
    pageHead("Visualizer", "How the visualizer looks. An open show picks up changes immediately; the window and resolution apply the next time it opens."),
    el("div", { class: "page-notice" }),
    el("h2", {}, "Display"),
    el("div", { class: "group" },
      setting("Window Mode", onMac() ? "Full screen is the only mode in which macOS allows frame rates above the display's." : "Full screen gives the visualizer the whole display.", choice("window", [["windowed", "Window"], ["fullscreen", "Full Screen"]], renderVisualizer)),
      setting("Resolution", "", el("select", { "aria-label": "Resolution", onchange: (event) => save({ resolution: event.target.value }, renderVisualizer) },
        resolutions.map((value) => el("option", { value, selected: value === s.resolution }, value.replace("x", " × ")))))),
    el("h2", {}, "Stage and Atmosphere"),
    el("div", { class: "group" },
      setting("Exposure", "Fixtures are as bright as their files state, so the camera is stopped down. Lower is darker.",
        slider("exposure", -12, 0, 0.5, (value) => `${value > 0 ? "+" : ""}${value} EV`, renderVisualizer)),
      setting("Haze", "How strongly the air reveals beams.", slider("haze", 0, 0.2, 0.01, (value) => `${Math.round(value * 500)}%`, renderVisualizer)),
      setting("Haze Texture", "Real haze drifts unevenly through the room. Even is a hazer left running; Heavy is freshly released smoke.",
        choice("haze_drift", [[0, "Even"], [0.4, "Light"], [0.7, "Medium"], [1, "Heavy"]], renderVisualizer)),
      setting("Hazers in the Rig", "Fog and haze machines patched in the rig add to the haze as the console drives them, building over about 15 seconds and clearing over a minute.",
        toggle("rig_haze", "Hazers in the rig", renderVisualizer)),
      setting("Work Light", "An even light on the stage and fixtures, so the rig is visible when nothing is lit. Does not light the haze.",
        slider("house_lights", 0, 1, 0.05, (value) => value ? `${Math.round(value * 100)}%` : "Off", renderVisualizer)),
      setting("Floor Finish", "A gloss floor shows the lit stage and the fixtures' lenses in it, as a polished or wet stage does. Beams in haze are not reflected.",
        choice("floor", [[0.8, "Matte"], [0.45, "Satin"], [0.15, "Gloss"]], renderVisualizer)),
      setting("Floor Colour", "Most stage floors are black or dark grey; a lighter floor gives back more light.", choice("floor_shade", [[0.02, "Black"], [0.14, "Dark Grey"], [0.35, "Grey"], [0.7, "White"]], renderVisualizer))),
    el("h2", {}, "Lens"),
    el("div", { class: "group" },
      setting("Glow", "How far bright lenses and hot spots glow into the picture around them, as they do on a camera.",
        choice("bloom", [[0, "Off"], [0.3, "Low"], [0.675, "Normal"], [1.5, "High"]], renderVisualizer)),
      setting("Motion Blur", "Fast movement, of the camera or of beams, is blurred along its path, as on video. Off gives sharper single frames.",
        toggle("motion_blur", "Motion blur", renderVisualizer))),
    el("h2", {}, "Camera and Controls"),
    el("div", { class: "group" },
      setting("Mouse Look", "Hold Right Button keeps the pointer free and turns the view only while the right button is held, as in the Unreal Editor. Always hides the pointer, as in games. Tab switches while watching.",
        choice("mouse_look", [["hold", "Hold Right Button"], ["always", "Always"]], renderVisualizer)),
      setting("Look Sensitivity", "How far the view turns for a given mouse movement.",
        slider("look_speed", 0.2, 3, 0.1, (value) => `${value.toFixed(1)}×`, renderVisualizer)),
      setting("Movement Speed", "How fast W A S D, Q and E move the camera. Hold Shift for four times as fast.",
        slider("move_speed", 1, 30, 1, (value) => `${value} m/s`, renderVisualizer)),
      setting("Invert Vertical Look", "Moving the mouse up looks down.", toggle("invert_y", "Invert vertical look", renderVisualizer)),
      setting("Views and Pictures", "While watching, hold a number key 1–9 (or press it with Shift) to store the current view; tap it to recall. 0 returns to the start. P saves a picture.",
        el("button", { onclick: () => reveal("pictures") }, icon("folder"), "Pictures"))),
    el("h2", {}, "Quality"),
    el("div", { class: "group" },
      setting("Preset", "Sets the beam and shadow options below. Performance suits large rigs or slower computers; Quality adds surface shadows, which cost the most.",
        el("div", { class: "seg", role: "group" }, Object.entries(PRESETS).map(([name, values]) => {
          const on = Object.entries(values).every(([key, value]) => s[key] === value);
          return el("button", { class: on ? "on" : "", "aria-pressed": String(on), onclick: () => save(values, renderVisualizer) }, name);
        }))),
      setting("Beam Rendering", "Volumetric draws each beam as a defined shaft from lens to floor. Fog only lights the haze, which blends beams together and costs slightly less.",
        choice("beams", [["cone", "Volumetric"], ["fog", "Fog"]], renderVisualizer)),
      setting("Beam Resolution", "Beams in haze are soft, so they can render below full resolution. Half is hard to tell from Full and much faster with many gobos and prisms. Light on surfaces is unaffected.",
        choice("beam_detail", [[100, "Full"], [75, "75%"], [50, "Half"]], renderVisualizer)),
      setting("Adaptive Beam Resolution", "Lowers beam resolution when a look drops the frame rate below 30 fps, and restores it when it recovers. Off, a heavy look slows the frame rate instead.",
        toggle("adapt_beams", "Adaptive beam resolution", renderVisualizer)),
      setting("Beam Shadows", "Truss, set pieces and screens cut the beams passing through them. Costs frame rate only for beams with something in them.",
        toggle("beam_shadows", "Beam shadows", renderVisualizer)),
      setting("Surface Shadows", "Every beam casts shadows on the floor and set. Costs frame rate.", toggle("shadows", "Surface shadows", renderVisualizer)),
      setting("Colour Gobos on Surfaces", "Coloured gobos and split colours land on the floor and set in colour; otherwise they show in colour in the haze and only as brightness on surfaces. Costs some frame rate. Applies the next time the visualizer opens; the first time, Unreal may prepare its shaders for a few minutes.",
        toggle("surface_colour", "Colour gobos on surfaces", renderVisualizer)),
      setting("MegaLights", `Unreal's renderer for many lights at once; much faster with large rigs. ${onMac() ? "Experimental on macOS." : "Experimental in Unreal 5.8."}`, toggle("megalights", "MegaLights", renderVisualizer)),
      setting("Preview Fixture Count", "How many fixtures a fixture preview hangs in a row.",
        slider("preview_count", 1, 24, 1, (value) => String(value), renderVisualizer))));
}

// ------------------------------------------------------------------------------------- system

function renderSystem() {
  const s = state.settings;
  const build = el("button", { onclick: async (event) => {
    const button = event.currentTarget;    // gone from the event once it has been awaited
    button.disabled = true;
    try { renderRun({ running: false, building: await api("/api/build", { json: {} }) }); pollRun(); }
    catch (error) { pageNotice().replaceChildren(notice(error.message, "error")); button.disabled = false; }
  } }, "Build Now");
  const stale = state.project_built && state.project_stale && state.project_stale[0];
  put($("system"),
    pageHead("System", "Unreal Engine, the visualizer build and where Openviz keeps its files."),
    el("div", { class: "page-notice" }),
    el("div", { class: "group" },
      setting("Unreal Engine", state.engine_found ? el("span", { class: "good" }, "Unreal Engine 5.8 found.") : el("span", { class: "bad" }, "No Unreal Engine in this folder."),
        el("input", { type: "text", value: s.engine, spellcheck: "false", "aria-label": "Unreal Engine folder",
                      onchange: (event) => save({ engine: event.target.value }, renderSystem) })),
      setting("Visualizer", stale ? `Its code has changed since it was built (${state.project_stale[1]}). It is rebuilt before it next opens, or now.`
        : state.project_built ? el("span", { class: "good" }, "Built and up to date.")
        : el("span", {}, el("span", { class: "bad" }, "Not built yet. "), "It is built before it first opens, or now."),
        stale || !state.project_built ? build : el("span", { class: "badge good" }, "Ready")),
      setting("Core Library", state.core_built ? "Built: files are checked as they are added, and patches can be listed."
        : el("span", { class: "bad" }, "Not built: files are not checked and patches cannot be listed."), el("span", { class: `badge ${state.core_built ? "good" : "bad"}` }, state.core_built ? "Ready" : "Missing")),
      setting("Files", el("span", {}, "Shows, fixtures and settings are kept in ", el("code", {}, state.library), "."),
        [el("button", { onclick: () => reveal("shows") }, icon("folder"), "Shows"), el("button", { onclick: () => reveal("logs") }, "Logs")])),
    !state.project_built ? el("p", { class: "help" }, "To build from a terminal instead: ", el("code", {}, state.build_command)) : null);
}

// ------------------------------------------------------------------------------------- parity

const STATES = { done: "Done", wip: "In progress", stock: "In Unreal, not wired up", todo: "Not started" };
const HAS = { D: "Depence", C: "Capture", "D C": "Both" };

async function loadParity() {
  const holder = $("parity");
  let answer;
  try { answer = await api("/api/parity"); } catch (error) { return put(holder, pageHead("Parity"), notice(error.message, "error")); }
  const all = Object.values(answer.totals).reduce((sum, count) => sum + count, 0);
  const bar = (counts, total) => el("span", { class: "progress" }, ["done", "wip", "stock"].map((key) => fill(key, total ? counts[key] / total * 100 : 0)));
  put(holder,
    pageHead("Parity", `Feature by feature, where Openviz stands against Depence and Capture: ${answer.totals.done} of ${all} done.`),
    el("div", { class: "totals" }, Object.entries(STATES).map(([key, label]) => el("div", { class: "total" }, el("b", {}, String(answer.totals[key])), el("span", { class: `state ${key}` }, label)))),
    el("h2", {}, "By Area"),
    answer.sections.map((section) => {
      const counts = { done: 0, wip: 0, stock: 0, todo: 0 };
      for (const row of section.rows) counts[row.state] += 1;
      return el("details", { class: "area" },
        el("summary", {}, el("span", { class: "name" }, section.name), bar(counts, section.rows.length),
          el("span", { class: "count" }, `${counts.done} done · ${counts.wip} in progress · ${section.rows.length} total`)),
        el("div", { class: "tablewrap" }, el("table", {},
          el("thead", {}, el("tr", {}, el("th", {}, "Feature"), el("th", {}, "In"), el("th", { class: "num" }, "Phase"), el("th", {}, "Openviz"), el("th", {}, "Notes"))),
          el("tbody", {}, section.rows.map((row) => el("tr", {}, el("td", { class: "wrap" }, row.feature), el("td", {}, HAS[row.has] || row.has),
            el("td", { class: "num" }, row.phase), el("td", {}, el("span", { class: `state ${row.state}` }, STATES[row.state])), el("td", { class: "wrap" }, row.note)))))));
    }),
    el("p", { class: "help" }, "Read from docs/parity-tracker.md in the Openviz folder."));
}

// --------------------------------------------------------------------------------- status bar

let runTimer = 0;

function renderRun(run) {
  state.run = run;
  const bar = $("runbar");
  if (run && run.building && (run.building.running || run.building.error) && !run.running) {
    // The visualizer's code changed since it was built: it is built first, and then starts by itself.
    const b = run.building;
    bar.hidden = false;
    bar.className = b.running ? "running" : "trouble";
    const title = b.running ? `Building the visualizer${b.then ? ` before opening ${bare(b.then)}` : ""}` : "The visualizer could not be built";
    const lines = b.running
      ? [el("div", {}, `Its code changed since the last build (${b.reason || "an update"}). This takes a few minutes; it opens automatically when done.`),
         b.step ? el("div", { class: "faint" }, b.step) : null]
      : [el("div", { class: "note" }, b.error)];
    put(bar, el("span", { class: "dot" }), el("div", { class: "body" }, el("div", { class: "title" }, title), el("div", { class: "lines" }, lines)),
      el("button", { class: "small", onclick: () => { bar.hidden = true; } }, "Dismiss"));
    return;
  }
  if (!run || (!run.running && run.exit_code === undefined)) { bar.hidden = true; return; }
  bar.hidden = false;
  const crashed = !run.running && run.exit_code && !run.stopped;
  bar.className = run.running ? "running" : crashed ? "trouble" : "";
  const name = bare(run.name);
  let title;
  if (run.running) title = run.editor ? `Unreal Editor · ${name}` : run.rendering ? `Rendering ${run.rendering.replace(/\.dmxrec$/, "")} · ${name}` : `Watching · ${name}`;
  else title = run.exit_code === 0 || run.stopped ? `Closed · ${name}` : run.crash ? `${name} crashed` : `${name} stopped unexpectedly (exit code ${run.exit_code})`;
  const lines = run.lines.length ? run.lines.map((line) => el("div", { class: line.note ? "note" : "" }, line.text))
    : [run.running ? (run.editor ? "Starting the Unreal Editor. This takes about a minute." : "Starting the visualizer. This takes about half a minute.") : ""];
  if (crashed) {
    // A crash: what the log says, and a file to send to whoever looks into it.
    const said = el("span", { class: "faint" });
    if (run.crash && run.crash.message) lines.push(el("div", { class: "note" }, run.crash.message));
    if (run.crash && run.crash.where.length) lines.push(el("div", { class: "note" }, `In Openviz's code: ${run.crash.where.join("  ←  ")}`));
    lines.push(el("div", {}, "A crash report collects the log and crash files of this run into one file. ",
      el("button", { class: "small", onclick: async (event) => {
        event.currentTarget.disabled = true;
        try {
          const answer = await api("/api/report", { json: {} });
          said.textContent = ` Saved ${answer.file}.`;
          reveal("exports");
        } catch (error) { said.textContent = ` ${error.message}`; }
      } }, "Save Crash Report"), said));
  }
  if (run.rendering && run.running) lines.push(el("div", {}, run.progress
    ? `Frame ${run.progress[0].toLocaleString()} of ${run.progress[1].toLocaleString()} (${Math.round(100 * run.progress[0] / Math.max(run.progress[1], 1))}%). The window closes by itself at the end.`
    : "The first look is held for two seconds while the fixtures settle, then the movie begins."));
  for (const video of run.videos || []) {
    const working = video.state === "converting" || video.state === "adding sound";
    lines.push(el("div", { class: video.error ? "note" : "" }, video.state === "adding sound" ? `Adding the sound to ${video.file}…` : working ? `Converting ${video.file} to MP4…` : `Video saved: ${video.file}. ${video.error || ""}`,
      working ? null : [" ", revealButton("videos", "Show Videos")]));
  }
  for (const round of run.panoramas || []) {
    lines.push(el("div", { class: round.error ? "note" : "" }, round.state === "stitching" ? "Making the 360° picture…" : round.error || `360° picture saved: ${round.file}.`,
      round.state === "stitching" ? null : [" ", revealButton("pictures", "Show Pictures")]));
  }
  if (run.running && !run.editor && !run.rendering && run.lines.length) lines.push(el("div", { class: "faint" }, "W A S D move · Q/E down/up · Shift faster · 1–9 recall a view, hold to store · 0 start · P picture · T, F, L plan, front and side views."));
  if (run.running && run.editor && run.lines.length) lines.push(el("div", { class: "faint" }, "Select a fixture and press W to move or E to rotate it. Universe, address and mode are in the Details panel under Openviz. Edits save automatically."));
  // While watching: the views and pictures the keys give, from here as well.
  let steer = null;
  if (run.running && !run.editor && !run.rendering && run.kind === "show" && run.lines.length) {
    const said = el("span", { class: "faint small-text" });
    const ask = (command, value, label) => el("button", { class: "small", onclick: async () => {
      try { await api("/api/visualizer", { json: { command, value } }); said.textContent = ""; }
      catch (error) { said.textContent = ` ${error.message}`; }
    } }, label);
    const stored = new Set(run.views || []);
    const keep = el("select", { "aria-label": "Store the current view as", onchange: async (event) => {
      if (!event.target.value) return;
      try { await api("/api/visualizer", { json: { command: "store", value: event.target.value } }); said.textContent = ` Stored as view ${event.target.value}.`; }
      catch (error) { said.textContent = ` ${error.message}`; }
      event.target.value = "";
    } }, el("option", { value: "" }, "Store view as…"), [1, 2, 3, 4, 5, 6, 7, 8, 9].map((number) => el("option", { value: String(number) }, `View ${number}${stored.has(number) ? " (replace)" : ""}`)));
    steer = el("div", { class: "steer" },
      el("span", { class: "label" }, "Views"), ask("view", "0", "Start"),
      [1, 2, 3, 4, 5, 6, 7, 8, 9].filter((number) => stored.has(number)).map((number) => ask("view", String(number), String(number))), keep,
      el("span", { class: "label" }, "Ortho"), ask("view", "top", "Plan"), ask("view", "front", "Front"), ask("view", "side", "Side"),
      el("span", { class: "label" }, "Picture"), ask("picture", "", "Screen Size"), ask("picture", "3840x2160", "4K"), ask("picture", "7680x4320", "8K"),
      el("span", { title: "A 360-degree picture from where the camera stands, to look round in a photo viewer or a headset" }, ask("panorama", "8192", "360°")),
      el("button", { class: "small subtle", onclick: () => reveal("pictures") }, icon("folder")),
      el("span", { class: "label" }, "Light"), el("button", { class: run.metering ? "small primary" : "small",
        title: "Light levels in lux on the floor and what stands on it, from the beams as they are now, in false colour; the reading under the pointer is shown in the visualizer. The M key does the same.",
        onclick: async () => {
          try { await api("/api/visualizer", { json: { command: "meter", value: run.metering ? "off" : "on" } }); said.textContent = ""; }
          catch (error) { said.textContent = ` ${error.message}`; }
        } }, run.metering ? "Hide Lux" : "Lux Levels"),
      el("span", { class: "label" }, "Video"), run.filming
        ? el("button", { class: "small primary", onclick: async () => { await api("/api/visualizer", { json: { command: "record", value: "stop" } }).catch(() => null); } }, icon("stop"), "Stop")
        : ask("record", "start", [icon("record"), "Record"]),
      said);
  }
  put(bar, el("span", { class: "dot" }), el("div", { class: "body" }, el("div", { class: "title" }, title), el("div", { class: "lines" }, lines), steer),
    run.running ? el("button", { class: "small", onclick: async (event) => { event.currentTarget.disabled = true; renderRun(await api("/api/stop", { json: {} })); } }, icon("stop"), "Stop")
      : el("button", { class: "small", onclick: () => { bar.hidden = true; } }, "Dismiss"));
}

function pollRun() {
  clearTimeout(runTimer);
  runTimer = setTimeout(async () => {
    try { renderRun(await api("/api/run")); } catch (error) { return; }
    if (state.run.running || (state.run.building && state.run.building.running)) pollRun();
    else loadShows();    // Unreal has closed: show what it changed
  }, 1500);
}

// -------------------------------------------------------------------------------------- start

async function start() {
  try {
    state = await api("/api/state");
  } catch (error) {
    put(document.querySelector("main"), el("div", { class: "page" }, notice(error.code === "forbidden"
      ? "This page must be opened by Openviz itself. Close it and start Openviz again."
      : error.message, "error")));
    return;
  }
  buildNav();
  $("version").textContent = state.version || "0.1 beta";
  $("remember-words").textContent = `Stay signed in on ${thisComputer()}`;
  $("remember-where").textContent = passwordStore();
  $("reveal-fixtures").replaceChildren(icon("folder"), showInFolder());
  $("build-fixture").replaceChildren(icon("plus"), "New Fixture Type");
  // With a window of its own (Windows), Openviz ends when the page has gone: this says it has not.
  setInterval(() => { api("/api/here").catch(() => {}); }, 30000);
  $("engine-dot").classList.toggle("off", !!ready());
  warnIfStale();

  const file = $("file");
  file.addEventListener("change", () => { if (file.files.length) addFiles([...file.files]); file.value = ""; });
  // A file dropped anywhere on the window is taken, so a near miss does not make the browser open it.
  let over = 0;
  window.addEventListener("dragenter", () => { over += 1; const drop = $("drop"); if (drop) drop.classList.add("over"); });
  window.addEventListener("dragover", (event) => { event.preventDefault(); });
  window.addEventListener("dragleave", () => { over = Math.max(0, over - 1); const drop = $("drop"); if (drop && !over) drop.classList.remove("over"); });
  window.addEventListener("drop", (event) => {
    event.preventDefault();
    over = 0;
    const drop = $("drop");
    if (drop) drop.classList.remove("over");
    if (event.dataTransfer && event.dataTransfer.files.length) {
      const files = [...event.dataTransfer.files];
      if ($("view-shows").hidden) go("shows");
      addFiles(files);
    }
  });

  // Escape clears the patch selection (and closes the inspector), unless a field or a dialog has it.
  document.addEventListener("keydown", (event) => {
    if (event.key !== "Escape" || document.querySelector("dialog[open], .menu")) return;
    if (event.target.closest && event.target.closest("input, select, textarea")) return;
    const clear = [...document.querySelectorAll(".inspector.has-selection button")].find((button) => button.textContent === "Clear");
    if (clear) clear.click();
  });
  $("q").addEventListener("input", () => { clearTimeout(searchTimer); searchTimer = setTimeout(search, 180); });
  $("login").addEventListener("submit", login);
  $("reveal-fixtures").addEventListener("click", () => reveal("fixtures"));
  $("build-fixture").addEventListener("click", () => builderDialog({ done: (answer) => {
    pageNotice().replaceChildren(notice(`${typeName(answer.file)} is saved in your fixture library. Add it to a show from the show's Patch tab, or use it to replace a placeholder under Fixture Types.`, "ok"));
    loadLibrary();
  } }));

  try { route = Object.assign(route, JSON.parse(sessionStorage.getItem("route") || "{}")); } catch (error) { /* fine */ }
  go(route.view, route);
  search();
  renderRun(state.run);
  if (state.run.running) pollRun();
  loadXchange();
  // A show edited in Unreal while this window sat in the background shows its changes on return.
  window.addEventListener("focus", async () => {
    loadShows();
    try { state.stale = (await api("/api/state")).stale; warnIfStale(); } catch (error) { /* closed */ }
  });
}

// Openviz keeps running the program it was opened with. After an update, say so, once.
function warnIfStale() {
  if (!state.stale || $("stale")) return;
  const banner = notice(`Openviz has been updated since this window opened. ${onMac() ? "Quit Openviz (⌘Q)" : "Close this window"} and open it again to use the new version. Close Unreal first if it is open.`, "info");
  banner.id = "stale";
  document.querySelector("main").prepend(banner);
}

start();
