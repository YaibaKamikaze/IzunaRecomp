// Izuna Mod Studio - Start: the game's folder, the mods' folder, what to do next.
"use strict";
var Izuna = (globalThis.Izuna = globalThis.Izuna || {});

Izuna.pageStart = {
  title: "Start",
  icon: "home",
  render(el) {
    const h = Izuna.h, app = Izuna.app, s = app.state;
    const rerender = () => app.go("start");

    const gameCard = h("div.card",
      h("h2", "Your copy of the game"),
      h("div.status", s.game
        ? [h("span.good", "Ready: "), s.gameFolder.name + " · " + s.game.files.length + " files (" + s.game.files.filter((f) => f.dlc).length + " DLC)"]
        : "The folder you extracted the disc to - the one with default.xex and the .ng2 archives. The studio reads it; it never changes it."),
      h("div.row",
        h("button.primary", { onclick: () => Izuna.act(async () => {
          await app.openGame(await Izuna.Folder.pick("izuna-game"));
          Izuna.toast("Read the game: " + s.game.files.length + " files.");
          rerender();
        }) }, s.game ? "Choose another" : "Choose the game's folder"),
        !s.game && s.remembered_game ? h("button", { onclick: () => Izuna.act(async () => {
          if (await s.remembered_game.permitted(true)) await app.openGame(s.remembered_game);
          rerender();
        }) }, "Open " + s.remembered_game.name + " again") : null));

    const modsCard = h("div.card",
      h("h2", "Your mods"),
      h("div.status", s.modsFolder
        ? [h("span.good", "Ready: "), s.modsFolder.name + " · " + s.mods.length + " mod(s)"]
        : "A folder on this PC for the mods you make, one folder each. To play one, copy its folder into Mods/ in the app's folder on the console (over FTP), next to Game Data/."),
      h("div.row",
        h("button.primary", { onclick: () => Izuna.act(async () => {
          await app.openMods(await Izuna.Folder.pick("izuna-mods"));
          rerender();
        }) }, s.modsFolder ? "Choose another" : "Choose a folder for mods"),
        !s.modsFolder && s.remembered_mods ? h("button", { onclick: () => Izuna.act(async () => {
          if (await s.remembered_mods.permitted(true)) await app.openMods(s.remembered_mods);
          rerender();
        }) }, "Open " + s.remembered_mods.name + " again") : null,
        s.modsFolder ? h("button", { onclick: () => app.go("mods") }, "Open the mods") : null));

    const doCard = (title, text, page, label) =>
      h("div.card", h("h2", title), h("p.muted", text), h("button", { onclick: () => app.go(page) }, label));

    el.append(
      h("h1", "Izuna Mod Studio"),
      h("p.lead", "Make mods for IzunaRecomp, the Ninja Gaiden II recompilation: replace the game's files, models and textures, swap its own files around, hide what it draws, and change how it plays with scripts that hook its functions."),
      Izuna.canWrite ? null : h("div.banner", "This browser cannot write to folders: you can look around and export mods as ZIP files, but to make and save mods open the studio in Microsoft Edge or Google Chrome."),
      h("div.cards", gameCard, modsCard),
      h("h3", "What you can do"),
      h("div.cards",
        doCard("Game files", "Every one of the game's 5099 files by name - models, motions, effects, stages, text. Copy them out, or into a mod to edit.", "files", "Browse the game's files"),
        doCard("Mods", "Make a mod: its details, the files it replaces, swaps of the game's own files, textures, hidden shaders, its script - checked as the game will check it, then exported as a ZIP.", "mods", "Make a mod"),
        doCard("Textures", "Open a texture dump from the console (MODS page, R2: Save textures), find a texture, and replace or hide it in a mod.", "textures", "Open a texture dump"),
        doCard("Scripting", "The script API - memory, the controller, the screen, hooks on any of the game's 48,072 functions - with examples to start from.", "scripting", "Read the API")));
  },
};
