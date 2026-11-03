// Izuna Mod Studio - folders on the player's PC, through the browser's File
// System Access API (Edge, Chrome; in the Electron build the same). Folders
// picked are remembered (IndexedDB) and opened again with one click - the
// browser asks the player once per session. Browsers without the API (Firefox)
// get read-only folders (a folder chosen in a file dialog) and ZIP downloads.
"use strict";
var Izuna = (globalThis.Izuna = globalThis.Izuna || {});

Izuna.canWrite = typeof window !== "undefined" && "showDirectoryPicker" in window;

Izuna.Folder = class Folder {
  // handle: a FileSystemDirectoryHandle, or (read-only) a Map of path -> File.
  constructor(handle, name) {
    this.handle = handle;
    this.name = name || (handle && handle.name) || "folder";
    this.readOnly = handle instanceof Map;
  }

  static async pick(id) {
    if (Izuna.canWrite) {
      const handle = await window.showDirectoryPicker({ id, mode: "readwrite" });
      return new Folder(handle);
    }
    // Read-only: <input webkitdirectory>.
    return new Promise((resolve, reject) => {
      const input = document.createElement("input");
      input.type = "file";
      input.webkitdirectory = true;
      input.onchange = () => {
        const files = new Map();
        let root = "";
        for (const f of input.files) {
          const parts = f.webkitRelativePath.split("/");
          root = parts.shift();
          files.set(parts.join("/"), f);
        }
        files.size ? resolve(new Folder(files, root)) : reject(new Error("no folder chosen"));
      };
      input.click();
    });
  }

  static split(path) {
    return String(path).replace(/\\/g, "/").split("/").filter((p) => p && p !== ".");
  }

  // The handle of a sub-folder; created on the way if `create`.
  async dirHandle(path, create) {
    let h = this.handle;
    for (const part of Folder.split(path)) {
      try {
        h = await h.getDirectoryHandle(part, { create: !!create });
      } catch (e) {
        // (a name in another case)
        let found = null;
        for await (const [n, sub] of h.entries()) if (sub.kind === "directory" && n.toLowerCase() === part.toLowerCase()) found = sub;
        if (!found) throw e;
        h = found;
      }
    }
    return h;
  }

  async sub(path, create) {
    if (this.readOnly) {
      const prefix = Folder.split(path).join("/") + "/";
      const files = new Map();
      for (const [p, f] of this.handle) if (p.toLowerCase().startsWith(prefix.toLowerCase())) files.set(p.slice(prefix.length), f);
      return new Folder(files, Folder.split(path).pop());
    }
    return new Folder(await this.dirHandle(path, create));
  }

  // [{name, kind: "file" | "directory"}] directly in `path`.
  async list(path = "") {
    if (this.readOnly) {
      const prefix = Folder.split(path).join("/");
      const seen = new Map();
      for (const p of this.handle.keys()) {
        if (prefix && !p.toLowerCase().startsWith(prefix.toLowerCase() + "/")) continue;
        const rest = prefix ? p.slice(prefix.length + 1) : p;
        const [first, ...more] = rest.split("/");
        seen.set(first, more.length ? "directory" : "file");
      }
      return [...seen].map(([name, kind]) => ({ name, kind }));
    }
    let h;
    try {
      h = await this.dirHandle(path, false);
    } catch (e) {
      return [];
    }
    const out = [];
    for await (const [name, sub] of h.entries()) out.push({ name, kind: sub.kind });
    return out.sort((a, b) => a.name.localeCompare(b.name));
  }

  // Every file under `path`, as paths relative to it.
  async walk(path = "") {
    if (this.readOnly) {
      const prefix = Folder.split(path).join("/");
      return [...this.handle.keys()].filter((p) => !prefix || p.toLowerCase().startsWith(prefix.toLowerCase() + "/")).map((p) => (prefix ? p.slice(prefix.length + 1) : p));
    }
    const out = [];
    const go = async (h, rel) => {
      for await (const [name, sub] of h.entries()) {
        if (name.startsWith(".")) continue;
        if (sub.kind === "directory") await go(sub, rel + name + "/");
        else out.push(rel + name);
      }
    };
    try {
      await go(await this.dirHandle(path, false), "");
    } catch (e) {
      return [];
    }
    return out.sort();
  }

  async file(path) {
    if (this.readOnly) {
      const want = Folder.split(path).join("/").toLowerCase();
      for (const [p, f] of this.handle) if (p.toLowerCase() === want) return f;
      throw new Error(path + ": not found");
    }
    const parts = Folder.split(path), leaf = parts.pop();
    const dir = await this.dirHandle(parts.join("/"), false);
    try {
      return await (await dir.getFileHandle(leaf)).getFile();
    } catch (e) {
      for await (const [n, sub] of dir.entries()) if (sub.kind === "file" && n.toLowerCase() === leaf.toLowerCase()) return sub.getFile();
      throw new Error(path + ": not found");
    }
  }

  async exists(path) {
    try {
      await this.file(path);
      return true;
    } catch (e) {
      return false;
    }
  }

  async read(path) {
    return new Uint8Array(await (await this.file(path)).arrayBuffer());
  }

  async text(path) {
    return new TextDecoder().decode(await this.read(path));
  }

  async slice(path, offset, size) {
    const f = await this.file(path);
    return new Uint8Array(await f.slice(offset, offset + size).arrayBuffer());
  }

  async write(path, data) {
    if (this.readOnly) throw new Error("this browser cannot write to folders: use Edge or Chrome, or the ZIP export");
    const parts = Folder.split(path), leaf = parts.pop();
    const dir = await this.dirHandle(parts.join("/"), true);
    const w = await (await dir.getFileHandle(leaf, { create: true })).createWritable();
    await w.write(typeof data === "string" ? new TextEncoder().encode(data) : data);
    await w.close();
  }

  async remove(path) {
    if (this.readOnly) throw new Error("this browser cannot change folders");
    const parts = Folder.split(path), leaf = parts.pop();
    const dir = await this.dirHandle(parts.join("/"), false);
    await dir.removeEntry(leaf, { recursive: true });
  }

  // Whether the browser still lets us use it (a remembered folder after a restart).
  async permitted(ask) {
    if (this.readOnly || !this.handle.queryPermission) return true;
    const opts = { mode: "readwrite" };
    if ((await this.handle.queryPermission(opts)) === "granted") return true;
    return ask ? (await this.handle.requestPermission(opts)) === "granted" : false;
  }
};

