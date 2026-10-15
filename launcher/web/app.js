"use strict";
// Openviz's launcher page. All it knows comes from launcher.py over /api; names from files and
// from GDTF Share are only ever put on the page as text, never as markup.

// The token arrives after the # of the address, so it is never sent to the server in a URL or
// kept in history. A reload within the same window finds it in sessionStorage.
let token = location.hash.slice(1);
try {
  if (token) sessionStorage.setItem("token", token);
  else token = sessionStorage.getItem("token") || "";
} catch (error) { /* storage is off: the token lives for this page load only */ }
if (location.hash) history.replaceState(null, "", location.pathname);

const VIEWS = ["shows", "fixtures", "dmx", "picture", "parity", "engine"];
const $ = (id) => document.getElementById(id);

function el(tag, props, ...kids) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(props || {})) {
    if (value == null || value === false) continue;
    if (key === "class") node.className = value;
    else if (key.startsWith("on")) node.addEventListener(key.slice(2), value);
    else if (key === "value" || key === "disabled" || key === "selected" || key === "hidden" || key === "open") node[key] = value;
    else node.setAttribute(key, value === true ? "" : value);
  }
  for (const kid of kids.flat()) if (kid != null && kid !== false) node.append(kid);
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
    throw Object.assign(new Error("Openviz is not answering. It may have been closed."), { code: "offline" });
  }
  let answer = {};
  try { answer = await response.json(); } catch (error) { /* not JSON */ }
  if (!response.ok) throw Object.assign(new Error(answer.error || `Something went wrong (${response.status}).`), { code: answer.code });
  return answer;
}

const size = (bytes) => bytes >= 1048576 ? `${(bytes / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(bytes / 1024))} KB`;
const plural = (count, word) => `${count} ${word}${count === 1 ? "" : "s"}`;
const notice = (text, kind, ...more) => el("div", { class: `notice ${kind || ""}`, role: kind === "error" ? "alert" : "status" }, text, ...more);
const bare = (name) => name.replace(/\.(mvr|gdtf)$/i, "");

let state = null;

// Words that depend on the system Openviz runs on (state.system is Python's sys.platform).
const onMac = () => !state || state.system === "darwin";
const onWindows = () => !!state && state.system === "win32";
const thisComputer = () => (onMac() ? "this Mac" : "this computer");
const passwordStore = () => (onMac() ? "the macOS Keychain" : onWindows() ? "Windows' Credential Manager" : "the system's password store");
const showInFolder = () => `Show in ${(state && state.file_manager) || "Finder"}`;
let pendingDownload = null;     // a revision to fetch once logged in
const openPatches = new Set();  // shows whose patch is unfolded
const openLayers = new Set();   // shows whose layers are unfolded
const openScreens = new Set();  // shows whose screens are unfolded
const openTypes = new Set();    // shows whose fixture types are unfolded
const openPieces = new Set();   // shows whose set pieces are unfolded
const typesKept = {};           // per show: what GDTF Share had for its types and what was last said under each, kept while the list of shows is drawn again
const patchKept = {};           // per show: the patch list's filter, which fixtures are ticked and what was last said, kept likewise
// While the list of shows is built again, the panels that are open fetch what they show. They are
// gathered here, so that the new list is only put on the page once they have it: the page then
// keeps its length, and whoever was half way down a patch list stays there.
let gathering = null;
const afterDraw = [];           // what panels want done once they are on the page: a list scrolled back to where it was
let building = 0;               // the list of shows is being built off the page: nothing is on it yet to scroll
const drawn = () => { if (!building) for (const restore of afterDraw.splice(0)) restore(); };
const gather = (loading) => { if (gathering) gathering.push(loading); return loading; };

// ------------------------------------------------------------------------------------- views

function show(view) {
  if (!VIEWS.includes(view)) view = "shows";
  for (const name of VIEWS) {
    $(`view-${name}`).hidden = name !== view;
    document.querySelector(`[data-view="${name}"]`).classList.toggle("on", name === view);
  }
  try { sessionStorage.setItem("view", view); } catch (error) { /* fine */ }
  if (view === "shows") { loadShows(); loadXchange(); }
  if (view === "fixtures") { loadLibrary(); renderShare(); }
  if (view === "dmx") renderDmx();
  if (view === "picture") renderPicture();
  if (view === "parity") loadParity();
  if (view === "engine") renderEngine();
}

function ready() {
  if (!state.engine_found) return "Unreal Engine was not found. Set its folder under Engine.";
  if (!state.project_built) return "The Unreal project has not been built yet. See Engine.";
  return "";
}

// ------------------------------------------------------------------------------------- shows

async function launch(kind, name, extra, button) {
  button.disabled = true;
  try {
    renderRun(await api("/api/launch", { json: Object.assign({ kind, name }, extra) }));
    pollRun();
  } catch (error) {
    $(kind === "show" ? "shows-notice" : "fixtures-notice").replaceChildren(notice(error.message, "error"));
  }
  button.disabled = false;
}

const PROBLEMS = {
  overlap: "shares slots with another fixture",
  past_end: "runs off the end of its universe",
  unpatched: "no DMX address",
};
const NOTES = { moved: "moved", repatched: "re-patched", remoded: "mode changed", renamed: "renamed", reinverted: "pan or tilt mirrored",
  notype: "fixture type missing: left out", nomode: "mode not found: first mode used", added: "added here", removed: "removed",
  unplaced: "no place in the rig file: left dark", retyped: "own fixture type" };
const CHANGED = ["moved", "repatched", "remoded", "renamed", "reinverted", "retyped"];    // what "Back to the rig file" undoes
// Places are shown and typed in metres, to the millimetre; turns in degrees, to the hundredth.
const metres = (millimetres) => String(Math.round(millimetres) / 1000);
const degrees = (value) => String(Math.round(value * 100) / 100);

// What each sortable column of the patch list sorts by: a key, or null for a row with nothing there.
const SORTS = {
  id: (row) => row.id === "" || row.id === undefined ? null : row.id,
  name: (row) => row.name,
  type: (row) => row.type.split("@").slice(0, 2).join(" "),
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

// What goes on a show's paper, kept with the show: the title block, which sheets and lists, notes,
// what labels say, the names in the key, and what is written about each fixture. Every field is
// saved as it is left; a preview draws the PDF here without saving it among the exports.
const paperKept = {};    // per show: whether the editor is open, and the fixture filter, across redraws
function paperEditor(detail, choice) {
  const kept = paperKept[detail.name] || (paperKept[detail.name] = { open: false, filter: "" });
  const holder = el("div");
  const box = el("details", { class: "how", open: kept.open }, el("summary", {}, "Edit what goes on paper"), holder);
  const said = el("div");
  const change = async (changes) => {
    try { await api("/api/paper/set", { json: { name: detail.name, changes } }); said.replaceChildren(); }
    catch (error) { said.replaceChildren(notice(error.message, "error")); }
  };
  const field = (key, label, value, hint, wide) => {
    const input = el("input", { type: "text", class: wide ? "" : "mid", value, placeholder: hint || "", "aria-label": label, autocomplete: "off" });
    input.addEventListener("change", () => change({ [key]: input.value }));
    return el("label", {}, label, input);
  };
  const tick = (group, key, label, on) => {
    const input = el("input", { type: "checkbox", checked: on });
    input.addEventListener("change", () => change({ [group]: { [key]: input.checked } }));
    return el("label", { class: "inline" }, input, ` ${label}`);
  };
  const preview = el("div");
  const show = async (what, button) => {
    button.disabled = true;
    preview.replaceChildren(el("p", { class: "muted" }, "Drawing…"));
    try {
      const { paper, units } = choice();
      const response = await fetch(`/api/export/preview?name=${encodeURIComponent(detail.name)}&what=${what}&paper=${paper}&units=${units}`, { headers: { "X-Launcher-Token": token } });
      if (!response.ok) {
        let answer = {};
        try { answer = await response.json(); } catch (error) { /* not JSON */ }
        throw new Error(answer.error || `Something went wrong (${response.status}).`);
      }
      const url = URL.createObjectURL(await response.blob());
      preview.replaceChildren(el("iframe", { class: "paper-preview", src: url, title: what === "plot" ? "The light plot" : "The paperwork" }),
        el("p", { class: "muted" }, "As it will be saved with Light plot or Paperwork above. ", el("button", { class: "link", onclick: () => preview.replaceChildren() }, "Close the preview")));
    } catch (error) { preview.replaceChildren(notice(error.message, "error")); }
    button.disabled = false;
  };
  const fill = (answer) => {
    const e = answer.edits;
    const LABELS = { id_patch: "ID and address", id: "ID", patch: "address", purpose: "purpose (else ID)", none: "nothing" };
    const labels = el("select", { "aria-label": "Labels" }, answer.labels.map((key) => el("option", { value: key, selected: key === e.labels }, LABELS[key])));
    labels.addEventListener("change", () => change({ labels: labels.value }));
    const notes = el("textarea", { rows: "3", "aria-label": "Notes", placeholder: "Notes for every sheet: hang heights, power, who to call…" }, e.notes);
    notes.addEventListener("change", () => change({ notes: notes.value }));
    const SHEETS = { plan: "Plan", front: "Front view", side: "Side view", lux: "Light levels" };
    const LISTS = { summary: "Summary", schedule: "Fixture schedule", patch: "Patch" };

    // The types on stage, as the key and the lists name them.
    const counts = {};
    for (const row of detail.fixtures) {
      if (row.notes.includes("notype") || row.notes.includes("removed")) continue;
      const name = (row.type || "").replace(/@/g, " ").trim() || "(no type)";
      counts[name] = (counts[name] || 0) + 1;
    }
    const typeRows = Object.keys(counts).sort((a, b) => a.localeCompare(b)).map((name) => {
      const entry = e.types[name] || { name: "", hidden: false };
      const shownAs = el("input", { type: "text", value: entry.name, placeholder: name, "aria-label": `${name}: shown as`, autocomplete: "off" });
      shownAs.addEventListener("change", () => change({ types: { [name]: { name: shownAs.value } } }));
      const out = el("input", { type: "checkbox", checked: entry.hidden, "aria-label": `Leave ${name} off paper` });
      out.addEventListener("change", () => change({ types: { [name]: { hidden: out.checked } } }));
      return el("tr", {}, el("td", { class: "wrap" }, name), el("td", { class: "num" }, String(counts[name])), el("td", {}, shownAs), el("td", {}, out));
    });

    // What is written about each fixture: in the schedule, and as a label if asked for.
    const body = el("tbody");
    const filter = el("input", { type: "search", value: kept.filter, placeholder: "Filter by ID, name or type", "aria-label": "Filter fixtures on paper" });
    const onStage = detail.fixtures.filter((row) => row.uuid && !row.notes.includes("notype") && !row.notes.includes("removed"));
    const drawRows = () => {
      kept.filter = filter.value;
      const terms = filter.value.toLowerCase().split(/\s+/).filter(Boolean);
      const rows = onStage.filter((row) => terms.every((term) => `${row.id} ${row.name} ${row.type}`.toLowerCase().includes(term)));
      body.replaceChildren(...rows.slice(0, 150).map((row) => {
        const written = e.fixtures[row.uuid] || {};
        const cells = answer.fields.map((key) => {
          const input = el("input", { type: "text", value: written[key] || "", "aria-label": `${row.name}: ${key}`, autocomplete: "off" });
          input.addEventListener("change", () => { (e.fixtures[row.uuid] = e.fixtures[row.uuid] || {})[key] = input.value; change({ fixtures: { [row.uuid]: { [key]: input.value } } }); });
          return el("td", {}, input);
        });
        return el("tr", {}, el("td", { class: "num" }, row.id), el("td", {}, row.name), el("td", { class: "wrap" }, (row.type || "").split("@").slice(0, 2).join(" ")), ...cells);
      }));
      if (rows.length > 150) body.append(el("tr", {}, el("td", { colspan: "6", class: "muted" }, `and ${rows.length - 150} more: filter to find them`)));
    };
    filter.addEventListener("input", drawRows);
    drawRows();

    put(holder,
      el("p", { class: "muted" }, "Kept with this show: every Light plot and Paperwork saved from now on has it. Fields save as you leave them; Undo on the show's row takes a change back."),
      el("h3", {}, "Title block"),
      el("div", { class: "row" }, field("title", "Title", e.title, bare(detail.name), true), field("event", "Event", e.event), field("venue", "Venue", e.venue)),
      el("div", { class: "row" }, field("designer", "Lighting designer", e.designer), field("drawn_by", "Drawn by", e.drawn_by), field("company", "Company", e.company), field("contact", "Contact", e.contact)),
      el("div", { class: "row" }, field("revision", "Revision", e.revision, "A"), field("revision_note", "What changed", e.revision_note, "", true), field("date", "Date", e.date, "the day it is drawn")),
      el("label", { class: "block" }, "Notes", notes),
      el("h3", {}, "What goes in"),
      el("div", { class: "row" }, el("span", { class: "muted" }, "Plot sheets:"), answer.sheets.map((key) => tick("sheets", key, SHEETS[key], e.sheets[key]))),
      el("div", { class: "row" }, el("span", { class: "muted" }, "Lists:"), answer.lists.map((key) => tick("lists", key, LISTS[key], e.lists[key]))),
      el("div", { class: "row" }, el("label", {}, "Beside each fixture on the plan", labels)),
      el("h3", {}, "Fixture types"),
      el("div", { class: "tablewrap" }, el("table", {}, el("thead", {}, el("tr", {}, el("th", {}, "In the rig file"), el("th", { class: "num" }, "On stage"), el("th", {}, "Shown as"), el("th", {}, "Leave off"))),
        el("tbody", {}, typeRows))),
      el("h3", {}, "About each fixture"),
      el("p", { class: "muted" }, "Purpose, colour and a note go into the fixture schedule (a column only where something is written), and the purpose can be the label on the plan."),
      filter,
      el("div", { class: "tablewrap" }, el("table", {}, el("thead", {}, el("tr", {}, el("th", { class: "num" }, "ID"), el("th", {}, "Name"), el("th", {}, "Type"),
        el("th", {}, "Purpose"), el("th", {}, "Colour"), el("th", {}, "Note"))), body)),
      said,
      el("div", { class: "row" }, el("button", { onclick: (event) => show("plot", event.currentTarget) }, "Preview the plot"),
        el("button", { onclick: (event) => show("paperwork", event.currentTarget) }, "Preview the paperwork")),
      preview);
  };
  box.addEventListener("toggle", () => {
    kept.open = box.open;
    if (box.open && !holder.childNodes.length) {
      holder.replaceChildren(el("p", { class: "muted" }, "Reading…"));
      api(`/api/paper?name=${encodeURIComponent(detail.name)}`).then(fill).catch((error) => holder.replaceChildren(notice(error.message, "error")));
    }
  });
  if (kept.open) api(`/api/paper?name=${encodeURIComponent(detail.name)}`).then(fill).catch((error) => holder.replaceChildren(notice(error.message, "error")));
  return box;
}

// Undo and Redo of what was changed in a show from this page, in order; a show that is open follows.
function undoLine(row) {
  if (!row.undo && !row.redo) return null;
  const step = (which) => async (event) => {
    event.currentTarget.disabled = true;
    try {
      const answer = await api(`/api/${which}`, { json: { name: row.name } });
      $("shows-notice").replaceChildren(notice(`${which === "undo" ? "Undone" : "Redone"}: ${answer.done}.`, "ok"));
    } catch (error) { $("shows-notice").replaceChildren(notice(error.message, "error")); }
    loadShows();
  };
  return el("div", { class: "meta" },
    row.undo ? el("button", { class: "link", title: "Takes back the last change made here", onclick: step("undo") }, `Undo: ${row.undo}`) : null,
    row.undo && row.redo ? " · " : null,
    row.redo ? el("button", { class: "link", title: "Makes the change undone again", onclick: step("redo") }, `Redo: ${row.redo}`) : null);
}

function patchPanel(detail) {
  const words = [];
  if (detail.conflicts.length) words.push(el("span", { class: "bad" }, `${plural(detail.conflicts.length, "address conflict")}. `));
  const adrift = detail.fixtures.filter((row) => row.problems.includes("past_end")).length;
  if (adrift) words.push(el("span", { class: "bad" }, `${plural(adrift, "fixture")} run off the end of a universe. `));
  if (!detail.conflicts.length && !adrift) words.push(el("span", { class: "good" }, "No address conflicts. "));
  words.push(`${plural(detail.universes.length, "universe")} in use.`);

  // What the rig weighs and draws, as far as the fixtures' files say.
  const totals = detail.totals;
  const load = [];
  if (totals && (totals.weight || totals.power)) {
    const kilograms = totals.weight >= 100 ? Math.round(totals.weight).toLocaleString() : totals.weight.toFixed(1);
    const power = totals.power >= 1000 ? `${(totals.power / 1000).toFixed(1)} kW` : `${Math.round(totals.power)} W`;
    load.push(`${plural(totals.fixtures, "fixture")} on stage weigh ${kilograms} kg and draw ${power}. `);
    const caveats = [];
    if (totals.no_weight) caveats.push(`${plural(totals.no_weight, "fixture")} state no weight`);
    if (totals.sources_only) caveats.push(`for ${totals.sources_only} the power is that of the light sources alone, so the real draw is higher`);
    if (caveats.length) load.push(el("span", { class: "muted" }, `As far as the fixture files say: ${caveats.join("; ")}. `));
  }
  const saved = el("span", { class: "muted" });
  load.push(el("button", { class: "link", onclick: async (event) => {
    event.currentTarget.disabled = true;
    try {
      const answer = await api("/api/export/patch", { json: { name: detail.name } });
      saved.replaceChildren(` Saved as ${answer.file}. `, el("button", { class: "link", onclick: () => api("/api/reveal", { json: { what: "exports" } }) }, showInFolder()));
    } catch (error) { saved.replaceChildren(` ${error.message}`); }
    event.currentTarget.disabled = false;
  } }, "Save the patch as a spreadsheet"), saved);

  // The same on paper: a plot drawn to scale, and the lists that go with it.
  const paper = el("select", { "aria-label": "Paper for the plot", onchange: (event) => save({ plot_paper: event.target.value }, () => {}) },
    state.plot.papers.map((row) => el("option", { value: row.key, selected: row.key === state.plot.paper }, row.label)));
  const measures = el("select", { "aria-label": "Measures", onchange: (event) => save({ plot_units: event.target.value }, () => {}) },
    [["metric", "metres"], ["imperial", "feet and inches"]].map(([key, label]) => el("option", { value: key, selected: key === state.plot.units }, label)));
  const made = el("span", { class: "muted" });
  const make = (what, label) => el("button", { onclick: async (event) => {
    const button = event.currentTarget;
    button.disabled = true;
    made.replaceChildren(" Drawing…");
    try {
      const answer = await api("/api/export/paper", { json: { name: detail.name, what, paper: paper.value, units: measures.value } });
      made.replaceChildren(` Saved as ${answer.file}. `,
        el("button", { class: "link", onclick: () => api("/api/export/open", { json: { file: answer.file } }).catch((error) => made.append(` ${error.message}`)) }, "Open"), " ",
        el("button", { class: "link", onclick: () => api("/api/reveal", { json: { what: "exports" } }) }, showInFolder()));
    } catch (error) { made.replaceChildren(` ${error.message}`); }
    button.disabled = false;
  } }, label);
  const onPaper = el("div", {}, el("p", { class: "on-paper" }, el("b", {}, "On paper: "), make("plot", "Light plot"), " ", make("paperwork", "Paperwork"),
    " as PDF, on ", paper, " in ", measures, ".", made), paperEditor(detail, () => ({ paper: paper.value, units: measures.value })));

  const universes = el("div", { class: "universes" }, detail.universes.map((u) =>
    el("span", { class: `universe ${u.conflict ? "clash" : ""}`, title: `${plural(u.fixtures, "fixture")}, ${512 - u.used} slots free` },
      el("b", {}, `U${u.universe}`), ` ${u.used}/512`, el("span", { class: "bar" }, fill("", u.used / 5.12)))));

  // What is kept while the list of shows, and this panel with it, is drawn again after a change.
  const kept = patchKept[detail.name] || (patchKept[detail.name] = { filter: "", only: false, ticked: new Set(), note: null, sort: null, down: false });
  const listed = new Map(detail.fixtures.filter((row) => row.uuid).map((row) => [row.uuid, row]));
  for (const uuid of [...kept.ticked]) if (!listed.has(uuid)) kept.ticked.delete(uuid);
  const said = el("div");
  if (kept.note) { said.replaceChildren(notice(kept.note[0], kept.note[1])); kept.note = null; }

  const body = el("tbody");
  const filter = el("input", { type: "search", placeholder: "Filter by name, type, mode, address or remark", "aria-label": "Filter fixtures", value: kept.filter });
  const box = el("input", { type: "checkbox", checked: kept.only });
  const only = el("label", { class: "muted" }, box, " problems only");
  const editor = el("div");
  const count = el("span", { class: "muted" });
  let shown = [];
  // Headings that sort the list: once by that column, again the other way round.
  const heads = [["ID", "id"], ["Name", "name"], ["Type", "type"], ["Mode", "mode"], ["Address", "address"], ["Slots", "slots"], ["Place (m)", "place"], ["Layer", "layer"]]
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
  const redrawEditor = () => {
    const chosen = detail.fixtures.filter((row) => kept.ticked.has(row.uuid));
    count.textContent = chosen.length ? ` ${plural(chosen.length, "fixture")} ticked.` : "";
    editor.replaceChildren(chosen.length ? editFixtures(detail, chosen, kept) : el("p", { class: "muted" },
      "Tick fixtures to change them: where they stand, how they are turned, their address, mode, name and number. Several can be changed at once."));
  };
  const draw = () => {
    kept.filter = filter.value;
    kept.only = box.checked;
    const terms = filter.value.toLowerCase().split(/\s+/).filter(Boolean);
    shown = detail.fixtures.filter((row) => {
      if (box.checked && !row.problems.length && !row.notes.includes("notype")) return false;
      const text = `${row.id} ${row.name} ${row.type} ${row.mode} ${row.layer} ${row.universe}.${row.address} ${remarksOf(row)}`.toLowerCase();
      return terms.every((term) => text.includes(term));
    });
    if (kept.sort && SORTS[kept.sort]) {
      const order = SORTS[kept.sort];
      // Rows with nothing to sort by (no address, no place) stay at the foot either way round.
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
      const address = out ? "" : row.universe > 0 ? `${row.universe}.${row.address}` : "none";
      const place = out || !row.place ? "" : row.notes.includes("unplaced") ? "none" : `${metres(row.place[0])}, ${metres(row.place[1])}, ${metres(row.place[2])}`;
      const tick = el("input", { type: "checkbox", checked: kept.ticked.has(row.uuid), disabled: !row.uuid, "aria-label": `Tick ${row.name}` });
      const line = el("tr", { class: `${row.problems.length ? "problem" : out ? "out" : ""} ${kept.ticked.has(row.uuid) ? "chosen" : ""}` },
        el("td", {}, tick), el("td", { class: "num" }, row.id), el("td", {}, row.name), el("td", { title: row.type }, row.type.split("@").slice(0, 2).join(" ")), el("td", {}, row.mode),
        el("td", { class: "num" }, address), el("td", { class: "num" }, row.slots ? String(row.slots) : ""), el("td", { class: "num" }, place),
        el("td", {}, row.layer), el("td", { class: "wrap" }, remarksOf(row)));
      const toggle = () => {
        if (!row.uuid) return;
        if (kept.ticked.has(row.uuid)) kept.ticked.delete(row.uuid); else kept.ticked.add(row.uuid);
        tick.checked = kept.ticked.has(row.uuid);
        line.classList.toggle("chosen", tick.checked);
        redrawEditor();
      };
      tick.addEventListener("click", (event) => { event.stopPropagation(); toggle(); });
      line.addEventListener("click", toggle);    // anywhere on its line ticks a fixture
      return line;
    }));
    if (!shown.length) body.replaceChildren(el("tr", {}, el("td", { colspan: "10", class: "muted" }, "No fixtures match.")));
  };
  filter.addEventListener("input", draw);
  box.addEventListener("change", draw);

  const tickThese = (rows) => { for (const row of rows) if (row.uuid) kept.ticked.add(row.uuid); draw(); redrawEditor(); };
  const loose = detail.fixtures.filter((row) => row.notes.includes("unplaced"));
  const pick = el("div", { class: "filter" }, el("span", { class: "muted" }, "Tick:"),
    el("button", { class: "link", onclick: () => tickThese(shown) }, "all that are listed"),
    el("button", { class: "link", onclick: () => { kept.ticked.clear(); draw(); redrawEditor(); } }, "none"),
    loose.length ? el("button", { class: "link", onclick: () => { kept.ticked.clear(); tickThese(loose); } }, `the ${loose.length} without a place`) : null, count);
  // Fixtures a console patched and never set down in its 3D space have no place in the rig file: they would all stand in one spot.
  const noPlace = !loose.length ? null : el("div", { class: "warn" },
    `${plural(loose.length, "fixture")} ${loose.length === 1 ? "has" : "have"} no place in the rig file: the console patched ${loose.length === 1 ? "it" : "them"} and never set ${loose.length === 1 ? "it" : "them"} down in its 3D space. ` +
    `${loose.length === 1 ? "It is" : "They are"} left dark, since ${loose.length === 1 ? "it" : "they"} would all light up in one spot. Tick ${loose.length === 1 ? "it" : "them"}, give the first a place, and say how far apart the rest go; or place them one by one in the Unreal Editor.`);

  const wrap = el("div", { class: "tablewrap" }, el("table", { class: "live" },
    el("thead", {}, el("tr", {}, ["", "ID", "Name", "Type", "Mode", "Address", "Slots", "Place (m)", "Layer", "Remarks"].map((title, index) => {
      const head = heads.find((h) => h.title === title);
      return head ? head.cell : el("th", {}, title);
    }))), body));
  draw();
  redrawEditor();
  wrap.addEventListener("scroll", () => { kept.scroll = wrap.scrollTop; });
  if (kept.scroll) afterDraw.push(() => { wrap.scrollTop = kept.scroll; });    // once it is on the page: before that it has no height to scroll
  return el("div", { class: "detail" },
    el("div", {}, words), el("div", {}, load), onPaper, universes, noPlace,
    el("div", { class: "filter" }, filter, only), pick, wrap,
    said, editor,
    el("p", { class: "muted" }, "Addresses are universe.slot on the fixture's first DMX break. A place is x, y, z in metres: x toward stage left (the audience's right), y upstage, z up. " +
      (detail.open ? "The show is open: what is changed here shows there within half a second." : "A show that is open takes up what is changed here at once.") +
      " The rig file itself is never changed: Export as MVR writes a new one."),
    addFixturesForm(detail, said));
}

