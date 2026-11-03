// Izuna Mod Studio - the app: its state (the game's folder and file index,
// the mods' folder, the texture dump's folder), the side bar, the pages.
"use strict";
var Izuna = (globalThis.Izuna = globalThis.Izuna || {});

Izuna.app = (function () {
  const h = Izuna.h;
  const state = {
    gameFolder: null, game: null,   // the player's copy of the game, its file index
    modsFolder: null, mods: [],     // the folder of mods (one folder each)
    dumpFolder: null,               // a texture dump (Texture Dump/ from the console)
    page: "start",
    modOpen: null,                  // the mod being edited (its folder name)
  };
  const pages = {};  // name -> {title, icon, render(el)}

  function register(name, page) {
    pages[name] = page;
  }

  function go(name, arg) {
    state.page = name;
    if (arg !== undefined) state.pageArg = arg;
    renderNav();
    const el = document.getElementById("page");
    el.replaceChildren();
    el.scrollTop = 0;
    Promise.resolve(pages[name].render(el, arg)).catch((e) => {
      console.error(e);
      el.appendChild(h("div.banner", String(e.message || e)));
    });
  }

  function renderNav() {
    const nav = document.getElementById("nav");
    nav.replaceChildren(...Object.entries(pages).map(([name, p]) =>
      h("button" + (state.page === name ? ".on" : ""), { onclick: () => go(name) }, Izuna.icon(p.icon), p.title, p.soon ? h("span.soon", "soon") : null)));
    const s = document.getElementById("side-status");
    s.replaceChildren(
      h("div" + (state.game ? ".ok" : ""), state.game ? "Game: " + state.gameFolder.name : "Game: not chosen"),
      h("div" + (state.modsFolder ? ".ok" : ""), state.modsFolder ? "Mods: " + state.modsFolder.name + " (" + state.mods.length + ")" : "Mods: not chosen"),
      h("div", "v0.1 · " + (Izuna.canWrite ? "folders writable" : "read-only browser")));
  }

  // --- The game -----------------------------------------------------------------------

  async function openGame(folder) {
    for (const name of ["default.xex", "load_img.bin", "pack_img.bin"]) {
      if (!(await folder.exists(name))) throw new Error(folder.name + " has no " + name + ": choose the folder of your copy of the game (the one with default.xex and the .ng2 archives)");
    }
    const [xex, load, pack] = await Promise.all(["default.xex", "load_img.bin", "pack_img.bin"].map((n) => folder.read(n)));
    state.game = Izuna.GameIndex.build(xex, load, pack);
    state.gameFolder = folder;
    const list = document.getElementById("game-paths");
    list.replaceChildren(...state.game.files.map((f) => h("option", { value: f.path })));
    if (!(folder instanceof (Izuna.UrlFolder || Object))) await Izuna.remember.set("game", folder);
    renderNav();
  }

  // A game file's bytes, out of its archive.
  async function readGameFile(f) {
    if (f.dlc) throw new Error(f.path + " is a DLC file: not on the disc");
    return state.gameFolder.slice(f.archive, f.offset, f.size);
  }

  // --- The mods -----------------------------------------------------------------------

  async function openMods(folder) {
    state.modsFolder = folder;
    await reloadMods();
    await Izuna.remember.set("mods", folder);
  }

  async function reloadMods() {
    state.mods = state.modsFolder ? await Izuna.Mod.all(state.modsFolder) : [];
    renderNav();
    return state.mods;
  }

  function needGame(el) {
    if (state.game) return false;
    el.appendChild(h("div.card", h("h2", "Choose your copy of the game first"),
      h("p.muted", "The studio reads the game's files from the folder you extracted the disc to (the one with default.xex). Nothing of the game is in the studio."),
      h("button.primary", { onclick: () => go("start") }, "Go to Start")));
    return true;
  }

  function needMods(el) {
    if (state.modsFolder) return false;
    el.appendChild(h("div.card", h("h2", "Choose a folder for your mods"),
      h("p.muted", "Any folder on this PC: each mod is a folder in it. Copy a mod's folder to the console's Mods folder to play it."),
      h("button.primary", { onclick: () => go("start") }, "Go to Start")));
    return true;
  }

  // A mod by its folder name, or a new one.
  async function pickMod(title) {
    if (!state.modsFolder) throw new Error("choose a folder for your mods first (Start)");
    const sel = h("select", state.mods.map((m) => h("option", { value: m.folder }, m.name)), h("option", { value: "" }, "A new mod..."));
    const name = h("input", { placeholder: "The new mod's name" });
    const v = await Izuna.dialog(title, [h("label.field", h("span", "Mod"), sel), h("label.field", h("span", "Or a new mod's name"), name)],
      [{ label: "Cancel", value: null }, { label: "OK", kind: "primary", value: () => ({ folder: sel.value, name: name.value.trim() }) }]);
    if (!v) return null;
    if (v.folder && !v.name) return state.mods.find((m) => m.folder === v.folder);
    return createMod(v.name || "New mod");
  }

  async function createMod(name) {
    const folder = name.replace(/[<>:"/\\|?*\x00-\x1f]/g, "").trim() || "New mod";
    if (state.mods.some((m) => m.folder.toLowerCase() === folder.toLowerCase())) throw new Error("there is a mod folder called " + folder + " already");
    const m = new Izuna.Mod(state.modsFolder, folder);
    m.info.name = name;
    m.info.author = localStorage.getItem("izuna.author") || "";
    m.info.version = "1.0";
    m.info.description = "";
    await m.save();
    await reloadMods();
    return state.mods.find((x) => x.folder === folder);
  }

  async function start() {
    register("start", Izuna.pageStart);
    register("files", Izuna.pageFiles);
    register("mods", Izuna.pageMods);
    register("textures", Izuna.pageTextures);
    register("models", Izuna.pageModels);
    register("scripting", Izuna.pageScripting);
    renderNav();
    window.addEventListener("error", (e) => Izuna.toast(String(e.message), "error"));
    window.addEventListener("unhandledrejection", (e) => Izuna.toast(String((e.reason && e.reason.message) || e.reason), "error"));
    const params = new URLSearchParams(location.search);
    if (params.get("dev-game")) await Izuna.act(() => openGame(new Izuna.UrlFolder(params.get("dev-game"))));
    if (params.get("page")) {
      go(params.get("page"), params.get("arg") || undefined);
      return;
    }
    // The folders of the last session, if the browser still allows them.
    for (const [key, open] of [["game", openGame], ["mods", openMods]]) {
      const f = await Izuna.remember.get(key);
      if (f) state["remembered_" + key] = f;
      if (f && (await f.permitted(false))) await Izuna.act(() => open(f));
    }
    go("start");
  }

  return { state, register, go, renderNav, openGame, openMods, reloadMods, readGameFile, needGame, needMods, pickMod, createMod, start };
})();