// [dev] A read-only folder over HTTP (tools/studio/test/serve.py), so the
// pages can be looked at with a game folder in a browser that cannot pick one:
// index.html?dev-game=http://127.0.0.1:8765/game/&page=files
Izuna.UrlFolder = class UrlFolder {
  constructor(base) {
    this.base = base.endsWith("/") ? base : base + "/";
    this.name = decodeURIComponent(this.base.split("/").filter(Boolean).pop());
    this.readOnly = true;
  }
  url(path) { return this.base + Izuna.Folder.split(path).map(encodeURIComponent).join("/"); }
  async exists(path) { return (await fetch(this.url(path), { method: "HEAD" })).ok; }
  async read(path) {
    const r = await fetch(this.url(path));
    if (!r.ok) throw new Error(path + ": " + r.status);
    return new Uint8Array(await r.arrayBuffer());
  }
  async slice(path, offset, size) {
    const r = await fetch(this.url(path), { headers: { Range: "bytes=" + offset + "-" + (offset + size - 1) } });
    if (r.status !== 206) throw new Error(path + ": no ranges (" + r.status + ")");
    return new Uint8Array(await r.arrayBuffer());
  }
  async permitted() { return true; }
};

// Remembered folders (their handles; nothing of their contents).
Izuna.remember = (function () {
  const open = () =>
    new Promise((resolve, reject) => {
      const r = indexedDB.open("izuna-studio", 1);
      r.onupgradeneeded = () => r.result.createObjectStore("folders");
      r.onsuccess = () => resolve(r.result);
      r.onerror = () => reject(r.error);
    });
  async function set(key, folder) {
    if (folder.readOnly) return;
    const db = await open();
    await new Promise((res) => {
      const t = db.transaction("folders", "readwrite");
      t.objectStore("folders").put(folder.handle, key);
      t.oncomplete = res;
      t.onerror = res;
    });
  }
  async function get(key) {
    try {
      const db = await open();
      const h = await new Promise((res) => {
        const r = db.transaction("folders").objectStore("folders").get(key);
        r.onsuccess = () => res(r.result);
        r.onerror = () => res(null);
      });
      return h ? new Izuna.Folder(h) : null;
    } catch (e) {
      return null;
    }
  }
  return { set, get };
})();

// A download of bytes (the ZIP export; PNGs where a folder cannot be written).
Izuna.download = function (name, data, type) {
  const url = URL.createObjectURL(new Blob([data], { type: type || "application/octet-stream" }));
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
};