// The form under the patch list for the fixtures that are ticked. For one fixture every field shows
// what it has, and what is changed is saved. For several a field left empty leaves them as they
// are; a place is that of the first with the others set out in a row from it, a patch runs on from
// one to the next, names and numbers count up.
function editFixtures(detail, chosen, kept) {
  const one = chosen.length === 1 ? chosen[0] : null;
  const many = chosen.length > 1;
  const live = chosen.filter((row) => !row.notes.includes("removed"));
  const said = el("div");
  const input = (label, value, wide) => el("input", { type: "text", class: wide ? "" : "narrow", "aria-label": label, value: value == null ? "" : String(value), autocomplete: "off" });
  const labelled = (text, control) => el("label", {}, text, control);

  const name = input("Name", one ? one.name : "", true);
  const numbered = input("Numbered from", "");
  const id = input(many ? "First number" : "Number", one ? one.id : "");
  const patched = one && one.universe > 0;
  const universe = input("Universe", patched ? one.universe : "");
  const address = input("Address", patched ? one.address : "");
  // The modes every ticked fixture has: one of them can be set for all.
  const shared = live.length && live.every((row) => row.modes.length) ? live[0].modes.filter((mode) => live.every((row) => row.modes.some((other) => other.name === mode.name))) : [];
  const mode = !shared.length ? null : el("select", { "aria-label": "Mode" },
    many ? el("option", { value: "" }, "as they are") : null,
    shared.map((entry) => el("option", { value: entry.name, selected: !!one && entry.name === one.mode }, `${entry.name} (${plural(entry.slots, "slot")})`)));
  const placed = one && one.place && !one.notes.includes("unplaced");
  const at = ["x", "y", "z"].map((axis, index) => input(axis, placed ? metres(one.place[index]) : ""));
  const step = ["x", "y", "z"].map((axis) => input(`each next one further along ${axis}`, ""));
  const by = ["x", "y", "z"].map((axis) => input(`Move along ${axis}`, ""));
  const turn = ["X", "Y", "Z"].map((axis, index) => input(`Turned about ${axis}`, one && one.place ? degrees(one.place[3 + index]) : ""));
  const mirror = (key, has) => !live.some((row) => row[has]) ? null : el("select", { "aria-label": key === "invert_pan" ? "Pan" : "Tilt" },
    many ? el("option", { value: "" }, "as they are") : null,
    el("option", { value: "0", selected: !!one && !one[key] }, "as its fixture type runs"), el("option", { value: "1", selected: !!one && one[key] }, "mirrored"));
  const pan = mirror("invert_pan", "has_pan");
  const tilt = mirror("invert_tilt", "has_tilt");

  // What the form says is to change. Throws with what is wrong.
  const read = () => {
    const changes = {};
    const number = (field, what, whole) => {
      const text = field.value.trim().replace(",", ".");
      const value = Number(text);
      if (!text || !Number.isFinite(value) || (whole && !Number.isInteger(value))) throw new Error(`${what} is ${whole ? "a whole number" : "a number"}.`);
      return value;
    };
    const was = (field, value) => one && field.value.trim() === String(value == null ? "" : value);    // one fixture, and the field as it was shown
    if (name.value.trim() && !was(name, one && one.name)) {
      changes.name = name.value.trim();
      if (many && numbered.value.trim()) changes.number = number(numbered, "The number names count from", true);
    }
    if (!was(id, one && one.id) && (id.value.trim() || one)) changes.id = id.value.trim() ? number(id, "A fixture number", true) : "";
    if (universe.value.trim() || address.value.trim()) {
      if (!(patched && was(universe, one.universe) && was(address, one.address))) {
        changes.universe = number(universe, "The universe", true);
        if (changes.universe > 0) changes.address = number(address, "The address", true);
      }
    } else if (patched) {
      changes.universe = 0;    // both emptied: unpatched
    }
    if (mode && mode.value && !(one && mode.value === one.mode)) changes.mode = mode.value;
    ["x", "y", "z"].forEach((axis, index) => {
      if (at[index].value.trim() && !(placed && was(at[index], metres(one.place[index])))) changes[axis] = number(at[index], `The place along ${axis}`) * 1000;
      if (many && step[index].value.trim()) changes[`step${axis}`] = number(step[index], `The distance along ${axis}`) * 1000;
      if (by[index].value.trim()) changes[`d${axis}`] = number(by[index], `The move along ${axis}`) * 1000;
    });
    // A fixture without a place gets one from the first number typed: the others are then where it lay, at 0.
    ["rx", "ry", "rz"].forEach((key, index) => {
      if (turn[index].value.trim() && !(one && one.place && was(turn[index], degrees(one.place[3 + index])))) changes[key] = number(turn[index], "A turn");
    });
    if (pan && pan.value && !(one && (pan.value === "1") === one.invert_pan)) changes.invertpan = pan.value === "1";
    if (tilt && tilt.value && !(one && (tilt.value === "1") === one.invert_tilt)) changes.inverttilt = tilt.value === "1";
    return changes;
  };
  const send = async (changes, what, rows) => {
    await api("/api/show/set", { json: { name: detail.name, uuids: (rows || live).map((row) => row.uuid), changes } });
    kept.note = [what, "ok"];
    loadShows();
  };
  const doing = (button, work) => async () => {
    button.disabled = true;
    try { await work(); }
    catch (error) { said.replaceChildren(notice(error.message, "error")); button.disabled = false; }
  };
  const title = one ? `${one.id ? `${one.id} ` : ""}${one.name}` : `${plural(chosen.length, "fixture")}: ${chosen.slice(0, 4).map((row) => row.id || row.name).join(", ")}${chosen.length > 4 ? "…" : ""}`;
  const save = el("button", { class: "primary" }, "Save");
  save.addEventListener("click", doing(save, async () => {
    const changes = read();
    if (!Object.keys(changes).length) throw new Error("Nothing was changed.");
    if (!live.length) throw new Error("A fixture that was removed has to be put back before it can be changed.");
    await send(changes, `${one ? title : plural(live.length, "fixture")} changed.`);
  }));
  const half = el("button", { title: "Turns each about its own vertical axis: a moving head then tilts the other way" }, "Half a turn round");
  half.addEventListener("click", doing(half, () => send({ spin: 180 }, `${one ? title : plural(live.length, "fixture")} turned half a turn round.`)));
  const changed = live.filter((row) => row.notes.some((note) => CHANGED.includes(note)) && !row.notes.includes("added"));
  const back = !changed.length ? null : el("button", { title: "Its place, patch, mode, name, mirroring and type as the rig file has them" }, "Back to the rig file");
  if (back) back.addEventListener("click", doing(back, () => send({ reset: "all" }, `${plural(changed.length, "fixture")} put back as the rig file has ${changed.length === 1 ? "it" : "them"}.`, changed)));
  // Taking fixtures out, or putting those of the rig file back. An added one that is taken out is simply gone.
  const gone = chosen.filter((row) => row.notes.includes("removed"));
  const remove = async (rows, restore) => {
    await api("/api/show/remove", { json: { name: detail.name, uuids: rows.map((row) => row.uuid), restore } });
    kept.note = [`${plural(rows.length, "fixture")} ${restore ? "put back in the show" : "taken out of the show"}.`, "ok"];
    loadShows();
  };
  const out = !live.length ? null : el("button", { title: "Takes them out of the show. One of the rig file's can be put back; one that was added here is gone for good." }, "Remove");
  if (out) out.addEventListener("click", doing(out, () => remove(live, false)));
  const putBack = !gone.length ? null : el("button", {}, gone.length === chosen.length ? "Put back" : `Put the ${gone.length} removed back`);
  if (putBack) putBack.addEventListener("click", doing(putBack, () => remove(gone, true)));
  const close = el("button", { class: "link", onclick: () => { kept.ticked.clear(); loadShows(); } }, "Untick");

  // Another fixture type for these fixtures alone, from the fixture library.
  const retyped = live.filter((row) => row.notes.includes("retyped"));
  const typeHolder = el("div");
  const typeOpen = !live.length ? null : el("details", { class: "how" },
    el("summary", {}, many ? "Give them another fixture type" : "Give it another fixture type"), typeHolder);
  if (typeOpen) typeOpen.addEventListener("toggle", async () => {
    if (!typeOpen.open || typeHolder.childNodes.length) return;
    let types = [];
    try { types = (await api("/api/fixtures")).fixtures.filter((row) => !row.error && (row.modes || []).length); }
    catch (error) { typeHolder.replaceChildren(notice(error.message, "error")); return; }
    if (!types.length) {
      typeHolder.replaceChildren(el("p", { class: "muted" }, "Your fixture library is empty. Fetch fixture types under Fixtures first, or drop GDTF files on this window."));
      return;
    }
    const type = el("select", { "aria-label": "Fixture type" }, types.map((row) => el("option", { value: row.file }, bare(row.file).split("@").slice(0, 2).join(" "))));
    const mode = el("select", { "aria-label": "Mode" });
    const fillModes = () => {
      const chosen = types.find((row) => row.file === type.value);
      mode.replaceChildren(el("option", { value: "" }, "one as long as the mode it has now"),
        ...(chosen ? chosen.modes : []).map((entry) => el("option", { value: entry.name }, `${entry.name} (${plural(entry.footprint, "slot")})`)));
    };
    type.addEventListener("change", fillModes);
    fillModes();
    const use = el("button", { class: "primary" }, "Use it");
    use.addEventListener("click", doing(use, () => send(Object.assign({ type: type.value }, mode.value ? { mode: mode.value } : {}),
      `${one ? title : plural(live.length, "fixture")} now ${one ? "has" : "have"} the type ${bare(type.value).split("@").slice(0, 2).join(" ")}.`)));
    typeHolder.replaceChildren(
      el("div", { class: "row" }, labelled("Type", type), labelled("Mode", mode), use),
      el("p", { class: "muted" }, `For ${one ? "this fixture" : "these fixtures"} alone, in place of what the rig file says; the rig file is not changed and an export carries the new type. ` +
        "For every fixture of a type, give the type a stand-in under Fixture types instead. The patch stays as it is: with a mode of another length, re-patch afterwards."));
  });
  const typeBack = !retyped.length ? null : el("button", { title: "The fixture type and mode the rig file gives" }, "Back to the rig file's type");
  if (typeBack) typeBack.addEventListener("click", doing(typeBack, () => send({ reset: "type" }, `${plural(retyped.length, "fixture")} back to the rig file's type.`, retyped)));

  // Hanging them on a truss of the show's set pieces: at its height, spread evenly along it, turned with it.
  const hangRow = el("div", { class: "row" });
  if (live.length) {
    api(`/api/show/pieces?name=${encodeURIComponent(detail.name)}`).then((answer) => {
      const trusses = answer.pieces.filter((piece) => piece.kind === "truss");
      if (!trusses.length) return;
      const pick = el("select", { "aria-label": "Truss" }, trusses.map((truss) => el("option", { value: truss.id }, `${truss.name} (${metres(truss.width)} m)`)));
      const hang = el("button", {}, "Hang on it");
      hang.addEventListener("click", doing(hang, () => {
        const truss = trusses.find((piece) => piece.id === pick.value);
        const angle = truss.turn * Math.PI / 180;
        const along = [Math.cos(angle), Math.sin(angle)];
        const spacing = truss.width / live.length;
        const first = -truss.width / 2 + spacing / 2;
        return send({ x: Math.round(truss.x + along[0] * first), y: Math.round(truss.y + along[1] * first), z: Math.round(truss.z),
                      stepx: Math.round(along[0] * spacing * 1000) / 1000, stepy: Math.round(along[1] * spacing * 1000) / 1000, rz: truss.turn },
          `${one ? title : plural(live.length, "fixture")} hung on ${truss.name}.`);
      }));
      hangRow.replaceChildren(el("span", { class: "muted lead" }, many ? "Hang them evenly along a truss of the set pieces:" : "Hang it in the middle of a truss of the set pieces:"), pick, hang);
    }).catch(() => null);
  }
  const loose = live.length && live.every((row) => row.notes.includes("unplaced"));
  return el("div", { class: "screen edit" },
    el("div", {}, el("b", {}, title), many ? el("span", { class: "muted" }, "  A field left empty leaves them as they are.") : null),
    !live.length ? el("p", { class: "muted" }, "Removed from the show. Put back, it can be changed again.") : [
      el("div", { class: "row" }, labelled("Name", name), many ? labelled("numbered from", numbered) : null, labelled(many ? "Number, counting up from" : "Number", id)),
      el("div", { class: "row" }, labelled("Universe", universe), labelled(many ? "Address, one after another from" : "Address", address), mode ? labelled("Mode", mode) : null),
      el("div", { class: "row" }, el("span", { class: "muted lead" }, many ? "Place of the first, metres" : "Place, metres"),
        labelled("x", at[0]), labelled("y", at[1]), labelled("z", at[2])),
      many ? el("div", { class: "row" }, el("span", { class: "muted lead" }, "Each next one further by, metres"), labelled("x", step[0]), labelled("y", step[1]), labelled("z", step[2])) : null,
      el("div", { class: "row" }, el("span", { class: "muted lead" }, "Move by, metres"), labelled("x", by[0]), labelled("y", by[1]), labelled("z", by[2]),
        el("span", { class: "muted lead" }, "Turned, degrees about"), labelled("X", turn[0]), labelled("Y", turn[1]), labelled("Z", turn[2])),
      hangRow,
      pan || tilt ? el("div", { class: "row" }, pan ? labelled("Its pan", pan) : null, tilt ? labelled("Its tilt", tilt) : null,
        el("span", { class: "muted" }, "For one fixture hung the other way round. If a whole fixture type runs the wrong way, set that under Fixture types.")) : null,
      loose ? el("p", { class: "muted" }, many
        ? "These have no place yet. Give the first its x, y and z and say how far further each next one goes (0.5 along x, say, for a row half a metre apart): they light up as soon as they are saved."
        : "It has no place yet. Give it its x, y and z: it lights up as soon as it is saved.") : null],
    el("div", { class: "row" }, live.length ? save : null, live.length ? half : null, back, typeBack, out, putBack, close),
    typeOpen,
    said);
}

