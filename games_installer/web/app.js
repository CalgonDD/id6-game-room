// ID6 App Installer - front end. Talks to the Go side only through the
// goXxx() functions main.go binds (each returns a Promise) and the single
// window.__ipcEvent(kind, payload) callback Go pushes results through.
// All UI state (busy, detected cars, accepted fingerprint, pending trust
// action) lives here - the Go side is stateless per call.

const LANGUAGES = [
  { code: "en", name: "English" },
  { code: "ru", name: "Русский" },
  { code: "es", name: "Español" },
  { code: "fr", name: "Français" },
  { code: "de", name: "Deutsch" },
  { code: "it", name: "Italiano" },
  { code: "pt", name: "Português" },
  { code: "nl", name: "Nederlands" },
  { code: "pl", name: "Polski" },
  { code: "tr", name: "Türkçe" },
  { code: "zh-Hans", name: "简体中文" },
  { code: "ja", name: "日本語" },
];

// Error/result codes the Go side returns directly (not through an SSH
// round-trip), so they can be localized exactly instead of as raw English.
const ERROR_CODES = {
  err_need_ips: "errNeedIps",
  err_need_custom_size: "errNeedCustomSize",
  err_need_diag_choice: "errNeedDiagChoice",
  err_car_busy: "errCarBusy",
};

const state = {
  theme: "dark",
  lang: "en",
  strings: {},
  fallbackStrings: {},
  busy: false,
  detected: [],
  fingerprint: "",          // the fingerprint this session has accepted
  pendingFingerprint: "",   // shown, not yet accepted
  pendingAction: null,      // Saved action kind, input and choices, retried after Trust
  ready: false,             // Check compatibility/Install succeeded, nothing changed since
};

const el = (id) => document.getElementById(id);

// ---------------- i18n ----------------

function t(key, vars) {
  let s = state.strings[key];
  if (s === undefined) s = state.fallbackStrings[key];
  if (s === undefined) return key;
  if (vars) for (const k in vars) s = s.replace("{" + k + "}", vars[k]);
  return s;
}

async function loadLanguage(code) {
  const resp = await fetch("i18n/" + code + ".json");
  if (!resp.ok) throw new Error("missing language file " + code);
  return resp.json();
}

async function applyLanguage(code) {
  if (!LANGUAGES.some((l) => l.code === code)) code = "en";
  state.lang = code;
  document.documentElement.lang = code;
  state.fallbackStrings = await loadLanguage("en");
  state.strings = code === "en" ? state.fallbackStrings : await loadLanguage(code).catch(() => ({}));
  document.querySelectorAll("[data-i18n]").forEach((node) => {
    node.textContent = t(node.getAttribute("data-i18n"));
  });
  document.querySelectorAll("[data-i18n-title]").forEach((node) => {
    node.title = t(node.getAttribute("data-i18n-title"));
  });
  el("check-remember").title = t("rememberTooltip");
  renderLanguageMenu();
  renderAppLists();
  renderDiagSections();
}

function renderLanguageMenu() {
  const list = el("language-list");
  list.innerHTML = "";
  for (const lang of LANGUAGES) {
    const btn = document.createElement("button");
    btn.className = "menu-item" + (lang.code === state.lang ? " selected" : "");
    btn.innerHTML = '<span class="dot"></span><span></span>';
    btn.querySelector("span:last-child").textContent = lang.name;
    btn.onclick = () => {
      closeMenus();
      applyLanguage(lang.code);
      window.goSetLanguage(lang.code);
    };
    list.appendChild(btn);
  }
}

// ---------------- theme ----------------

function applyTheme(theme) {
  state.theme = theme === "light" ? "light" : "dark";
  document.body.setAttribute("data-theme", state.theme);
  document.querySelectorAll("#menu-view [data-theme-choice]").forEach((btn) => {
    btn.classList.toggle("selected", btn.getAttribute("data-theme-choice") === state.theme);
  });
}

// ---------------- menu bar ----------------

function closeMenus() {
  document.querySelectorAll(".menu.open").forEach((m) => m.classList.remove("open"));
}

function wireMenus() {
  for (const menu of [el("menu-file"), el("menu-view")]) {
    menu.querySelector(".menu-btn").onclick = (e) => {
      const wasOpen = menu.classList.contains("open");
      closeMenus();
      if (!wasOpen) menu.classList.add("open");
      e.stopPropagation();
    };
  }
  document.addEventListener("click", closeMenus);
  document.querySelectorAll("#menu-view [data-theme-choice]").forEach((btn) => {
    btn.onclick = () => {
      closeMenus();
      applyTheme(btn.getAttribute("data-theme-choice"));
      window.goSetTheme(state.theme);
    };
  });
  el("action-quit").onclick = () => window.goQuit();
}

