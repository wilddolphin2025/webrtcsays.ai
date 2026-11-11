(function (root) {
  function initialState() {
    return { video: "off", mic: "off" };
  }

  function nextState(state, event) {
    const next = { video: state.video, mic: state.mic };
    if (!event || event.type === "cancel") return next;
    if (event.type === "disconnect") return initialState();
    if (event.type === "grant" && event.kind === "screen") next.video = "screen";
    if (event.type === "grant" && event.kind === "camera") next.video = "camera";
    if (event.type === "grant" && event.kind === "mic") next.mic = "on";
    if (event.type === "stop" && event.kind === "screen" && next.video === "screen") next.video = "off";
    if (event.type === "stop" && event.kind === "camera" && next.video === "camera") next.video = "off";
    if (event.type === "stop" && event.kind === "mic") next.mic = "off";
    if (event.type === "ended" && event.kind === "screen" && next.video === "screen") next.video = "off";
    if (event.type === "ended" && event.kind === "camera" && next.video === "camera") next.video = "off";
    if (event.type === "ended" && event.kind === "mic") next.mic = "off";
    return next;
  }

  function liveTrack(track) {
    return track && track.readyState === "live" ? track : null;
  }

  function trackToSend(realTrack, placeholder) {
    return liveTrack(realTrack) || liveTrack(placeholder) || null;
  }

  function sdpDirection(sdp, kind) {
    let current = "";
    let dir = "missing";
    String(sdp || "").split(/\r?\n/).forEach((line) => {
      if (line.indexOf("m=") === 0) {
        current = line.slice(2).split(" ")[0];
        if (current === kind) dir = "missing";
        return;
      }
      if (current !== kind) return;
      if (line === "a=sendrecv" || line === "a=sendonly" || line === "a=recvonly" || line === "a=inactive") {
        dir = line.slice(2);
      }
    });
    return dir;
  }

  function buttonLabel(kind, state) {
    if (kind === "screen") return state.video === "screen" ? "Stop screen" : "Share screen";
    if (kind === "camera") return state.video === "camera" ? "Stop camera" : "Camera";
    if (kind === "mic") return state.mic === "on" ? "Stop microphone" : "Microphone";
    return kind;
  }

  function sendLabel(state) {
    const video = state.video === "off" ? "video off" : state.video;
    const mic = state.mic === "on" ? "mic on" : "mic off";
    return video + ", " + mic;
  }

  function initialListen() {
    return { userStop: false, socket: "closed", failures: 0, sawOpen: false, retryMs: 1000, afterBye: false };
  }

  function listenAction(state, event) {
    const next = {
      userStop: state.userStop,
      socket: state.socket,
      failures: state.failures,
      sawOpen: state.sawOpen,
      retryMs: state.retryMs,
      afterBye: state.afterBye
    };
    if (event === "pagehide" || event === "click-disconnect") {
      next.userStop = true;
      next.socket = "closed";
      next.afterBye = false;
      return { state: next, action: "close", delay: 0 };
    }
    if (event === "click-connect") {
      next.userStop = false;
      next.failures = 0;
      next.retryMs = 1000;
    }
    if (event === "page") next.userStop = false;
    if (event === "bye") next.afterBye = true;
    if (next.userStop) return { state: next, action: "none", delay: 0 };
    if (event === "opened") {
      next.socket = "open";
      next.sawOpen = true;
      next.failures = 0;
      next.retryMs = 1000;
      next.afterBye = false;
      return { state: next, action: "view", delay: 0 };
    }
    if (event === "closed") {
      const afterBye = next.afterBye;
      next.socket = "closed";
      next.afterBye = false;
      if (!next.sawOpen) next.failures += 1;
      next.sawOpen = false;
      if (next.failures >= 5) return { state: next, action: "sign-in", delay: 0 };
      const delay = afterBye ? 400 : next.retryMs;
      if (!afterBye) next.retryMs = Math.min(next.retryMs * 2, 15000);
      return { state: next, action: "wait", delay: delay };
    }
    if (next.socket === "open" || next.socket === "connecting") {
      return { state: next, action: "none", delay: 0 };
    }
    next.socket = "connecting";
    return { state: next, action: "open", delay: 0 };
  }

  async function bindOutbound(pc, videoTrack, audioTrack) {
    const found = { video: null, audio: null };
    pc.getTransceivers().forEach((transceiver) => {
      const kind = transceiver.receiver && transceiver.receiver.track
        ? transceiver.receiver.track.kind : "";
      if (kind === "video" && !found.video) found.video = transceiver;
      if (kind === "audio" && !found.audio) found.audio = transceiver;
    });
    const result = { video: "none", audio: "none" };
    if (found.video) {
      await found.video.sender.replaceTrack(videoTrack || null);
      found.video.direction = videoTrack ? "sendrecv" : "recvonly";
      result.video = found.video.direction;
    }
    if (found.audio) {
      await found.audio.sender.replaceTrack(audioTrack || null);
      found.audio.direction = audioTrack ? "sendrecv" : "recvonly";
      result.audio = found.audio.direction;
    }
    return result;
  }

  root.FrameMedia = {
    initialState: initialState,
    nextState: nextState,
    liveTrack: liveTrack,
    trackToSend: trackToSend,
    sdpDirection: sdpDirection,
    buttonLabel: buttonLabel,
    sendLabel: sendLabel,
    bindOutbound: bindOutbound,
    initialListen: initialListen,
    listenAction: listenAction
  };
})(typeof globalThis !== "undefined" ? globalThis : this);