// Adding fixtures of a type from the fixture library to a show. They are set down in front of
// the stage; where each hangs is then a matter for the Unreal Editor, like any other fixture.
function addFixturesForm(detail, said) {
  const holder = el("div");
  const open = el("details", { class: "how" }, el("summary", {}, "Add fixtures"), holder);
  open.addEventListener("toggle", async () => {
    if (!open.open || holder.childNodes.length) return;
    let types = [];
    try { types = (await api("/api/fixtures")).fixtures.filter((row) => !row.error && (row.modes || []).length); }
    catch (error) { holder.replaceChildren(notice(error.message, "error")); return; }
    if (!types.length) {
      holder.replaceChildren(el("p", { class: "muted" }, "Your fixture library is empty. Fetch fixture types under Fixtures first, or drop GDTF files on this window."));
      return;
    }
    const type = el("select", { "aria-label": "Fixture type" }, types.map((row) => el("option", { value: row.file }, bare(row.file).split("@").slice(0, 2).join(" "))));
    const mode = el("select", { "aria-label": "Mode" });
    const fillModes = () => {
      const chosen = types.find((row) => row.file === type.value);
      mode.replaceChildren(...chosen.modes.map((entry) => el("option", { value: entry.name }, `${entry.name} (${plural(entry.footprint, "slot")})`)));
    };
    type.addEventListener("change", fillModes);
    fillModes();
    const number = (label, value, min, max) => el("input", { type: "number", class: "narrow", "aria-label": label, value: String(value), min: String(min), max: String(max) });
    const count = number("How many", 1, 1, 512);
    const universe = number("Universe", 0, 0, 32767);
    const address = number("Address", 1, 1, 512);
    const firstId = number("First fixture ID", 0, 0, 999999);
    const add = el("button", { class: "primary", onclick: async () => {
      add.disabled = true;
      try {
        const answer = await api("/api/show/add", { json: { name: detail.name, fixture: type.value, mode: mode.value, count: Number(count.value),
          universe: Number(universe.value), address: Number(address.value), first_id: Number(firstId.value) } });
        // Said above the list of shows: this panel is drawn afresh when the list is.
        $("shows-notice").replaceChildren(notice(`${plural(answer.added.length, "fixture")} added to ${bare(detail.name)}. They stand in a row in front of the stage: open the show in the Unreal Editor to hang them.`, "ok"));
        loadShows();
      } catch (error) { said.replaceChildren(notice(error.message, "error")); add.disabled = false; }
    } }, "Add");
    holder.replaceChildren(
      el("div", { class: "row" }, el("label", {}, "Type ", type), el("label", {}, "Mode ", mode), el("label", {}, "How many ", count)),
      el("div", { class: "row" }, el("label", {}, "From universe ", universe), el("label", {}, "address ", address), el("label", {}, "First fixture ID ", firstId), add),
      el("p", { class: "muted" }, "Universe 0 leaves them unpatched, and fixture ID 0 leaves them without numbers. Each next fixture is patched straight after the one before; one that would run off the end of its universe starts the next. Removing a fixture you added takes it out for good; removing one of the rig file's can be undone with Put back."));
  });
  return open;
}

function showItem(row) {
  const facts = [];
  if (row.error) facts.push(el("span", { class: "bad" }, `Cannot be read: ${row.error}`));
  else if (row.unchecked) facts.push("Not checked: the core is not built");
  else {
    facts.push(plural(row.fixtures || 0, "fixture"), plural(row.types.length, "fixture type"));
    if (row.universes) facts.push(`universes up to ${row.universes}`);
    if (row.objects_with_shape) facts.push(plural(row.objects_with_shape, "set piece"));
  }
  facts.push(size(row.size));
  const meta = el("div", { class: "meta" });
  facts.forEach((fact, index) => meta.append(index ? " · " : "", fact));

  const warnings = [];
  for (const missing of row.missing || []) {
    warnings.push(`${plural(missing.count, "fixture")} use ${missing.name}, which is not inside the file, so they will be left out.`);
  }
  if (row.without_type) warnings.push(`${plural(row.without_type, "fixture")} name no fixture type and will be left out.`);
  if (row.unpatched) warnings.push(`${plural(row.unpatched, "fixture")} have no DMX address.`);
  const edited = !row.edited ? null : el("div", { class: "meta" },
    `${plural(row.edited, "fixture")} changed, added or removed since the rig file. `,
    el("button", { class: "link", onclick: async (event) => {
      event.currentTarget.disabled = true;
      try {
        const answer = await api("/api/edits/discard", { json: { name: row.name } });
        $("shows-notice").replaceChildren(answer.discarded ? notice(`${bare(row.name)} is back to how its MVR file describes it. The changes were set aside as ${answer.kept_as} in the shows folder.`, "ok") : "");
      } catch (error) { $("shows-notice").replaceChildren(notice(error.message, "error")); }
      loadShows();
    } }, "Discard changes"),
    " · ",
    el("button", { class: "link", onclick: async (event) => {
      event.currentTarget.disabled = true;
      try {
        const answer = await api("/api/export", { json: { name: row.name } });
        const pieces = answer.pieces ? ` and ${plural(answer.pieces, "set piece")}` : "";
        $("shows-notice").replaceChildren(notice(`Exported as ${answer.file}, with the changes to ${plural(answer.changed, "fixture")}${pieces} written in. `, "ok",
          el("button", { class: "link", onclick: () => api("/api/reveal", { json: { what: "exports" } }) }, showInFolder())));
      } catch (error) { $("shows-notice").replaceChildren(notice(error.message, "error")); }
      event.currentTarget.disabled = false;
    } }, "Export as MVR"),
    " · ",
    el("button", { class: "link", title: "One file with the show and everything it needs on another computer: its changes, the fixture types it uses, its screens' pictures and films", onclick: async (event) => {
      event.currentTarget.disabled = true;
      try {
        const answer = await api("/api/package", { json: { name: row.name } });
        const extra = [plural(answer.fixtures, "fixture type"), answer.media ? `${answer.media} ${answer.media === 1 ? "picture or film" : "pictures and films"}` : ""].filter(Boolean).join(" and ");
        $("shows-notice").replaceChildren(notice(`Packed as ${answer.file} (${size(answer.size)}), with ${extra}. Drop it on Openviz on another computer to open the show there as it is here. ` +
          (answer.missing.length ? `Not in your library, so not packed: ${answer.missing.join(", ")}. ` : ""), answer.missing.length ? "" : "ok",
          el("button", { class: "link", onclick: () => api("/api/reveal", { json: { what: "exports" } }) }, showInFolder())));
      } catch (error) { $("shows-notice").replaceChildren(notice(error.message, "error")); }
      event.currentTarget.disabled = false;
    } }, "Save as package"));
  // With the exchange on, a show can be offered to the group's stations, changed here or not.
  let sending = null;
  if (xchange && xchange.running && !row.error && !row.unchecked) {
    sending = el("div", { class: "meta" }, el("button", { class: "link", title: "Offers this show, with what was changed here written in, to the stations of the group", onclick: async (event) => {
      event.currentTarget.disabled = true;
      try {
        const answer = await api("/api/xchange/send", { json: { name: row.name } });
        $("shows-notice").replaceChildren(notice(answer.told.length ? `${bare(row.name)} was offered to ${answer.told.join(", ")}. It is theirs to take in.`
          : `${bare(row.name)} is on offer, but no other station is in the group right now. Whoever joins will be told.`, answer.told.length ? "ok" : ""));
      } catch (error) { $("shows-notice").replaceChildren(notice(error.message, "error")); }
      event.currentTarget.disabled = false;
    } }, `Send to the group ${xchange.group}`));
  }

  const blocked = ready() || (row.error ? "This file cannot be opened." : "");
  const holder = el("div");
  const patchButton = el("button", { "aria-expanded": "false", disabled: !!row.error || !!row.unchecked }, "Patch");
  const fold = async (open) => {
    patchButton.setAttribute("aria-expanded", String(open));
    if (!open) { openPatches.delete(row.name); holder.replaceChildren(); return; }
    openPatches.add(row.name);
    holder.replaceChildren(el("div", { class: "detail muted" }, "Reading the patch…"));
    try { holder.replaceChildren(patchPanel(await gather(api(`/api/show?name=${encodeURIComponent(row.name)}`)))); drawn(); }
    catch (error) { holder.replaceChildren(notice(error.message, "error")); }
  };
  patchButton.addEventListener("click", () => fold(!openPatches.has(row.name)));
  if (openPatches.has(row.name)) fold(true);

  // Layers and classes: which of them to leave out when the show is opened.
  const groups = (row.layer_list || []).length + (row.class_list || []).length;
  const hiddenCount = row.hidden ? row.hidden.layers.length + row.hidden.classes.length : 0;
  const layersHolder = el("div");
  const layersButton = el("button", { "aria-expanded": "false", disabled: !groups }, hiddenCount ? `Layers (${hiddenCount} hidden)` : "Layers");
  const foldLayers = (open) => {
    layersButton.setAttribute("aria-expanded", String(open));
    if (!open) { openLayers.delete(row.name); layersHolder.replaceChildren(); return; }
    openLayers.add(row.name);
    layersHolder.replaceChildren(layersPanel(row));
  };
  layersButton.addEventListener("click", () => foldLayers(!openLayers.has(row.name)));
  if (openLayers.has(row.name)) foldLayers(true);

  // Screens: what the show's video screens and LED walls show, and screens of one's own.
  const screensHolder = el("div");
  const screensButton = el("button", { "aria-expanded": "false", disabled: !!row.error || !!row.unchecked }, "Screens");
  const foldScreens = (open) => {
    screensButton.setAttribute("aria-expanded", String(open));
    if (!open) { openScreens.delete(row.name); screensHolder.replaceChildren(); return; }
    openScreens.add(row.name);
    screensHolder.replaceChildren(screensPanel(row.name));
  };
  screensButton.addEventListener("click", () => foldScreens(!openScreens.has(row.name)));
  if (openScreens.has(row.name)) foldScreens(true);

  // Set pieces: decks, drapes, trusses and figures added here, for a rig file that brought no stage.
  const piecesHolder = el("div");
  const piecesButton = el("button", { "aria-expanded": "false", disabled: !!row.error }, row.pieces ? `Set pieces (${row.pieces})` : "Set pieces");
  const foldPieces = (open) => {
    piecesButton.setAttribute("aria-expanded", String(open));
    if (!open) { openPieces.delete(row.name); piecesHolder.replaceChildren(); return; }
    openPieces.add(row.name);
    piecesHolder.replaceChildren(piecesPanel(row.name));
  };
  piecesButton.addEventListener("click", () => foldPieces(!openPieces.has(row.name)));
  if (openPieces.has(row.name)) foldPieces(true);

  // Fixture types: which of them the rig file says next to nothing about, and the real fixture files that stand in for them.
  const typesHolder = el("div");
  const placeholders = row.placeholders || [];
  const typesButton = el("button", { "aria-expanded": "false", disabled: !!row.error || !!row.unchecked }, placeholders.length ? `Fixture types (${placeholders.length} to do)` : "Fixture types");
  const foldTypes = (open) => {
    typesButton.setAttribute("aria-expanded", String(open));
    if (!open) { openTypes.delete(row.name); delete typesKept[row.name]; typesHolder.replaceChildren(); return; }
    openTypes.add(row.name);
    typesHolder.replaceChildren(typesPanel(row.name));
  };
  typesButton.addEventListener("click", () => foldTypes(!openTypes.has(row.name)));
  if (openTypes.has(row.name)) foldTypes(true);
  if (row.unplaced) {
    warnings.push([`${plural(row.unplaced, "fixture")} ${row.unplaced === 1 ? "has" : "have"} no place in the rig file and ${row.unplaced === 1 ? "is" : "are"} left dark: all in one spot, they would only blind the picture and slow it down. `,
      el("button", { class: "link", onclick: () => { (patchKept[row.name] || (patchKept[row.name] = { filter: "", only: false, ticked: new Set(), note: null })).filter = "no place"; fold(true); } }, "Give them places")]);
  }
  if (placeholders.length) {
    const lamps = placeholders.reduce((sum, entry) => sum + entry.count, 0);
    warnings.push([`${plural(placeholders.length, "fixture type")} (${plural(lamps, "fixture")}) came from the console as placeholders: the file gives their channel count and their dimmer and nothing else, so they will only dim, without colour, gobos, pan or tilt. `,
      el("button", { class: "link", onclick: () => foldTypes(true) }, "Give them real fixture files")]);
  }

  return el("div", {},
    el("div", { class: "item" },
      el("div", { class: "body" }, el("div", { class: "title" }, bare(row.name)), meta, edited, undoLine(row), sending, warnings.map((text) => el("div", { class: "warn" }, text))),
      el("div", { class: "actions" }, patchButton, layersButton, screensButton, piecesButton, typesButton,
        el("button", { title: blocked, disabled: !!blocked, onclick: (event) => launch("show", row.name, { editor: true }, event.currentTarget) }, "Edit in Unreal Editor"),
        el("button", { class: "primary", title: blocked, disabled: !!blocked, onclick: (event) => launch("show", row.name, {}, event.currentTarget) }, "Watch live"))),
    holder, layersHolder, screensHolder, piecesHolder, typesHolder);
}