// ---------------- apps / diagnostics lists ----------------

let appList = []; // [{Name, Display}]
let diagSectionList = []; // [{Key, Label}]

// renderAppLists (re)builds the install/uninstall checkbox lists. Called
// again on every language switch (to relabel them), so any boxes already
// present keep whatever the user ticked/unticked rather than resetting to
// the first-render default.
function renderAppLists() {
  for (const [containerId, prefix] of [["install-apps", "install"], ["uninstall-apps", "uninstall"]]) {
    const container = el(containerId);
    const previous = {};
    container.querySelectorAll("input[type=checkbox]").forEach((i) => (previous[i.dataset.app] = i.checked));
    const hasPrevious = Object.keys(previous).length > 0;
    container.innerHTML = "";
    for (const app of appList) {
      const label = document.createElement("label");
      label.className = "check";
      // First render: install defaults all ticked, uninstall starts empty.
      const checked = hasPrevious ? !!previous[app.Name] : prefix === "install";
      label.innerHTML = `<input type="checkbox" data-app="${app.Name}"${checked ? " checked" : ""}><span></span>`;
      label.querySelector("span").textContent = app.Display;
      label.querySelector("input").onchange = updateButtonStates;
      container.appendChild(label);
    }
  }
}

// renderDiagSections: same re-render-keeps-state rule as renderAppLists.
function renderDiagSections() {
  const container = el("diag-sections");
  const known = {}; // translated label, falling back to the Go-sent label
  for (const s of diagSectionList) known[s.Key] = t("diag_" + s.Key) !== "diag_" + s.Key ? t("diag_" + s.Key) : s.Label;
  const previous = {};
  container.querySelectorAll("input[type=checkbox]").forEach((i) => (previous[i.dataset.diag] = i.checked));
  const hasPrevious = Object.keys(previous).length > 0;
  container.innerHTML = "";
  for (const s of diagSectionList) {
    const label = document.createElement("label");
    label.className = "check";
    const checked = hasPrevious ? !!previous[s.Key] : true; // all start ticked
    label.innerHTML = `<input type="checkbox" data-diag="${s.Key}"${checked ? " checked" : ""}><span></span>`;
    label.querySelector("span").textContent = known[s.Key];
    container.appendChild(label);
  }
}

function checkedApps(prefix) {
  return Array.from(document.querySelectorAll('#' + prefix + '-apps input[type=checkbox]:checked')).map((i) => i.dataset.app);
}

// ---------------- status / busy ----------------

function setStatus(text) {
  el("status-text").textContent = text;
}

function setBusy(busy) {
  state.busy = busy;
  el("spinner").hidden = !busy;
  updateButtonStates();
}

function updateButtonStates() {
  const busy = state.busy;
  document.querySelectorAll(".btn").forEach((b) => (b.disabled = busy));
  if (!busy) {
    el("action-install").disabled = checkedApps("install").length === 0;
    el("action-uninstall-selected").disabled = checkedApps("uninstall").length === 0;
    el("action-add-games").disabled = checkedOwn("yours-checked").length === 0;
    el("action-remove-games").disabled = checkedOwn("car-games").length === 0;
  }
}

// ---------------- building the action input ----------------

function buildInput() {
  const resolution = document.querySelector('input[name="res"]:checked');
  return {
    Head: el("field-head").value.trim(),
    Gateway: el("field-gateway").value.trim(),
    Local: state.local || "",
    Version: el("field-version").value.trim(),
    Password: el("field-password").value,
    Fingerprint: state.fingerprint,
    Apps: checkedApps("install"),
    UninstallApps: checkedApps("uninstall"),
    Display: resolution ? resolution.value : "",
    Width: parseInt(el("field-width").value, 10) || 0,
    Height: parseInt(el("field-height").value, 10) || 0,
    Remember: el("field-remember").checked,
  };
}

function resetConnectionState() {
  state.fingerprint = "";
  state.pendingFingerprint = "";
  state.pendingAction = null;
  state.ready = false;
  el("panel-fingerprint").hidden = true;
  el("panel-recover").hidden = true;
  el("field-fingerprint").value = "";
}

// ---------------- result handling (mirrors the old applyResult) ----------------

