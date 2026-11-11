(function (root, factory) {
	"use strict";
	var api = factory();
	var isBrowser = root && root.document;
	if (!isBrowser && typeof module === "object" && module.exports) {
		module.exports = api;
	} else {
		root.DoomNetStreamTunes = api;
	}
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
	"use strict";

	function cleanCode(value) {
		var raw = String(value || "").toUpperCase().replace(/[^A-Z0-9]/g, "").slice(0, 8);
		return raw.length === 8 ? raw.slice(0, 4) + "-" + raw.slice(4) : raw;
	}

	function noticeFor(reason) {
		switch (reason) {
			case "host-ended": return "Host Ended Session";
			case "host-disconnected": return "Host Disconnected";
			case "connection-lost": return "Connection Lost";
			case "stream-error": return "Stream Error";
			default: return "";
		}
	}

	function normalizeIceServers(value) {
		if (!Array.isArray(value) || value.length > 8) {
			throw new Error("Invalid StreamTunes ICE server list");
		}
		return value.map(function (entry) {
			if (!entry || typeof entry !== "object" || Array.isArray(entry)) {
				throw new Error("Invalid StreamTunes ICE server");
			}
			var urls = Array.isArray(entry.urls) ? entry.urls.slice() : [entry.urls];
			if (!urls.length || urls.length > 8 || urls.some(function (url) {
				return typeof url !== "string" || url.length > 2048 || !/^(stun|stuns|turn|turns):/i.test(url);
			})) {
				throw new Error("Invalid StreamTunes ICE server URL");
			}
			var result = { urls: Array.isArray(entry.urls) ? urls : urls[0] };
			if (entry.username !== undefined) {
				if (typeof entry.username !== "string" || entry.username.length > 512) throw new Error("Invalid StreamTunes ICE username");
				result.username = entry.username;
			}
			if (entry.credential !== undefined) {
				if (typeof entry.credential !== "string" || entry.credential.length > 512) throw new Error("Invalid StreamTunes ICE credential");
				result.credential = entry.credential;
			}
			return result;
		});
	}

	function StreamTunesClient(options) {
		options = options || {};
		this.url = String(options.url || "");
		this.name = String(options.name || "").trim();
		this.WebSocketImpl = options.WebSocket || (typeof WebSocket !== "undefined" ? WebSocket : null);
		this.PeerImpl = options.RTCPeerConnection || (typeof RTCPeerConnection !== "undefined" ? RTCPeerConnection : null);
		this.iceServers = normalizeIceServers(options.iceServers === undefined ?
			[{ urls: "stun:stun.l.google.com:19302" }] : options.iceServers);
		this.onRemoteStream = options.onRemoteStream || function () {};
		this.onRemoteStop = options.onRemoteStop || function () {};
		this.onNotice = options.onNotice || function () {};
		this.reconnectMs = options.reconnectMs == null ? 15000 : Math.max(0, Number(options.reconnectMs));
		if (!Number.isFinite(this.reconnectMs)) this.reconnectMs = 15000;
		this.setTimeoutImpl = options.setTimeout || setTimeout;
		this.clearTimeoutImpl = options.clearTimeout || clearTimeout;
		this.peerReconnect = new Map();
		this.socket = null;
		this.queue = [];
		this.listeners = [];
		this.peers = new Map();
		this.hostStream = null;
		this.role = "none";
		this.active = false;
		this.code = "";
		this.count = 0;
		this.capacity = 5;
		this.pingMs = null;
		this.roster = [];
		this.hostId = "";
		this.statsTimer = null;
		this.statsBytes = 0;
		this.statsAt = 0;
		this.pending = null;
		this.closedByUser = false;
	}

	StreamTunesClient.prototype.snapshot = function () {
		return {
			role: this.role,
			active: this.active,
			code: this.code,
			count: this.count,
			capacity: this.capacity,
			pingMs: this.pingMs,
			peers: this.roster.slice(),
		};
	};

	StreamTunesClient.prototype.subscribe = function (listener) {
		if (typeof listener !== "function") {
			return function () {};
		}
		this.listeners.push(listener);
		var self = this;
		return function () {
			var at = self.listeners.indexOf(listener);
			if (at >= 0) self.listeners.splice(at, 1);
		};
	};

	StreamTunesClient.prototype.publish = function () {
		var value = this.snapshot();
		this.listeners.slice().forEach(function (listener) {
			try { listener(value); } catch (error) {}
		});
	};

	StreamTunesClient.prototype.validate = function () {
		if (!this.url || !/^wss?:\/\//i.test(this.url)) {
			throw new Error("StreamTunes relay URL is not configured");
		}
		if (!this.name || this.name.length > 64) {
			throw new Error("Steam persona name is required");
		}
		if (!this.WebSocketImpl || !this.PeerImpl) {
			throw new Error("This Steam browser cannot start StreamTunes");
		}
	};

	StreamTunesClient.prototype.connect = function () {
		this.validate();
		if (this.socket && (this.socket.readyState === 0 || this.socket.readyState === 1)) {
			return;
		}
		var self = this;
		var socket = new this.WebSocketImpl(this.url);
		this.socket = socket;
		socket.onopen = function () {
			var queued = self.queue.splice(0);
			queued.forEach(function (message) { socket.send(message); });
		};
		socket.onmessage = function (event) {
			var message;
			try {
				message = JSON.parse(String(event.data || ""));
			} catch (error) {
				self.fail("Stream Error");
				return;
			}
			Promise.resolve(self.handle(message)).catch(function () { self.fail("Stream Error"); });
		};
		socket.onerror = function () {
			if (self.pending) self.rejectPending(new Error("Couldn't Join Party"));
		};
		socket.onclose = function () {
			self.socket = null;
			if (self.active && !self.closedByUser) {
				self.fail(self.role === "listener" ? "Host Disconnected" : "Connection Lost");
			}
			self.closedByUser = false;
		};
	};

	StreamTunesClient.prototype.send = function (value) {
		this.connect();
		var text = JSON.stringify(value);
		if (this.socket && this.socket.readyState === 1) {
			this.socket.send(text);
		} else {
			this.queue.push(text);
		}
	};

	StreamTunesClient.prototype.waitForRoom = function (message) {
		if (this.pending) {
			return Promise.reject(new Error("A StreamTunes action is already pending"));
		}
		var self = this;
		return new Promise(function (resolve, reject) {
			self.pending = { resolve: resolve, reject: reject };
			try {
				self.send(message);
			} catch (error) {
				self.pending = null;
				reject(error);
			}
		});
	};

	StreamTunesClient.prototype.resolvePending = function () {
		if (!this.pending) return;
		var pending = this.pending;
		this.pending = null;
		pending.resolve(this.snapshot());
	};

	StreamTunesClient.prototype.rejectPending = function (error) {
		if (!this.pending) return;
		var pending = this.pending;
		this.pending = null;
		pending.reject(error);
	};

	StreamTunesClient.prototype.start = function (stream) {
		if (!stream || !stream.getAudioTracks || !stream.getAudioTracks().length) {
			return Promise.reject(new Error("StreamTunes has no host audio"));
		}
		this.hostStream = stream;
		this.role = "host";
		this.closedByUser = false;
		return this.waitForRoom({ type: "create", name: this.name });
	};

	StreamTunesClient.prototype.join = function (code) {
		code = cleanCode(code);
		if (code.length !== 9) {
			return Promise.reject(new Error("Couldn't Join Party"));
		}
		this.role = "listener";
		this.closedByUser = false;
		return this.waitForRoom({ type: "join", code: code, name: this.name });
	};

	StreamTunesClient.prototype.end = function () {
		this.disconnectRelay(this.active && this.role === "host" ? "end" : null);
		this.reset();
		return Promise.resolve();
	};

	StreamTunesClient.prototype.leave = function () {
		this.disconnectRelay(this.active && this.role === "listener" ? "leave" : null);
		this.reset();
		return Promise.resolve();
	};

	StreamTunesClient.prototype.peer = function (peerId, hosting) {
		if (this.peers.has(peerId)) return this.peers.get(peerId);
		var self = this;
		var peer = new this.PeerImpl({ iceServers: this.iceServers });
		peer.onicecandidate = function (event) {
			if (event.candidate) self.send({ type: "signal", to: peerId, data: { candidate: event.candidate } });
		};
		peer.onconnectionstatechange = function () {
			var state = peer.connectionState;
			if (state === "connected" || state === "connecting") {
				self.clearPeerReconnect(peerId);
				return;
			}
			if (state !== "disconnected" && state !== "failed") {
				return;
			}
			// Brief blips get a reconnect window before the party tears down.
			self.armPeerReconnect(peerId);
		};
		if (hosting && this.hostStream) {
			this.hostStream.getTracks().forEach(function (track) { peer.addTrack(track, self.hostStream); });
		} else {
			peer.ontrack = function (event) {
				var stream = event.streams && event.streams[0];
				if (stream) self.onRemoteStream(stream);
			};
		}
		this.peers.set(peerId, peer);
		return peer;
	};

	StreamTunesClient.prototype.offer = async function (peerId) {
		var peer = this.peer(peerId, true);
		await peer.setLocalDescription(await peer.createOffer());
		this.send({ type: "signal", to: peerId, data: { description: peer.localDescription } });
	};

	StreamTunesClient.prototype.signal = async function (message) {
		var peerId = String(message.from || "");
		var data = message.data || {};
		var peer = this.peer(peerId, this.role === "host");
		if (data.description) {
			await peer.setRemoteDescription(data.description);
			if (data.description.type === "offer") {
				await peer.setLocalDescription(await peer.createAnswer());
				this.send({ type: "signal", to: peerId, data: { description: peer.localDescription } });
			}
		}
		if (data.candidate) await peer.addIceCandidate(data.candidate);
	};

	StreamTunesClient.prototype.handle = async function (message) {
		switch (message.type) {
			case "created":
				if (Object.prototype.hasOwnProperty.call(message, "iceServers")) {
					this.iceServers = normalizeIceServers(message.iceServers);
				}
				this.role = "host";
				this.active = true;
				this.code = String(message.code || "");
				this.count = Number(message.count) || 1;
				this.capacity = Number(message.capacity) || 5;
				this.resolvePending();
				this.publish();
				return;
			case "joined":
				if (Object.prototype.hasOwnProperty.call(message, "iceServers")) {
					this.iceServers = normalizeIceServers(message.iceServers);
				}
				this.role = "listener";
				this.active = true;
				this.code = String(message.code || "");
				this.hostId = String(message.hostId || "");
				this.count = Number(message.count) || 2;
				this.capacity = Number(message.capacity) || 5;
				this.resolvePending();
				this.startStats();
				this.publish();
				return;
			case "peer-joined":
				if (this.role === "host") await this.offer(String(message.peerId || ""));
				return;
			case "peer-left":
				this.closePeer(String(message.peerId || ""));
				return;
			case "signal":
				await this.signal(message);
				return;
			case "room":
				this.count = Number(message.count) || this.count;
				this.capacity = Number(message.capacity) || 5;
				this.publish();
				return;
			case "roster":
				this.code = String(message.code || this.code);
				this.count = Number(message.count) || 1;
				this.capacity = Number(message.capacity) || 5;
				this.roster = Array.isArray(message.peers) ? message.peers : [];
				this.publish();
				return;
			case "ended":
				this.fail(noticeFor(message.reason) || "Host Disconnected");
				return;
			case "error":
				this.rejectPending(new Error(String(message.message || "Couldn't Join Party")));
				if (!this.active) this.reset();
				return;
		}
	};

	StreamTunesClient.prototype.startStats = function () {
		this.stopStats();
		var self = this;
		this.statsTimer = setInterval(function () { self.sampleStats(); }, 1000);
		if (this.statsTimer && typeof this.statsTimer.unref === "function") this.statsTimer.unref();
	};

	StreamTunesClient.prototype.sampleStats = async function () {
		var peer = this.peers.get(this.hostId);
		if (!peer || !peer.getStats) return;
		var reports = await peer.getStats();
		var ping = this.pingMs;
		var bytes = this.statsBytes;
		reports.forEach(function (report) {
			if (report.type === "candidate-pair" && report.state === "succeeded" && report.currentRoundTripTime != null) {
				ping = Math.round(report.currentRoundTripTime * 1000);
			}
			if (report.type === "inbound-rtp" && report.kind === "audio" && !report.isRemote) {
				bytes = Number(report.bytesReceived) || bytes;
			}
		});
		var now = Date.now();
		var kbps = this.statsAt && now > this.statsAt ? Math.round((bytes - this.statsBytes) * 8 / (now - this.statsAt)) : 0;
		this.statsBytes = bytes;
		this.statsAt = now;
		this.pingMs = ping;
		this.send({ type: "stats", pingMs: ping || 0, kbps: Math.max(0, kbps) });
		this.publish();
	};

	StreamTunesClient.prototype.stopStats = function () {
		if (this.statsTimer) clearInterval(this.statsTimer);
		this.statsTimer = null;
		this.statsBytes = 0;
		this.statsAt = 0;
	};

	StreamTunesClient.prototype.clearPeerReconnect = function (peerId) {
		var timer = this.peerReconnect.get(peerId);
		if (timer) this.clearTimeoutImpl(timer);
		this.peerReconnect.delete(peerId);
	};

	StreamTunesClient.prototype.clearAllPeerReconnect = function () {
		var self = this;
		Array.from(this.peerReconnect.keys()).forEach(function (peerId) {
			self.clearPeerReconnect(peerId);
		});
	};

	StreamTunesClient.prototype.armPeerReconnect = function (peerId) {
		if (this.peerReconnect.has(peerId)) return;
		var self = this;
		var finish = function () {
			self.peerReconnect.delete(peerId);
			var peer = self.peers.get(peerId);
			if (!peer) return;
			var state = peer.connectionState;
			if (state === "connected" || state === "connecting") return;
			if (self.role === "host") {
				self.closePeer(peerId);
				return;
			}
			self.fail("Connection Lost");
		};
		if (this.reconnectMs <= 0) {
			finish();
			return;
		}
		var timer = this.setTimeoutImpl(finish, this.reconnectMs);
		this.peerReconnect.set(peerId, timer);
	};

	StreamTunesClient.prototype.closePeer = function (peerId) {
		this.clearPeerReconnect(peerId);
		var peer = this.peers.get(peerId);
		if (!peer) return;
		try { peer.close(); } catch (error) {}
		this.peers.delete(peerId);
	};

	StreamTunesClient.prototype.reset = function () {
		this.clearAllPeerReconnect();
		this.stopStats();
		for (var peerId of Array.from(this.peers.keys())) this.closePeer(peerId);
		this.onRemoteStop();
		this.hostStream = null;
		this.role = "none";
		this.active = false;
		this.code = "";
		this.count = 0;
		this.pingMs = null;
		this.roster = [];
		this.hostId = "";
		this.publish();
	};

	StreamTunesClient.prototype.disconnectRelay = function (action) {
		var socket = this.socket;
		if (socket && socket.readyState === 1) {
			if (action === "leave" && this.active && this.role === "listener") {
				try { socket.send(JSON.stringify({ type: "leave" })); } catch (error) {}
			} else if (action === "end" && this.active && this.role === "host") {
				try { socket.send(JSON.stringify({ type: "end" })); } catch (error) {}
			}
		}
		this.closedByUser = true;
		this.queue.length = 0;
		if (socket) {
			try { socket.close(); } catch (error) {}
		}
		this.socket = null;
	};

	StreamTunesClient.prototype.fail = function (message) {
		this.rejectPending(new Error(message || "Stream Error"));
		var action = this.role === "host" ? "end" : (this.role === "listener" ? "leave" : null);
		this.disconnectRelay(action);
		this.reset();
		if (message) this.onNotice(message);
	};

	return {
		StreamTunesClient: StreamTunesClient,
		cleanCode: cleanCode,
		normalizeIceServers: normalizeIceServers,
		noticeFor: noticeFor,
	};
});