// A show's set pieces: a rig file from a console often has no stage at all, and a rig that hangs
// in the void is hard to read. Decks, drapes, trusses, boxes and figures are added here, by
// numbers; they take light and stop beams like the rig file's own scenery, and a show that is open
// shows every change at once.
function piecesPanel(name) {
  const panel = el("div", { class: "detail" }, el("div", { class: "muted" }, "Reading the set pieces\u2026"));
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
  // Which of its measures a kind has, and what each is called.
  const MEASURES = {
    deck: [["width", "wide"], ["depth", "deep"], ["height", "high"]],
    drape: [["width", "wide"], ["height", "high"]],
    truss: [["width", "long"], ["depth", "its side"]],
    box: [["width", "wide"], ["depth", "deep"], ["height", "high"]],
    person: [["height", "tall"]],
    circle: [["width", "across"], ["depth", "its side"]],
    corner: [["width", "each arm"], ["depth", "its side"]],
    model: [],
  };
  const UNITS = [["", "units as its file has them"], ["mm", "millimetres"], ["cm", "centimetres"], ["m", "metres"], ["in", "inches"], ["ft", "feet"]];
  const UPS = [["", "upright as its file has it"], ["y", "its Y axis up"], ["z", "its Z axis up"]];
  const draw = (answer) => {
    const act = async (path, body) => {
      try { draw(await api(path, { json: { name, ...body } })); said.replaceChildren(); loadShows(); }
      catch (error) { said.replaceChildren(notice(error.message, "error")); }
    };
    const line = (piece) => {
      // A field that is changed is saved by itself, and the list is left as it is: whoever tabs on to the next field stays in it.
      const send = async (changes, field) => {
        try { await api("/api/piece/set", { json: { name, id: piece.id, changes } }); Object.assign(piece, changes); said.replaceChildren(); }
        catch (error) { said.replaceChildren(notice(error.message, "error")); if (field) field.focus(); }
      };
      const length = (key, label) => {
        const field = el("input", { type: "text", class: "narrow", value: metres(piece[key]), "aria-label": `${piece.name}: ${label}, metres`, autocomplete: "off" });
        field.addEventListener("change", () => {
          const value = Number(field.value.trim().replace(",", "."));
          if (!field.value.trim() || !Number.isFinite(value)) { said.replaceChildren(notice("That is a number of metres.", "error")); return; }
          send({ [key]: Math.round(value * 1000) }, field);
        });
        return el("label", {}, label, field);
      };
      const title = el("input", { type: "text", value: piece.name, "aria-label": "Name", autocomplete: "off" });
      title.addEventListener("change", () => send({ name: title.value.trim() }, title));
      const turn = el("input", { type: "text", class: "narrow", value: degrees(piece.turn), "aria-label": `${piece.name}: turned, degrees`, autocomplete: "off" });
      turn.addEventListener("change", () => {
        const value = Number(turn.value.trim().replace(",", "."));
        if (!turn.value.trim() || !Number.isFinite(value)) { said.replaceChildren(notice("A turn is a number of degrees.", "error")); return; }
        send({ turn: value }, turn);
      });
      const colour = el("input", { type: "color", value: toHex(piece.colour), "aria-label": `${piece.name}: colour` });
      colour.addEventListener("change", () => send({ colour: fromHex(colour.value) }));
      const kind = answer.kinds.find((entry) => entry.kind === piece.kind);
      // A model is its file, as big as it is: which file, what its numbers are and which way is up.
      const choose = (key, options, label) => {
        const field = el("select", { "aria-label": `${piece.name}: ${label}` }, options.map(([value, text]) => el("option", { value, selected: piece[key] === value }, text)));
        field.addEventListener("change", () => send({ [key]: field.value }, field));
        return field;
      };
      // A drape, box or deck can wear a picture on its side toward the house.
      const pictureField = !["drape", "box", "deck"].includes(piece.kind) ? null : el("span", {},
        el("button", { class: "small", title: piece.picture || "", onclick: async () => {
          try {
            const chosen = await api("/api/screen/choose", { json: { kind: "picture" } });
            if (chosen.path) { await send({ picture: chosen.path }); draw(await api(`/api/show/pieces?name=${encodeURIComponent(name)}`)); }
          } catch (error) { said.replaceChildren(notice(error.message, "error")); }
        } }, piece.picture ? `picture: ${piece.picture.split(/[\\/]/).pop()}` : "a picture\u2026"),
        piece.picture ? el("button", { class: "link small", onclick: async () => { await send({ picture: "" }); draw(await api(`/api/show/pieces?name=${encodeURIComponent(name)}`)); } }, "none") : null);
      const modelFields = piece.kind !== "model" ? [] : [
        choose("file", [...new Set([piece.file, ...answer.models])].map((file) => [file, file]), "model file"),
        choose("units", UNITS, "units"), choose("up", UPS, "which way up")];
      return el("div", { class: "screen" },
        el("div", { class: "row" }, el("label", {}, kind ? kind.label : piece.kind, title), modelFields,
          MEASURES[piece.kind].map(([key, label]) => length(key, label)),
          el("span", { class: "muted lead" }, "at"), length("x", "x"), length("y", "y"), length("z", "z"),
          el("label", {}, "turned", turn), piece.kind === "model" ? null : el("label", {}, "colour", colour), pictureField,
          el("button", { title: "Another like it, set down beside it", onclick: () => act("/api/piece/remove", { id: piece.id, copy: true }) }, "Copy"),
          el("button", { class: "link", onclick: () => act("/api/piece/remove", { id: piece.id }) }, "Remove")),
        piece.trouble ? el("div", { class: "warn" }, piece.trouble) : null);
    };
    // Adding a model: one of the model files there are, or a file chosen now.
    const modelPick = el("select", { "aria-label": "Model file" }, answer.models.map((file) => el("option", { value: file }, file)));
    const addModel = el("span", { class: "row" },
      answer.models.length ? [modelPick, el("button", { onclick: () => act("/api/piece/add", { kind: "model", changes: { file: modelPick.value } }) }, "Add this model")] : null,
      el("button", { onclick: async () => {
        try {
          const chosen = await api("/api/model/choose", { json: {} });
          if (!chosen.name) return;
          if (chosen.warnings && chosen.warnings.length) said.replaceChildren(notice(chosen.warnings.join(" "), ""));
          await act("/api/piece/add", { kind: "model", changes: { file: chosen.name } });
        } catch (error) { said.replaceChildren(notice(error.message, "error")); }
      } }, "Choose a model file\u2026"));
    put(panel,
      el("p", { class: "muted" }, answer.pieces.length
        ? "What stands on stage besides the rig. Sizes and places are in metres; a place is the middle of the piece at its foot: x toward stage left (the audience's right), y upstage, z up."
        : "A rig file from a console often brings fixtures and nothing for them to hang on or shine at. Add a stage here: decks and risers, drapes, lengths of truss, boxes for whatever else, and figures to give it scale."),
      el("div", { class: "row" }, el("span", { class: "muted" }, "Add:"),
        answer.kinds.filter((kind) => kind.kind !== "model").map((kind) => el("button", { onclick: () => act("/api/piece/add", { kind: kind.kind }) }, kind.label))),
      el("div", { class: "row" }, el("span", { class: "muted" }, "Or a model, a venue or a set from a drawing program:"), addModel),
      answer.pieces.map(line), said,
      el("p", { class: "muted" }, "A truss lies along x until it is turned, and z is the height of its underside, where fixtures hang: give it the z of the fixtures on it. " +
        "A truss circle lies flat round its middle; a truss corner's arms run along x and y from the corner it stands on. " +
        "A drape faces the house until it is turned, and can wear a picture there, as a printed backdrop does (a box or a deck too, on its front). A model is as big as its file says (GLB in metres, 3DS in millimetres, OBJ as written): if it comes out a hundred times too big or lying on its back, say what its numbers are and which way is up. " +
        "GLB brings its colours and pictures inside it; an OBJ needs its MTL file and pictures beside it, which \"Choose a model file\" brings along. " +
        "Beams land on set pieces and are cut by them. A show that is open shows every change at once. Set pieces are on the light plot and go into an MVR export."));
  };
  gather(api(`/api/show/pieces?name=${encodeURIComponent(name)}`)).then(draw).catch((error) => panel.replaceChildren(notice(error.message, "error")));
  return panel;
}