function applyInspection(result) {
  if (result.CarGamesListed) renderCarGames(result.CarGames || []);
  state.ready = !!result.Ready;
  el("panel-recover").hidden = !(result.CanRecover || result.CanUnlockEmpty);
  el("action-recover").hidden = !result.CanRecover;
  el("action-unlock-empty").hidden = !result.CanUnlockEmpty;
  const lines = [];
  if (result.Version) lines.push(result.Version);
  if (result.Display) lines.push(t("statusDisplay", { value: result.Display }));
  if (result.Touch) lines.push(result.Touch);
  if (result.Result) lines.push(result.Result);
  if (result.Fingerprint && !state.fingerprint) {
    state.pendingFingerprint = result.Fingerprint;
    el("field-fingerprint").value = result.Fingerprint;
    el("panel-fingerprint").hidden = false;
    lines.push(t("statusCheckFingerprint"));
  }
  setStatus(lines.join("\n"));
  updateButtonStates();
}

// ---------------- action wiring ----------------

function wireFindCar() {
  el("action-find").onclick = () => {
    setStatus(t("statusFinding"));
    setBusy(true);
    window.goFindCar();
  };
}

function onFindCarResult(result) {
  setBusy(false);
  if (result.Error) {
    setStatus(result.Error);
    return;
  }
  state.detected = result.Items || [];
  const select = el("field-detected");
  select.innerHTML = "";
  state.detected.forEach((item, i) => {
    const opt = document.createElement("option");
    opt.value = i;
    opt.textContent = item.Gateway + " via " + item.Local;
    select.appendChild(opt);
  });
  if (state.detected.length === 1) {
    select.selectedIndex = 0;
    pickCar(0);
  } else if (state.detected.length > 1) {
    state.local = "";
    el("field-gateway").value = "";
    resetConnectionState();
    select.selectedIndex = -1;
    setStatus(t("statusMultipleGateways"));
  } else {
    setStatus(t("statusNoGateway"));
  }
}

function pickCar(i) {
  const item = state.detected[i];
  if (!item) return;
  state.local = item.Local;
  el("field-gateway").value = item.Gateway;
  if (item.Head) el("field-head").value = item.Head;
  resetConnectionState();
  if (item.Head) setStatus(t("statusFoundBoth"));
  else if (el("field-head").value.trim()) setStatus(t("statusFoundGatewayHaveHead"));
  else setStatus(t("statusFoundGatewayOnly"));
}

function wireConnectionFields() {
  el("field-detected").onchange = (e) => pickCar(parseInt(e.target.value, 10));
  el("field-head").oninput = resetConnectionState;
  el("field-gateway").oninput = resetConnectionState;
  el("field-remember").onchange = () => {
    if (!el("field-remember").checked) window.goClearRemembered();
  };
  document.querySelectorAll('input[name="res"]').forEach((rb) => {
    rb.onchange = () => {
      el("custom-size").hidden = rb.value !== "custom";
      el("panel-recover").hidden = true;
      state.ready = false;
      updateButtonStates();
    };
  });
}

// ACTIONS is the single source of truth for "what does this action kind do
// when (re)started" - every button handler below registers itself here
// instead of being called directly, and action-trust's resume logic looks
// a kind up here too. That used to be a hand-written if/else chain that
// resumed connect/inspect/install/recover but quietly did nothing for
// uninstall/uninstallselected/deletebackups/diagnostics - whatever kind is
// actually pending, this always finds it (or correctly does nothing for an
// unknown one, rather than silently going stale).
const ACTIONS = {};

function runAction(action, label, input) {
  state.ready = false;
  setBusy(true);
  setStatus(label);
  window.goDoAction(action, input).then((code) => {
    if (code) {
      setBusy(false);
      setStatus(t(ERROR_CODES[code] || code));
    }
    // "" means it started; the result arrives via __ipcEvent(action, ...).
  });
}

