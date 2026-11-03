// Izuna Mod Studio - Models: the game's .gmd models - what each holds, and
// its textures as pictures. The exporter (glTF, for Blender) and the importer
// come next: the meshes, vertex layouts and the skeleton are being mapped.
"use strict";
var Izuna = (globalThis.Izuna = globalThis.Izuna || {});

Izuna.pageModels = {
  title: "Models",
  icon: "models",
  query: "ryu",
  selected: null,
  render(el) {
    const h = Izuna.h, app = Izuna.app, s = app.state, page = this;
    el.append(h("h1", "Models"),
      h("p.lead", "The game's character, enemy, weapon and stage models (.gmd). See what a model holds and save its textures as pictures. Exporting a model to Blender (glTF) and importing one back - a new model in place of one of the game's - is the next step of the studio."));
    if (app.needGame(el)) return;
    const models = s.game.files.filter((f) => /\.gmd$/i.test(f.path) && !f.dlc);
    const search = h("input", { placeholder: "Search models: ryu, enemy, weapon ...", value: page.query, style: { width: "320px" }, oninput: () => { page.query = search.value; draw(); } });
    const list = h("div.list"), view = h("div.main");
    el.append(h("div.row", { style: { marginBottom: "12px" } }, search, h("span.muted", models.length + " models")),
      h("div.split", h("div", { style: { width: "320px", flex: "none" } }, h("div.scroll", list)), view));

    function draw() {
      const q = page.query.trim().toLowerCase();
      list.replaceChildren(...models.filter((f) => !q || f.path.toLowerCase().includes(q)).slice(0, 500).map((f) =>
        h("div.item" + (page.selected === f.id ? ".on" : ""), { onclick: () => { page.selected = f.id; draw(); open(f); } },
          h("div.mono", { style: { fontSize: "12.5px" } }, f.path.split("\\").pop()), h("div.sub", f.path.split("\\").slice(0, -1).join("\\") + " · " + Izuna.size(f.size)))));
    }

    async function open(f) {
      view.replaceChildren(h("div.card.muted", "Reading " + f.path + "..."));
      const d = await app.readGameFile(f);
      const m = Izuna.model.inspect(d);
      const grid = h("div.grid");
      const pngOf = (t) => {
        const c = h("canvas", { width: t.w, height: t.h });
        c.getContext("2d").putImageData(new ImageData(Izuna.model.texturePixels(d, t), t.w, t.h), 0, 0);
        return c;
      };
      const save = async (t) => {
        const c = pngOf(t);
        const blob = await new Promise((res) => c.toBlob(res, "image/png"));
        return new Uint8Array(await blob.arrayBuffer());
      };
      view.replaceChildren(h("div.card",
        h("h2.mono", f.path),
        h("div.kv",
          h("span", "Size"), h("span", Izuna.size(f.size)),
          h("span", "Holds"), h("span", m.bundle ? [m.textures.length + " textures", (m.counts.VBUF || 0) + " vertex buffers", (m.counts.IBUF || 0) + " index buffers", "parts: " + (m.parts || []).join(", ")].join(" · ") : "no resource bundle (" + m.magic + ")")),
        h("div.row",
          h("button", { disabled: !m.textures.length || null, onclick: () => Izuna.act(async () => {
            const out = await Izuna.Folder.pick("izuna-model-textures");
            const leaf = f.path.split("\\").pop().replace(/\.gmd$/i, "");
            for (const t of m.textures) {
              try { await out.write(leaf + "_" + t.index + "_" + t.w + "x" + t.h + ".png", await save(t)); } catch (e) { /* (a format not shown yet) */ }
            }
            Izuna.toast("Saved " + m.textures.length + " textures to " + out.name + ".");
          }) }, "Save all its textures..."),
          h("button", { disabled: true, title: "Next: the meshes, vertex layouts and skeleton are being mapped" }, "Export to glTF (soon)"),
          h("button", { disabled: true, title: "After the exporter" }, "Import a model (soon)"))),
        h("h3", "Textures"), grid,
        h("p.muted", { style: { marginTop: "10px" } }, "To replace one of these in the game, find it in a texture dump (Textures): the dump names it by the id the game uses."));
      for (const t of m.textures) {
        const cell = h("div.thumb", h("canvas", { width: 1, height: 1 }), h("div.cap", t.w + "x" + t.h + " " + t.name), h("div.cap.muted", "#" + t.index));
        cell.addEventListener("click", () => Izuna.act(async () => Izuna.download(f.path.split("\\").pop().replace(/\.gmd$/i, "") + "_" + t.index + ".png", await save(t), "image/png")));
        cell.title = "Click to save it as a PNG";
        grid.append(cell);
        await new Promise((r) => setTimeout(r, 0));  // (one at a time: the page stays responsive)
        try { cell.firstChild.replaceWith(pngOf(t)); } catch (e) { cell.querySelector(".cap").textContent = e.message; }
      }
    }

    draw();
    const sel = page.selected !== null && s.game.files[page.selected];
    if (sel) open(sel);
    else view.append(h("div.card.muted", "Choose a model."));
  },
};