// A show's fixture types. A console does not always say what its fixtures can do: grandMA3 sends
// every type of its own library as a placeholder. Here each can be given a real fixture file,
// from the library or from GDTF Share, matched to the modes in use by their length.
function typesPanel(name) {
  const panel = el("div", { class: "detail" }, el("div", { class: "muted" }, "Reading the fixture types\u2026"));
  const said = el("div");
  // Kept outside the panel: a change redraws the list of shows, and this panel with it.
  const kept = typesKept[name] || (typesKept[name] = { found: {}, notes: {} });
  const found = kept.found;    // what GDTF Share has for a type, once looked for
  const notes = kept.notes;    // what there is to say under a type: what was done, or why it could not be
  let asking = null;           // the fetch that waits for a GDTF Share login: { spec, row }
  const slots = (count) => `${count} ${count === 1 ? "slot" : "slots"}`;
  const fixtureName = (row) => [row.manufacturer, row.name || row.fixture].filter(Boolean).join(" \u2014 ");
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
    // Fetches a fixture from GDTF Share and puts it in the type's place; GDTF Share wants a login for that.
    const fetchFor = async (entry, row) => {
      try {
        const next = await api("/api/types/share", { json: { name, type: entry.spec, rid: row.rid } });
        asking = null;
        notes[entry.spec] = notice(`${fixtureName(row)} was fetched and now stands in for ${entry.name}.`, "ok");
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
    const loginFor = (entry) => {
      const user = el("input", { type: "text", autocomplete: "username", placeholder: "GDTF Share user name", "aria-label": "GDTF Share user name" });
      const password = el("input", { type: "password", autocomplete: "current-password", placeholder: "Password", "aria-label": "GDTF Share password" });
      const remember = el("input", { type: "checkbox" });
      const wrong = el("div");
      const form = el("form", { class: "row" }, user, password, el("label", { class: "inline" }, remember, ` Stay logged in on ${thisComputer()}`),
        el("button", { class: "primary", type: "submit" }, "Log in and get it"),
        el("button", { class: "link", type: "button", onclick: () => { asking = null; draw(answer); } }, "Not now"));
      form.addEventListener("submit", async (event) => {
        event.preventDefault();
        const button = form.querySelector("button");
        button.disabled = true;
        try {
          state.share = await api("/api/share/login", { json: { user: user.value, password: password.value, remember: remember.checked } });
          password.value = "";
          await fetchFor(entry, asking.row);
        } catch (error) {
          password.value = "";
          wrong.replaceChildren(notice(error.message, "error"));
          button.disabled = false;
        }
      });
      return el("div", { class: "section" },
        el("div", {}, `To fetch ${fixtureName(asking.row)}, GDTF Share wants your login. It is free; the account is made on gdtf-share.com.`), form, wrong);
    };
    const todo = answer.types.filter((entry) => entry.bare && entry.standin_state !== "ok");
    const ready = todo.filter((entry) => entry.library.some((row) => row.fits && row.score >= 0.75));
    // Every fixture of a type taken out of the show, or put back: for what the visualizer can do without.
    const leaveOut = (entry) => el("button", { title: "Takes every fixture of this type out of the show here: not drawn, not lit, not patched. The rig file keeps them, and Put back brings them back.",
      onclick: () => act("/api/types/remove", { type: entry.spec }, (next) => `${entry.name}: ${plural(next.count, "fixture")} taken out of the show.`, entry.spec) },
      `Leave ${entry.fixtures === 1 ? "it" : `all ${entry.fixtures}`} out of the show`);
    const putBack = (entry) => el("button", { class: "link",
      onclick: () => act("/api/types/remove", { type: entry.spec, restore: true }, (next) => `${entry.name}: ${plural(next.count, "fixture")} back in the show.`, entry.spec) },
      "Put them back");
    const block = (entry) => {
      if (!entry.fixtures) {
        return el("div", { class: "screen" }, el("div", {}, el("b", {}, entry.name), el("span", { class: "muted" }, `  all ${plural(entry.removed, "fixture")} taken out of the show  `), putBack(entry)),
          notes[entry.spec] || null);
      }
      const inUse = entry.modes.map((mode) => `${mode.name} (${slots(mode.slots)}${entry.modes.length > 1 ? `, ${plural(mode.fixtures, "fixture")}` : ""})`).join(", ");
      const head = el("div", {}, el("b", {}, entry.name), el("span", { class: "muted" }, `  ${plural(entry.fixtures, "fixture")}, in the mode ${inUse}`),
        entry.removed ? el("span", { class: "muted" }, ` (and ${entry.removed} taken out of the show: `, putBack(entry), ")") : null);
      const lines = [head];
      if (entry.standin && entry.standin_state === "ok") {
        const map = entry.modes.map((mode) => mode.standin ? `${mode.name} is its ${mode.standin}` : `${mode.name} has no mode of its own there`).join("; ");
        lines.push(el("div", { class: "good" }, `Read from ${bare(entry.standin)}. `, el("span", { class: "muted" }, `${map}. `),
          el("button", { class: "link", onclick: () => act("/api/types/clear", { type: entry.spec }, null, entry.spec) }, "Use the file's own again")));
        // The one thing a placeholder says is where its dimmer is: a stand-in that has it elsewhere is in another order.
        const astray = entry.modes.filter((mode) => mode.dimmer === false).map((mode) => mode.name);
        if (astray.length) lines.push(el("div", { class: "bad" }, `Its dimmer is not on the slot where the console has it (${astray.join(", ")}): the channels of this file are not in the console's order, and the picture will be wrong. Another revision of the fixture, or another file, may fit.`));
        else if (entry.modes.some((mode) => mode.dimmer === true)) lines.push(el("div", { class: "muted" }, "Its dimmer is on the slot where the console has it."));
      } else {
        if (entry.standin) lines.push(el("div", { class: "bad" }, `${bare(entry.standin)} was to stand in for it, but is no longer in your fixture library.`));
        const worst = entry.modes.reduce((most, mode) => mode.silent > most.silent ? mode : most, entry.modes[0] || { silent: 0, channels: 0 });
        lines.push(entry.bare ? el("div", { class: "warn" }, `A placeholder: the file says nothing about ${worst.silent} of its ${plural(worst.channels, "channel")}, ${worst.silent < worst.channels ? "so it only dims" : "so nothing the console sends reaches it"}.`)
          : el("div", { class: "muted" }, "The file describes it."));
      }
      if (entry.bare || entry.standin) {
        const fits = (row) => row.fits ? "fits" : `has no mode of ${entry.needs.map(slots).join(" and ")}`;
        // From the library: what is called alike.
        for (const row of entry.library.filter((row) => row.file !== entry.standin || entry.standin_state !== "ok")) {
          lines.push(el("div", { class: "row" }, el("span", {}, fixtureName(row)), el("span", { class: "muted" }, `in your library, ${fits(row)}`),
            el("button", { disabled: !row.fits, onclick: () => act("/api/types/set", { type: entry.spec, file: row.file }, () => `${fixtureName(row)} now stands in for ${entry.name}.`, entry.spec) }, "Use it")));
        }
        // From GDTF Share: looked for when asked.
        const share = found[entry.spec];
        const other = el("select", { "aria-label": `Another file of the library for ${entry.name}` },
          el("option", { value: "" }, "another file of your library\u2026"), answer.files.map((row) => el("option", { value: row.file }, `${fixtureName(row)} (${bare(row.file)})`)));
        other.addEventListener("change", () => { if (other.value) act("/api/types/set", { type: entry.spec, file: other.value }, () => `${bare(other.value)} now stands in for ${entry.name}.`, entry.spec); });
        lines.push(el("div", { class: "row" },
          el("button", { onclick: async (event) => {
            event.currentTarget.disabled = true;
            try { found[entry.spec] = await api(`/api/types/find?name=${encodeURIComponent(name)}&type=${encodeURIComponent(entry.spec)}`); notes[entry.spec] = null; }
            catch (error) { notes[entry.spec] = notice(error.code === "login_required" ? "Openviz has no list of GDTF Share's fixtures yet. Log in once on the Fixtures page and it is fetched." : error.message, "error"); }
            draw(answer);
          } }, share ? "Look on GDTF Share again" : "Look on GDTF Share"), other));
        if (share) {
          if (!share.rows.length) lines.push(el("div", { class: "muted" }, "GDTF Share has nothing of that name. Search the Fixtures page under another name, or ask the maker for a GDTF file and drop it on Openviz."));
          // Those that fit, and two of the others so that a near miss can be seen for what it is.
          const offered = share.rows.filter((row) => row.fits).concat(share.rows.filter((row) => !row.fits).slice(0, 2));
          for (const row of offered) {
            lines.push(el("div", { class: "row" }, el("span", {}, fixtureName(row)),
              el("span", { class: "muted" }, `GDTF Share${row.by_manufacturer ? ", from the maker" : ""}, ${row.revision ? `${row.revision}, ` : ""}${fits(row)}${row.have ? ", in your library already" : ""}`),
              el("button", { disabled: !row.fits, onclick: (event) => {
                const button = event.currentTarget;
                button.disabled = true;
                button.textContent = row.have ? "Putting it in place\u2026" : "Fetching it\u2026";
                fetchFor(entry, row);
              } }, row.have ? "Use it" : "Get it and use it")));
          }
        }
      }
      // Which way its pan and tilt run, and from where: for every fixture of the type.
      if (entry.has_pan || entry.has_tilt || entry.invert_pan || entry.invert_tilt || entry.pan_offset || entry.tilt_offset) {
        const send = (change, done) => act("/api/types/moves", Object.assign({ type: entry.spec, invert_pan: entry.invert_pan, invert_tilt: entry.invert_tilt,
          pan_offset: entry.pan_offset, tilt_offset: entry.tilt_offset }, change), () => done, entry.spec);
        const other = (key, axis) => {
          const tick = el("input", { type: "checkbox", checked: entry[key] });
          tick.addEventListener("change", () => send({ [key]: tick.checked }, `${entry.name}: ${axis} now runs ${tick.checked ? "the other way" : "as the fixture file has it"}.`));
          return el("label", { class: "inline" }, tick, ` ${axis} runs the other way`);
        };
        const turned = (key, axis) => {
          const field = el("input", { type: "text", class: "narrow", value: degrees(entry[key]), "aria-label": `${axis} turned by, degrees`, autocomplete: "off" });
          field.addEventListener("change", () => {
            const value = Number(field.value.trim().replace(",", "."));
            if (!field.value.trim() || !Number.isFinite(value) || value < -360 || value > 360) { notes[entry.spec] = notice("A turn is a number of degrees between -360 and 360.", "error"); draw(answer); return; }
            send({ [key]: value }, `${entry.name}: ${axis} is now turned by ${degrees(value)} degrees.`);
          });
          return el("label", { class: "inline" }, `${axis} turned by `, field, "\u00b0");
        };
        lines.push(el("div", { class: "row moves" }, el("span", { class: "muted" }, "Pan and tilt:"),
          entry.has_pan || entry.invert_pan ? other("invert_pan", "pan") : null, entry.has_tilt || entry.invert_tilt ? other("invert_tilt", "tilt") : null,
          entry.has_pan || entry.pan_offset ? turned("pan_offset", "pan") : null, entry.has_tilt || entry.tilt_offset ? turned("tilt_offset", "tilt") : null));
      }
      lines.push(el("div", { class: "row" }, leaveOut(entry),
        el("span", { class: "muted" }, entry.bare ? "if the visualizer can do without them: dimmer channels, house lights, media server layers." : "if the visualizer can do without them.")));
      // What came of the last click on this type, and the login a fetch may be waiting for: here, where the click was.
      if (asking && asking.spec === entry.spec) lines.push(loginFor(entry));
      if (notes[entry.spec]) lines.push(notes[entry.spec]);
      return el("div", { class: "screen" }, lines);
    };
    const gone = answer.types.filter((entry) => !entry.fixtures);
    const plain = answer.types.filter((entry) => entry.fixtures && !entry.bare && !entry.standin);
    const rest = answer.types.filter((entry) => entry.fixtures && !plain.includes(entry));
    put(panel,
      el("p", { class: "muted" }, todo.length
        ? "A rig file says which fixtures hang where and how they are patched. What each fixture can do, it leaves to a fixture file packed with it, and grandMA3 packs only a placeholder for every fixture of its own library. Give those a real fixture file (the maker's GDTF) and Openviz reads colour, gobos, pan, tilt and the rest from that, by the mode that is as long as the one the console uses. The rig file is not changed."
        : rest.length ? "Every placeholder of this show has a real fixture file standing in for it." : "The rig file describes every fixture type of this show itself."),
      ready.length ? el("div", { class: "row" }, el("button", { class: "primary", onclick: () => act("/api/types/all", {}, (next) =>
        `${next.done.map((entry) => entry.type).join(", ")}: read from your library now.${next.left.length ? ` Still placeholders: ${next.left.join(", ")}.` : ""}`) },
        `Use what fits from your library (${ready.length})`)) : null,
      rest.map(block), said,
      plain.length ? el("details", { class: "how", open: true }, el("summary", {}, `${plural(plain.length, "fixture type")} the rig file describes itself`), plain.map(block)) : null,
      gone.length ? el("details", { class: "how", open: true }, el("summary", {}, `${plural(gone.length, "fixture type")} taken out of the show`), gone.map(block)) : null,
      el("p", { class: "muted" }, "A file fits if it has a mode exactly as long as each mode in use here; whether its channels are in the console's order is for you to check on the first look. " +
        "Pan and tilt: a fixture file and a console's own profile do not always agree which side of a moving head is its front, and then tilt runs opposite ways in the two. " +
        "If a type's lights point backwards, tick \u201ctilt runs the other way\u201d; if they then swing left where the console says right, tick pan as well; if they are a fixed angle off, turn them by it. " +
        "A show that is open takes all of this up at once, so it can be tried while watching."));
  };
  gather(api(`/api/show/types?name=${encodeURIComponent(name)}`)).then(draw).catch((error) => panel.replaceChildren(notice(error.message, "error")));
  return panel;
}

// A show's screens: the video screens its rig file has, and screens added here. Each shows a test
// pattern, a picture, a film or a live NDI stream. What is chosen is kept beside the show, and a
// show that is open takes it up at once.
function screensPanel(name) {
  const panel = el("div", { class: "detail" }, el("div", { class: "muted" }, "Looking at the screens\u2026"));
  const said = el("div");
  let streams = [];    // the NDI streams found when last looked for
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
        node.textContent = now ? `In the visualizer: ${now.text}` : "";
        node.className = now && now.note ? "bad" : "muted";
      }
      Object.assign(word, fresh);
    } catch (error) { /* the next look will say */ }
  };

  const metres = (mm) => (mm / 1000).toFixed(2);
  const draw = (answer) => {
    const change = async (id, changes) => {
      try { draw(await api("/api/screen/set", { json: { name, id, changes } })); said.replaceChildren(); }
      catch (error) { said.replaceChildren(notice(error.message, "error")); }
    };
    // The controls for what a screen shows, or one section of a split screen: `target` is the screen or the
    // section, `label` what it is called, `send` takes the changes, and `size` is its size in millimetres.
    const content = (screen, target, label, send, size) => {
      // Pixels lit over DMX start out a hand's width apart, from universe 1 on: the numbers are there to be changed.
      const firstPixels = `1.1 ${Math.max(1, Math.min(1024, Math.round(size[0] / 100)))}x${Math.max(1, Math.min(1024, Math.round(size[1] / 100)))}`;
      const kinds = [["pattern", "Test pattern"], ["picture", "Picture"], ["film", "Film"], ["ndi", "NDI stream"], ["pixels", "DMX pixels"], ["off", "Off"]];
      if (screen.replaces.length) kinds.unshift(["fixtures", "The fixtures, driven by DMX as patched"]);
      const source = el("select", { "aria-label": `What ${label} shows`, onchange: (event) =>
        send(event.target.value === "pixels" ? { source: "pixels", value: firstPixels } : { source: event.target.value, value: "" }) },
        kinds.map(([key, text]) => el("option", { value: key, selected: key === target.source }, text)));
      const controls = [el("label", {}, "Shows ", source)];
      if (target.source === "picture" || target.source === "film") {
        const what = target.source;
        const path = el("input", { type: "text", class: "path", value: target.value, placeholder: `The ${what}'s file, with its whole path`, "aria-label": `File for ${label}`,
          onchange: (event) => send({ value: event.target.value }) });
        controls.push(path, el("button", { onclick: async (event) => {
          const button = event.currentTarget;
          button.disabled = true;
          try {
            const chosen = await api("/api/screen/choose", { json: { kind: what } });
            if (chosen.path) await send({ value: chosen.path });
          } catch (error) { said.replaceChildren(notice(error.message, "error")); }
          button.disabled = false;
        } }, "Choose…"));
      } else if (target.source === "ndi") {
        const listId = `ndi-${screen.id}-${label.length}`;
        const list = el("datalist", { id: listId }, streams.map((stream) => el("option", { value: stream })));
        const field = el("input", { type: "text", class: "path", value: target.value, list: listId, placeholder: "COMPUTER (Stream), as NDI lists it", "aria-label": `NDI stream for ${label}`,
          onchange: (event) => send({ value: event.target.value }) });
        controls.push(field, list, el("button", { onclick: async (event) => {
          const button = event.currentTarget;
          button.disabled = true;
          try {
            streams = (await api("/api/ndi")).sources;
            list.replaceChildren(...streams.map((stream) => el("option", { value: stream })));
            said.replaceChildren(notice(streams.length ? `Streams on the network now: ${streams.join(", ")}.` : "No NDI stream is being sent on the network right now.", streams.length ? "ok" : "error",
              ...streams.map((stream) => el("button", { class: "link", onclick: () => send({ value: stream }) }, ` Use ${stream} for ${label}`))));
          } catch (error) { said.replaceChildren(notice(error.message, "error")); }
          button.disabled = false;
        } }, "Look for streams"));
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
        controls.push(
          el("label", { class: "inline" }, "from universe ", count("universe", "Universe", 32767)),
          el("label", { class: "inline" }, "address ", count("address", "Address", 512)),
          el("label", { class: "inline" }, count("across", "Pixels across", 1024), " × ", count("down", "Pixels down", 1024), " pixels"),
          el("label", { class: "inline" }, "each ", pick("order", "Colour order", ["RGB", "GRB", "BGR", "RBG", "GBR", "BRG", "RGBW", "GRBW"].map((order) => [order, order]))),
          el("label", { class: "inline" }, "wired ", pick("wiring", "Wiring", [["rows", "row by row, each left to right"], ["snake", "row by row, as a snake"],
            ["columns", "column by column, each top to bottom"], ["snake-columns", "column by column, as a snake"]])));
      }
      if (["picture", "film", "ndi"].includes(target.source)) {
        // Which part of the picture: all of it, or a part, so that one picture can be spread over several screens or sections.
        const same = (one, other) => one.every((edge, index) => Math.abs(edge - other[index]) < 0.0005);
        const parts = [["whole", "all of the picture", [0, 0, 1, 1]]];
        if (target.own) parts.unshift(["own", "its own part of the screen's picture", target.own]);
        parts.push(["left", "the picture's left half", [0, 0, 0.5, 1]], ["right", "its right half", [0.5, 0, 1, 1]], ["top", "its top half", [0, 0, 1, 0.5]], ["bottom", "its bottom half", [0, 0.5, 1, 1]]);
        const now = parts.find((entry) => same(entry[2], target.part));
        const percent = (edge) => `${+(edge * 100).toFixed(1)}%`;
        controls.push(el("label", { class: "inline" }, "of it ", el("select", { "aria-label": `Which part of its picture ${label} shows`, onchange: (event) => {
          const chosen = parts.find((entry) => entry[0] === event.target.value);
          if (chosen) send({ part: chosen[0] === "own" || chosen[0] === "whole" ? (target.own ? chosen[0] : chosen[2]) : chosen[2] });
        } }, parts.map((entry) => el("option", { value: entry[0], selected: entry === now }, entry[1])),
          now ? null : el("option", { value: "other", selected: true }, `from ${percent(target.part[0])}, ${percent(target.part[1])} to ${percent(target.part[2])}, ${percent(target.part[3])}`))));
      }
      if (target.source !== "off" && target.source !== "fixtures") {
        controls.push(el("label", { class: "inline" }, "White at ", el("input", { type: "number", class: "narrow", min: "0", max: "10000", step: "50", value: String(Math.round(target.nits)),
          "aria-label": `Brightness of ${label} in nits`, onchange: (event) => send({ nits: Number(event.target.value) }) }), " nits"));
      }
      const reach = !target.pixels ? null : el("div", { class: "muted" },
        `${plural(target.pixels.across * target.pixels.down, "pixel")} from the top left, on ${target.pixels.universes === 1 ? `universe ${target.pixels.universe}` :
          `universes ${target.pixels.universe} to ${target.pixels.universe + target.pixels.universes - 1}`}. A pixel that no longer fits in a universe starts the next one, as pixel mappers count.`);
      return [el("div", { class: "row" }, controls), reach];
    };

    const blocks = answer.screens.map((screen) => {
      const title = screen.name || "Screen";
      const split = screen.sections.length > 0;
      const where = screen.replaces.length ? `in the place of ${plural(screen.replaces.length, "fixture")}${screen.stands_for < screen.replaces.length ? `, ${screen.replaces.length - screen.stands_for} of which no longer hang there` : ""}`
        : screen.added ? "added here" : `of the rig file${screen.layer ? `, layer ${screen.layer}` : ""}`;
      const head = el("div", {}, el("b", {}, title), el("span", { class: "muted" }, `  ${metres(screen.width)} × ${metres(screen.height)} m, ${where}`),
        !split && screen.trouble ? el("span", { class: "bad" }, `  ${screen.trouble}`) : "",
        !screen.added && screen.named ? el("span", { class: "muted" }, `  (the rig file names ${screen.named_type === "NDI" ? "the stream" : "the file"} ${screen.named})`) : "");
      const block = el("div", { class: "screen" }, head);
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
          block.append(el("div", { class: "section" },
            el("div", {}, el("span", { class: "muted" }, `Section ${index + 1}: row ${Math.floor(index / screen.columns) + 1}, column ${index % screen.columns + 1}`),
              section.trouble ? el("span", { class: "bad" }, `  ${section.trouble}`) : ""),
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
      block.append(el("div", { class: "row" },
        el("label", { class: "inline" }, split ? "In sections: " : "Split into sections: ", columns, " across, ", rows, " down"),
        el("button", { onclick: () => cut(Math.round(Number(columns.value)), Math.round(Number(rows.value))) }, split ? "Split again" : "Split"),
        split ? el("button", { class: "link", onclick: () => cut(1, 1) }, "One piece again") : null,
        el("span", { class: "muted" }, split ? "Splitting again starts every section from the screen's own picture." : "Sections are counted row by row from the top left, as the audience sees it.")));
      if (screen.added) {
        const field = (label, key, scale, step) => el("label", {}, `${label} `, el("input", { type: "number", class: "narrow", step: String(step), value: String(+(screen[key] / scale).toFixed(3)), "data-key": key, "data-scale": String(scale) }));
        const named = el("input", { type: "text", value: screen.name, "aria-label": "Name", "data-key": "name" });
        const wall = screen.replaces.length > 0;    // as large as its fixtures and where they hang: only its name is its own
        const fields = wall ? [] : [field("Width", "width", 1000, 0.1), field("Height", "height", 1000, 0.1), field("To stage left", "x", 1000, 0.1), field("Upstage", "y", 1000, 0.1),
          field("Foot above the floor", "z", 1000, 0.1), field("Turned (degrees)", "turn", 1, 5)];
        const place = el("details", { class: "how" }, el("summary", {}, wall ? "Name" : "Name, size and place"),
          el("div", { class: "row" }, el("label", {}, "Name ", named)),
          el("div", { class: "row" }, fields,
            el("button", { class: "primary", onclick: () => {
              const changes = { name: named.value };
              for (const input of place.querySelectorAll("input[type=number]")) changes[input.dataset.key] = Number(input.value) * Number(input.dataset.scale);
              change(screen.id, changes);
            } }, "Save")),
          el("p", { class: "muted" }, wall ? "This screen is as large as the fixtures it stands in for and lies where they hang. Wherever it shows a picture of its own, those fixtures are left out of the visualizer; set to the fixtures, or removed, it gives the place back to them."
            : "Metres, measured as the rig file measures: from the middle of the stage, to stage left (the audience's right) and upstage. The foot is the lower edge. Turned by 0 the screen faces the house; a positive turn is anticlockwise seen from above."),
          el("button", { class: "link", onclick: async () => {
            try { draw(await api("/api/screen/remove", { json: { name, id: screen.id } })); said.replaceChildren(); }
            catch (error) { said.replaceChildren(notice(error.message, "error")); }
          } }, wall ? "Remove this screen: the fixtures show again" : "Remove this screen"));
        block.append(place);
      }
      return block;
    });
    // Walls of LED panels among the fixtures, which a screen can take the place of.
    const walls = !answer.walls.length ? null : el("div", { class: "screen" },
      el("div", {}, el("b", {}, answer.walls.length === 1 ? "An LED wall in this rig" : "LED walls in this rig")),
      el("p", { class: "muted" }, "These fixtures hang edge to edge as one flat surface. A screen in their place can show a picture, a film or an NDI stream across all of them, or across some sections of it while the rest stay the fixtures they are, driven by DMX as patched. Nothing looks different until the screen is given something to show."),
      answer.walls.map((wall) => el("div", { class: "row" },
        el("span", {}, wall.count === 1 ? wall.first : `${wall.first} to ${wall.last}`),
        el("span", { class: "muted" }, `${plural(wall.count, "fixture")} of ${bare(wall.type)}, ${metres(wall.width)} × ${metres(wall.height)} m`),
        el("button", { onclick: async (event) => {
          event.currentTarget.disabled = true;
          try { draw(await api("/api/screen/wall", { json: { name, fixtures: wall.uuids } })); said.replaceChildren(); }
          catch (error) { said.replaceChildren(notice(error.message, "error")); }
        } }, "Make it a screen"))));
    put(panel,
      el("p", { class: "muted" }, answer.screens.length
        ? "What each screen shows. A show that is open takes a change up at once; a film starts from its beginning, an NDI stream takes about ten seconds to appear, and under each screen the visualizer says what it is showing."
        : answer.walls.length ? "This show has no screens yet." : "This show has no screens. Its rig file has no video screen with a shape, and none was added here."),
      blocks, walls, said,
      el("div", { class: "row" }, el("button", { onclick: async () => {
        try { draw(await api("/api/screen/add", { json: { name } })); said.replaceChildren(); }
        catch (error) { said.replaceChildren(notice(error.message, "error")); }
      } }, "Add a screen")),
      el("p", { class: "muted" }, `A screen added here is four metres wide and stands upstage until you say otherwise. Pictures: ${answer.pictures.join(", ").toUpperCase()}. Films: ${answer.films.join(", ").toUpperCase()} in H.264, HEVC or ProRes; HAP and DXV films do not play. The picture is stretched to the screen. An NDI stream is named the way NDI lists it, for example the output Resolume sends with NDI switched on under Output.`));
  };
  gather(api(`/api/show/screens?name=${encodeURIComponent(name)}`)).then((answer) => { draw(answer); askVisualizer(); wordTimer = setInterval(askVisualizer, 2000); })
    .catch((error) => panel.replaceChildren(notice(error.message, "error")));
  return panel;
}

// The layers and classes of a show, each with a tick for "show it". What is unticked is left out
// the next time the show is opened, in the visualizer and in the Unreal Editor alike.
function layersPanel(row) {
  const hidden = { layers: new Set(row.hidden.layers), classes: new Set(row.hidden.classes) };
  const status = el("span", { class: "muted" });
  const send = async () => {
    try {
      await api("/api/show/hidden", { json: { name: row.name, layers: [...hidden.layers], classes: [...hidden.classes] } });
      row.hidden = { layers: [...hidden.layers], classes: [...hidden.classes] };
      status.textContent = " Saved. A show that is open takes it up at once.";
    } catch (error) { status.textContent = ` ${error.message}`; }
  };
  const list = (title, key, entries) => !entries.length ? null : el("div", {},
    el("h3", {}, title),
    entries.map((entry) => {
      const box = el("input", { type: "checkbox", checked: !hidden[key].has(entry.name) });
      box.addEventListener("change", () => { if (box.checked) hidden[key].delete(entry.name); else hidden[key].add(entry.name); send(); });
      const holds = [entry.fixtures ? plural(entry.fixtures, "fixture") : "", entry.pieces ? plural(entry.pieces, "set piece") : ""].filter(Boolean).join(", ");
      return el("label", { class: "check" }, box, ` ${entry.name || "(no name)"} `, el("span", { class: "muted" }, holds));
    }));
  return el("div", { class: "detail" },
    el("p", { class: "muted" }, "Untick a layer or a class to leave what is in it out of the picture. A fixture that is hidden stays patched; it is only not drawn and not lit.", status),
    list("Layers", "layers", row.layer_list || []), list("Classes", "classes", row.class_list || []));
}

async function loadShows() {
  let answer;
  try { answer = await api("/api/shows"); } catch (error) { return $("shows").replaceChildren(notice(error.message, "error")); }
  // Built off the page, with whatever its open panels fetch gathered; put on the page when that has
  // come (or after a second and a half, whichever is first), at the place the page was scrolled to.
  gathering = [];
  building += 1;
  const items = answer.shows.length ? answer.shows.map(showItem) : [el("div", { class: "empty" }, "No shows yet. Drop an MVR file above to add one.")];
  const fetched = gathering;
  gathering = null;
  await Promise.race([Promise.allSettled(fetched), new Promise((resolve) => setTimeout(resolve, 1500))]);
  await new Promise((resolve) => setTimeout(resolve, 0));    // let the panels draw what they were sent
  const page = document.querySelector("main");
  const scrolled = page ? page.scrollTop : 0;
  $("shows").replaceChildren(...items);
  if (page) page.scrollTop = scrolled;
  building -= 1;
  drawn();
  $("found").replaceChildren(...(answer.found.length ? [
    el("h2", {}, "Found on this computer"),
    el("div", { class: "list" }, answer.found.map((row) => el("div", { class: "item slim" },
      el("div", { class: "body" },
        el("div", { class: "title" }, bare(row.name)),
        el("div", { class: "meta" }, `In ${row.application}'s export folder · ${size(row.size)}`)),
      el("div", { class: "actions" }, el("button", { onclick: async (event) => {
        event.currentTarget.disabled = true;
        try { await api("/api/import", { json: { path: row.path } }); $("shows-notice").replaceChildren(); }
        catch (error) { $("shows-notice").replaceChildren(notice(error.message, "error")); }
        loadShows();
      } }, "Add"))))),
  ] : []));
}

// What to say about a file that was just taken in; and, if a show of that name was here already,
// the offer to put the newer one in its place.
function addedNotice(answer) {
  if (answer.kind === "package") {
    // A show packed by Openviz on this computer or another: it came with its fixture types and media.
    const more = [answer.types_added ? `${plural(answer.types_added, "fixture type")} added to your library` : "",
      answer.media ? `${answer.media} ${answer.media === 1 ? "picture or film" : "pictures and films"} for its screens` : ""].filter(Boolean);
    return notice(`${bare(answer.name)} was unpacked${answer.made ? ` (packed ${answer.made})` : ""}${answer.renamed ? ", under a new name, because a show already had its name" : ""}` +
      `${more.length ? `, with ${more.join(" and ")}` : ""}. ${(answer.warnings || []).join(" ")}`, answer.warnings && answer.warnings.length ? "" : "ok");
  }
  if (answer.kind === "model") {
    return notice(`${answer.name} ${answer.already_here ? "is already among" : "was added to"} your models: add it to a show under the show's Set pieces. ${(answer.warnings || []).join(" ")}`,
      answer.warnings && answer.warnings.length ? "" : "ok");
  }
  const where = answer.kind === "gdtf" ? "your fixture library" : "your shows";
  const said = notice(answer.already_here ? `${answer.name} is already in ${where}.`
    : `${answer.name} was added to ${where}${answer.renamed ? " under a new name, because a different file already had its name" : ""}.`, "ok");
  if (answer.instead_of) {
    // A newer export of a show already here? Then it can take that show's place and keep what was done to it.
    said.append(" If it is a newer version of ", bare(answer.instead_of), ": ",
      el("button", { class: "link", onclick: async (event) => {
        event.currentTarget.disabled = true;
        try {
          const done = await api("/api/show/update", { json: { name: answer.instead_of, newer: answer.name } });
          const words = [`${bare(done.name)} is now the newer file: ${plural(done.fixtures, "fixture")}`];
          if (done.added) words.push(`${done.added} new`);
          if (done.gone) words.push(`${done.gone} no longer in it`);
          let kept = `. The changes to ${plural(done.changes_kept, "fixture")} were kept`;
          if (done.changes_lost) kept += `; those to ${done.changes_lost} belong to fixtures the newer file no longer has`;
          $("shows-notice").replaceChildren(notice(`${words.join(", ")}${done.changes_kept || done.changes_lost ? kept : ""}. The older file was set aside as ${done.kept_as}.`, "ok"));
        } catch (error) { $("shows-notice").replaceChildren(notice(error.message, "error")); }
        loadShows();
      } }, `Update ${bare(answer.instead_of)} with it`), ", keeping your changes, views and hidden layers.");
  }
  return said;
}

async function addFiles(files) {
  const drop = $("drop");
  drop.classList.add("busy");
  const messages = [];
  for (const file of files) {
    try {
      const answer = await api(`/api/files?name=${encodeURIComponent(file.name)}`, { body: file });
      const said = addedNotice(answer);
      messages.push(said);
    } catch (error) {
      messages.push(notice(error.message, error.code === "export_needed" ? "" : "error"));
    }
  }
  drop.classList.remove("busy");
  $("shows-notice").replaceChildren(...messages);
  loadShows();
}

// ----------------------------------------------------------------- exchange with the console

// MVR-xchange: the console and the drawing program on the same network offer their rigs, and
// take ours, without a file changing hands. Shown under the drop area; looked at again every few
// seconds while it is on.
let xchange = null;
let xchangeTimer = null;
let xchangeOpen = false;
let xchangeSaid = null;

let xchangeShown = "";

async function loadXchange() {
  if ($("view-shows").hidden) return;
  let now;
  try { now = await api("/api/xchange"); } catch (error) { return; }
  const was = xchange;
  xchange = now;
  const active = document.activeElement;
  if (active && $("xchange").contains(active) && active.tagName === "INPUT") return;    // somebody is typing: leave it be
  if (JSON.stringify(now) === xchangeShown) return;                                      // nothing new: leave what is there alone
  renderXchange();
  if (was ? was.running !== now.running || was.group !== now.group : now.running) loadShows();       // "Send to the group" comes and goes with it
}

function renderXchange() {
  const x = xchange;
  if (!x) return;
  const say = (text, kind) => { xchangeSaid = text ? notice(text, kind) : null; renderXchange(); };
  const set = async (changes) => {
    try { xchange = await api("/api/xchange/set", { json: changes }); say("", ""); }
    catch (error) { say(error.message, "error"); }
    loadShows();
  };
  const listId = "xchange-groups";
  const group = el("input", { type: "text", value: x.group, list: listId, placeholder: "The group's name", "aria-label": "Group" });
  const name = el("input", { type: "text", value: x.name, placeholder: x.default_name, "aria-label": "This station's name" });
  const groups = el("datalist", { id: listId }, Object.keys(x.groups).map((entry) => el("option", { value: entry })));
  const others = Object.entries(x.groups).filter(([entry]) => entry.toLowerCase() !== x.group.toLowerCase());
  const stations = x.stations.length
    ? `With ${x.stations.map((station) => station.provider && station.provider !== station.name ? `${station.name} (${station.provider})` : station.name).join(", ")}.`
    : x.running ? "No other station in this group yet." : "";
  const offers = x.offers.filter((offer) => !offer.mine);
  const body = el("div", {},
    el("p", { class: "muted" }, "A console or drawing program on the same network that has MVR-xchange switched on, and is in the same group, can send its rig here and take ours. Nothing is taken in until you say so."),
    el("div", { class: "row" },
      el("label", {}, "Group ", group, groups),
      el("label", {}, "This station is called ", name),
      el("button", { onclick: () => set({ group: group.value.trim(), name: name.value.trim() }) }, "Save"),
      el("button", { class: x.on ? "" : "primary", onclick: () => set({ on: !x.on, group: group.value.trim(), name: name.value.trim() }) }, x.on ? "Leave the group" : "Join"),
      el("button", { class: "link", onclick: async (event) => {
        event.currentTarget.disabled = true;
        try {
          const looked = await api("/api/xchange/groups");
          const found = looked.groups;
          const names = Object.keys(found);
          const how = "In grandMA3 that is Menu, In & Out, MVR: Enable has to be on there.";
          if (!names.length) {
            return say(looked.mine ? `Nobody else on the network has MVR-xchange switched on right now. Openviz is in the group ${looked.mine} and will show a station as soon as one joins it. ${how}`
              : `No MVR-xchange station is on the network right now. ${how} You can join a group here all the same and wait.`, "");
          }
          const others = (count) => plural(count, "other station");
          xchangeSaid = notice("On the network now: ", "ok", names.flatMap((entry, index) => [index ? ", " : "",
            entry === looked.mine ? `${entry}, the group Openviz is in (${others(found[entry])})`
              : [el("button", { class: "link", onclick: () => set({ on: true, group: entry, name: name.value.trim() }) }, `join ${entry}`), ` (${plural(found[entry], "station")})`]].flat()),
            ".", looked.mine && !names.includes(looked.mine) ? ` Nobody else is in ${looked.mine}, the group Openviz is in.` : "");
          renderXchange();
        } catch (error) { say(error.message, "error"); }
      } }, "Look for groups")),
    x.trouble ? el("div", { class: "bad" }, x.trouble) : null,
    x.running ? el("div", {}, el("span", { class: "good" }, `In the group ${x.group}. `), stations,
      others.length ? el("span", { class: "muted" }, ` Other groups on the network: ${others.map(([entry]) => entry).join(", ")}.`) : null) : null,
    offers.length ? el("div", { class: "list" }, offers.map((offer) => el("div", { class: "item slim" },
      el("div", { class: "body" },
        el("div", { class: "title" }, offer.file || "A rig"),
        el("div", { class: "meta" }, `Offered by ${offer.from} · ${size(offer.size)}${offer.comment ? ` · ${offer.comment}` : ""}`)),
      el("div", { class: "actions" }, el("button", { onclick: async (event) => {
        event.currentTarget.disabled = true;
        try { $("shows-notice").replaceChildren(addedNotice(await api("/api/xchange/fetch", { json: { uuid: offer.uuid } }))); }
        catch (error) { $("shows-notice").replaceChildren(notice(error.message, "error")); }
        loadShows();
        loadXchange();
      } }, offer.have ? "Take it in again" : "Take it in"))))) : null,
    xchangeSaid);
  const details = el("details", { class: "how", open: xchangeOpen || x.on },
    el("summary", {}, x.running ? `Exchange with the console over the network: in the group ${x.group}${offers.length ? `, ${plural(offers.length, "rig")} offered` : ""}` : "Exchange with the console over the network (MVR-xchange)"),
    body);
  details.addEventListener("toggle", () => { xchangeOpen = details.open; });
  $("xchange").replaceChildren(details);
  xchangeShown = JSON.stringify(x);
  if (!xchangeTimer) xchangeTimer = setInterval(loadXchange, 4000);
}

// ---------------------------------------------------------------------------------- fixtures

function renderShare() {
  const share = state.share;
  const status = $("share-status");
  if (!share.has_list) status.replaceChildren("Log in once to fetch the fixture list");
  else {
    const days = Math.floor((Date.now() / 1000 - share.list_fetched) / 86400);
    const listed = `${share.revisions.toLocaleString()} files listed ${days <= 0 ? "today" : days === 1 ? "yesterday" : `${days} days ago`} · `;
    if (share.logged_in || share.remembered) {
      put(status, listed, share.remembered ? `Stays logged in as ${share.user}` : "Logged in until Openviz quits", " · ",
        el("button", { class: "link", onclick: refreshList }, "Refresh list"),
        el("button", { class: "link", onclick: logout }, "Log out"));
    } else {
      put(status, listed, el("button", { class: "link", onclick: () => askLogin("Log in to refresh the list or download fixtures.") }, "Log in"));
    }
  }
  if (!share.has_list && !share.logged_in) askLogin("GDTF Share is the fixture library the manufacturers publish to. Logging in fetches its list of fixtures, which is then kept on this computer for searching.");
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
    state.share.remembered = false;    // a saved login that stopped working has been forgotten
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
      $("fixtures-notice").replaceChildren(notice(`You are logged in, but the password could not be put in ${passwordStore()}, so Openviz will ask again next time.`, ""));
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
  const range = !slots.length ? "" : Math.min(...slots) === Math.max(...slots) ? `, ${slots[0]} channels` : `, ${Math.min(...slots)} to ${Math.max(...slots)} channels`;
  return `${plural(modes.length, "mode")}${range}`;
}

async function download(rid, button) {
  button.disabled = true;
  button.textContent = "Downloading…";
  try {
    await api("/api/share/download", { json: { rid } });
    button.replaceWith(el("span", { class: "good" }, "In library"));
    loadLibrary();
  } catch (error) {
    button.disabled = false;
    button.textContent = "Download";
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
  if (!query) return results.replaceChildren(el("div", { class: "empty" }, "Type a fixture or manufacturer name to search GDTF Share."));
  const mine = ++searchCount;
  let answer;
  try { answer = await api(`/api/share/search?q=${encodeURIComponent(query)}&limit=40`); }
  catch (error) { return failed(error); }
  if (mine !== searchCount) return;    // a newer search has started
  const rows = answer.rows.map((row) => el("div", { class: "item slim" },
    el("div", { class: "body" },
      el("div", { class: "title" }, `${row.manufacturer} ${row.fixture}`,
        el("span", { class: `tag ${row.by_manufacturer ? "maker" : ""}` }, row.by_manufacturer ? "manufacturer's file" : "uploaded by a user")),
      el("div", { class: "meta" }, [row.revision, modesText(row.modes), size(row.size),
        row.more ? `${plural(row.more, "other revision")}` : ""].filter(Boolean).join(" · "))),
    el("div", { class: "actions" }, row.have ? el("span", { class: "good" }, "In library")
      : el("button", { onclick: (event) => download(row.rid, event.currentTarget) }, "Download"))));
  if (!rows.length) rows.push(el("div", { class: "empty" }, `Nothing on GDTF Share matches "${query}".`));
  else if (answer.total > answer.rows.length) rows.push(el("div", { class: "muted" }, `Showing ${answer.rows.length} of ${answer.total}. Add a word to narrow it down.`));
  results.replaceChildren(...rows);
}

function fixtureItem(row) {
  const title = row.name ? `${row.manufacturer || ""} ${row.name}`.trim() : bare(row.file);
  let verdict;
  if (row.error) verdict = el("span", { class: "bad" }, `Cannot be read: ${row.error}`);
  else if (row.unchecked) verdict = "Not checked: the core is not built";
  else if (row.footprints_differ && row.footprints_differ.length) verdict = el("span", { class: "warn" }, `Channel count differs from GDTF Share's in: ${row.footprints_differ.join(", ")}`);
  else if (row.warnings) verdict = el("span", { class: "warn" }, `Reads with ${plural(row.warnings, "warning")}`);
  else verdict = el("span", { class: "good" }, row.footprints_checked ? "Reads cleanly, channel counts match GDTF Share's" : "Reads cleanly");
  const meta = el("div", { class: "meta" }, [row.revision, modesText(row.modes || []), size(row.size)].filter(Boolean).join(" · "), " · ", verdict);

  const blocked = ready() || (row.error ? "This file cannot be opened." : "");
  const mode = el("select", { "aria-label": `DMX mode for ${title}` },
    (row.modes || []).map((entry) => el("option", { value: entry.name }, `${entry.name} (${entry.footprint})`)));
  return el("div", { class: "item slim" },
    el("div", { class: "body" }, el("div", { class: "title" }, title), meta),
    el("div", { class: "actions" }, (row.modes || []).length > 1 ? mode : null,
      el("button", { title: blocked, disabled: !!blocked, onclick: (event) => launch("fixture", row.file, { mode: mode.value }, event.currentTarget) }, "Preview")));
}

async function loadLibrary() {
  let answer;
  try { answer = await api("/api/fixtures"); } catch (error) { return $("library").replaceChildren(notice(error.message, "error")); }
  $("library").replaceChildren(...(answer.fixtures.length ? answer.fixtures.map(fixtureItem)
    : [el("div", { class: "empty" }, "No fixtures yet. Search above and download one, or drop a GDTF file on the Shows page.")]));
}

// ------------------------------------------------------------- settings: DMX, picture, engine

async function save(changes, redraw) {
  try {
    await api("/api/settings", { json: changes });
    state = await api("/api/state");
  } catch (error) {
    document.querySelector("main section:not([hidden])").prepend(notice(error.message, "error"));
    return;
  }
  redraw();
  $("engine-dot").classList.toggle("off", !!ready());
}

function setting(title, help, control) {
  return el("div", { class: "setting" }, el("div", { class: "what" }, el("b", {}, title), el("span", { class: "muted" }, help)),
    el("div", { class: "control" }, control));
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
    return notice(`No ${name} levels arrived in ${result.seconds} seconds${result.other_packets ? `, though ${plural(result.other_packets, "other packet")} did, so something on the network is talking ${name}` : ""}. Check that the console is sending ${name}, that it is on the same network as this computer, and the address under Listen on.`, "");
  }
  return el("div", { class: "notice ok" }, `${name} is arriving on ${plural(result.universes.length, "universe")}:`,
    el("div", { class: "tablewrap" }, el("table", {},
      el("thead", {}, el("tr", {}, el("th", { class: "num" }, "Universe"), el("th", { class: "num" }, "Packets a second"), el("th", { class: "num" }, "Slots above zero"), el("th", {}, "Sent from"))),
      el("tbody", {}, result.universes.map((u) => el("tr", {}, el("td", { class: "num" }, String(u.universe)), el("td", { class: "num" }, String(u.rate)),
        el("td", { class: "num" }, String(u.slots_in_use)), el("td", {}, u.sources.join(", "))))))));
}

// What is arriving right now. Openviz receives the console's DMX itself and passes it to the
// visualizer, so it can show every universe on the way, with or without a show open.
let live = null;          // what /api/dmx/monitor last said
let liveTimer = null;
let watched = null;       // the universe whose levels are shown

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
      el("p", { class: "muted" }, relay.possible ? "Openviz was started without its own DMX receiver, so the visualizer listens for itself and nothing can be shown here."
        : "Art-Net sent to this computer's own address (127.0.0.1) goes straight to the visualizer and cannot be shown on the way. Choose this computer's address on the lighting network under Listen on, and it passes through Openviz."),
      el("div", { class: "group" },
        setting("Check DMX input", `Listens for two seconds, the way the visualizer will, and lists the universes that have ${name} arriving. Unreal has to be closed while it listens.`,
          el("button", { onclick: checkDmx }, "Check now"))),
      checkResult()];
  }
  if (relay.error) return notice(relay.error, "error");
  const seen = live.monitor;
  if (!seen) return el("p", { class: "muted" }, `Starting to listen for ${name} on ${relay.address}…`);
  if (!seen.universes.length) {
    return notice(`Nothing is arriving. Openviz is listening for ${name} on ${relay.address}${seen.other ? `, and ${plural(seen.other, "packet")} of another kind did arrive, so something on the network is talking ${name}` : ""}. Check that the console is sending ${name}, that it is on the same network as this computer, and the address under Listen on.`, "");
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
    el("p", { class: "muted" }, `${name} on ${relay.address}: ${plural(seen.universes.length, "universe")} arriving, passed on to the visualizer. Click a universe to see its levels.`),
    el("div", { class: "tablewrap" }, el("table", { class: "live" },
      el("thead", {}, el("tr", {}, el("th", { class: "num" }, "Universe"), el("th", { class: "num" }, "Packets a second"), el("th", { class: "num" }, "Slots above zero"),
        el("th", { class: "num" }, "Holding for"), el("th", {}, "Sent from"))),
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
    cell.style.backgroundColor = `rgba(255, 176, 64, ${(level / 255 * 0.9).toFixed(3)})`;    // through the style object: the page's content policy allows that
    grid.append(cell);
  }
  return el("div", {}, el("h3", {}, `Universe ${watch.universe}`), grid,
    el("p", { class: "muted" }, "Slots 1 to 512, thirty-two to a row, as passed on to the visualizer. Hover over a cell for its slot number."));
}

// Recording what arrives, and playing a recording back to the visualizer.
let takes = null;        // what /api/recordings last said
let takesTimer = null;
let takesNotice = null;

async function loadTakes() {
  clearTimeout(takesTimer);
  try { takes = await api("/api/recordings"); } catch (error) { takes = { recordings: [], state: {}, error: error.message }; }
  if ($("takes")) put($("takes"), takesPanel());
  // While something is running the figures move: look again in a second.
  if (takes.state.recording || takes.state.playing) takesTimer = setTimeout(loadTakes, 1000);
}

async function takeAction(path, body, said) {
  takesNotice = null;
  try {
    const answer = await api(path, { json: body || {} });
    if (said) takesNotice = notice(said(answer), "ok");
  } catch (error) { takesNotice = notice(error.message, "error"); }
  loadTakes();
}

const clock = (seconds) => `${Math.floor(seconds / 60)}:${String(Math.floor(seconds % 60)).padStart(2, "0")}`;

function takesPanel() {
  if (!takes) return el("p", { class: "muted" }, "Looking for recordings…");
  const now = takes.state;
  // Through Openviz's own receiver a recording can be made at any time; without it, Art-Net only while Unreal is closed.
  const artnet = state.settings.protocol === "artnet" && !(live && live.relay && live.relay.running);
  const record = now.recording
    ? [el("button", { class: "primary", onclick: () => takeAction("/api/record/stop", {}, (a) => a.saved ? `Saved ${clock(a.seconds)} of DMX on ${plural(a.universes, "universe")}.` : "Stopped.") }, "Stop recording"),
       el("span", { class: "muted" }, ` ${clock(now.seconds)} so far, ${plural(now.universes, "universe")} heard`)]
    : [el("button", { onclick: () => takeAction("/api/record/start") }, "Record")];
  const rows = takes.recordings.map((take) => {
    const playing = now.playing === take.name;
    const facts = take.error ? take.error : `${clock(take.seconds)} · ${plural(take.universes, "universe")} · ${size(take.size)}`;
    return el("div", { class: "item" },
      el("div", { class: "body" }, el("div", { class: "title" }, take.name.replace(/\.dmxrec$/, "")),
        el("div", { class: "meta" }, playing ? `${facts} · playing, ${clock(now.position)} of ${clock(now.length)}` : facts)),
      el("div", { class: "actions" }, playing
        ? el("button", { class: "primary", onclick: () => takeAction("/api/play/stop") }, "Stop")
        : el("button", { disabled: !!take.error || !take.frames, onclick: () => takeAction("/api/play/start", { name: take.name, loop: true }) }, "Play")));
  });
  const videos = (takes.videos || []).map((video) => el("div", { class: "item" },
    el("div", { class: "body" }, el("div", { class: "title" }, video.name),
      el("div", { class: "meta" }, `${size(video.size)} · ${new Date(video.made * 1000).toLocaleString()}${video.converting ? " · being made into an MP4" : ""}`))));
  return [
    el("div", { class: "group" },
      setting("Record what arrives", `Keeps the levels the console sends, to play back later without it. ${artnet ? "Unreal has to be closed while recording." : "The visualizer can stay open while recording."} A recording stops by itself after four hours.`, record)),
    takesNotice,
    rows.length ? el("div", { class: "list" }, rows) : el("p", { class: "muted" }, "No recordings yet."),
    rows.length ? el("p", { class: "muted" }, "Play sends a recording to the visualizer round and round, as Art-Net, the way the console would. Open a show to watch it. ",
      el("button", { class: "link", onclick: () => api("/api/reveal", { json: { what: "recordings" } }) }, "Show folder")) : null,
    rows.length ? renderForm() : null,
    el("h3", {}, "Videos"),
    videos.length ? el("div", { class: "list" }, videos) : el("p", { class: "muted" }, "No videos yet. Record one while watching a show (Video, Record, on the bar at the foot of the window), or render a recording here."),
    videos.length ? el("p", { class: "muted" }, el("button", { class: "link", onclick: () => api("/api/reveal", { json: { what: "videos" } }) }, "Show videos")) : null,
  ];
}

// Rendering a recording to video: a show, a recording, a frame rate and a view. The visualizer
// opens, draws every frame in full at a steady clock and closes when the recording ends.
let renderNotice = null;
function renderForm() {
  const shows = takes.shows || [];
  if (!shows.length) return null;
  const show = el("select", { "aria-label": "Show" }, shows.map((name) => el("option", { value: name }, bare(name))));
  const take = el("select", { "aria-label": "Recording" }, takes.recordings.filter((row) => !row.error && row.frames).map((row) => el("option", { value: row.name }, row.name.replace(/\.dmxrec$/, ""))));
  const rate = el("select", { "aria-label": "Frames a second" }, [24, 25, 30, 50, 60].map((n) => el("option", { value: String(n), selected: n === 30 }, `${n} frames a second`)));
  const view = el("select", { "aria-label": "View" }, el("option", { value: "0" }, "from the starting view"),
    [1, 2, 3, 4, 5, 6, 7, 8, 9].map((n) => el("option", { value: String(n) }, `from view ${n}`)));
  const then = el("input", { type: "text", "aria-label": "Then the camera goes to", placeholder: "then, say: 3@8 5@15", autocomplete: "off" });
  const go = el("button", { class: "primary", onclick: async () => {
    go.disabled = true;
    renderNotice = null;
    try {
      renderRun(await api("/api/render", { json: { name: show.value, recording: take.value, rate: Number(rate.value), view: Number(view.value), path: then.value.trim() ? `${view.value} ${then.value}` : "" } }));
      pollRun();
      renderNotice = notice("Rendering: the visualizer's window shows how far it has got. Leave it be until it closes by itself.", "ok");
    } catch (error) { renderNotice = notice(error.message, "error"); }
    if ($("takes")) put($("takes"), takesPanel());
  } }, "Render");
  return el("details", { class: "how", open: !!renderNotice },
    el("summary", {}, "Render a recording to video"),
    el("div", { class: "row" }, show, take, rate, view, then, go),
    renderNotice,
    el("p", { class: "muted" }, `The visualizer opens the show, plays the recording at a steady clock and writes every frame into a movie the size of its window (${(state.settings.resolution || "").replace("x", " × ")}, under Picture), whatever the computer can manage live: a heavy look takes longer to render, not less smooth. ` +
      "Only the recording drives the fixtures; what it does not drive stays dark. The camera stays at the view chosen, or moves: \"3@8 5@15\" glides on to view 3, arriving at 8 seconds, and on to view 5 by 15; " +
      "name a view twice to hold it (\"1@4 3@8\" holds the first view four seconds). Views are those stored while watching the show. The movie goes to Videos, and is made into an MP4 when it is done."));
}

// Remote control by OSC: a console, TouchOSC or any program that sends OSC can switch views,
// take pictures and change the picture. The heard list is redrawn on its own, so that the port
// field is not taken from under the cursor.
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
  if (!$("view-dmx").hidden && oscState.running) oscTimer = setTimeout(loadHeard, 1500);
}