// Every ACTIONS.* entry takes the `saved` pendingAction it was (re)started
// from: undefined on the original button press, or the exact object Trust
// is resuming. When resuming, it replays saved.input verbatim instead of
// calling buildInput() again - buildInput() reads the form's CURRENT state,
// which may have changed while the fingerprint prompt was up (e.g. the
// uninstall selection), and resuming must act on what was actually
// confirmed, not on whatever happens to be checked now. Fingerprint is the
// one field meant to change between confirm-time and resume-time - that is
// the whole point of the trust flow - so it is refreshed to the now-trusted
// value even on resume, with everything else replayed as-is.
function resumedInput(saved) {
  return saved ? { ...saved.input, Fingerprint: state.fingerprint } : buildInput();
}
function wireCoreActions() {
  ACTIONS.connect = (saved) => {
    const input = resumedInput(saved);
    state.pendingAction = { kind: "connect", input };
    setBusy(true);
    setStatus(t("statusConnecting"));
    window.goConnect(input).then((code) => {
      if (code) {
        setBusy(false);
        setStatus(t(ERROR_CODES[code] || code));
      }
    });
  };
  ACTIONS.inspect = (saved) => {
    const input = resumedInput(saved);
    state.pendingAction = { kind: "inspect", input };
    runAction("inspect", t("statusChecking"), input);
  };
  ACTIONS.install = (saved) => {
    const input = resumedInput(saved);
    state.pendingAction = { kind: "install", input };
    runAction("install", t("statusInstalling"), input);
  };
  ACTIONS.recover = (saved) => {
    const input = resumedInput(saved);
    state.pendingAction = { kind: "recover", input };
    runAction("recover", t("statusInstalling"), input);
  };
  ACTIONS.unlockempty = (saved) => {
    const input = resumedInput(saved);
    state.pendingAction = { kind: "unlockempty", input };
    runAction("unlockempty", t("unlockEmptyBtn"), input);
  };
  el("action-unlock-empty").onclick = () => ACTIONS.unlockempty();
  el("action-connect").onclick = () => ACTIONS.connect();
  el("action-inspect").onclick = () => ACTIONS.inspect();
  el("action-install").onclick = () => ACTIONS.install();
  el("action-recover").onclick = () => ACTIONS.recover();
  el("action-trust").onclick = () => {
    state.fingerprint = state.pendingFingerprint;
    state.pendingFingerprint = "";
    el("panel-fingerprint").hidden = true;
    const action = state.pendingAction;
    state.pendingAction = null;
    if (action && ACTIONS[action.kind]) ACTIONS[action.kind](action);
  };
}

function onConnectResult(result) {
  setBusy(false);
  if (result.Ok) {
    setStatus(result.Result);
    return;
  }
  const lines = [];
  if (result.Result) lines.push(result.Result);
  if (result.Fingerprint && !state.fingerprint) {
    state.pendingFingerprint = result.Fingerprint;
    el("field-fingerprint").value = result.Fingerprint;
    el("panel-fingerprint").hidden = false;
    lines.push(t("statusCheckFingerprint"));
  }
  setStatus(lines.join("\n"));
}

function onDiagnosticsResult(result) {
  const lines = [];
  if (result.Error) lines.push(result.Error);
  if (result.Text) lines.push(result.Text);
  if (result.Fingerprint && !state.fingerprint) {
    state.pendingFingerprint = result.Fingerprint;
    el("field-fingerprint").value = result.Fingerprint;
    el("panel-fingerprint").hidden = false;
    lines.push(t("statusCheckFingerprint"));
  }
  setStatus(lines.join("\n"));
}

function confirmModal(titleKey, bodyKey, vars, onConfirm) {
  el("modal-title").textContent = t(titleKey);
  el("modal-body").textContent = t(bodyKey, vars);
  el("modal-backdrop").hidden = false;
  const close = () => (el("modal-backdrop").hidden = true);
  el("modal-cancel").onclick = close;
  el("modal-confirm").onclick = () => {
    close();
    onConfirm();
  };
}

function wireUninstallAndBackups() {
  ACTIONS.uninstall = (saved) => {
    const input = resumedInput(saved);
    state.pendingAction = { kind: "uninstall", input };
    runAction("uninstall", t("statusRemovingAll"), input);
  };
  ACTIONS.uninstallselected = (saved) => {
    const input = resumedInput(saved);
    // The selected-apps label is likewise captured once up front, not
    // recomputed from the current checkboxes on resume.
    const label = saved ? saved.label : uninstallSelectedNames();
    if (input.UninstallApps.length === 0) return;
    state.pendingAction = { kind: "uninstallselected", input, label };
    runAction("uninstallselected", t("statusRemoving", { apps: label }), input);
  };
  ACTIONS.deletebackups = (saved) => {
    const input = resumedInput(saved);
    state.pendingAction = { kind: "deletebackups", input };
    runAction("deletebackups", t("statusDeletingBackups"), input);
  };
  el("action-uninstall-all").onclick = () => {
    confirmModal("uninstallAllTitle", "uninstallAllBody", null, () => ACTIONS.uninstall());
  };
  el("action-uninstall-selected").onclick = () => {
    if (checkedApps("uninstall").length === 0) return;
    confirmModal("uninstallSelectedTitle", "uninstallSelectedBody",
      { apps: uninstallSelectedNames() }, () => ACTIONS.uninstallselected());
  };
  el("action-delete-backups").onclick = () => {
    confirmModal("deleteBackupsTitle", "deleteBackupsBody", null, () => ACTIONS.deletebackups());
  };
}

