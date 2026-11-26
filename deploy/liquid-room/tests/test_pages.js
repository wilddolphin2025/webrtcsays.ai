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
    assert(/<title>CetaCall Demo \| Wilddolphin LLC<\/title>/.test(www));
    assert(/Opening the CetaCall demo/.test(www));
  });

  // ---------------------------------------------------------------- page/index.html
  const page = fs.readFileSync(path.join(ROOT, "page", "index.html"), "utf8");
  const pageScript = lastScript(page);

  function makePage(pathname, options) {
    const store = Object.assign({}, options && options.storage);
    const els = {};
    const el = (id) => els[id] || (els[id] = {
      id, textContent: "", hidden: false, className: "", scrollTop: 0, scrollHeight: 0, srcObject: null,
      listeners: {}, attrs: {}, children: [],
      addEventListener(t, f) { this.listeners[t] = f; },
      setAttribute(name, value) { this.attrs[name] = value; },
      replaceChildren() { this.children = []; },
      appendChild(child) { this.children.push(child); },
      play() { return Promise.resolve(); },
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
      document: {
        getElementById: el,
        createElement() { return { textContent: "", hidden: false }; },
      },
      localStorage: {
        getItem(key) { return Object.prototype.hasOwnProperty.call(store, key) ? store[key] : null; },
        setItem(key, value) { store[key] = String(value); },
      },
      window: { addEventListener() {} },
      navigator: { mediaDevices: { getUserMedia: async (c) => { gum.push(c); return { getTracks: () => [{ stop() {} }], getAudioTracks: () => [{ kind: "audio" }] }; } } },
      fetch: async () => ({ status: 503, ok: false, type: "basic" }),
      WebSocket: FakeWS, RTCPeerConnection: FakePC, MediaStream: class {},
      setTimeout, clearTimeout, setInterval, clearInterval, console, JSON, Number, Date, Math,
    };
    vm.runInNewContext(pageScript, ctx);
    return { els, el, sockets, pcs, gum, ctx, store };
  }

  function guestNames(p) {
    return p.el("guestList").children.map((item) => item.textContent);
  }

  await test("page: full caller sequence for room101", async () => {
    const p = makePage("/demo/room/room101");
    assert.strictEqual(p.el("roomName").textContent, "room101");
    p.el("startBtn").listeners.click();
    await settle();
    const ws = p.sockets[0];
    assert.strictEqual(ws.url, "wss://rtc.wilddolphin.us/demo/room/ws/room101");
    ws.open();
    assert(/^HELLO:Web-[0-9a-f]{4}$/.test(ws.sent[0]), ws.sent[0]);
    assert.strictEqual(ws.sent.length, 1);
    assert.strictEqual(p.el("nick").value, ws.sent[0].slice("HELLO:".length));
    assert.strictEqual(p.store["cetabridge-nickname"], p.el("nick").value);
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
    assert.strictEqual(p.el("nickMsg").hidden, true);
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

  await test("page: guests follow PEERS and the log stays hidden", async () => {
    const p = makePage("/demo/room/room101");
    assert.strictEqual(p.el("logCard").hidden, true);
    assert.strictEqual(p.el("logToggle").textContent, "Show log");
    assert.strictEqual(p.el("logToggle").attrs["aria-expanded"], "false");
    assert.deepStrictEqual(guestNames(p), ["You"]);
    assert.strictEqual(p.el("guestWait").hidden, true);
    assert.strictEqual(p.el("guestSummary").hidden, true);
    p.el("startBtn").listeners.click(); await settle();
    const ws = p.sockets[0];
    ws.open();
    await ws.recv("200 OK"); await settle();
    await ws.recv("PEERS 1"); await settle();
    assert.strictEqual(p.el("guestSummary").textContent, "In room101: 1");
    assert.deepStrictEqual(guestNames(p), ["You"]);
    assert.strictEqual(p.el("guestWait").hidden, false);
    assert.strictEqual(p.el("guestWait").textContent, "Waiting for others");
    await ws.recv("PEERS   3  extra"); await settle();
    assert.strictEqual(p.el("guestSummary").textContent, "In room101: 3");
    assert.deepStrictEqual(guestNames(p), ["You"], "PEERS alone does not invent guest labels");
    assert.strictEqual(p.el("guestWait").hidden, true);
    assert(/PEERS\s+3/.test(p.el("log").textContent), "PEERS is written to the hidden log");
    await ws.recv("NAMES iphone,Pixel_8,Guest-1"); await settle();
    assert.strictEqual(p.el("guestSummary").textContent, "In room101: 3");
    assert.deepStrictEqual(guestNames(p), ["iphone", "Pixel_8", "Guest-1"]);
    await ws.recv("NAMES alice, bad"); await settle();
    assert.deepStrictEqual(guestNames(p), ["iphone", "Pixel_8", "Guest-1"], "a malformed roster is ignored");
    await ws.recv("NOBODY home"); await settle();
    assert.deepStrictEqual(guestNames(p), ["iphone", "Pixel_8", "Guest-1"]);
    assert.strictEqual(ws.sent.length, 2);
    p.el("hangupBtn").listeners.click();
    assert.deepStrictEqual(guestNames(p), ["You"]);
    assert.strictEqual(p.el("guestSummary").hidden, true);
    assert.strictEqual(p.el("guestWait").hidden, true);
  });

  await test("page: disconnect clears guests and the log toggle is remembered", async () => {
    const first = makePage("/demo/room/room101");
    first.el("logToggle").listeners.click();
    assert.strictEqual(first.el("logCard").hidden, false);
    assert.strictEqual(first.el("logToggle").textContent, "Hide log");
    assert.strictEqual(first.store["cetabridge-room-show-log"], "1");
    const again = makePage("/demo/room/room101", { storage: { "cetabridge-room-show-log": "1" } });
    assert.strictEqual(again.el("logCard").hidden, false);
    assert.strictEqual(again.el("panels").className, "panels show-log");
    again.el("startBtn").listeners.click(); await settle();
    const ws = again.sockets[0];
    ws.open();
    await ws.recv("PEERS 2"); await settle();
    await ws.recv("NAMES alice,bob"); await settle();
    assert.deepStrictEqual(guestNames(again), ["alice", "bob"]);
    ws.onclose();
    await settle();
    assert.deepStrictEqual(guestNames(again), ["You"]);
    assert.strictEqual(again.el("guestSummary").hidden, true);
  });

  await test("page: nickname is sanitized into HELLO and marked on the roster", async () => {
    const blank = makePage("/demo/room/room101");
    const generated = blank.el("nick").value;
    assert(/^Web-[0-9a-f]{4}$/.test(generated), generated);
    assert.strictEqual(blank.store["cetabridge-nickname"], generated);
    blank.el("nick").value = "   ";
    blank.el("startBtn").listeners.click(); await settle();
    blank.sockets[0].open();
    assert.deepStrictEqual(blank.sockets[0].sent, ["HELLO:" + generated]);
    assert.strictEqual(blank.el("nick").value, generated);

    const messy = makePage("/demo/room/room101");
    messy.el("nick").value = "My Phone!";
    messy.el("startBtn").listeners.click(); await settle();
    const mws = messy.sockets[0];
    mws.open();
    assert.strictEqual(mws.sent[0], "HELLO:MyPhone");
    await mws.recv("NAMES MyPhone,iphone"); await settle();
    assert.deepStrictEqual(guestNames(messy), ["MyPhone (you)", "iphone"]);

    const dup = makePage("/demo/room/room101");
    dup.el("nick").value = "iphone";
    dup.el("startBtn").listeners.click(); await settle();
    dup.sockets[0].open();
    await dup.sockets[0].recv("NAMES iphone,iphone"); await settle();
    assert.deepStrictEqual(guestNames(dup), ["iphone", "iphone"], "a repeated name is not guessed as us");

    const empty = makePage("/demo/room/room101");
    const fallback = empty.el("nick").value;
    empty.el("nick").value = "@@@";
    empty.el("startBtn").listeners.click(); await settle();
    empty.sockets[0].open();
    assert.strictEqual(empty.sockets[0].sent[0], "HELLO:" + fallback);
    assert.strictEqual(empty.el("nick").value, fallback);

    const long = makePage("/demo/room/room101");
    long.el("nick").value = "a".repeat(80);
    long.el("startBtn").listeners.click(); await settle();
    long.sockets[0].open();
    assert.strictEqual(long.sockets[0].sent[0], "HELLO:" + "a".repeat(64));

    const kept = makePage("/demo/room/room101", { storage: { "cetabridge-nickname": "Web-ab12" } });
    assert.strictEqual(kept.el("nick").value, "Web-ab12");
    kept.el("nick").value = "";
    kept.el("startBtn").listeners.click(); await settle();
    kept.sockets[0].open();
    assert.strictEqual(kept.sockets[0].sent[0], "HELLO:Web-ab12");

    const badStore = makePage("/demo/room/room101", { storage: { "cetabridge-nickname": "@@@" } });
    assert(/^Web-[0-9a-f]{4}$/.test(badStore.el("nick").value), badStore.el("nick").value);
    assert.notStrictEqual(badStore.el("nick").value, "@@@");
  });

  await test("page: nickname refusal is explained", async () => {
    async function refused(line, pattern) {
      const p = makePage("/demo/room/room101");
      p.el("nick").value = "alice";
      p.el("startBtn").listeners.click(); await settle();
      const ws = p.sockets[0];
      ws.open();
      assert.strictEqual(ws.sent[0], "HELLO:alice");
      await ws.recv(line); await settle();
      assert.strictEqual(p.el("nickMsg").hidden, false);
      assert(pattern.test(p.el("nickMsg").textContent), p.el("nickMsg").textContent);
      assert(pattern.test(p.el("status").textContent), p.el("status").textContent);
      assert.strictEqual(ws.sent.length, 1);
    }
    await refused("409 Nickname In Use", /already in use/i);
    await refused("400 Invalid Nickname", /not valid/i);
    await refused("400 Nickname Required", /required/i);
    await refused("400 Bad Request", /rejected/i);
  });

  await test("page: CetaBridge wordmark and title", () => {
    assert(/<title>CetaBridge demo<\/title>/.test(page));
    assert(/<h1 class="wm">CetaBridge<\/h1>/.test(page));
    assert(/by Wilddolphin LLC/.test(page));
    assert(/CetaBridge demo · cb\.wilddolphin\.us/.test(page));
    assert(/CetaCall iPhone app/.test(page));
    assert(/CetaBridge turns the iPhone's CetaCall audio/.test(page));
    const brand = [
      page.match(/<title>([^<]*)<\/title>/)[1],
      page.match(/<h1 class="wm">([^<]*)<\/h1>/)[1],
      page.match(/<p class="foot">([^<]*)<\/p>/)[1],
    ];
    for (const text of brand) assert.strictEqual(/liquid/i.test(text), false, text);
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
