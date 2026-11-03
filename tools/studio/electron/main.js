// Izuna Mod Studio as a Windows app (electron/build-windows.ps1): Electron's
// window around the same page the browser opens (index.html). Its folders go
// through the same File System Access API; Electron asks nothing more.
const { app, BrowserWindow, session, shell } = require("electron");
const path = require("path");

app.whenReady().then(() => {
  // Folders the player picks: allowed (the page only sees what is picked).
  session.defaultSession.setPermissionRequestHandler((wc, permission, done) => done(true));
  session.defaultSession.setPermissionCheckHandler(() => true);
  const win = new BrowserWindow({
    width: 1440, height: 900, minWidth: 1000, minHeight: 640,
    backgroundColor: "#0e0e10", title: "Izuna Mod Studio", autoHideMenuBar: true,
    webPreferences: { contextIsolation: true, sandbox: true },
  });
  win.loadFile(path.join(__dirname, "..", "index.html"));
  // Links open in the browser, not in the studio's window.
  win.webContents.setWindowOpenHandler(({ url }) => {
    shell.openExternal(url);
    return { action: "deny" };
  });
});
app.on("window-all-closed", () => app.quit());
