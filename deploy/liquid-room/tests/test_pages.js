// node deploy/liquid-room/tests/test_pages.js
// Runs the www redirect and the room page scripts in a VM with browser stubs.
"use strict";
const assert = require("assert");
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const ROOT = path.resolve(__dirname, "..");
let failures = 0;
async function test(name, fn) {
  try {
    await fn();
    console.log("ok   " + name);
  } catch (err) {
    failures++;
    console.log("FAIL " + name + "\n     " + (err && err.stack || err));
  }
}
const tick = () => new Promise((r) => setTimeout(r, 0));
async function settle() { for (let i = 0; i < 10; i++) await tick(); }

function scriptById(html, id) {
  const m = html.match(new RegExp('<script id="' + id + '">([\\s\\S]*?)</script>'));
  assert(m, "script " + id + " not found");
  return m[1];
}
function lastScript(html) {
  const all = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)];
  assert(all.length, "no inline script");
  return all[all.length - 1][1];
}

// ---------------------------------------------------------------- www/demo.html
const www = fs.readFileSync(path.join(ROOT, "www", "demo.html"), "utf8");

function runRedirect(search) {
  const replaced = [];
  const ctx = { location: { search, replace: (u) => replaced.push(u) } };
  vm.runInNewContext(scriptById(www, "demo-redirect"), ctx);
  return { target: ctx.demoTarget, replaced };
}

