#!/usr/bin/env python3
"""Doba in a browser tab: the real firmware (sim/build/otp-sim) with its
panel shown live. Click the panel to tap it; the side buttons, GPS places, a
walk, and the device console are next to it.

    python3 sim/simview.py --data /Volumes/SSD/doba-build/data   # then open http://127.0.0.1:8090

--data points the sim's SD card at real build output: tiles/<region> as
/doba/map, dem-pack as /doba/dem, lib as /doba/lib. The device keeps its
settings and points between runs in --dir (default ~/.doba-sim).
"""
import argparse
import json
import math
import os
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

# device.py and mesh.py straight from the package dir: the package's __init__
# pulls in the phone-link protobufs (and protoc), which the viewer doesn't need.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "client", "otpsim"))
from device import SimDevice  # noqa: E402

PLACES = {
    "Groenlo": (52.0421, 6.6170),
    "Noordzee (IJmuiden)": (52.48, 4.45),
    "Wadden (Texel)": (53.05, 4.95),
    "Kyiv": (50.4501, 30.5234),
    "Odesa (sea)": (46.43, 30.85),
    "Kherson": (46.635, 32.616),
}

PAGE = """<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Doba sim</title>
<style>
 body{background:#1b1d1f;color:#ddd;font:14px -apple-system,system-ui,sans-serif;margin:0;display:flex;gap:24px;padding:20px}
 #dev{background:#2a2c2e;padding:18px;border-radius:22px;box-shadow:0 8px 30px #0008}
 #panel{width:432px;height:768px;cursor:crosshair;display:block;background:#fff;border-radius:4px;image-rendering:auto}
 .btns{display:flex;gap:8px;margin-top:12px} button{background:#3a3d40;color:#eee;border:0;border-radius:8px;padding:9px 12px;font:inherit;cursor:pointer}
 button:hover{background:#4a4d50} button.on{background:#7a5c1e}
 h3{margin:16px 0 6px;font-size:12px;letter-spacing:.08em;color:#999;text-transform:uppercase}
 #side{max-width:440px} #places button{margin:0 6px 6px 0}
 #log{white-space:pre-wrap;font:11px ui-monospace,monospace;background:#111;padding:8px;height:260px;overflow:auto;border-radius:6px}
 input{background:#111;color:#eee;border:1px solid #444;border-radius:6px;padding:7px;font:12px ui-monospace,monospace;width:300px}
 #st{font:12px ui-monospace,monospace;color:#9a9}
 @media (max-width:700px){ /* a phone: the panel fills the width, the rest below it */
  body{flex-direction:column;padding:8px;gap:12px} #dev{padding:8px;border-radius:14px}
  #panel{width:100%;height:auto;aspect-ratio:540/960} #side{max-width:none} input{width:70%}
  button{padding:12px 14px} }
</style></head><body>
<div id="dev"><img id="panel" src="/panel.png">
 <div class="btns"><button onclick="key('home')">HOME</button><button onclick="key('boot')">BOOT</button>
 <button onclick="key('boot',2500)">BOOT (hold)</button></div></div>
<div id="side">
 <div id="st">…</div>
 <h3>GPS</h3><div id="places">%PLACES%</div>
 <div class="btns"><button id="walk" onclick="walk()">WALK 5 km/h</button><button onclick="post('/nofix')">LOSE FIX</button>
 <button onclick="post('/radio')">NEIGHBOUR SAYS HI</button></div>
 <h3>Console</h3><input id="cmd" placeholder="screen points · dem · topo · greytest · mils" onkeydown="if(event.key=='Enter'){post('/console?c='+encodeURIComponent(this.value));this.value=''}">
 <h3>Log</h3><div id="log"></div>
</div>
<script>
const img=document.getElementById('panel');let paints=-1,walking=false;
// A tap, or a finger drawn across the panel (drags the map).
let down=null;
function px(e){const r=img.getBoundingClientRect();return [Math.round((e.clientX-r.left)*540/r.width),Math.round((e.clientY-r.top)*960/r.height)]}
img.style.touchAction='none';img.draggable=false;
img.onpointerdown=e=>{down=px(e);e.preventDefault()};
img.onpointerup=e=>{if(!down)return;const u=px(e),[x0,y0]=down;down=null;
 if(Math.hypot(u[0]-x0,u[1]-y0)>25)post('/swipe?x0='+x0+'&y0='+y0+'&x1='+u[0]+'&y1='+u[1]);
 else post('/tap?x='+x0+'&y='+y0)};
function post(u){return fetch(u,{method:'POST'}).then(()=>setTimeout(tick,150))}
function key(n,ms){post('/key?n='+n+(ms?'&ms='+ms:''))}
function walk(){walking=!walking;document.getElementById('walk').classList.toggle('on',walking);post('/walk?on='+(walking?1:0))}
async function tick(){try{const s=await (await fetch('/status')).json();
 document.getElementById('st').textContent=s.line;
 if(s.paints!==paints){paints=s.paints;img.src='/panel.png?'+Date.now()}
 const l=document.getElementById('log');const at=l.scrollTop+l.clientHeight>=l.scrollHeight-4;l.textContent=s.log;if(at)l.scrollTop=l.scrollHeight;}catch(e){}}
setInterval(tick,700);tick();
</script></body></html>"""