function uninstallSelectedNames() {
  return checkedApps("uninstall")
    .map((n) => appList.find((a) => a.Name === n)?.Display || n)
    .join(", ");
}

function wireDiagnostics() {
  ACTIONS.diagnostics = (saved) => {
    const input = resumedInput(saved);
    let sections, crashDump, libraries;
    if (saved) {
      ({ sections, crashDump, libraries } = saved);
    } else {
      sections = {};
      document.querySelectorAll("#diag-sections input[type=checkbox]").forEach((cb) => {
        sections[cb.dataset.diag] = cb.checked;
      });
      crashDump = el("field-dump").checked;
      libraries = false; // the games installer does not copy the car's libraries
    }
    state.pendingAction = { kind: "diagnostics", input, sections, crashDump, libraries };
    setBusy(true);
    setStatus(t("statusPullingDiagnostics"));
    window.goPullDiagnostics(input, sections, crashDump, libraries).then((code) => {
      if (code) {
        setBusy(false);
        setStatus(t(ERROR_CODES[code] || code));
      }
    });
  };
  el("action-diagnostics").onclick = () => ACTIONS.diagnostics();
}

// ---------------- Your games ----------------

function checkedOwn(listId) {
  return Array.from(document.querySelectorAll("#" + listId + " input[type=checkbox]:checked")).map((i) => i.dataset.key);
}

function ownRow(key, checked, enabled, title, lines, bad) {
  const row = document.createElement("label");
  row.className = "own-row" + (bad ? " own-bad" : "");
  const box = document.createElement("input");
  box.type = "checkbox";
  box.dataset.key = key;
  box.checked = checked;
  box.disabled = !enabled;
  box.onchange = updateButtonStates;
  const text = document.createElement("span");
  const head = document.createElement("div");
  head.textContent = title;
  text.appendChild(head);
  for (const [cls, line] of lines) {
    if (!line) continue;
    const d = document.createElement("div");
    d.className = cls;
    d.textContent = line;
    text.appendChild(d);
  }
  row.append(box, text);
  return row;
}

function sizeText(n) {
  if (n >= 1 << 20) return (n / (1 << 20)).toFixed(1) + " MB";
  return Math.max(1, Math.round(n / 1024)) + " KB";
}

function renderChecked(result) {
  const list = el("yours-checked");
  list.innerHTML = "";
  const games = result.Games || [];
  for (const g of games) {
    if (g.Problem) {
      list.appendChild(ownRow(g.Key, false, false, g.File.split(/[\\/]/).pop(), [["own-problem", g.Problem]], true));
    } else {
      list.appendChild(ownRow(g.Key, true, true, g.Title + "  →  " + (g.App || g.Game),
        [["own-file", g.File + " (" + sizeText(g.Size) + ")"], ["own-note", g.Note]], false));
    }
  }
  el("yours-skipped").hidden = !result.Skipped;
  el("yours-skipped").textContent = t("yoursSkipped", { count: result.Skipped || 0 });
  el("yours-checked-group").hidden = games.length === 0;
  const ok = games.filter((g) => !g.Problem).length;
  setStatus(t("yoursCheckedStatus", { ok: ok, bad: games.length - ok }));
  updateButtonStates();
}

function renderCarGames(paths) {
  const list = el("car-games");
  list.innerHTML = "";
  if (paths.length === 0) {
    const p = document.createElement("div");
    p.className = "hint";
    p.textContent = t("carGamesEmpty");
    list.appendChild(p);
  }
  for (const p of paths) {
    const parts = p.split("/");
    list.appendChild(ownRow(p, false, true, parts[parts.length - 1], [["own-file", parts.slice(0, -1).join(" / ")]], false));
  }
  updateButtonStates();
}

