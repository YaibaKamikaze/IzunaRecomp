// Izuna Mod Studio - building the page: h() for elements, toasts, dialogs.
"use strict";
var Izuna = (globalThis.Izuna = globalThis.Izuna || {});

// h("div.card#x", {onclick, title, ...}, children...) - children: nodes, strings, arrays, null.
Izuna.h = function h(spec, attrs, ...children) {
  const m = /^([a-z0-9]+)?((?:[.#][\w-]+)*)$/i.exec(spec);
  const el = document.createElement((m && m[1]) || "div");
  for (const part of (m && m[2] ? m[2].match(/[.#][\w-]+/g) : []) || []) {
    if (part[0] === ".") el.classList.add(part.slice(1));
    else el.id = part.slice(1);
  }
  if (attrs !== null && attrs !== undefined && (typeof attrs !== "object" || attrs instanceof Node || Array.isArray(attrs))) {
    children.unshift(attrs);
    attrs = null;
  }
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === undefined || v === null || v === false) continue;
    if (k.startsWith("on")) el.addEventListener(k.slice(2), v);
    else if (k === "style" && typeof v === "object") Object.assign(el.style, v);
    else if (k === "value") el.value = v;
    else if (k === "checked") el.checked = !!v;
    else el.setAttribute(k, v === true ? "" : v);
  }
  const add = (c) => {
    if (c === null || c === undefined || c === false) return;
    if (Array.isArray(c)) c.forEach(add);
    else el.appendChild(c instanceof Node ? c : document.createTextNode(String(c)));
  };
  children.forEach(add);
  return el;
};

Izuna.icon = function (name) {
  const paths = {
    home: "M3 11 12 3l9 8v10h-6v-6H9v6H3z",
    files: "M4 4h6l2 2h8v14H4z",
    mods: "M12 2 14.5 9.5 22 12l-7.5 2.5L12 22l-2.5-7.5L2 12l7.5-2.5z",
    textures: "M3 5h18v14H3zM3 16l5-5 4 4 3-3 6 6",
    models: "M12 2 21 7v10l-9 5-9-5V7zM3 7l9 5 9-5M12 12v10",
    script: "M8 6 2 12l6 6M16 6l6 6-6 6M14 4l-4 16",
    help: "M12 22a10 10 0 1 1 0-20 10 10 0 0 1 0 20zM9.5 9a2.5 2.5 0 1 1 3.5 2.3c-.6.3-1 .9-1 1.6V14M12 17v.5",
  };
  const ns = "http://www.w3.org/2000/svg";
  const svg = document.createElementNS(ns, "svg");
  svg.setAttribute("viewBox", "0 0 24 24");
  svg.setAttribute("class", "icon");
  const p = document.createElementNS(ns, "path");
  p.setAttribute("d", paths[name] || paths.help);
  svg.appendChild(p);
  return svg;
};

Izuna.size = function (n) {
  if (n >= 1 << 20) return (n / (1 << 20)).toFixed(n >= 10 << 20 ? 0 : 1) + " MB";
  if (n >= 1024) return Math.round(n / 1024) + " KB";
  return n + " B";
};

Izuna.toast = function (text, kind) {
  const box = document.getElementById("toasts");
  const t = Izuna.h("div.toast" + (kind ? "." + kind : ""), text);
  box.appendChild(t);
  setTimeout(() => t.classList.add("gone"), kind === "error" ? 7000 : 3500);
  setTimeout(() => t.remove(), kind === "error" ? 7600 : 4100);
};

// A modal dialog: body (nodes), buttons [{label, kind, value}] -> the value chosen (null: closed).
Izuna.dialog = function (title, body, buttons) {
  return new Promise((resolve) => {
    const close = (v) => {
      back.remove();
      resolve(v);
    };
    const back = Izuna.h("div.modal-back", { onmousedown: (e) => e.target === back && close(null) },
      Izuna.h("div.modal",
        Izuna.h("h2", title),
        Izuna.h("div.modal-body", body),
        Izuna.h("div.modal-buttons", (buttons || [{ label: "OK", kind: "primary", value: true }]).map((b) =>
          Izuna.h("button" + (b.kind ? "." + b.kind : ""), { onclick: () => close(typeof b.value === "function" ? b.value() : b.value) }, b.label)))));
    document.body.appendChild(back);
    const first = back.querySelector("input, textarea, select");
    if (first) first.focus();
    back.addEventListener("keydown", (e) => {
      if (e.key === "Escape") close(null);
    });
  });
};

Izuna.confirm = (title, text, yes) =>
  Izuna.dialog(title, Izuna.h("p", text), [{ label: "Cancel", value: false }, { label: yes || "OK", kind: "danger", value: true }]);

// A field with suggestions from the game's file list (a <datalist> of every path).
Izuna.pathInput = function (value, placeholder) {
  return Izuna.h("input.path", { value: value || "", placeholder: placeholder || "char\\ryu_b\\ryu_b.gmd", list: "game-paths", spellcheck: "false" });
};

// Runs an async action with errors shown as a toast.
Izuna.act = async function (fn) {
  try {
    return await fn();
  } catch (e) {
    if (e && e.name === "AbortError") return undefined;  // (a picker closed)
    console.error(e);
    Izuna.toast(String((e && e.message) || e), "error");
    return undefined;
  }
};