async function loadHeard() {
  clearTimeout(oscTimer);
  if ($("view-dmx").hidden || !$("osc-heard")) return;    // nobody is looking
  try { oscState = await api("/api/osc"); } catch (error) { return; }
  put($("osc-heard"), heardPanel());
  if (oscState.running) oscTimer = setTimeout(loadHeard, 1500);
}

function heardPanel() {
  const heard = (oscState.heard || []).slice().reverse();
  if (!oscState.running) return [];
  if (!heard.length) return el("p", { class: "muted" }, "Nothing heard yet.");
  return el("div", { class: "tablewrap" }, el("table", {},
    el("thead", {}, el("tr", {}, el("th", {}, "At"), el("th", {}, "From"), el("th", {}, "Address"), el("th", {}, "Values"), el("th", {}, "What came of it"))),
    el("tbody", {}, heard.map((m) => el("tr", {}, el("td", {}, m.at), el("td", {}, m.from), el("td", {}, el("code", {}, m.address)),
      el("td", {}, m.arguments), el("td", { class: m.result.startsWith("not") || m.result.startsWith("an address") ? "muted" : "" }, m.result))))));
}

function oscPanel() {
  if (!oscState) return el("p", { class: "muted" }, "Looking…");
  const o = oscState;
  const port = el("input", { type: "number", class: "narrow", min: "1024", max: "65535", step: "1", value: String(o.port || 8000), "aria-label": "OSC port",
    onchange: (event) => loadOsc({ osc_port: Number(event.target.value) }) });
  const where = o.local && o.local.length ? o.local.join(" or ") : "this computer's address";
  const status = !o.on ? null
    : o.running ? notice(`Listening on port ${o.port}. Send to ${where}, port ${o.port}.`, "ok")
    : o.trouble ? notice(o.trouble, "error") : notice("Not listening: quit Openviz and open it again.", "");
  const sample = `SendOSC 1 "/openviz/view,i,3"`;
  return [
    el("div", { class: "group" },
      setting("Listen for OSC", "Lets a console, TouchOSC or any other program that sends OSC switch views, take pictures and change the picture while a show is open. Openviz hears it on every network address of this computer.",
        el("button", { class: "switch", role: "switch", "aria-checked": String(!!o.on), "aria-label": "Listen for OSC", onclick: () => loadOsc({ osc: !o.on }) })),
      setting("Port", "The UDP port the sender sends to. 8000 unless something else on this computer has it.", port)),
    oscNotice, status,
    el("details", { class: "how" }, el("summary", {}, "What Openviz answers to"),
      el("div", { class: "tablewrap" }, el("table", {},
        el("thead", {}, el("tr", {}, el("th", {}, "Address"), el("th", {}, "Takes"))),
        el("tbody", {}, (o.addresses || []).map((a) => el("tr", {}, el("td", {}, el("code", {}, a.address)), el("td", {}, a.takes)))))),
      el("p", { class: "muted" }, "Views, stores and pictures need a show open in the visualizer. The picture's settings change Openviz's own, as on the Picture page, and an open show takes them up. Anything in front of /openviz/ is ignored, so a prefix the sender adds does no harm.")),
    el("details", { class: "how" }, el("summary", {}, "Sending from grandMA3"),
      el("p", {}, "In Menu, In & Out, OSC: switch on Enable Output, and add a line with this computer's address as Destination IP, ",
        el("code", {}, String(o.port || 8000)), " as Port, Mode UDP and Send on. Then a macro or a cue's command can say, for the first line:"),
      el("p", {}, el("code", {}, sample), " ",
        el("button", { class: "link", onclick: () => navigator.clipboard && navigator.clipboard.writeText(sample) }, "Copy")),
      el("p", { class: "muted" }, `That goes to stored view 3. "/openviz/view/top,i,1" shows the plan; "/openviz/picture,i,4" saves a 4K picture; "/openviz/haze,f,0.5" sets the haze half way.`)),
    o.running ? el("p", { class: "muted" }, "Heard lately, newest first:") : null,
    el("div", { id: "osc-heard" }, heardPanel()),
  ];
}

