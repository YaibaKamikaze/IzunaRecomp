// Izuna Mod Studio - Mods: the mod maker. A list of the mods in the mods'
// folder and an editor for one: its details, the game files it replaces,
// swaps, textures, hidden shaders, its script; checked as the game will
// check it; exported as a ZIP.
"use strict";
var Izuna = (globalThis.Izuna = globalThis.Izuna || {});

Izuna.pageMods = {
  title: "Mods",
  icon: "mods",
  tab: "overview",
  render(el) {
    const h = Izuna.h, app = Izuna.app, s = app.state, page = this;
    if (app.needMods(el)) return;
    const mod = s.mods.find((m) => m.folder === s.modOpen) || s.mods[0] || null;
    if (mod) s.modOpen = mod.folder;
    const redraw = () => app.go("mods");

    const list = h("div.list", s.mods.map((m) => {
      const checks = m.check(s.game);
      const bad = checks.some((c) => c.level === "error"), warn = checks.some((c) => c.level === "warn");
      return h("div.item" + (m === mod ? ".on" : ""), { onclick: () => { s.modOpen = m.folder; redraw(); } },
        h("div", m.name, " ", bad ? h("span.bad", "●") : warn ? h("span.warn", "●") : null),
        h("div.sub", [m.files.length && m.files.length + " file(s)", Object.keys(m.replace).length && Object.keys(m.replace).length + " swap(s)",
          m.textures.length && m.textures.length + " texture(s)", m.script && "script"].filter(Boolean).join(" · ") || "empty"));
    }));

    el.append(h("div.row", h("h1.grow", "Mods"),
      h("button.primary", { onclick: () => Izuna.act(async () => {
        const name = h("input", { placeholder: "Ryu's New Look" });
        const author = h("input", { value: localStorage.getItem("izuna.author") || "" });
        const v = await Izuna.dialog("A new mod", [h("label.field", h("span", "Name"), name), h("label.field", h("span", "Your name (as its author)"), author)],
          [{ label: "Cancel", value: null }, { label: "Make it", kind: "primary", value: () => ({ name: name.value.trim(), author: author.value.trim() }) }]);
        if (!v || !v.name) return;
        localStorage.setItem("izuna.author", v.author);
        const m = await app.createMod(v.name);
        s.modOpen = m.folder;
        page.tab = "overview";
        redraw();
      }) }, "New mod"),
      h("button", { onclick: () => Izuna.act(async () => { await app.reloadMods(); redraw(); }) }, "Reload")),
      h("p.lead", "Each mod is a folder in " + s.modsFolder.name + ". When one is ready, copy its folder into Mods/ on the console, or export it as a ZIP to share."));

    if (!mod) {
      el.append(h("div.card.empty", "No mods yet: make one with New mod."));
      return;
    }

    const editor = h("div.main");
    el.append(h("div.split", h("div", { style: { width: "250px", flex: "none" } }, list), editor));

    const tabs = [["overview", "Overview"], ["files", "Files", mod.files.length], ["swaps", "Swaps", Object.keys(mod.replace).length],
      ["textures", "Textures", mod.textures.length], ["script", "Script"], ["drawing", "Drawing", mod.hideShaders.length]];
    const body = h("div");
    editor.append(
      h("div.row", h("h2.grow", { style: { margin: 0, fontSize: "20px" } }, mod.name), h("span.muted.mono", mod.folder + "/")),
      h("div.tabs", tabs.map(([k, label, n]) => h("button" + (page.tab === k ? ".on" : ""), { onclick: () => { page.tab = k; redraw(); } }, label, n ? h("span.count", n) : null))),
      body);
    const save = async (msg) => {
      await mod.save();
      await app.reloadMods();
      if (msg) Izuna.toast(msg);
      redraw();
    };
    page["tab_" + page.tab](body, mod, save);
  },

  // --- Overview: its details, the checks, export ---------------------------------------
  tab_overview(body, mod, save) {
    const h = Izuna.h, app = Izuna.app, s = app.state;
    const field = (key, label, multi) => {
      const input = multi ? h("textarea", { rows: 4 }, mod.info[key] || "") : h("input", { value: mod.info[key] || "" });
      input.addEventListener("change", () => { mod.info[key] = input.value; save("Saved."); });
      return h("label.field", h("span", label), input);
    };
    const checks = mod.check(s.game);
    body.append(h("div.split",
      h("div.main",
        field("name", "Name (as the hub's MODS page shows it)"),
        h("div.row", h("div.grow", field("author", "Author")), h("div", { style: { width: "160px" } }, field("version", "Version"))),
        field("description", "Description", true),
        h("div.row",
          h("button.primary", { onclick: () => Izuna.act(async () => {
            const zip = Izuna.zip(await mod.entries());
            Izuna.download(mod.folder + ".zip", zip, "application/zip");
            Izuna.toast("Exported " + mod.folder + ".zip (" + Izuna.size(zip.length) + "): unzip it into Mods/ on the console.");
          }) }, "Export as ZIP"),
          h("button.danger", { onclick: () => Izuna.act(async () => {
            if (!(await Izuna.confirm("Delete " + mod.name, "The folder " + mod.folder + " and everything in it is deleted from this PC.", "Delete"))) return;
            await s.modsFolder.remove(mod.folder);
            s.modOpen = null;
            await app.reloadMods();
            app.go("mods");
          }) }, "Delete the mod"))),
      h("div.aside",
        h("h3", { style: { marginTop: 0 } }, "Checked as the game will"),
        s.game ? null : h("p.warn", "Choose your copy of the game (Start) to check the game files it names."),
        h("div.checks", checks.map((c) => h("div.check." + c.level, c.text))))));
  },

  // --- Files: game files it brings ------------------------------------------------------
  tab_files(body, mod, save) {
    const h = Izuna.h, app = Izuna.app, s = app.state;
    const rows = mod.files.map((f) => {
      const g = s.game && s.game.find(f.path);
      const ratio = g && g.size ? f.size / g.size : 0;
      return h("tr",
        h("td.mono", f.path),
        h("td.num", Izuna.size(f.size)),
        h("td.num.muted", g ? Izuna.size(g.size) : "-"),
        h("td", !s.game ? h("span.muted", "?") : !g ? h("span.bad", "not a game file") : ratio > 3 ? h("span.warn", ratio.toFixed(1) + "x the game's") : h("span.good", "ok")),
        h("td", h("button.small.danger", { onclick: () => Izuna.act(async () => {
          if (!(await Izuna.confirm("Remove " + f.path, "Its copy in files/ is deleted.", "Remove"))) return;
          await (await mod.dir()).remove("files/" + f.rel);
          await save("Removed.");
        }) }, "Remove")));
    });
    body.append(
      h("p.muted", "The game loads these in place of its own (by their path in files/). Edit them where they are: " + mod.folder + "/files/."),
      h("div.row", { style: { marginBottom: "12px" } },
        h("button.primary", { onclick: () => Izuna.act(async () => {
          if (!s.game) throw new Error("choose your copy of the game first (Start)");
          const path = Izuna.pathInput();
          const v = await Izuna.dialog("Copy a game file into the mod", [h("p.muted", "To edit it: the copy goes in files/ by its path."), h("label.field", h("span", "Game file"), path)],
            [{ label: "Cancel", value: null }, { label: "Copy it", kind: "primary", value: () => path.value.trim() }]);
          if (!v) return;
          const f = s.game.find(v);
          if (!f) throw new Error(v + " is not one of the game's files");
          await (await mod.dir()).write("files/" + f.path.replace(/\\/g, "/"), await app.readGameFile(f));
          await save("Copied " + f.path + ".");
        }) }, "Add a game file..."),
        h("button", { onclick: () => Izuna.act(async () => {
          const input = h("input", { type: "file" });
          const picked = await new Promise((res) => { input.onchange = () => res(input.files[0]); input.click(); });
          if (!picked) return;
          const path = Izuna.pathInput("", "the game file it replaces");
          const v = await Izuna.dialog("Use " + picked.name + " as a game file", [h("label.field", h("span", "It replaces"), path)],
            [{ label: "Cancel", value: null }, { label: "Add it", kind: "primary", value: () => path.value.trim() }]);
          if (!v) return;
          const f = s.game ? s.game.find(v) : null;
          if (s.game && !f) throw new Error(v + " is not one of the game's files");
          const target = (f ? f.path : v).replace(/\\/g, "/");
          await (await mod.dir()).write("files/" + target, new Uint8Array(await picked.arrayBuffer()));
          await save(picked.name + " replaces " + target + ".");
        }) }, "Import a file from this PC..."),
        h("button", { onclick: () => app.go("files") }, "Browse the game's files")),
      rows.length ? h("div.table-wrap", h("table", h("thead", h("tr", h("th", "Game file"), h("th", "Mod's"), h("th", "Game's"), h("th", "Check"), h("th", ""))), h("tbody", rows)))
        : h("div.card.empty", "No game files yet."));
  },

  // --- Swaps: [replace] ----------------------------------------------------------------
  tab_swaps(body, mod, save) {
    const h = Izuna.h, s = Izuna.app.state;
    const target = Izuna.pathInput("", "the game file to replace (char\\ryu_b\\ryu_b.gmd)");
    const source = Izuna.pathInput("", "the game file to load instead (char\\ryu_c\\ryu_c.gmd)");
    const rows = Object.entries(mod.replace).map(([t, src]) => {
      const sf = s.game && s.game.find(src);
      return h("tr", h("td.mono", t), h("td.muted", "loads"), h("td.mono", src),
        h("td", sf && sf.dlc ? h("span.bad", "DLC: not usable") : ""),
        h("td", h("button.small.danger", { onclick: () => Izuna.act(async () => { delete mod.replace[t]; await save("Removed the swap."); }) }, "Remove")));
    });
    body.append(
      h("p.muted", "A swap loads one of the game's own files in place of another: a costume for Ryu's model, one enemy for another. The mod holds no game files: the console copies the file out of its own archive."),
      h("div.card", { style: { marginBottom: "14px" } },
        h("div.row", h("div.grow", target), h("span.muted", "loads"), h("div.grow", source),
          h("button.primary", { onclick: () => Izuna.act(async () => {
            const t = s.game ? s.game.find(target.value) : null, src = s.game ? s.game.find(source.value) : null;
            if (s.game && (!t || !src)) throw new Error((t ? source.value : target.value) + " is not one of the game's files");
            if (src && src.dlc) throw new Error(src.path + " is a DLC file: only the disc's files can be swapped in");
            mod.replace[(t ? t.path : target.value).replace(/\\/g, "/")] = (src ? src.path : source.value).replace(/\\/g, "/");
            await save("Added the swap.");
          }) }, "Add"))),
      rows.length ? h("div.table-wrap", h("table", h("tbody", rows))) : h("div.card.empty", "No swaps yet."));
  },

  // --- Textures --------------------------------------------------------------------------
  tab_textures(body, mod, save) {
    const h = Izuna.h, app = Izuna.app;
    const grid = h("div.grid");
    (async () => {
      const dir = await mod.dir();
      for (const t of mod.textures) {
        const img = h("img", { style: { width: "100%", aspectRatio: "1", objectFit: "contain", background: "repeating-conic-gradient(#2a2a30 0 25%, #1c1c21 0 50%) 0 0 / 16px 16px", borderRadius: "4px" } });
        img.src = URL.createObjectURL(await dir.file(t.rel));
        grid.append(h("div.thumb", img, h("div.cap", { title: t.id }, t.id),
          h("button.small.danger", { style: { marginTop: "6px" }, onclick: () => Izuna.act(async () => {
            if (!(await Izuna.confirm("Remove " + t.id, "The picture is deleted from the mod.", "Remove"))) return;
            await dir.remove(t.rel);
            await save("Removed.");
          }) }, "Remove")));
      }
    })();
    body.append(
      h("p.muted", "Pictures drawn in place of the game's textures, named by the texture's id. Find a texture in a texture dump (Textures) and choose Replace in a mod: its picture comes here to edit (" + mod.folder + "/textures/). Up to 2048 x 2048, any size - a bigger picture is a sharper texture."),
      h("div.row", { style: { marginBottom: "12px" } },
        h("button.primary", { onclick: () => app.go("textures") }, "Open a texture dump"),
        h("button", { onclick: () => Izuna.act(async () => {
          const input = h("input", { type: "file", accept: "image/png,image/jpeg,.tga,.bmp" });
          const picked = await new Promise((res) => { input.onchange = () => res(input.files[0]); input.click(); });
          if (!picked) return;
          const guess = picked.name.replace(/\.[^.]+$/, "").toLowerCase();
          const id = h("input.path", { value: Izuna.texture.isId(guess) ? guess : "", placeholder: "512x256-18-0123456789abcdef" });
          const v = await Izuna.dialog("Import " + picked.name, [h("label.field", h("span", "The texture's id it replaces"), id)],
            [{ label: "Cancel", value: null }, { label: "Import", kind: "primary", value: () => id.value.trim().toLowerCase() }]);
          if (!v) return;
          if (!Izuna.texture.isId(v)) throw new Error(v + " is not a texture's id (<w>x<h>-<format>-<hash>)");
          const ext = (picked.name.match(/\.[^.]+$/) || [".png"])[0].toLowerCase();
          await (await mod.dir()).write("textures/" + v + ext, new Uint8Array(await picked.arrayBuffer()));
          await save("Imported.");
        }) }, "Import a picture...")),
      mod.textures.length ? grid : h("div.card.empty", "No textures yet."));
  },

  // --- Script ----------------------------------------------------------------------------
  tab_script(body, mod, save) {
    const h = Izuna.h;
    const path = (mod.info.script || "scripts/main.lua").replace(/^scripts\//, "");
    const gutter = h("div.gutter");
    const area = h("textarea", { spellcheck: "false", wrap: "off" });
    const lines = () => { gutter.textContent = Array.from({ length: area.value.split("\n").length }, (_, i) => i + 1).join("\n"); };
    area.addEventListener("input", () => { lines(); dirty = true; status.textContent = "not saved"; });
    area.addEventListener("scroll", () => { gutter.scrollTop = area.scrollTop; });
    area.addEventListener("keydown", (e) => {
      if (e.key === "Tab") {
        e.preventDefault();
        const a = area.selectionStart;
        area.setRangeText("  ", a, area.selectionEnd, "end");
        lines();
      } else if (e.key === "s" && (e.ctrlKey || e.metaKey)) {
        e.preventDefault();
        doSave();
      }
    });
    let dirty = false;
    const status = h("span.muted");
    const doSave = () => Izuna.act(async () => {
      await (await mod.dir()).write("scripts/" + path, area.value);
      dirty = false;
      status.textContent = "saved";
      if (!mod.scripts.includes(path)) await save("Saved scripts/" + path + ".");
      else Izuna.toast("Saved scripts/" + path + ".");
    });
    const templates = Izuna.pageScripting.templates;
    const tsel = h("select", h("option", { value: "" }, "Insert an example..."), Object.keys(templates).map((k) => h("option", { value: k }, k)));
    tsel.addEventListener("change", () => {
      if (!tsel.value) return;
      area.setRangeText(templates[tsel.value], area.selectionStart, area.selectionEnd, "end");
      tsel.value = "";
      lines();
      dirty = true;
      status.textContent = "not saved";
      area.focus();
    });
    body.append(
      h("div.row", { style: { marginBottom: "10px" } },
        h("span.mono", "scripts/" + path), status, h("div.grow"), tsel,
        h("button", { onclick: () => Izuna.app.go("scripting") }, "The API"),
        h("button.primary", { onclick: doSave }, "Save (Ctrl+S)")),
      h("div.editor", gutter, area),
      h("p.muted", { style: { marginTop: "8px" } }, "Runs when the game starts; on_frame() then runs every frame. Errors go to the game's log as [script] " + mod.name + ": ... with the line."));
    (async () => {
      const dir = await mod.dir();
      area.value = (await dir.exists("scripts/" + path)) ? await dir.text("scripts/" + path)
        : "-- " + mod.name + "\n-- Runs once when the game starts; on_frame() then runs every frame.\n\nfunction on_frame()\nend\n";
      lines();
    })();
    window.onbeforeunload = () => (dirty ? "The script is not saved." : undefined);
  },

  // --- Drawing: hidden shaders ------------------------------------------------------------
  tab_drawing(body, mod, save) {
    const h = Izuna.h;
    const input = h("input.path", { placeholder: "0123456789abcdef", style: { width: "260px" } });
    body.append(
      h("p.muted", "Draws made with these shaders are left out: an effect, a flash, a full-screen filter. A shader's hash is in the Kasumi frame capture's draw log (the overlay's Capture frame). To hide a HUD element instead, give its texture a transparent picture (Textures)."),
      h("div.row", { style: { marginBottom: "12px" } }, input,
        h("button.primary", { onclick: () => Izuna.act(async () => {
          const v = input.value.trim().toLowerCase();
          if (!/^[0-9a-f]{16}$/.test(v)) throw new Error("a shader's hash is 16 hex digits");
          mod.info.hide_shaders = [...new Set([...mod.hideShaders, v])];
          await save("Added.");
        }) }, "Hide this shader")),
      mod.hideShaders.length ? h("div.table-wrap", h("table", h("tbody", mod.hideShaders.map((x) => h("tr", h("td.mono", x),
        h("td", h("button.small.danger", { onclick: () => Izuna.act(async () => {
          mod.info.hide_shaders = mod.hideShaders.filter((y) => y !== x);
          await save("Removed.");
        }) }, "Remove"))))))) : h("div.card.empty", "No hidden shaders."));
  },
};
