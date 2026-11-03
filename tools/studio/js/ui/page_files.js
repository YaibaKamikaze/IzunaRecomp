// Izuna Mod Studio - Game files: the game's 5099 files by name; copy them
// out, or into a mod (to edit, or to swap).
"use strict";
var Izuna = (globalThis.Izuna = globalThis.Izuna || {});

Izuna.pageFiles = {
  title: "Game files",
  icon: "files",
  query: "",
  folder: "",
  kind: "",
  selected: null,
  render(el) {
    const h = Izuna.h, app = Izuna.app, s = app.state, page = this;
    if (app.needGame(el)) return;
    const files = s.game.files;
    const top = [...new Set(files.map((f) => f.path.split("\\")[0].toLowerCase()))].sort();
    const kinds = { "": "All", gmd: "Models", mot: "Motions", mtn: "Motion sets", emc: "Effects", sty: "Story", bin: "Data" };

    const search = h("input.grow", { placeholder: "Search by path: ryu, char\\ryu_b\\*, *.gmd ...", value: page.query, oninput: () => { page.query = search.value; draw(); } });
    const folderSel = h("select", { onchange: () => { page.folder = folderSel.value; draw(); } },
      h("option", { value: "" }, "Every folder"), top.map((t) => h("option", { value: t, selected: page.folder === t }, t)));
    const kindSel = h("select", { onchange: () => { page.kind = kindSel.value; draw(); } },
      Object.entries(kinds).map(([k, v]) => h("option", { value: k, selected: page.kind === k }, v)));
    const body = h("tbody");
    const count = h("span.muted");
    const aside = h("div.aside");
    let shown = [];

    function matches(f) {
      const p = f.path.toLowerCase();
      if (page.folder && !p.startsWith(page.folder + "\\")) return false;
      if (page.kind && !p.endsWith("." + page.kind)) return false;
      const q = page.query.trim().toLowerCase().replace(/\//g, "\\");
      if (!q) return true;
      if (/[*?]/.test(q)) {
        const re = new RegExp("^" + q.replace(/[.+^${}()|[\]\\]/g, "\\$&").replace(/\*/g, ".*").replace(/\?/g, ".") + "$");
        return re.test(p);
      }
      return q.split(/\s+/).every((w) => p.includes(w) || String(f.id) === w);
    }

    function draw(limit) {
      shown = files.filter(matches);
      const max = limit || 400;
      body.replaceChildren(...shown.slice(0, max).map((f) =>
        h("tr" + (page.selected === f.id ? ".sel" : ""), { onclick: () => { page.selected = f.id; draw(max); detail(f); } },
          h("td.num.muted", f.id), h("td.mono", f.path), h("td", Izuna.GameIndex.kindOf(f.path)),
          h("td.num", f.size ? Izuna.size(f.size) : "-"), h("td", f.dlc ? h("span.tag.dlc", "DLC") : h("span.muted", f.archive)))));
      if (shown.length > max) body.appendChild(h("tr", h("td", { colspan: 5 }, h("button.small", { onclick: () => draw(max + 1000) }, "Show " + Math.min(1000, shown.length - max) + " more"))));
      count.textContent = shown.length + " of " + files.length + " files";
    }

    async function detail(f) {
      const magic = h("span.mono", "...");
      aside.replaceChildren(h("div.card",
        h("h2.mono", { style: { wordBreak: "break-all" } }, f.path),
        h("div.kv",
          h("span", "Id"), h("span", "#" + f.id),
          h("span", "Kind"), h("span", Izuna.GameIndex.kindOf(f.path)),
          h("span", "Size"), h("span", f.size ? f.size.toLocaleString() + " bytes" : "-"),
          h("span", "In"), h("span", f.dlc ? "DLC (not on the disc)" : f.archive + " at 0x" + f.offset.toString(16)),
          h("span", "Starts"), magic),
        h("div.row",
          h("button.primary", { disabled: f.dlc || null, onclick: () => Izuna.act(() => addToMod([f])) }, "Add to a mod..."),
          h("button", { disabled: f.dlc || null, onclick: () => Izuna.act(() => swap(f)) }, "Swap in a mod..."),
          h("button", { disabled: f.dlc || null, onclick: () => Izuna.act(() => saveOne(f)) }, "Save a copy...")),
        h("p.muted", { style: { marginTop: "12px", fontSize: "12px" } },
          "Add to a mod copies it into the mod's files/ folder to edit; the game then loads the mod's copy. Swap loads another of the game's files in its place, with nothing of the game's in the mod.")));
      if (!f.dlc) {
        const head = await s.gameFolder.slice(f.archive, f.offset, 16);
        const text = Array.from(head.slice(0, 8), (b) => (b >= 32 && b < 127 ? String.fromCharCode(b) : ".")).join("");
        magic.textContent = text + "  " + Array.from(head.slice(0, 8), (b) => b.toString(16).padStart(2, "0")).join(" ");
      } else {
        magic.textContent = "-";
      }
    }

    async function addToMod(list) {
      const mod = await app.pickMod("Add " + (list.length === 1 ? list[0].path : list.length + " files") + " to a mod");
      if (!mod) return;
      const dir = await mod.dir();
      let n = 0;
      for (const f of list) {
        if (f.dlc) continue;
        await dir.write("files/" + f.path.replace(/\\/g, "/"), await app.readGameFile(f));
        n++;
      }
      await app.reloadMods();
      Izuna.toast(n + " file(s) copied into " + mod.name + "/files: edit them there.");
    }

    async function swap(f) {
      const target = Izuna.pathInput("", "the game file to load it in place of");
      const mod = await app.pickMod("Swap: " + f.path + " loaded in place of...");
      if (!mod) return;
      const v = await Izuna.dialog("Load " + f.path + " in place of", [h("label.field", h("span", "Game file"), target)],
        [{ label: "Cancel", value: null }, { label: "Add the swap", kind: "primary", value: () => target.value.trim() }]);
      if (!v) return;
      const t = s.game.find(v);
      if (!t) throw new Error(v + " is not one of the game's files");
      mod.replace[t.path.replace(/\\/g, "/")] = f.path.replace(/\\/g, "/");
      await mod.save();
      await app.reloadMods();
      Izuna.toast(mod.name + ": " + t.path + " now loads " + f.path + ".");
    }

    async function saveOne(f) {
      const data = await app.readGameFile(f);
      const leaf = f.path.split("\\").pop();
      if (window.showSaveFilePicker) {
        const handle = await window.showSaveFilePicker({ suggestedName: leaf });
        const w = await handle.createWritable();
        await w.write(data);
        await w.close();
      } else {
        Izuna.download(leaf, data);
      }
      Izuna.toast("Saved " + leaf + ".");
    }

    async function extractShown() {
      const list = shown.filter((f) => !f.dlc);
      if (!list.length) return;
      if (list.length > 300 && !(await Izuna.confirm("Copy " + list.length + " files", "That is " + Izuna.size(list.reduce((n, f) => n + f.size, 0)) + ". Copy them all?", "Copy"))) return;
      const out = await Izuna.Folder.pick("izuna-extract");
      let n = 0;
      for (const f of list) {
        await out.write(f.path.replace(/\\/g, "/"), await app.readGameFile(f));
        if (++n % 50 === 0) count.textContent = "copied " + n + " of " + list.length + "...";
      }
      Izuna.toast("Copied " + n + " file(s) to " + out.name + ", in the game's folders.");
      draw();
    }

    el.append(
      h("h1", "Game files"),
      h("p.lead", "The game's files by the names it gives them. They come from your copy of the game each time; nothing is kept."),
      h("div.row", { style: { marginBottom: "12px" } }, search, kindSel, folderSel,
        h("button", { onclick: () => Izuna.act(extractShown) }, "Copy the shown files..."),
        h("button", { onclick: () => Izuna.act(() => addToMod(shown.filter((f) => !f.dlc).slice(0, 200))) }, "Add the shown to a mod...")),
      h("div.row", { style: { marginBottom: "8px" } }, count),
      h("div.split",
        h("div.main", h("div.table-wrap.scroll", h("table", h("thead", h("tr", h("th", "#"), h("th", "Path"), h("th", "Kind"), h("th", "Size"), h("th", "Archive"))), body))),
        aside));
    draw();
    const sel = page.selected !== null && files[page.selected];
    if (sel) detail(sel);
    else aside.appendChild(h("div.card.muted", "Choose a file to see what it is and copy it."));
  },
};
