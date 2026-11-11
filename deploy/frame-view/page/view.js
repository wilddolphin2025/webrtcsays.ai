(() => {
  const statusEl = document.getElementById("view-status");
  const codecEl = document.getElementById("view-codec");
  const logEl = document.getElementById("view-log");
  const remoteVideo = document.getElementById("view-remote");
  const connectBtn = document.getElementById("view-connect");
  const disconnectBtn = document.getElementById("view-disconnect");
  const room = (location.pathname.match(/^\/demo\/view\/([A-Za-z0-9][A-Za-z0-9_-]{0,63})\/?$/) || [])[1];

  let ws = null;
  let pc = null;
  let statsTimer = null;
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

  async function unlockPlayback() {
    try {
      await remoteVideo.play();
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
    } catch (error) {
      return "preferences-failed";
    }
    return "h264-preferred";
  }

  async function showCodec() {
    if (!pc || !pc.getStats) return;
    const report = await pc.getStats();
    let mime = "";
    let profile = "";
    report.forEach((stat) => {
      if (stat.type === "inbound-rtp" && stat.kind === "video" && stat.codecId) {
        const codec = report.get(stat.codecId);
        if (codec) {
          mime = codec.mimeType || "";
          const fmtp = codec.sdpFmtpLine || "";
          const match = /profile-level-id=([0-9a-fA-F]+)/.exec(fmtp);
          profile = match ? match[1] : "";
        }
      }
    });
    codecEl.textContent = mime
      ? (mime + (profile ? " profile-level-id=" + profile : ""))
      : "codec pending";
  }

  function iceServersFromTurn(payload) {
    const servers = [{ urls: "stun:stun.l.google.com:19302" }];
    if (!payload || !payload.urls) return servers;
    servers.push({
      urls: payload.urls,
      username: payload.username,
      credential: payload.credential
    });
    return servers;
  }

  async function loadIceServers() {
    try {
      const response = await fetch("/demo/turn", { credentials: "same-origin", cache: "no-store" });
      if (!response.ok) return [{ urls: "stun:stun.l.google.com:19302" }];
      return iceServersFromTurn(await response.json());
    } catch (_error) {
      return [{ urls: "stun:stun.l.google.com:19302" }];
    }
  }

  function signalingUrl() {
    const scheme = location.protocol === "https:" ? "wss" : "ws";
    return scheme + "://" + location.host + "/demo/view/ws/" + room;
  }

  async function ensurePeer() {
    if (pc) return pc;
    const iceServers = await loadIceServers();
    pc = new RTCPeerConnection({ iceServers });
    const video = pc.addTransceiver("video", { direction: "recvonly" });
    pc.addTransceiver("audio", { direction: "recvonly" });
    log("video " + preferH264(video));
    pc.ontrack = (event) => {
      const stream = (event.streams && event.streams[0]) || new MediaStream([event.track]);
      if (event.track.kind === "video") remoteVideo.srcObject = stream;
      unlockPlayback();
      setStatus("receiving");
    };
    pc.onicecandidate = (event) => {
      if (!event.candidate || !ws || ws.readyState !== WebSocket.OPEN) return;
      ws.send("ICE:" + (event.candidate.sdpMLineIndex || 0) + ":" + event.candidate.candidate);
    };
    pc.onconnectionstatechange = () => setStatus("pc:" + pc.connectionState);
    return pc;
  }

  async function applyOffer(sdp) {
    const peer = await ensurePeer();
    await peer.setRemoteDescription({ type: "offer", sdp });
    while (pendingIce.length) {
      await peer.addIceCandidate(pendingIce.shift());
    }
    const answer = await peer.createAnswer();
    await peer.setLocalDescription(answer);
    ws.send("ANSWER:" + answer.sdp);
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
      applyOffer(text.slice("OFFER:".length)).catch((error) => setStatus("answer-failed"));
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
    if (kind === "BYE" || text.startsWith("486") || text.startsWith("480") || text.startsWith("400")) {
      setStatus(kind === "BYE" ? "remote-bye" : "rejected");
      disconnect(false);
    }
  }

  function disconnect(sendBye) {
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
      ws.close();
      ws = null;
    }
    if (statusEl.textContent.indexOf("remote-bye") !== 0 && statusEl.textContent !== "rejected") {
      setStatus("disconnected");
    }
  }

  function connect() {
    if (!room || !hostnameOk()) {
      setStatus("bad-address");
      return;
    }
    unlockPlayback();
    setStatus("connecting");
    ws = new WebSocket(signalingUrl());
    ws.onopen = () => {
      ws.send("VIEW");
      setStatus("waiting");
    };
    ws.onmessage = (event) => handleMessage(event.data);
    ws.onerror = () => setStatus("signaling-error");
    ws.onclose = () => {
      if (statusEl.textContent === "waiting" || statusEl.textContent === "connecting") {
        setStatus("disconnected");
      }
    };
    statsTimer = setInterval(() => { showCodec().catch(() => {}); }, 2000);
  }

  connectBtn.addEventListener("click", connect);
  disconnectBtn.addEventListener("click", () => disconnect(true));
  remoteVideo.addEventListener("click", () => { unlockPlayback(); });
  if (!room) setStatus("missing-room");
})();
