(() => {
  const statusEl = document.getElementById("view-status");
  const codecEl = document.getElementById("view-codec");
  const logEl = document.getElementById("view-log");
  const remoteVideo = document.getElementById("view-remote");
  const remoteAudio = document.getElementById("view-remote-audio");
  const connectBtn = document.getElementById("view-connect");
  const disconnectBtn = document.getElementById("view-disconnect");
  const room = (location.pathname.match(/^\/demo\/view\/([A-Za-z0-9][A-Za-z0-9_-]{0,63})\/?$/) || [])[1];

  let ws = null;
  let pc = null;
  let statsTimer = null;
  let localVideoTrack = null;
  let localAudioTrack = null;
  let answerSent = false;
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
    } catch (error) {
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
    if (outbound) parts.push("out " + outbound + (outboundSize ? " " + outboundSize : ""));
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

  async function shareScreen() {
    const devices = navigator.mediaDevices;
    if (!devices || !devices.getDisplayMedia) {
      log("screen=unavailable");
      if (!ws) await connect();
      return;
    }
    try {
      const display = await devices.getDisplayMedia({
        video: {
          width: { ideal: 1920 },
          height: { ideal: 1080 },
          frameRate: { ideal: 30 }
        },
        audio: false
      });
      localVideoTrack = display.getVideoTracks()[0];
      log("screen=" + screenSettings(localVideoTrack));
      if (answerSent) log("screen=granted-after-answer");
    } catch (error) {
      if (localVideoTrack) localVideoTrack.stop();
      localVideoTrack = null;
      const name = error && error.name;
      const unavailable = name === "NotSupportedError" || name === "NotFoundError" || name === "NotReadableError";
      log(unavailable ? "screen=unavailable" : "screen=cancelled");
    }
    if (!ws) await connect();
  }

  async function shareMic() {
    const devices = navigator.mediaDevices;
    if (!devices || !devices.getUserMedia) {
      log("mic=unavailable");
      return;
    }
    try {
      const mic = await devices.getUserMedia({ audio: true, video: false });
      if (localAudioTrack) localAudioTrack.stop();
      localAudioTrack = mic.getAudioTracks()[0] || null;
      log(localAudioTrack ? "mic=browser" : "mic=unavailable");
      if (answerSent && localAudioTrack) log("mic=granted-after-answer");
    } catch (_error) {
      log("mic=cancelled");
    }
  }

  function stopLocalMedia() {
    if (localVideoTrack) localVideoTrack.stop();
    if (localAudioTrack) localAudioTrack.stop();
    localVideoTrack = null;
    localAudioTrack = null;
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
    pc.onconnectionstatechange = () => setStatus("pc:" + pc.connectionState);
    return pc;
  }

  function attachLocalTracks() {
    let videoTransceiver = null;
    let audioTransceiver = null;
    pc.getTransceivers().forEach((transceiver) => {
      const kind = transceiver.receiver && transceiver.receiver.track
        ? transceiver.receiver.track.kind : "";
      if (kind === "video" && !videoTransceiver) videoTransceiver = transceiver;
      if (kind === "audio" && !audioTransceiver) audioTransceiver = transceiver;
    });
    if (videoTransceiver) {
      if (localVideoTrack) {
        videoTransceiver.direction = "sendrecv";
        videoTransceiver.sender.replaceTrack(localVideoTrack);
        log("screen=sending");
      } else {
        videoTransceiver.direction = "recvonly";
      }
      log("video " + preferH264(videoTransceiver));
    }
    if (audioTransceiver && localAudioTrack) {
      audioTransceiver.direction = "sendrecv";
      audioTransceiver.sender.replaceTrack(localAudioTrack);
      log("audio=mic-sendrecv");
    } else if (audioTransceiver) {
      audioTransceiver.direction = "recvonly";
      log("audio=recvonly");
    }
  }

  async function applyOffer(sdp) {
    const peer = await ensurePeer();
    await peer.setRemoteDescription({ type: "offer", sdp });
    attachLocalTracks();
    while (pendingIce.length) {
      await peer.addIceCandidate(pendingIce.shift());
    }
    const answer = await peer.createAnswer();
    await peer.setLocalDescription(answer);
    ws.send("ANSWER:" + answer.sdp);
    answerSent = true;
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
    if (text.startsWith("480")) {
      log("idle-timeout");
      failWait("disconnected");
      return;
    }
    if (kind === "BYE" || text.startsWith("486") || text.startsWith("400")) {
      setStatus(kind === "BYE" ? "remote-bye" : "rejected");
      disconnect(false);
    }
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
    stopLocalMedia();
    answerSent = false;
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
    stopLocalMedia();
    answerSent = false;
    if (statusEl.textContent.indexOf("remote-bye") !== 0 && statusEl.textContent !== "rejected") {
      setStatus("disconnected");
    }
  }

  async function connect() {
    if (!room || !hostnameOk()) {
      setStatus("bad-address");
      return;
    }
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
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
      if (statsTimer) {
        clearInterval(statsTimer);
        statsTimer = null;
      }
      const status = statusEl.textContent;
      if (status === "remote-bye" || status === "rejected" || status === "disconnected") return;
      codecEl.textContent = "no media";
      setStatus("disconnected");
    };
    statsTimer = setInterval(() => { showCodec().catch(() => {}); }, 2000);
  }

  connectBtn.addEventListener("click", () => {
    connect().catch(() => setStatus("connect-failed"));
  });
  document.getElementById("view-share").addEventListener("click", () => {
    shareScreen().catch(() => setStatus("connect-failed"));
  });
  document.getElementById("view-mic").addEventListener("click", () => {
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
  disconnectBtn.addEventListener("click", () => disconnect(true));
  remoteVideo.addEventListener("click", () => { unlockPlayback(); });
  if (!room) setStatus("missing-room");
})();