(async () => {
  await test("www: founder URL goes to the room101 viewer", () => {
    const r = runRedirect("?room=room101");
    assert.deepStrictEqual(r.replaced, ["https://rtc.wilddolphin.us/demo/view/room101"]);
  });
  await test("www: defaults, via=room, and hostile input", () => {
    const { target } = runRedirect("");
    const cases = {
      "": "https://rtc.wilddolphin.us/demo/view/room101",
      "?room=room202": "https://rtc.wilddolphin.us/demo/view/room202",
      "?room=room101&via=room": "https://rtc.wilddolphin.us/demo/room/room101",
      "?via=room": "https://rtc.wilddolphin.us/demo/room/room101",
      "?via=evil&room=x": "https://rtc.wilddolphin.us/demo/view/x",
      "?room=..%2F..%2Fevil": "https://rtc.wilddolphin.us/demo/view/room101",
      "?room=%2F%2Fevil.example": "https://rtc.wilddolphin.us/demo/view/room101",
      "?room=ws": "https://rtc.wilddolphin.us/demo/view/room101",
      "?room=assets": "https://rtc.wilddolphin.us/demo/view/room101",
      "?room=a%0Ab": "https://rtc.wilddolphin.us/demo/view/room101",
      "?room=%E0%A4%A": "https://rtc.wilddolphin.us/demo/view/room101",
      ["?room=" + "a".repeat(65)]: "https://rtc.wilddolphin.us/demo/view/room101",
      "?token=secret&room=room101": "https://rtc.wilddolphin.us/demo/view/room101",
    };
    for (const [q, want] of Object.entries(cases)) assert.strictEqual(target(q), want, q);
  });
  await test("www: no token, password, or IP in the page", () => {
    assert(!/token=|password|\b\d{1,3}(\.\d{1,3}){3}\b/i.test(www.replace(/2\.2\.19|6\.0\.0/g, "")));
    assert(!/LiquidCodec/.test(www));
  });

  // ---------------------------------------------------------------- page/index.html
  const page = fs.readFileSync(path.join(ROOT, "page", "index.html"), "utf8");
  const pageScript = lastScript(page);

  function makePage(pathname) {
    const els = {};
    const el = (id) => els[id] || (els[id] = {
      id, textContent: "", scrollTop: 0, scrollHeight: 0, srcObject: null, listeners: {},
      addEventListener(t, f) { this.listeners[t] = f; }, play() { return Promise.resolve(); },
    });
    const sockets = [];
    class FakeWS {
      constructor(url) { this.url = url; this.sent = []; this.readyState = 0; sockets.push(this); }
      send(t) { this.sent.push(t); }
      close() { this.readyState = 3; }
      open() { this.readyState = 1; this.onopen && this.onopen(); }
      recv(t) { return this.onmessage({ data: t }); }
    }
    FakeWS.OPEN = 1;
    const pcs = [];
    class FakePC {
      constructor(cfg) { this.cfg = cfg; this.tracks = []; this.remote = null; this.cands = []; this.closed = false; pcs.push(this); }
      addTrack(t) { this.tracks.push(t); }
      async createOffer(opts) {
        this.offerOpts = opts;
        return { type: "offer", sdp: "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111 63\r\na=mid:0\r\na=rtpmap:111 opus/48000/2\r\na=rtpmap:63 red/48000/2\r\n" };
      }
      async setLocalDescription(d) { this.local = d; }
      async setRemoteDescription(d) { this.remote = d; this.remoteDescription = d; }
      async addIceCandidate(c) { this.cands.push(c); }
      async getStats() { return new Map(); }
      close() { this.closed = true; }
    }
    const gum = [];
    const ctx = {
      location: { pathname, hostname: "rtc.wilddolphin.us", host: "rtc.wilddolphin.us", protocol: "https:", assign(u) { this.assigned = u; } },
      document: { getElementById: el },
      window: { addEventListener() {} },
      navigator: { mediaDevices: { getUserMedia: async (c) => { gum.push(c); return { getTracks: () => [{ stop() {} }], getAudioTracks: () => [{ kind: "audio" }] }; } } },
      fetch: async () => ({ status: 503, ok: false, type: "basic" }),
      WebSocket: FakeWS, RTCPeerConnection: FakePC, MediaStream: class {},
      setTimeout, clearTimeout, setInterval, clearInterval, console, JSON, Number, Date,
    };
    vm.runInNewContext(pageScript, ctx);
    return { els, el, sockets, pcs, gum, ctx };
  }

  await test("page: full caller sequence for room101", async () => {
    const p = makePage("/demo/room/room101");
    assert.strictEqual(p.el("roomName").textContent, "room101");
    p.el("startBtn").listeners.click();
    await settle();
    const ws = p.sockets[0];
    assert.strictEqual(ws.url, "wss://rtc.wilddolphin.us/demo/room/ws/room101");
    ws.open();
    assert.deepStrictEqual(ws.sent, ["HELLO"]);
    await ws.recv("200 OK"); await settle();
    assert.strictEqual(ws.sent[1], 'INVITE:{"agent":"audio","room_name":"room101","video":false}');
    await ws.recv("200 OK"); await settle();
    assert.strictEqual(ws.sent.length, 2, "a second 200 OK must not resend INVITE");
    await ws.recv("WAITING"); await settle();
    assert.deepStrictEqual(p.gum[0].video, false);
    const offer = ws.sent[2];
    assert(offer.startsWith("OFFER:v=0\r\n"));
    assert(!/red\/48000/.test(offer), "RED is stripped like the Slim page");
    assert.strictEqual(p.pcs[0].offerOpts.offerToReceiveVideo, false);
    await ws.recv("ICE:0:candidate:1 1 udp 1 203.0.113.5 7882 typ host"); await settle();
    assert.strictEqual(p.pcs[0].cands.length, 0, "queued until the answer");
    await ws.recv("ANSWER:v=0\r\nm=audio 7882 UDP/TLS/RTP/SAVPF 111\r\n"); await settle();
    assert.strictEqual(p.pcs[0].remote.type, "answer");
    assert.strictEqual(p.pcs[0].cands.length, 1);
    assert.strictEqual(p.pcs[0].cands[0].candidate, "candidate:1 1 udp 1 203.0.113.5 7882 typ host");
    p.pcs[0].onicecandidate({ candidate: { sdpMLineIndex: 0, candidate: "candidate:9 1 udp 2 192.0.2.1 5000 typ host" } });
    assert.strictEqual(ws.sent[3], "ICE:0:candidate:9 1 udp 2 192.0.2.1 5000 typ host");
    await ws.recv("BYE"); await settle();
    assert.strictEqual(ws.sent[4], "200 OK", "server BYE is acknowledged");
    assert(p.pcs[0].closed);
  });

  await test("page: rejection status ends the attempt", async () => {
    const p = makePage("/demo/room/room101");
    p.el("startBtn").listeners.click(); await settle();
    const ws = p.sockets[0];
    ws.open();
    await ws.recv("486 Busy Here"); await settle();
    assert(/^ended$/.test(p.el("status").textContent));
    assert.strictEqual(ws.sent.length, 1);
  });

  await test("page: hangup sends BYE", async () => {
    const p = makePage("/demo/room/room101");
    p.el("startBtn").listeners.click(); await settle();
    const ws = p.sockets[0];
    ws.open();
    p.el("hangupBtn").listeners.click();
    assert.strictEqual(ws.sent[ws.sent.length - 1], "BYE");
  });

  await test("page: invalid or reserved room never opens a socket", async () => {
    for (const pathname of ["/demo/room/ws", "/demo/room/", "/demo/room/a/b", "/demo/view/room101"]) {
      const p = makePage(pathname);
      p.el("startBtn").listeners.click(); await settle();
      assert.strictEqual(p.sockets.length, 0, pathname);
    }
  });

  await test("page: login redirect keeps the room", async () => {
    const p = makePage("/demo/room/room101");
    p.ctx.fetch = async () => ({ status: 401, ok: false, type: "basic" });
    p.el("startBtn").listeners.click(); await settle();
    p.sockets[0].open();
    await p.sockets[0].recv("200 OK"); await settle();
    await p.sockets[0].recv("WAITING"); await settle();
    assert.strictEqual(p.ctx.location.assigned, "/demo/login?next=/demo/room/room101");
  });

  await test("page: LiquidServer wordmark and title", () => {
    assert(/<title>LiquidServer demo<\/title>/.test(page));
    assert(/<h1 class="wm">LiquidServer<\/h1>/.test(page));
    assert(/by Wilddolphin LLC/.test(page));
    assert(/LiquidServer demo · ls\.wilddolphin\.us/.test(page));
    assert(!/<h1 class="wm">LiquidCall<\/h1>/.test(page));
    assert(!/<title>LiquidCall room<\/title>/.test(page));
  });

  await test("page: no secrets, IPs or legacy name", () => {
    assert(!/LiquidCodec/.test(page));
    assert(!/\b\d{1,3}(\.\d{1,3}){3}\b/.test(page.replace(/\\d\{1,3\}/g, "")));
    assert(!/token=/.test(page));
  });

  if (failures) { console.log(failures + " failed"); process.exit(1); }
  console.log("all page tests passed");
  process.exit(0);  // page timers (setup timeout) would keep node alive
})();
