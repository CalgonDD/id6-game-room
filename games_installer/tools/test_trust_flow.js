// Execute the real action registry and Trust handler with a small DOM/IPC stub.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const path = require("node:path");
const nodes = new Map();
const calls = [];
const node = id => {
  if (!nodes.has(id)) nodes.set(id, { value: "", checked: false, hidden: false, classList: { toggle() {} }, querySelectorAll() { return []; } });
  return nodes.get(id);
};
const window = {
  goDoAction: async (...args) => { calls.push(args); return ""; },
  goConnect: async (...args) => { calls.push(["connect", ...args]); return ""; },
  goPullDiagnostics: async (...args) => { calls.push(["diagnostics", ...args]); return ""; },
};
const ctx = vm.createContext({ window, document: { getElementById: node, querySelectorAll: () => [] }, console, navigator: { language: "en" } });
const source = fs.readFileSync(path.join(__dirname, "../web/app.js"), "utf8").replace(/boot\(\);\s*$/, "");
vm.runInContext(source, ctx);
vm.runInContext(`
  setBusy = () => {};
  setStatus = () => {};
  t = key => key;
  buildInput = () => { throw new Error('Trust must not rebuild the current form'); };
  wireCoreActions(); wireUninstallAndBackups(); wireDiagnostics();
`, ctx);
for (const kind of ["connect", "inspect", "install", "recover", "unlockempty", "uninstall", "uninstallselected", "deletebackups", "diagnostics"]) {
  vm.runInContext(`
    state.pendingFingerprint = 'SHA256:confirmed';
    state.pendingAction = {
      kind: ${JSON.stringify(kind)},
      input: { Head: '169.254.1.2', Apps: ['GameRoom'], UninstallApps: ['GameRoom'], Fingerprint: '' },
      label: 'GameRoom', sections: { system: true }, crashDump: true, libraries: false
    };
  `, ctx);
  node("action-trust").onclick();
  const [action, input, sections, dump, libraries] = calls.at(-1);
  assert.equal(action, kind);
  assert.equal(input.Fingerprint, "SHA256:confirmed");
  assert.equal(input.Head, "169.254.1.2");
  assert.deepEqual(Array.from(input.UninstallApps), ["GameRoom"]);
  if (kind === "diagnostics") {
    assert.equal(sections.system, true);
    assert.equal(dump, true);
    assert.equal(libraries, false);
  }
}
assert.equal(calls.length, 9);
console.log("Trust resumes all nine actions with the saved input and diagnostics selection.");

vm.runInContext(`
  applyInspection({CanRecover:false,CanUnlockEmpty:true,Result:'empty lock'});
`,ctx);
assert.equal(node('panel-recover').hidden,false);
assert.equal(node('action-recover').hidden,true);
assert.equal(node('action-unlock-empty').hidden,false);
vm.runInContext(`applyInspection({CanRecover:false,CanUnlockEmpty:false,Result:'released'});`,ctx);
assert.equal(node('panel-recover').hidden,true);
console.log('Release-only button appears independently of full install recovery.');