class Viewer:
    def __init__(self, a):
        self.a = a
        self.dev = None
        self.lock = threading.Lock()
        self.pos = PLACES["Groenlo"]
        self.course = 0.0
        self.walking = False
        self.shot = os.path.join(tempfile.mkdtemp(prefix="doba-view-"), "panel.png")
        self.prepare_card()
        self.boot()
        threading.Thread(target=self.walker, daemon=True).start()

    def prepare_card(self):
        doba = os.path.join(self.a.dir, "sd", "doba")
        os.makedirs(doba, exist_ok=True)
        if not self.a.data:
            return
        links = {"map": os.path.join(self.a.data, "tiles", self.a.region),
                 "dem": os.path.join(self.a.data, "dem-pack"),
                 "lib": os.path.join(self.a.data, "lib")}
        for name, target in links.items():
            p = os.path.join(doba, name)
            if os.path.islink(p):
                os.remove(p)
            if os.path.isdir(target) and not os.path.exists(p):
                os.symlink(target, p)
                print(f"card /doba/{name} -> {target}")

    def boot(self):
        args = ["--gps-baud", "115200", "--lora"]
        self.dev = SimDevice(dir=self.a.dir, args=args)
        os.environ.setdefault("DOBA_START", "home")
        self.dev.launch(60)
        self.fix()

    def alive(self):
        if self.dev.proc.poll() is not None:   # powered off / slept: wake it again
            self.boot()
        return self.dev

    def fix(self, speed=0.0):
        la, lo = self.pos
        self.dev.gps(la, lo, speed_kmh=speed, course=self.course)

    def walker(self):
        while True:
            time.sleep(1)
            if not self.walking:
                continue
            with self.lock:
                d = 5.0 / 3.6                       # m per second
                la, lo = self.pos
                la += d * math.cos(math.radians(self.course)) / 111320
                lo += d * math.sin(math.radians(self.course)) / (111320 * math.cos(math.radians(la)))
                self.pos = (la, lo)
                try:
                    self.alive()
                    self.fix(5.0)
                except Exception:
                    pass

    def status(self):
        with self.lock:
            d = self.alive()
            s = d.status()
            try:
                with open(d.log_path, "rb") as f:
                    f.seek(max(0, os.path.getsize(d.log_path) - 6000))
                    log = f.read().decode("utf-8", "replace")
            except OSError:
                log = ""
        r = s.get("ride", {})
        line = f"screen {s.get('screen')} · {self.pos[0]:.5f} {self.pos[1]:.5f} · sats {r.get('sats', '-')}"
        return {"paints": s.get("paints"), "line": line, "log": log}

    def panel(self):
        with self.lock:
            self.alive().screenshot(self.shot)
        return open(self.shot, "rb").read()


def serve(v, port, host="127.0.0.1"):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def send(self, body, ctype="text/html; charset=utf-8", code=200):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            u = urlparse(self.path)
            if u.path == "/":
                places = "".join(f'<button onclick="post(\'/place?n={k}\')">{k}</button>' for k in PLACES)
                return self.send(PAGE.replace("%PLACES%", places).encode())
            if u.path == "/panel.png":
                return self.send(v.panel(), "image/png")
            if u.path == "/status":
                return self.send(json.dumps(v.status()).encode(), "application/json")
            self.send(b"", code=404)

        def do_POST(self):
            u = urlparse(self.path)
            q = {k: x[0] for k, x in parse_qs(u.query).items()}
            with v.lock:
                d = v.alive()
                if u.path == "/tap":
                    d.tap(int(q["x"]), int(q["y"]))
                elif u.path == "/swipe":
                    d.swipe(int(q["x0"]), int(q["y0"]), int(q["x1"]), int(q["y1"]))
                elif u.path == "/key":
                    d.button(q["n"], ms=int(q.get("ms", 150)))
                elif u.path == "/place":
                    v.pos = PLACES[q["n"]]
                    v.fix()
                elif u.path == "/walk":
                    v.walking = q.get("on") == "1"
                    v.course = (v.course + 37) % 360 if v.walking else v.course
                    if not v.walking:
                        v.fix()
                elif u.path == "/nofix":
                    d.gps(v.pos[0], v.pos[1], fix=False)
                elif u.path == "/console":
                    d.console(q.get("c", ""), 300)
                elif u.path == "/radio":
                    # A neighbour on the band says hello (text on the default channel).
                    try:
                        from mesh import VirtualNode
                        n = VirtualNode(0x5EED0001, long_name="Sim neighbour", short_name="NBR1")
                        d.lora_rx(n.nodeinfo())
                        time.sleep(0.4)
                        d.lora_rx(n.text("Hi from a sim neighbour. Radio check, over."))
                    except Exception as e:
                        print("radio:", e)
            self.send(b"ok", "text/plain")

    srv = ThreadingHTTPServer((host, port), H)
    print(f"Doba sim: http://127.0.0.1:{port}")
    srv.serve_forever()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", help="build data dir (tiles/, dem-pack/, lib/)")
    ap.add_argument("--region", default="netherlands", help="tiles/<region> becomes the card's map")
    ap.add_argument("--dir", default=os.path.expanduser("~/.doba-sim"))
    ap.add_argument("--port", type=int, default=8090)
    ap.add_argument("--host", default="127.0.0.1",
                    help="0.0.0.0 to open it from a phone on the same Wi-Fi")
    a = ap.parse_args()
    os.makedirs(a.dir, exist_ok=True)
    v = Viewer(a)
    import atexit
    import signal
    atexit.register(lambda: v.dev and v.dev.stop())
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    signal.signal(signal.SIGHUP, lambda *_: sys.exit(0))   # Terminal window closed
    try:
        serve(v, a.port, a.host)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