(() => {
  const statusEl = document.getElementById("view-status");
  if (!statusEl) return;
  const codecEl = document.getElementById("view-codec");
  const sendEl = document.getElementById("view-send");
  const logEl = document.getElementById("view-log");
  const remoteVideo = document.getElementById("view-remote");
  const remoteAudio = document.getElementById("view-remote-audio");
  const connectBtn = document.getElementById("view-connect");
  const screenBtn = document.getElementById("view-share");
  const cameraBtn = document.getElementById("view-camera");
  const micBtn = document.getElementById("view-mic");
  const disconnectBtn = document.getElementById("view-disconnect");
  const room = (location.pathname.match(/^\/demo\/view\/([A-Za-z0-9][A-Za-z0-9_-]{0,63})\/?$/) || [])[1];

  let ws = null;
  let pc = null;
  let statsTimer = null;
  let answerSent = false;
  let prompting = false;
  let listen = FrameMedia.initialListen();
  let retryTimer = null;
  let state = FrameMedia.initialState();
  let screenTrack = null;
  let cameraTrack = null;
  let micTrack = null;
  let holdVideo = null;
  let holdAudio = null;
  let audioCtx = null;
  const pendingIce = [];

  function log(line) {
    logEl.textContent += line + "\n";
    logEl.scrollTop = logEl.scrollHeight;
  }

  function setStatus(text) {
    statusEl.textContent = text;
    log("status=" + text);
  }

  function hostnameOk() {
    const host = location.hostname;
    const ipv4 = /^(?:\d{1,3}\.){3}\d{1,3}$/.test(host);
    return Boolean(host) && host.indexOf(":") === -1 && !ipv4;
  }

  function paintShare() {
    if (sendEl) sendEl.textContent = "send: " + FrameMedia.sendLabel(state);
    screenBtn.textContent = FrameMedia.buttonLabel("screen", state);
    cameraBtn.textContent = FrameMedia.buttonLabel("camera", state);
    micBtn.textContent = FrameMedia.buttonLabel("mic", state);
    screenBtn.setAttribute("aria-pressed", state.video === "screen" ? "true" : "false");
    cameraBtn.setAttribute("aria-pressed", state.video === "camera" ? "true" : "false");
    micBtn.setAttribute("aria-pressed", state.mic === "on" ? "true" : "false");
  }

  function stopTrack(track) {
    try {
      if (track && track.stop) track.stop();
    } catch (_error) {}
  }

  function activeVideo() {
    if (state.video === "screen") return FrameMedia.liveTrack(screenTrack);
    if (state.video === "camera") return FrameMedia.liveTrack(cameraTrack);
    return null;
  }

  function ensureHold() {
    if (!holdVideo || holdVideo.readyState !== "live") {
      const canvas = document.createElement("canvas");
      canvas.width = 16;
      canvas.height = 16;
      const ctx = canvas.getContext("2d");
      if (ctx) {
        ctx.fillStyle = "#000000";
        ctx.fillRect(0, 0, 16, 16);
      }
      holdVideo = canvas.captureStream(1).getVideoTracks()[0];
    }
    if (!holdAudio || holdAudio.readyState !== "live") {
      const Ctx = window.AudioContext || window.webkitAudioContext;
      if (!Ctx) return;
      if (!audioCtx) audioCtx = new Ctx();
      if (audioCtx.state === "suspended") audioCtx.resume().catch(() => {});
      const osc = audioCtx.createOscillator();
      const gain = audioCtx.createGain();
      gain.gain.value = 0;
      const dest = audioCtx.createMediaStreamDestination();
      osc.connect(gain);
      gain.connect(dest);
      osc.start();
      holdAudio = dest.stream.getAudioTracks()[0];
    }
  }

  function releaseHold() {
    stopTrack(holdVideo);
    stopTrack(holdAudio);
    holdVideo = null;
    holdAudio = null;
    if (audioCtx) {
      audioCtx.close().catch(() => {});
      audioCtx = null;
    }
  }

  function watchTrack(track, kind) {
    track.onended = () => {
      if (kind === "screen" && screenTrack === track) screenTrack = null;
      if (kind === "camera" && cameraTrack === track) cameraTrack = null;
      if (kind === "mic" && micTrack === track) micTrack = null;
      state = FrameMedia.nextState(state, { type: "ended", kind: kind });
      log(kind + "=ended");
      publishOutbound().catch(() => {});
    };
  }

  async function publishOutbound() {
    paintShare();
    if (!pc) return;
    ensureHold();
    const video = FrameMedia.trackToSend(activeVideo(), holdVideo);
    const audio = FrameMedia.trackToSend(state.mic === "on" ? micTrack : null, holdAudio);
    const bound = await FrameMedia.bindOutbound(pc, video, audio);
    log("outbound video=" + (activeVideo() ? state.video : "hold") + " " + bound.video);
    log("outbound audio=" + (state.mic === "on" && micTrack ? "mic" : "hold") + " " + bound.audio);
  }

  async function unlockPlayback() {
    try {
      await remoteVideo.play();
      if (remoteAudio) await remoteAudio.play();
      return true;
    } catch (_error) {
      setStatus("tap-to-play");
      return false;
    }
  }

  function preferH264(transceiver) {
    const caps = window.RTCRtpReceiver && RTCRtpReceiver.getCapabilities
      ? RTCRtpReceiver.getCapabilities("video")
      : null;
    if (!caps || !transceiver.setCodecPreferences) return "preferences-unavailable";
    const h264 = [];
    const rest = [];
    caps.codecs.forEach((codec) => {
      const mime = String(codec.mimeType || "").toLowerCase();
      if (mime === "video/h264") h264.push(codec);
      else if (mime === "video/vp8" || mime === "video/vp9" || mime === "video/rtx" ||
               mime === "video/red" || mime === "video/ulpfec") rest.push(codec);
    });
    if (!h264.length) return "h264-unavailable";
    try {
      transceiver.setCodecPreferences(h264.concat(rest));
    } catch (_error) {
      return "preferences-failed";
    }
    return "h264-preferred";
  }

  function sizeOf(stat) {
    if (!stat || !stat.frameWidth || !stat.frameHeight) return "";
    return stat.frameWidth + "x" + stat.frameHeight;
  }

  async function showCodec() {
    if (!pc || !pc.getStats) return;
    const report = await pc.getStats();
    let inbound = "";
    let outbound = "";
    let inboundSize = "";
    let outboundSize = "";
    let profile = "";
    let inboundFrames = 0;
    let outboundFrames = 0;
    report.forEach((stat) => {
      if (stat.type === "inbound-rtp" && stat.kind === "video") {
        inboundFrames = stat.framesDecoded || 0;
        inboundSize = sizeOf(stat);
        if (stat.codecId) {
          const codec = report.get(stat.codecId);
          if (codec) {
            inbound = codec.mimeType || "";
            const match = /profile-level-id=([0-9a-fA-F]+)/.exec(codec.sdpFmtpLine || "");
            profile = match ? match[1] : "";
          }
        }
      }
      if (stat.type === "outbound-rtp" && stat.kind === "video") {
        outboundFrames = stat.framesSent || 0;
        outboundSize = sizeOf(stat);
        if (stat.codecId) {
          const codec = report.get(stat.codecId);
          if (codec) outbound = codec.mimeType || "";
        }
      }
    });
    const parts = [];
    if (inbound) {
      parts.push("in " + inbound + (inboundSize ? " " + inboundSize : "") +
        (profile ? " profile-level-id=" + profile : "") +
        " frames=" + inboundFrames);
    }
    if (outbound) {
      parts.push("out " + outbound + (outboundSize ? " " + outboundSize : "") +
        " frames=" + outboundFrames);
    }
    codecEl.textContent = parts.length ? parts.join(" ") : "codec pending";
  }

  const TURN_HOSTS = { "rtc.wilddolphin.us": 1 };

  function turnHost(url) {
    const match = /^(turns?):([A-Za-z0-9.-]+)(?::(\d{1,5}))?(?:\?transport=(udp|tcp))?$/.exec(String(url || ""));
    if (!match || url.indexOf("@") !== -1) return "";
    const host = match[2].toLowerCase();
    return TURN_HOSTS[host] ? host : "";
  }

  function iceServersFromTurn(payload) {
    const servers = [{ urls: "stun:stun.l.google.com:19302" }];
    if (!payload || payload.mode !== "static" || payload.temporary !== true ||
        payload.minted === true || payload.rest_api === true) {
      log("TURN unavailable");
      return servers;
    }
    const listed = payload.uris || payload.urls || [];
    const urls = [];
    let host = "";
    for (let i = 0; i < listed.length; i++) {
      const one = turnHost(listed[i]);
      if (!one) continue;
      urls.push(listed[i]);
      if (!host) host = one;
    }
    if (!urls.length || !payload.username || !payload.credential) {
      log("TURN unavailable");
      return servers;
    }
    servers.push({ urls: urls, username: payload.username, credential: payload.credential });
    log("TURN configured host=" + host);
    return servers;
  }

  async function loadIceServers() {
    const stunOnly = [{ urls: "stun:stun.l.google.com:19302" }];
    try {
      const response = await fetch("/demo/turn", { credentials: "same-origin", cache: "no-store" });
      if (!response.ok) {
        log("TURN unavailable");
        return stunOnly;
      }
      return iceServersFromTurn(await response.json());
    } catch (_error) {
      log("TURN unavailable");
      return stunOnly;
    }
  }

  function signalingUrl() {
    const scheme = location.protocol === "https:" ? "wss" : "ws";
    return scheme + "://" + location.host + "/demo/view/ws/" + room;
  }

  function screenSettings(track) {
    const settings = track && track.getSettings ? track.getSettings() : {};
    return (settings.width || "?") + "x" + (settings.height || "?") +
      "@" + (settings.frameRate || "?");
  }

  function takeVideo(kind, track) {
    if (screenTrack && screenTrack !== track) stopTrack(screenTrack);
    if (cameraTrack && cameraTrack !== track) stopTrack(cameraTrack);
    screenTrack = kind === "screen" ? track : null;
    cameraTrack = kind === "camera" ? track : null;
  }

  async function shareScreen() {
    if (prompting) return;
    if (state.video === "screen") {
      stopTrack(screenTrack);
      screenTrack = null;
      state = FrameMedia.nextState(state, { type: "stop", kind: "screen" });
      log("screen=off");
      await publishOutbound();
      return;
    }
    const devices = navigator.mediaDevices;
    if (!devices || !devices.getDisplayMedia) {
      log("screen=unavailable");
      return;
    }
    prompting = true;
    try {
      const display = await devices.getDisplayMedia({
        video: { width: { ideal: 1920 }, height: { ideal: 1080 }, frameRate: { ideal: 30 } },
        audio: false
      });
      const track = display.getVideoTracks()[0];
      if (!track) {
        log("screen=unavailable");
        return;
      }
      takeVideo("screen", track);
      state = FrameMedia.nextState(state, { type: "grant", kind: "screen" });
      watchTrack(track, "screen");
      log("screen=" + screenSettings(track));
      await publishOutbound();
    } catch (error) {
      const name = error && error.name;
      const unavailable = name === "NotSupportedError" || name === "NotFoundError" || name === "NotReadableError";
      log(unavailable ? "screen=unavailable" : "screen=cancelled");
      state = FrameMedia.nextState(state, { type: "cancel", kind: "screen" });
      paintShare();
    } finally {
      prompting = false;
    }
    if (!ws) await connect();
  }

  async function shareCamera() {
    if (prompting) return;
    if (state.video === "camera") {
      stopTrack(cameraTrack);
      cameraTrack = null;
      state = FrameMedia.nextState(state, { type: "stop", kind: "camera" });
      log("camera=off");
      await publishOutbound();
      return;
    }
    const devices = navigator.mediaDevices;
    if (!devices || !devices.getUserMedia) {
      log("camera=unavailable");
      return;
    }
    prompting = true;
    try {
      const camera = await devices.getUserMedia({
        audio: false,
        video: { width: { ideal: 1280 }, height: { ideal: 720 }, frameRate: { ideal: 30 } }
      });
      const track = camera.getVideoTracks()[0];
      if (!track) {
        log("camera=unavailable");
        return;
      }
      takeVideo("camera", track);
      state = FrameMedia.nextState(state, { type: "grant", kind: "camera" });
      watchTrack(track, "camera");
      log("camera=" + screenSettings(track));
      await publishOutbound();
    } catch (error) {
      const name = error && error.name;
      const unavailable = name === "NotSupportedError" || name === "NotFoundError" || name === "NotReadableError";
      log(unavailable ? "camera=unavailable" : "camera=cancelled");
      state = FrameMedia.nextState(state, { type: "cancel", kind: "camera" });
      paintShare();
    } finally {
      prompting = false;
    }
  }

  async function shareMic() {
    if (prompting) return;
    if (state.mic === "on") {
      stopTrack(micTrack);
      micTrack = null;
      state = FrameMedia.nextState(state, { type: "stop", kind: "mic" });
      log("mic=off");
      await publishOutbound();
      return;
    }
    const devices = navigator.mediaDevices;
    if (!devices || !devices.getUserMedia) {
      log("mic=unavailable");
      return;
    }
    prompting = true;
    try {
      const mic = await devices.getUserMedia({ audio: true, video: false });
      const track = mic.getAudioTracks()[0] || null;
      if (!track) {
        log("mic=unavailable");
        return;
      }
      stopTrack(micTrack);
      micTrack = track;
      state = FrameMedia.nextState(state, { type: "grant", kind: "mic" });
      watchTrack(track, "mic");
      log("mic=browser");
      await publishOutbound();
    } catch (_error) {
      log("mic=cancelled");
      state = FrameMedia.nextState(state, { type: "cancel", kind: "mic" });
      paintShare();
    } finally {
      prompting = false;
    }
  }

  function stopUserMedia() {
    stopTrack(screenTrack);
    stopTrack(cameraTrack);
    stopTrack(micTrack);
    screenTrack = null;
    cameraTrack = null;
    micTrack = null;
    state = FrameMedia.nextState(state, { type: "disconnect" });
    releaseHold();
    paintShare();
  }

  async function ensurePeer() {
    if (pc) return pc;
    const iceServers = await loadIceServers();
    pc = new RTCPeerConnection({ iceServers });
    pc.ontrack = (event) => {
      const stream = (event.streams && event.streams[0]) || new MediaStream([event.track]);
      if (event.track.kind === "video") remoteVideo.srcObject = stream;
      if (event.track.kind === "audio" && remoteAudio) remoteAudio.srcObject = stream;
      unlockPlayback();
      setStatus("receiving");
    };
    pc.onicecandidate = (event) => {
      if (!event.candidate || !ws || ws.readyState !== WebSocket.OPEN) return;
      ws.send("ICE:" + (event.candidate.sdpMLineIndex || 0) + ":" + event.candidate.candidate);
    };
    pc.onconnectionstatechange = () => {
      if (pc) setStatus("pc:" + pc.connectionState);
    };
    pc.getTransceivers().forEach((transceiver) => {
      if (transceiver.receiver && transceiver.receiver.track &&
          transceiver.receiver.track.kind === "video") {
        log("video " + preferH264(transceiver));
      }
    });
    return pc;
  }

  async function applyOffer(sdp) {
    const peer = await ensurePeer();
    await peer.setRemoteDescription({ type: "offer", sdp });
    peer.getTransceivers().forEach((transceiver) => {
      if (transceiver.receiver && transceiver.receiver.track &&
          transceiver.receiver.track.kind === "video") {
        log("video " + preferH264(transceiver));
      }
    });
    await publishOutbound();
    while (pendingIce.length) {
      await peer.addIceCandidate(pendingIce.shift());
    }
    const answer = await peer.createAnswer();
    await peer.setLocalDescription(answer);
    ws.send("ANSWER:" + answer.sdp);
    answerSent = true;
    log("answer video=" + FrameMedia.sdpDirection(answer.sdp, "video"));
    log("answer audio=" + FrameMedia.sdpDirection(answer.sdp, "audio"));
    log("ANSWER sent");
  }

  function handleMessage(raw) {
    const text = String(raw || "");
    const kind = text.split(":", 1)[0];
    log("<< " + kind);
    if (kind === "HELLO") {
      ws.send("200 OK");
      return;
    }
    if (text.startsWith("INVITE")) {
      ws.send("WAITING");
      return;
    }
    if (text.startsWith("OFFER:")) {
      applyOffer(text.slice("OFFER:".length)).catch(() => setStatus("answer-failed"));
      return;
    }
    if (text.startsWith("ICE:")) {
      const payload = text.slice("ICE:".length);
      const split = payload.indexOf(":");
      if (split < 0) return;
      const candidate = {
        candidate: payload.slice(split + 1),
        sdpMLineIndex: Number(payload.slice(0, split)) || 0
      };
      if (pc && pc.remoteDescription) pc.addIceCandidate(candidate).catch(() => {});
      else pendingIce.push(candidate);
      return;
    }
    if (text.startsWith("480")) {
      log("idle-timeout");
      finishCall("waiting");
      return;
    }
    if (kind === "BYE") {
      listen = FrameMedia.listenAction(listen, "bye").state;
      log("call-end");
      finishCall("waiting");
      return;
    }
    if (text.startsWith("486") || text.startsWith("400")) {
      setStatus("rejected");
      disconnect(false);
    }
  }

  function finishCall(reason) {
    if (pc) {
      pc.close();
      pc = null;
    }
    answerSent = false;
    pendingIce.length = 0;
    codecEl.textContent = "no media";
    setStatus(reason);
  }

  function failWait(reason) {
    codecEl.textContent = "no media";
    setStatus(reason);
    if (statsTimer) {
      clearInterval(statsTimer);
      statsTimer = null;
    }
    if (pc) {
      pc.close();
      pc = null;
    }
    if (ws) {
      const socket = ws;
      ws = null;
      try { socket.close(); } catch (_error) {}
    }
    stopUserMedia();
    answerSent = false;
    pendingIce.length = 0;
  }

  function disconnect(sendBye) {
    if (retryTimer) {
      clearTimeout(retryTimer);
      retryTimer = null;
    }
    if (statsTimer) {
      clearInterval(statsTimer);
      statsTimer = null;
    }
    if (sendBye && ws && ws.readyState === WebSocket.OPEN) {
      try { ws.send("BYE"); } catch (_error) {}
    }
    if (pc) {
      pc.close();
      pc = null;
    }
    if (ws) {
      const socket = ws;
      ws = null;
      socket.onclose = null;
      try { socket.close(); } catch (_error) {}
    }
    listen.socket = "closed";
    stopUserMedia();
    answerSent = false;
    pendingIce.length = 0;
    if (statusEl.textContent.indexOf("remote-bye") !== 0 && statusEl.textContent !== "rejected" &&
        statusEl.textContent !== "sign-in") {
      setStatus("disconnected");
    }
  }

  function scheduleListen(delay) {
    if (retryTimer || listen.userStop) return;
    retryTimer = setTimeout(() => {
      retryTimer = null;
      connect("retry").catch(() => setStatus("connect-failed"));
    }, delay);
  }

  async function connect(event) {
    const decision = FrameMedia.listenAction(listen, event || "page");
    listen = decision.state;
    if (decision.action !== "open") return;
    if (!room || !hostnameOk()) {
      listen.socket = "closed";
      setStatus("bad-address");
      return;
    }
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
      listen.socket = "open";
      return;
    }
    if (statsTimer) {
      clearInterval(statsTimer);
      statsTimer = null;
    }
    ensureHold();
    unlockPlayback();
    setStatus("connecting");
    ws = new WebSocket(signalingUrl());
    ws.onopen = () => {
      const opened = FrameMedia.listenAction(listen, "opened");
      listen = opened.state;
      ws.send("VIEW");
      setStatus("waiting");
    };
    ws.onmessage = (event) => handleMessage(event.data);
    ws.onerror = () => setStatus("signaling-error");
    ws.onclose = () => {
      ws = null;
      if (pc) {
        pc.close();
        pc = null;
      }
      answerSent = false;
      pendingIce.length = 0;
      const closed = FrameMedia.listenAction(listen, "closed");
      listen = closed.state;
      if (closed.action === "sign-in") {
        codecEl.textContent = "no media";
        setStatus("sign-in");
        return;
      }
      if (closed.action === "wait") {
        codecEl.textContent = "no media";
        setStatus("waiting");
        scheduleListen(closed.delay);
      }
    };
    statsTimer = setInterval(() => { showCodec().catch(() => {}); }, 2000);
  }

  connectBtn.addEventListener("click", () => {
    if (retryTimer) {
      clearTimeout(retryTimer);
      retryTimer = null;
    }
    if (listen.socket === "open" || listen.socket === "connecting") return;
    connect("click-connect").catch(() => setStatus("connect-failed"));
  });
  screenBtn.addEventListener("click", () => {
    shareScreen().catch(() => log("screen=cancelled"));
  });
  cameraBtn.addEventListener("click", () => {
    shareCamera().catch(() => log("camera=cancelled"));
  });
  micBtn.addEventListener("click", () => {
    shareMic().catch(() => log("mic=cancelled"));
  });
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState !== "visible") return;
    const status = statusEl.textContent;
    if (status !== "waiting" && status !== "connecting") return;
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      codecEl.textContent = "no media";
      setStatus("disconnected");
    }
  });
  disconnectBtn.addEventListener("click", () => {
    listen = FrameMedia.listenAction(listen, "click-disconnect").state;
    disconnect(true);
  });
  window.addEventListener("pagehide", () => {
    listen = FrameMedia.listenAction(listen, "pagehide").state;
    disconnect(true);
  });
  remoteVideo.addEventListener("click", () => { unlockPlayback(); });
  paintShare();
  if (!room || !hostnameOk()) {
    setStatus(room ? "bad-address" : "missing-room");
  } else {
    connect("page").catch(() => setStatus("connect-failed"));
  }
})();