function wireYours() {
  const choose = (kind) => {
    if (state.busy) return;
    setStatus(t("statusChoosing"));
    window.goChooseGames(kind).then((result) => {
      if (result.Canceled) {
        setStatus("");
        return;
      }
      renderChecked(result);
    });
  };
  el("action-choose-files").onclick = () => choose("files");
  el("action-choose-folder").onclick = () => choose("folder");
  ACTIONS.addgames = (saved) => {
    const input = saved ? { ...saved.input, Fingerprint: state.fingerprint } : { ...buildInput(), OwnGames: checkedOwn("yours-checked") };
    if (!input.OwnGames || input.OwnGames.length === 0) return;
    state.pendingAction = { kind: "addgames", input };
    runAction("addgames", t("statusAddingGames"), input);
  };
  ACTIONS.listgames = (saved) => {
    const input = resumedInput(saved);
    state.pendingAction = { kind: "listgames", input };
    runAction("listgames", t("statusListingGames"), input);
  };
  ACTIONS.removegames = (saved) => {
    const input = saved ? { ...saved.input, Fingerprint: state.fingerprint } : { ...buildInput(), RemoveGames: checkedOwn("car-games") };
    if (!input.RemoveGames || input.RemoveGames.length === 0) return;
    state.pendingAction = { kind: "removegames", input };
    runAction("removegames", t("statusRemovingGames"), input);
  };
  ACTIONS.deletestates = (saved) => {
    const input = resumedInput(saved);
    state.pendingAction = { kind: "deletestates", input };
    runAction("deletestates", t("statusDeletingStates"), input);
  };
  el("action-delete-states").onclick = () => {
    confirmModal("deleteStatesTitle", "deleteStatesBody", null, () => ACTIONS.deletestates());
  };
  el("action-add-games").onclick = () => ACTIONS.addgames();
  el("action-list-games").onclick = () => ACTIONS.listgames();
  el("action-remove-games").onclick = () => {
    const n = checkedOwn("car-games").length;
    if (n === 0) return;
    confirmModal("removeGamesTitle", "removeGamesBody", { count: n }, () => ACTIONS.removegames());
  };
}

// ---------------- events pushed from Go ----------------

window.__ipcEvent = function (kind, payload) {
  switch (kind) {
    case "findcar":
      onFindCarResult(payload);
      break;
    case "connect":
      onConnectResult(payload);
      break;
    case "progress":
      setStatus(payload.Text);
      break;
    case "diagnostics":
      setBusy(false);
      onDiagnosticsResult(payload);
      break;
    case "inspect":
    case "install":
    case "recover":
    case "unlockempty":
    case "uninstall":
    case "uninstallselected":
    case "deletebackups":
    case "addgames":
    case "listgames":
    case "removegames":
    case "deletestates":
      setBusy(false);
      applyInspection(payload);
      break;
    default:
      break;
  }
};

// ---------------- boot ----------------

// A file dropped on the window used to be opened by the browser inside it, replacing the installer page
// (owner, 2026-11-20: "it disappears straight away"). Drops are caught here and pointed at Choose files.
function wireDropGuard() {
  const stop = (e) => {
    e.preventDefault();
    e.stopPropagation();
  };
  window.addEventListener("dragover", (e) => {
    stop(e);
    if (e.dataTransfer) e.dataTransfer.dropEffect = "none";
  }, true);
  window.addEventListener("drop", (e) => {
    stop(e);
    if (!state.busy) setStatus(t("dropNotSupported"));
  }, true);
}

async function boot() {
  wireDropGuard();
  wireMenus();
  wireFindCar();
  wireConnectionFields();
  wireCoreActions();
  wireUninstallAndBackups();
  wireDiagnostics();
  wireYours();

  const data = await window.goBootstrap();
  appList = data.Apps || [];
  diagSectionList = data.DiagSections || [];
  await applyLanguage(data.Language || navigator.language || "en");
  applyTheme(data.Theme);

  if (data.Remembered) {
    el("field-head").value = data.Head || "";
    el("field-gateway").value = data.Gateway || "";
    el("field-version").value = data.Version || "";
    el("field-password").value = data.Password || "";
    el("field-remember").checked = true;
    const res = data.Display || "";
    const radio = document.querySelector(`input[name="res"][value="${res}"]`);
    if (radio) radio.checked = true;
    el("custom-size").hidden = res !== "custom";
    if (res === "custom") {
      el("field-width").value = data.Width || "";
      el("field-height").value = data.Height || "";
    }
    setStatus(t("statusLoadedSaved"));
  } else {
    document.querySelector('input[name="res"][value=""]').checked = true;
    setStatus(t("statusWelcome"));
  }
  updateButtonStates();
}

boot();