function renderDmx() {
  const s = state.settings;
  const addresses = state.addresses.includes(s.dmx_address) ? state.addresses : [...state.addresses, s.dmx_address];
  put($("dmx"),
    el("h2", {}, "Input"),
    el("div", { class: "group" },
      setting("Protocol", "What your console transmits. Openviz receives it on the address below, shows what arrives and passes it to the visualizer.",
        choice("protocol", [["artnet", "Art-Net"], ["sacn", "sACN"]], renderDmx)),
      setting("Listen on", "The network address of this computer that the console's data arrives at. 127.0.0.1 only hears programs on this computer.",
        el("select", { "aria-label": "Network address", onchange: (event) => save({ dmx_address: event.target.value }, renderDmx) },
          addresses.map((address) => el("option", { value: address, selected: address === s.dmx_address },
            address === "127.0.0.1" ? "127.0.0.1 (this computer)" : address)))),
      setting("Light fixtures until DMX arrives", "Shows every fixture at its default values with the dimmer up, so a rig is visible without a console.",
        toggle("idle_look", "Light fixtures until DMX arrives", renderDmx))),
    el("h2", {}, "What is arriving"),
    el("div", { id: "live" }, livePanel()),
    el("h2", {}, "Record and play back"),
    el("div", { id: "takes" }, takesPanel()),
    el("h2", {}, "Remote control (OSC)"),
    el("div", { id: "osc" }, oscPanel()));
  loadLive();
  loadTakes();
  loadOsc();
}

// What each quality preset sets. A preset shows as chosen when all of its settings are as it has them.
const PRESETS = {
  Fast: { beams: "cone", beam_detail: 50, beam_shadows: false, shadows: false, megalights: true },
  Balanced: { beams: "cone", beam_detail: 100, beam_shadows: true, shadows: false, megalights: true },
  Best: { beams: "cone", beam_detail: 100, beam_shadows: true, shadows: true, megalights: true },
};

