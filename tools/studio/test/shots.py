#!/usr/bin/env python3
"""[dev] Screenshots of the studio's pages through geckodriver (WebDriver),
waiting for each page to finish, with the page's errors printed.

    python3 tools/studio/test/serve.py "Game Data" &   # first
    python3 tools/studio/test/shots.py OUT_DIR page[:js to run first] ...
"""
import base64, json, subprocess, sys, time, urllib.request

def call(method, path, body=None):
    req = urllib.request.Request("http://127.0.0.1:4444" + path, method=method,
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(req, timeout=120).read())["value"]

out = sys.argv[1]
driver = subprocess.Popen(["firefox.geckodriver", "--port", "4444"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2)
try:
    sid = call("POST", "/session", {"capabilities": {"alwaysMatch": {"moz:firefoxOptions": {"args": ["-headless", "--width=1500", "--height=950"]}}}})["sessionId"]
    base = "http://127.0.0.1:8765/studio/index.html?dev-game=http://127.0.0.1:8765/game/"
    for spec in sys.argv[2:]:
        page, _, js = spec.partition(":")
        call("POST", f"/session/{sid}/url", {"url": f"{base}&page={page}"})
        call("POST", f"/session/{sid}/execute/async", {"script": """
            const done = arguments[arguments.length - 1];
            const t0 = Date.now();
            (function wait() {
              const p = document.getElementById('page');
              if ((p && p.children.length && !document.querySelector('.muted') ) || Date.now() - t0 > 8000) return done();
              if (p && p.children.length && Date.now() - t0 > 2500) return done();
              setTimeout(wait, 200);
            })();""", "args": []})
        if js:
            call("POST", f"/session/{sid}/execute/async", {"script": js + "; setTimeout(arguments[arguments.length-1], 3000);", "args": []})
        errs = call("POST", f"/session/{sid}/execute/sync", {"script": "return [...document.querySelectorAll('.toast.error, .banner')].map(e => e.textContent)", "args": []})
        png = base64.b64decode(call("GET", f"/session/{sid}/screenshot"))
        name = page + ("-" + str(abs(hash(js)) % 1000) if js else "")
        open(f"{out}/{name}.png", "wb").write(png)
        print(name, "errors:", errs)
    call("DELETE", f"/session/{sid}")
finally:
    driver.terminate()
