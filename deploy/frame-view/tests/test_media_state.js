"use strict";

const fs = require("fs");
const path = require("path");
const vm = require("vm");

const source = fs.readFileSync(path.join(__dirname, "../page/view.js"), "utf8");
const context = {
  console,
  document: {
    getElementById() {
      return null;
    },
    addEventListener() {}
  }
};
context.globalThis = context;
vm.runInNewContext(source, context, { filename: "view.js" });
const media = context.FrameMedia;
if (!media) {
  console.error("FrameMedia missing");
  process.exit(1);
}

let failed = 0;
function check(name, cond) {
  if (!cond) {
    failed += 1;
    console.error("FAIL " + name);
  }
}

const off = media.initialState();
check("initial off", off.video === "off" && off.mic === "off");

const screen = media.nextState(off, { type: "grant", kind: "screen" });
check("grant screen", screen.video === "screen" && screen.mic === "off");
const cameraKeeps = media.nextState(screen, { type: "cancel", kind: "camera" });
check("cancel keeps screen", cameraKeeps.video === "screen");
const camera = media.nextState(screen, { type: "grant", kind: "camera" });
check("camera replaces screen", camera.video === "camera");
const stopCameraOnly = media.nextState(camera, { type: "stop", kind: "screen" });
check("stop screen ignores camera", stopCameraOnly.video === "camera");
const mic = media.nextState(off, { type: "grant", kind: "mic" });
check("grant mic", mic.mic === "on" && mic.video === "off");
const micCancel = media.nextState(mic, { type: "cancel", kind: "mic" });
check("cancel keeps mic", micCancel.mic === "on");
const micStop = media.nextState(mic, { type: "stop", kind: "mic" });
check("stop mic", micStop.mic === "off");
const ended = media.nextState(screen, { type: "ended", kind: "screen" });
check("ended screen", ended.video === "off");
const dropped = media.nextState(mic, { type: "disconnect" });
check("disconnect clears", dropped.video === "off" && dropped.mic === "off");

const live = { readyState: "live" };
const dead = { readyState: "ended" };
const hold = { readyState: "live", hold: true };
check("real track wins", media.trackToSend(live, hold) === live);
check("hold when real ended", media.trackToSend(dead, hold) === hold);
check("hold when missing", media.trackToSend(null, hold) === hold);
check("nothing live", media.trackToSend(dead, dead) === null);

const sdp = [
  "m=audio 9 UDP/TLS/RTP/SAVPF 111",
  "a=sendrecv",
  "m=video 9 UDP/TLS/RTP/SAVPF 102",
  "a=recvonly"
].join("\r\n");
check("audio sendrecv", media.sdpDirection(sdp, "audio") === "sendrecv");
check("video recvonly", media.sdpDirection(sdp, "video") === "recvonly");
check("labels", media.buttonLabel("screen", screen) === "Stop screen");
check("labels off", media.buttonLabel("mic", off) === "Microphone");
check("send label", media.sendLabel(camera) === "camera, mic off");

if (failed) {
  console.error(failed + " failed");
  process.exit(1);
}
console.log("media-state ok");
