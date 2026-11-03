// Izuna Mod Studio - Textures: a texture dump from the console (the hub's
// MODS page, R2: Save textures -> Texture Dump/), as pictures; one replaced or
// hidden in a mod.
"use strict";
var Izuna = (globalThis.Izuna = globalThis.Izuna || {});

Izuna.pageTextures = {
  title: "Textures",
  icon: "textures",
  filter: "",
  minSize: 0,
  selected: null,
  dumps: null,  // [{name, id, w, h, format, vk}]
  render(el) {
    const h = Izuna.h, app = Izuna.app, s = app.state, page = this;
    const redraw = () => app.go("textures");
    el.append(h("div.row", h("h1.grow", "Textures"),
      h("button.primary", { onclick: () => Izuna.act(async () => {
        s.dumpFolder = await Izuna.Folder.pick("izuna-dump");
        page.dumps = null;
        page.selected = null;
        redraw();
      }) }, s.dumpFolder ? "Open another dump" : "Open a texture dump")),
      h("p.lead", "On the console, turn on Save textures on the hub's MODS page (R2) and play: each texture the game draws is saved once to Texture Dump/ beside Mods/. Copy that folder here and open it, find a texture, and replace or hide it in a mod."));
    if (!s.dumpFolder) {
      el.append(h("div.card.empty", "No texture dump open."));
      return;
    }
    const grid = h("div.grid"), aside = h("div.aside"), count = h("span.muted", "reading...");
    const search = h("input", { placeholder: "Filter: an id, 512x512, DXT5 ...", value: page.filter, style: { width: "300px" }, oninput: () => { page.filter = search.value; draw(); } });
    const sizeSel = h("select", { onchange: () => { page.minSize = +sizeSel.value; draw(); } },
      [[0, "Any size"], [64, "64 and up"], [256, "256 and up"], [512, "512 and up"], [1024, "1024 and up"]].map(([v, l]) => h("option", { value: v, selected: page.minSize === v }, l)));
    el.append(h("div.row", { style: { marginBottom: "12px" } }, search, sizeSel, count),
      h("div.split", h("div.main", grid), aside));

    // Decoded on the way into view, a few at a time.
    const cache = (page.cache = page.cache || new Map());
    async function pixels(d) {
      if (cache.has(d.name)) return cache.get(d.name);
      const bytes = await s.dumpFolder.read(d.name);
      const img = new ImageData(Izuna.texture.decode(d.vk, d.w, d.h, bytes), d.w, d.h);
      if (cache.size > 400) cache.delete(cache.keys().next().value);
      cache.set(d.name, img);
      return img;
    }
    function canvasOf(img) {
      const c = h("canvas", { width: img.width, height: img.height });
      c.getContext("2d").putImageData(img, 0, 0);
      return c;
    }
    const seen = new IntersectionObserver((entries) => {
      for (const e of entries) {
        if (!e.isIntersecting) continue;
        seen.unobserve(e.target);
        const d = e.target._dump;
        pixels(d).then((img) => e.target.querySelector("canvas").replaceWith(canvasOf(img)))
          .catch((err) => { e.target.querySelector(".cap").textContent = String(err.message || err); });
      }
    }, { rootMargin: "400px" });

    function draw() {
      const q = page.filter.trim().toLowerCase();
      const list = page.dumps.filter((d) => Math.max(d.w, d.h) >= page.minSize &&
        (!q || d.id.includes(q) || (d.w + "x" + d.h).includes(q) || Izuna.texture.formatName(d.format).toLowerCase().includes(q)));
      count.textContent = list.length + " of " + page.dumps.length + " textures";
      grid.replaceChildren(...list.slice(0, 600).map((d) => {
        const t = h("div.thumb" + (page.selected === d.name ? ".on" : ""), { onclick: () => { page.selected = d.name; draw(); detail(d); } },
          h("canvas", { width: 1, height: 1 }), h("div.cap", d.w + "x" + d.h + " " + Izuna.texture.formatName(d.format)), h("div.cap.muted", d.id.split("-").pop()));
        t._dump = d;
        seen.observe(t);
        return t;
      }));
      if (list.length > 600) grid.append(h("div.muted", "Showing the first 600: filter to see the rest."));
    }

    async function png(d) {
      const c = canvasOf(await pixels(d));
      return new Uint8Array(await (await new Promise((res) => c.toBlob(res, "image/png"))).arrayBuffer());
    }

    async function detail(d) {
      const preview = h("div.preview", h("canvas", { width: 1, height: 1 }));
      aside.replaceChildren(h("div.card", preview,
        h("div.kv", h("span", "Id"), h("span.mono", { style: { wordBreak: "break-all" } }, d.id),
          h("span", "Size"), h("span", d.w + " x " + d.h), h("span", "Format"), h("span", Izuna.texture.formatName(d.format) + " (Xenos " + d.format + ")")),
        h("div.row",
          h("button.primary", { onclick: () => Izuna.act(async () => {
            const mod = await app.pickMod("Replace " + d.id + " in a mod");
            if (!mod) return;
            await (await mod.dir()).write("textures/" + d.id + ".png", await png(d));
            await app.reloadMods();
            Izuna.toast("Its picture is in " + mod.folder + "/textures/" + d.id + ".png: edit it there (any size up to 2048).");
          }) }, "Replace in a mod..."),
          h("button", { onclick: () => Izuna.act(async () => {
            const mod = await app.pickMod("Hide " + d.id + " in a mod");
            if (!mod) return;
            const c = h("canvas", { width: Math.min(d.w, 64), height: Math.min(d.h, 64) });  // (fully transparent)
            const blob = await new Promise((res) => c.toBlob(res, "image/png"));
            await (await mod.dir()).write("textures/" + d.id + ".png", new Uint8Array(await blob.arrayBuffer()));
            await app.reloadMods();
            Izuna.toast(mod.name + ": the texture is drawn transparent - hidden.");
          }) }, "Hide in a mod..."),
          h("button", { onclick: () => Izuna.act(async () => Izuna.download(d.id + ".png", await png(d), "image/png")) }, "Save PNG"),
          h("button", { onclick: () => { navigator.clipboard.writeText(d.id); Izuna.toast("Copied the id."); } }, "Copy id")),
        h("p.muted", { style: { marginTop: "10px", fontSize: "12px" } }, "Channels as the texture holds them: a picture for it is read the same way. Keep its alpha where the game's has it (cut-outs, glows).")));
      preview.firstChild.replaceWith(canvasOf(await pixels(d)));
    }

    (async () => {
      if (!page.dumps) {
        const names = (await s.dumpFolder.list()).filter((e) => e.kind === "file").map((e) => e.name);
        page.dumps = names.map((n) => Object.assign({ name: n }, Izuna.texture.parseDumpName(n))).filter((d) => d.id);
        page.dumps.sort((a, b) => b.w * b.h - a.w * a.h);
      }
      if (!page.dumps.length) {
        count.textContent = "";
        grid.replaceWith(h("div.card.empty", s.dumpFolder.name + " has no texture dumps (files named <id>_vk<n>_<w>x<h>.bin)."));
        return;
      }
      draw();
      const sel = page.dumps.find((d) => d.name === page.selected);
      if (sel) detail(sel);
      else aside.append(h("div.card.muted", "Choose a texture."));
    })();
  },
};