function renderPicture() {
  const s = state.settings;
  const resolutions = ["1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160"];
  if (!resolutions.includes(s.resolution)) resolutions.push(s.resolution);
  put($("picture"),
    el("p", { class: "muted" }, "How the visualizer looks when you watch a show. A show that is open takes these up as you change them; only the window and its resolution wait for the next time it is opened."),
    el("h2", {}, "Window"),
    el("div", { class: "group" },
      setting("Window", onMac() ? "Fullscreen is the only mode in which macOS lets the frame rate go above the display's." : "Fullscreen gives the visualizer the whole screen.", choice("window", [["windowed", "Window"], ["fullscreen", "Fullscreen"]], renderPicture)),
      setting("Resolution", "", el("select", { "aria-label": "Resolution", onchange: (event) => save({ resolution: event.target.value }, renderPicture) },
        resolutions.map((value) => el("option", { value, selected: value === s.resolution }, value.replace("x", " × ")))))),
    el("h2", {}, "Stage"),
    el("div", { class: "group" },
      setting("Exposure", "Fixtures are as bright as their files say, so the camera needs stopping down. Lower is darker.",
        slider("exposure", -12, 0, 0.5, (value) => `${value > 0 ? "+" : ""}${value} stops`, renderPicture)),
      setting("Haze", "How much the air shows the beams.", slider("haze", 0, 0.2, 0.01, (value) => `${Math.round(value * 500)}%`, renderPicture)),
      setting("Drifts in the haze", "Haze does not hang evenly: it lies in drifts that wander slowly through the room, and the beams show them. Even is haze from a good hazer left running; Heavy is smoke that has just been let off.",
        choice("haze_drift", [[0, "Even"], [0.4, "Light"], [0.7, "Medium"], [1, "Heavy"]], renderPicture)),
      setting("House lights", "An even light on the stage and the fixtures, so the rig can be seen when nothing is lit. It does not light the haze.",
        slider("house_lights", 0, 1, 0.05, (value) => value ? `${Math.round(value * 100)}%` : "off", renderPicture))),
    el("h2", {}, "Camera and mouse"),
    el("div", { class: "group" },
      setting("Mouse", "With Hold right button, the pointer stays free and the view turns only while the right mouse button is held, as in the Unreal Editor. Game style hides the pointer and every mouse movement turns the view. Tab switches between the two while watching.",
        choice("mouse_look", [["hold", "Hold right button"], ["always", "Game style"]], renderPicture)),
      setting("Look speed", "How far the view turns for a given movement of the mouse.",
        slider("look_speed", 0.2, 3, 0.1, (value) => `${value.toFixed(1)}×`, renderPicture)),
      setting("Flying speed", "How fast W A S D, Q and E move the camera. Shift makes it four times as fast.",
        slider("move_speed", 1, 30, 1, (value) => `${value} m/s`, renderPicture)),
      setting("Invert vertical", "Moving the mouse up looks down.", toggle("invert_y", "Invert vertical", renderPicture)),
      setting("Views and pictures", "While watching: Shift with a number key from 1 to 9 stores the present view, the number key alone goes back to it, and 0 returns to where the view began. Stored views are kept with the show. P saves a picture of what is on screen.",
        el("button", { onclick: () => api("/api/reveal", { json: { what: "pictures" } }) }, "Show pictures"))),
    el("h2", {}, "Quality"),
    el("div", { class: "group" },
      setting("Preset", "Sets the four switches below in one go. Fast is for large rigs or a slower computer; Best adds shadows, which cost the most.",
        el("div", { class: "seg", role: "group" }, Object.entries(PRESETS).map(([name, values]) => {
          const on = Object.entries(values).every(([key, value]) => s[key] === value);
          return el("button", { class: on ? "on" : "", "aria-pressed": String(on), onclick: () => save(values, renderPicture) }, name);
        }))),
      setting("Beams", "Shafts draws every beam on its own, with a clean edge from lens to floor. Glow only lets beams light the haze, which blurs them together but costs a little less.",
        choice("beams", [["cone", "Shafts"], ["fog", "Glow"]], renderPicture)),
      setting("Beam detail", "Beams in haze are soft, so they can be drawn at a lower resolution than the rest of the picture and scaled up. Half is hard to tell from Full and costs far less when many fixtures have gobos and prisms in. What lands on the stage is not affected.",
        choice("beam_detail", [[100, "Full"], [75, "Three quarters"], [50, "Half"]], renderPicture)),
      setting("Keep the picture moving", "When a look lights so many beams that the picture drops below 30 frames a second, beams in haze are drawn at less detail until it runs freely again. They are soft, so little is lost; what lands on the stage is not affected. Off, beam detail stays where it is set and a heavy look slows the picture down instead.",
        toggle("adapt_beams", "Keep the picture moving", renderPicture)),
      setting("Shadows in beams", "What stands in a beam cuts the part of it behind: a truss, a set piece, a screen. Without it a beam ends whole where its middle lands. Costs frame rate only for beams that have something in them.",
        toggle("beam_shadows", "Shadows in beams", renderPicture)),
      setting("Shadows on surfaces", "Every beam casts shadows on the floor and the set. Costs frame rate.", toggle("shadows", "Shadows on surfaces", renderPicture)),
      setting("Hazers in the rig", "A fog or haze machine patched in the rig adds to the haze as the console drives it: the air thickens over a quarter of a minute and clears over a minute. Off, the haze stays at the setting above.",
        toggle("rig_haze", "Hazers in the rig", renderPicture)),
      setting("MegaLights", `Unreal's renderer for many lights at once. Much faster with large rigs; ${onMac() ? "still experimental on macOS" : "experimental in Unreal 5.8"}.`, toggle("megalights", "MegaLights", renderPicture)),
      setting("Fixtures in a preview", "How many of a fixture type the Preview button hangs in a row.",
        slider("preview_count", 1, 24, 1, (value) => String(value), renderPicture))));
}

function renderEngine() {
  const s = state.settings;
  put($("engine"),
    el("div", { class: "group" },
      setting("Unreal Engine folder", state.engine_found ? el("span", { class: "good" }, "Unreal Engine found.") : el("span", { class: "bad" }, "No Unreal Engine in this folder."),
        el("input", { type: "text", value: s.engine, spellcheck: "false", "aria-label": "Engine folder",
                      onchange: (event) => save({ engine: event.target.value }, renderEngine) })),
      setting("Openviz's Unreal project", state.project_built && state.project_stale && state.project_stale[0]
        ? el("span", {}, `Its code has changed since it was built (${state.project_stale[1]}). It is built again before it next opens, or now: `,
            el("button", { class: "small", onclick: async (event) => {
              const button = event.currentTarget;    // gone from the event once it has been awaited
              button.disabled = true;
              try { renderRun({ running: false, building: await api("/api/build", { json: {} }) }); pollRun(); }
              catch (error) { button.parentElement.append(notice(error.message, "error")); button.disabled = false; }
            } }, "Build now"))
        : state.project_built ? el("span", { class: "good" }, "Built and ready.")
        : el("span", {}, el("span", { class: "bad" }, "Not built yet. "), "Run this once in a terminal: ", el("code", {}, state.build_command)), null),
      setting("Fixture checks", state.core_built ? el("span", { class: "good" }, "The core library is built, so files are checked as they come in.")
        : el("span", { class: "bad" }, "The core library is not built, so files are not checked and patches cannot be listed."), null),
      setting("Files", el("span", {}, "Shows, fixtures and settings are kept in ", el("code", {}, state.library)),
        [el("button", { onclick: () => api("/api/reveal", { json: { what: "shows" } }).catch(() => {}) }, "Show folder"),
         el("button", { onclick: () => api("/api/reveal", { json: { what: "logs" } }).catch(() => {}) }, "Logs")])));
}

// ------------------------------------------------------------------------------------ parity

const STATES = { done: "done", wip: "in progress", stock: "in Unreal, not wired up", todo: "not started" };
const HAS = { D: "Depence", C: "Capture", "D C": "both" };

async function loadParity() {
  const holder = $("parity");
  let answer;
  try { answer = await api("/api/parity"); } catch (error) { return holder.replaceChildren(notice(error.message, "error")); }
  const all = Object.values(answer.totals).reduce((sum, count) => sum + count, 0);
  const bar = (counts, total) => el("span", { class: "progress" }, ["done", "wip", "stock"].map((key) => fill(key, total ? counts[key] / total * 100 : 0)));
  put(holder,
    el("p", { class: "muted" }, `What Depence and Capture do, feature by feature, and where Openviz stands on each. ${answer.totals.done} of ${all} are done, so this is a long way from parity; the list is the plan for closing the gap. It is read from docs/parity-tracker.md in the project.`),
    el("div", { class: "totals" }, Object.entries(STATES).map(([key, label]) => el("div", { class: "total" }, el("b", {}, String(answer.totals[key])), el("span", { class: `state ${key}` }, label)))),
    el("h2", {}, "By area"),
    answer.sections.map((section) => {
      const counts = { done: 0, wip: 0, stock: 0, todo: 0 };
      for (const row of section.rows) counts[row.state] += 1;
      return el("details", { class: "area" },
        el("summary", {}, el("span", { class: "name" }, section.name), bar(counts, section.rows.length),
          el("span", { class: "count" }, `${counts.done} done, ${counts.wip} in progress of ${section.rows.length}`)),
        el("div", { class: "tablewrap" }, el("table", {},
          el("thead", {}, el("tr", {}, el("th", {}, "Feature"), el("th", {}, "In"), el("th", { class: "num" }, "Phase"), el("th", {}, "Openviz"), el("th", {}, "Notes"))),
          el("tbody", {}, section.rows.map((row) => el("tr", {}, el("td", { class: "wrap" }, row.feature), el("td", {}, HAS[row.has] || row.has),
            el("td", { class: "num" }, row.phase), el("td", {}, el("span", { class: `state ${row.state}` }, STATES[row.state])), el("td", { class: "wrap" }, row.note)))))));
    }));
}

// --------------------------------------------------------------------------------- run status

let runTimer = 0;

function renderRun(run) {
  state.run = run;
  const bar = $("runbar");
  if (run && run.building && (run.building.running || run.building.error) && !run.running) {
    // The visualizer's code changed since it was built: it is built first, and then starts by itself.
    const b = run.building;
    bar.hidden = false;
    bar.classList.toggle("running", !!b.running);
    const title = b.running ? `Building the visualizer${b.then ? ` before it opens ${bare(b.then)}` : ""}` : "The visualizer could not be built";
    const lines = b.running
      ? [el("div", {}, `Its code has changed since it was last built (${b.reason || "a change"}). This takes a minute or two, the first time after a change longer; it opens by itself when done.`),
         b.step ? el("div", { class: "muted" }, b.step) : null]
      : [el("div", { class: "note" }, b.error)];
    bar.replaceChildren(el("span", { class: "dot" }), el("div", { class: "body" }, el("div", { class: "title" }, title), el("div", { class: "lines" }, lines)),
      el("button", { onclick: () => { bar.hidden = true; } }, "Dismiss"));
    return;
  }
  if (!run || (!run.running && run.exit_code === undefined)) { bar.hidden = true; return; }
  bar.hidden = false;
  bar.classList.toggle("running", run.running);
  const name = bare(run.name);
  let title;
  if (run.running) title = run.editor ? `Editing in the Unreal Editor: ${name}` : run.rendering ? `Rendering ${run.rendering.replace(/\.dmxrec$/, "")} on ${name}` : `Watching: ${name}`;
  else title = run.exit_code === 0 || run.stopped ? `Closed: ${name}` : run.crash ? `${name} crashed.` : `${name} stopped (exit code ${run.exit_code}). The log is under Engine, Logs.`;
  const lines = run.lines.length ? run.lines.map((line) => el("div", { class: line.note ? "note" : "" }, line.text))
    : [run.running ? (run.editor ? "Starting the Unreal Editor. This takes about a minute." : "Starting Unreal. This takes about half a minute.") : ""];
  if (!run.running && run.exit_code && !run.stopped) {
    // A crash: what the log says, and a file to send to whoever looks into it.
    const said = el("span", { class: "muted" });
    if (run.crash && run.crash.message) lines.push(el("div", { class: "note" }, run.crash.message));
    if (run.crash && run.crash.where.length) lines.push(el("div", { class: "note" }, `In Openviz's code: ${run.crash.where.join("  \u2190  ")}`));
    lines.push(el("div", {}, "Saving a report puts the visualizer's log and the crash files of this run into one file among the exports, to send with a word on what you were doing. ",
      el("button", { class: "small", onclick: async (event) => {
        event.currentTarget.disabled = true;
        try {
          const answer = await api("/api/report", { json: {} });
          said.textContent = ` Saved as ${answer.file}.`;
          api("/api/reveal", { json: { what: "exports" } });
        } catch (error) { said.textContent = ` ${error.message}`; }
      } }, "Save a report"), said));
  }
  if (run.rendering && run.running) lines.push(el("div", {}, run.progress
    ? `Frame ${run.progress[0]} of ${run.progress[1]} (${Math.round(100 * run.progress[0] / Math.max(run.progress[1], 1))}%). Every frame is drawn in full, however long it takes; the window closes by itself at the end.`
    : "The recording's first look is held for two seconds while the fixtures get there, then the movie begins."));
  for (const video of run.videos || []) {
    lines.push(el("div", { class: video.error ? "note" : "" }, video.state === "converting" ? `Making an MP4 of ${video.file}\u2026` : `Video saved: ${video.file}. ${video.error || ""}`,
      video.state === "converting" ? null : el("button", { class: "link small", onclick: () => api("/api/reveal", { json: { what: "videos" } }) }, "Show videos")));
  }
  if (run.running && !run.editor && !run.rendering && run.lines.length) lines.push(el("div", {}, "W A S D to move, Q and E down and up, Shift faster. Tap 1 to 9 for a stored view; hold the key to store the present view there; 0 is the start, P saves a picture. T, F and L show the rig flat, as a plot does: from above, from the house, from stage left. The show itself is changed here in Openviz or in the Unreal Editor, and the visualizer follows."));
  if (run.running && run.editor && run.lines.length) lines.push(el("div", {}, "Click a fixture to select it, press W to move it or E to turn it. Universe, address and mode are in the Details panel under Openviz. Changes save themselves; Play shows the rig under DMX."));
  // While watching: the views and pictures the keys give, from here as well.
  let steer = null;
  if (run.running && !run.editor && !run.rendering && run.kind === "show" && run.lines.length) {
    const said = el("span", { class: "muted" });
    const ask = (command, value, label) => el("button", { class: "small", onclick: async () => {
      try { await api("/api/visualizer", { json: { command, value } }); said.textContent = ""; }
      catch (error) { said.textContent = ` ${error.message}`; }
    } }, label);
    const stored = new Set(run.views || []);
    const keep = el("select", { "aria-label": "Store the present view under", onchange: async (event) => {
      if (!event.target.value) return;
      try { await api("/api/visualizer", { json: { command: "store", value: event.target.value } }); said.textContent = ` Stored as view ${event.target.value}.`; }
      catch (error) { said.textContent = ` ${error.message}`; }
      event.target.value = "";
    } }, el("option", { value: "" }, "store this view as\u2026"), [1, 2, 3, 4, 5, 6, 7, 8, 9].map((number) => el("option", { value: String(number) }, `view ${number}${stored.has(number) ? " (replaces it)" : ""}`)));
    steer = el("div", { class: "steer" },
      el("span", { class: "muted" }, "Views "), ask("view", "0", "Start"),
      [1, 2, 3, 4, 5, 6, 7, 8, 9].filter((number) => stored.has(number)).map((number) => ask("view", String(number), String(number))), keep,
      el("span", { class: "muted" }, "  Flat "), ask("view", "top", "From above"), ask("view", "front", "From the house"), ask("view", "side", "From stage left"),
      el("span", { class: "muted" }, "  Pictures "), ask("picture", "", "As on screen"), ask("picture", "3840x2160", "4K"), ask("picture", "7680x4320", "8K"),
      el("button", { class: "link small", onclick: () => api("/api/reveal", { json: { what: "pictures" } }) }, "Show pictures"),
      el("span", { class: "muted" }, "  Video "), run.filming
        ? el("button", { class: "small primary", onclick: async () => { await api("/api/visualizer", { json: { command: "record", value: "stop" } }).catch(() => null); } }, "Stop recording")
        : ask("record", "start", "Record"),
      said);
  }
  bar.replaceChildren(el("span", { class: "dot" }), el("div", { class: "body" }, el("div", { class: "title" }, title), el("div", { class: "lines" }, lines), steer),
    run.running ? el("button", { onclick: async (event) => { event.currentTarget.disabled = true; renderRun(await api("/api/stop", { json: {} })); } }, "Stop")
      : el("button", { onclick: () => { bar.hidden = true; } }, "Dismiss"));
}

function pollRun() {
  clearTimeout(runTimer);
  runTimer = setTimeout(async () => {
    try { renderRun(await api("/api/run")); } catch (error) { return; }
    if (state.run.running || (state.run.building && state.run.building.running)) pollRun();
    else if (!$("view-shows").hidden) loadShows();    // Unreal has closed: show what it changed
  }, 1500);
}

// -------------------------------------------------------------------------------------- start

async function start() {
  try {
    state = await api("/api/state");
  } catch (error) {
    document.querySelector("main").replaceChildren(el("section", {}, notice(error.code === "forbidden"
      ? "This page has to be opened by Openviz itself. Close it and start Openviz again."
      : error.message, "error")));
    return;
  }
  $("remember-words").textContent = ` Stay logged in on ${thisComputer()}`;
  $("remember-where").textContent = `${passwordStore()}, where your other passwords are`;
  // With a window of its own (Windows), Openviz ends when the page has gone: this says it has not.
  setInterval(() => { api("/api/here").catch(() => {}); }, 30000);
  for (const tab of document.querySelectorAll(".tab")) tab.addEventListener("click", () => show(tab.dataset.view));
  $("engine-dot").classList.toggle("off", !!ready());
  warnIfStale();

  const drop = $("drop");
  const file = $("file");
  drop.addEventListener("click", () => file.click());
  drop.addEventListener("keydown", (event) => { if (event.key === "Enter" || event.key === " ") { event.preventDefault(); file.click(); } });
  file.addEventListener("change", () => { if (file.files.length) addFiles([...file.files]); file.value = ""; });
  // A file dropped anywhere on the window is taken, so a near miss does not make the browser open it.
  window.addEventListener("dragover", (event) => { event.preventDefault(); drop.classList.add("over"); });
  window.addEventListener("dragleave", (event) => { if (!event.relatedTarget) drop.classList.remove("over"); });
  window.addEventListener("drop", (event) => {
    event.preventDefault();
    drop.classList.remove("over");
    if (event.dataTransfer && event.dataTransfer.files.length) { show("shows"); addFiles([...event.dataTransfer.files]); }
  });

  $("q").addEventListener("input", () => { clearTimeout(searchTimer); searchTimer = setTimeout(search, 180); });
  $("login").addEventListener("submit", login);
  $("reveal-fixtures").addEventListener("click", () => api("/api/reveal", { json: { what: "fixtures" } }).catch(() => {}));

  let view = "shows";
  try { view = sessionStorage.getItem("view") || view; } catch (error) { /* fine */ }
  show(view);
  search();
  renderRun(state.run);
  if (state.run.running) pollRun();
  // A show edited in Unreal while this window sat in the background shows its changes on return.
  window.addEventListener("focus", async () => {
    if (!$("view-shows").hidden) loadShows();
    try { state.stale = (await api("/api/state")).stale; warnIfStale(); } catch (error) { /* closed */ }
  });
}

// Openviz keeps running the program it was opened with. After an update, say so, once.
function warnIfStale() {
  if (!state.stale || $("stale")) return;
  document.querySelector("main").prepend(el("div", { id: "stale", class: "notice", role: "status" },
    `Openviz has been updated since this window was opened, and is still running the older version. ${onMac() ? "Quit it (Command-Q)" : "Close its window"} and open it again to use the update. Close Unreal first if it is open, and open the show again afterwards.`));
}

start();
