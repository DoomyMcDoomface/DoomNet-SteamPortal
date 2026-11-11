"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const { StreamTunesClient, cleanCode, noticeFor } = require("../../../src/streamtunes_client");

class MockSocket {
	static instances = [];
	constructor(url) {
		this.url = url;
		this.readyState = 0;
		this.sent = [];
		MockSocket.instances.push(this);
	}
	open() {
		this.readyState = 1;
		this.onopen();
	}
	send(value) {
		this.sent.push(JSON.parse(value));
	}
	receive(value) {
		this.onmessage({ data: JSON.stringify(value) });
	}
	closeUnexpected() {
		this.readyState = 3;
		this.onclose();
	}
}

class MockPeer {
	static instances = [];
	constructor(options) {
		this.options = options;
		this.connectionState = "new";
		this.tracks = [];
		this.localDescription = null;
		this.remoteDescription = null;
		this.closed = false;
		MockPeer.instances.push(this);
	}
	addTrack(track, stream) {
		this.tracks.push({ track, stream });
	}
	async createOffer() {
		return { type: "offer", sdp: "offer-sdp" };
	}
	async createAnswer() {
		return { type: "answer", sdp: "answer-sdp" };
	}
	async setLocalDescription(value) {
		this.localDescription = value;
	}
	async setRemoteDescription(value) {
		this.remoteDescription = value;
	}
	async addIceCandidate(value) {
		this.candidate = value;
	}
	async getStats() {
		return [
			{ type: "candidate-pair", state: "succeeded", currentRoundTripTime: 0.032 },
			{ type: "inbound-rtp", kind: "audio", bytesReceived: 16000, isRemote: false },
		];
	}
	close() {
		this.closed = true;
	}
}

function options(extra) {
	return Object.assign({
		url: "wss://relay.example/streamtunes",
		name: "Steam Persona",
		WebSocket: MockSocket,
		RTCPeerConnection: MockPeer,
		iceServers: [],
		reconnectMs: 0,
	}, extra || {});
}

function audioStream() {
	const track = { kind: "audio" };
	return {
		track,
		getTracks: () => [track],
		getAudioTracks: () => [track],
	};
}

function tick() {
	return new Promise((resolve) => setImmediate(resolve));
}

test.beforeEach(() => {
	MockSocket.instances.length = 0;
	MockPeer.instances.length = 0;
});

test("codes and end reasons are normalized for the player", () => {
	assert.equal(cleanCode("k7qm 4p2l"), "K7QM-4P2L");
	assert.equal(noticeFor("host-ended"), "Host Ended Session");
	assert.equal(noticeFor("host-disconnected"), "Host Disconnected");
	assert.equal(noticeFor("connection-lost"), "Connection Lost");
	assert.equal(noticeFor("stream-error"), "Stream Error");
});

test("the embedded client exports to the page even if Steam defines module", () => {
	const source = fs.readFileSync(path.resolve(__dirname, "../../../src/streamtunes_client.js"), "utf8");
	const page = {
		document: {},
		module: { exports: {} },
	};
	vm.runInNewContext(source, page);
	assert.equal(typeof page.DoomNetStreamTunes.StreamTunesClient, "function");
	assert.deepEqual(Object.keys(page.module.exports), []);
});

test("a host sends one mixed stream to a newly joined peer", async () => {
	const client = new StreamTunesClient(options());
	const stream = audioStream();
	const started = client.start(stream);
	const socket = MockSocket.instances[0];
	socket.open();
	assert.deepEqual(socket.sent[0], { type: "create", name: "Steam Persona" });
	socket.receive({ type: "created", code: "K7QM-4P2L", peerId: "host", count: 1, capacity: 5 });
	await started;
	assert.equal(client.snapshot().active, true);
	assert.equal(client.snapshot().role, "host");

	socket.receive({ type: "peer-joined", peerId: "listener", name: "Friend" });
	await tick();
	await tick();
	const peer = MockPeer.instances[0];
	assert.deepEqual(peer.options.iceServers, []);
	assert.equal(peer.tracks.length, 1);
	assert.equal(peer.tracks[0].stream, stream);
	const offer = socket.sent.find((message) => message.type === "signal");
	assert.equal(offer.to, "listener");
	assert.equal(offer.data.description.type, "offer");
});

test("relay-provided TURN settings are used for new peers", async () => {
	const client = new StreamTunesClient(options());
	const started = client.start(audioStream());
	const socket = MockSocket.instances[0];
	socket.open();
	socket.receive({
		type: "created",
		code: "K7QM-4P2L",
		peerId: "host",
		count: 1,
		capacity: 5,
		iceServers: [{
			urls: ["stun:stun.example.net", "turns:turn.example.net:5349"],
			username: "ephemeral-user",
			credential: "ephemeral-secret",
		}],
	});
	await started;
	socket.receive({ type: "peer-joined", peerId: "listener", name: "Friend" });
	await tick();
	await tick();
	assert.deepEqual(MockPeer.instances[0].options.iceServers, [{
		urls: ["stun:stun.example.net", "turns:turn.example.net:5349"],
		username: "ephemeral-user",
		credential: "ephemeral-secret",
	}]);
});

test("invalid relay-provided ICE settings reject the pending party", async () => {
	const notices = [];
	const client = new StreamTunesClient(options({ onNotice: (value) => notices.push(value) }));
	const joined = client.join("K7QM-4P2L");
	const socket = MockSocket.instances[0];
	socket.open();
	socket.receive({
		type: "joined",
		code: "K7QM-4P2L",
		peerId: "listener",
		hostId: "host",
		count: 2,
		capacity: 5,
		iceServers: [{ urls: "https://not-an-ice-server.example" }],
	});
	await assert.rejects(joined, /Stream Error/);
	assert.equal(client.snapshot().active, false);
	assert.equal(MockPeer.instances.length, 0);
	assert.equal(notices.at(-1), "Stream Error");
});

test("a listener answers an offer, attaches remote audio, and reports its own ping", async () => {
	let remote = null;
	let stopped = 0;
	const client = new StreamTunesClient(options({
		onRemoteStream: (stream) => { remote = stream; },
		onRemoteStop: () => { stopped += 1; },
	}));
	const joined = client.join("K7QM-4P2L");
	const socket = MockSocket.instances[0];
	socket.open();
	socket.receive({ type: "joined", code: "K7QM-4P2L", peerId: "listener", hostId: "host", count: 2, capacity: 5 });
	await joined;
	socket.receive({ type: "signal", from: "host", data: { description: { type: "offer", sdp: "offer-sdp" } } });
	await tick();
	await tick();
	const peer = MockPeer.instances[0];
	const incoming = audioStream();
	peer.ontrack({ streams: [incoming] });
	assert.equal(remote, incoming);
	const answer = socket.sent.find((message) => message.type === "signal");
	assert.equal(answer.data.description.type, "answer");

	await client.sampleStats();
	assert.equal(client.snapshot().pingMs, 32);
	assert.equal(socket.sent.at(-1).type, "stats");
	assert.equal(socket.sent.at(-1).pingMs, 32);
	await client.leave();
	assert.equal(stopped, 1);
});

test("clean host end and socket loss show different listener notices", async () => {
	const notices = [];
	const client = new StreamTunesClient(options({ onNotice: (value) => notices.push(value) }));
	const joined = client.join("K7QM-4P2L");
	const socket = MockSocket.instances[0];
	socket.open();
	socket.receive({ type: "joined", code: "K7QM-4P2L", peerId: "listener", hostId: "host", count: 2, capacity: 5 });
	await joined;
	socket.receive({ type: "ended", reason: "host-ended" });
	await tick();
	assert.equal(notices.at(-1), "Host Ended Session");

	const second = new StreamTunesClient(options({ onNotice: (value) => notices.push(value) }));
	const secondJoin = second.join("K7QM-4P2L");
	const secondSocket = MockSocket.instances[1];
	secondSocket.open();
	secondSocket.receive({ type: "joined", code: "K7QM-4P2L", peerId: "listener-2", hostId: "host", count: 2, capacity: 5 });
	await secondJoin;
	secondSocket.closeUnexpected();
	assert.equal(notices.at(-1), "Host Disconnected");
});

test("one failed listener peer does not end the host party", async () => {
	const notices = [];
	const client = new StreamTunesClient(options({ onNotice: (value) => notices.push(value) }));
	const started = client.start(audioStream());
	const socket = MockSocket.instances[0];
	socket.open();
	socket.receive({ type: "created", code: "K7QM-4P2L", peerId: "host", count: 1, capacity: 5 });
	await started;
	socket.receive({ type: "peer-joined", peerId: "listener-a", name: "Friend A" });
	socket.receive({ type: "peer-joined", peerId: "listener-b", name: "Friend B" });
	await tick();
	await tick();
	assert.equal(MockPeer.instances.length, 2);
	const failed = MockPeer.instances[0];
	const kept = MockPeer.instances[1];
	failed.connectionState = "failed";
	failed.onconnectionstatechange();
	await tick();
	assert.equal(failed.closed, true);
	assert.equal(kept.closed, false);
	assert.equal(client.snapshot().active, true);
	assert.equal(client.snapshot().role, "host");
	assert.equal(client.snapshot().code, "K7QM-4P2L");
	assert.equal(client.peers.has("listener-a"), false);
	assert.equal(client.peers.has("listener-b"), true);
	assert.equal(notices.length, 0);
	// Listener leave arrives later and is what updates capacity/roster.
	socket.receive({ type: "peer-left", peerId: "listener-a" });
	socket.receive({
		type: "roster",
		code: "K7QM-4P2L",
		count: 2,
		capacity: 5,
		peers: [
			{ peerId: "host", name: "Steam Persona", local: true, pingMs: 0 },
			{ peerId: "listener-b", name: "Friend B", local: false, pingMs: 55 },
		],
	});
	assert.deepEqual(client.snapshot().peers.map((entry) => entry.peerId), ["host", "listener-b"]);
	assert.equal(client.snapshot().count, 2);
});

test("a listener peer failure ends only that listener session", async () => {
	const notices = [];
	const client = new StreamTunesClient(options({ onNotice: (value) => notices.push(value) }));
	const joined = client.join("K7QM-4P2L");
	const socket = MockSocket.instances[0];
	socket.open();
	socket.receive({ type: "joined", code: "K7QM-4P2L", peerId: "listener", hostId: "host", count: 2, capacity: 5 });
	await joined;
	socket.receive({ type: "signal", from: "host", data: { description: { type: "offer", sdp: "offer-sdp" } } });
	await tick();
	await tick();
	const peer = MockPeer.instances[0];
	socket.close = function () { this.readyState = 3; };
	peer.connectionState = "failed";
	peer.onconnectionstatechange();
	await tick();
	assert.equal(client.snapshot().active, false);
	assert.equal(notices.at(-1), "Connection Lost");
	assert.equal(socket.sent.at(-1).type, "leave");
	assert.equal(client.socket, null);
});

test("a brief peer disconnect recovers inside the reconnect window", async () => {
	const timers = [];
	const notices = [];
	const client = new StreamTunesClient(options({
		reconnectMs: 40,
		setTimeout: (fn, ms) => {
			const handle = { fn, ms, cleared: false };
			timers.push(handle);
			return handle;
		},
		clearTimeout: (handle) => { if (handle) handle.cleared = true; },
		onNotice: (value) => notices.push(value),
	}));
	const joined = client.join("K7QM-4P2L");
	const socket = MockSocket.instances[0];
	socket.open();
	socket.receive({ type: "joined", code: "K7QM-4P2L", peerId: "listener", hostId: "host", count: 2, capacity: 5 });
	await joined;
	socket.receive({ type: "signal", from: "host", data: { description: { type: "offer", sdp: "offer-sdp" } } });
	await tick();
	await tick();
	const peer = MockPeer.instances[0];
	peer.connectionState = "disconnected";
	peer.onconnectionstatechange();
	assert.equal(timers.length, 1);
	assert.equal(client.snapshot().active, true);
	peer.connectionState = "connected";
	peer.onconnectionstatechange();
	assert.equal(timers[0].cleared, true);
	timers[0].fn();
	assert.equal(client.snapshot().active, true);
	assert.equal(notices.length, 0);
});

test("a peer that stays failed past the reconnect window shows Connection Lost", async () => {
	const timers = [];
	const notices = [];
	const client = new StreamTunesClient(options({
		reconnectMs: 40,
		setTimeout: (fn) => {
			const handle = { fn, cleared: false };
			timers.push(handle);
			return handle;
		},
		clearTimeout: (handle) => { if (handle) handle.cleared = true; },
		onNotice: (value) => notices.push(value),
	}));
	const joined = client.join("K7QM-4P2L");
	const socket = MockSocket.instances[0];
	socket.open();
	socket.receive({ type: "joined", code: "K7QM-4P2L", peerId: "listener", hostId: "host", count: 2, capacity: 5 });
	await joined;
	socket.receive({ type: "signal", from: "host", data: { description: { type: "offer", sdp: "offer-sdp" } } });
	await tick();
	await tick();
	const peer = MockPeer.instances[0];
	socket.close = function () { this.readyState = 3; };
	peer.connectionState = "failed";
	peer.onconnectionstatechange();
	assert.equal(client.snapshot().active, true);
	assert.equal(timers.length, 1);
	timers[0].fn();
	assert.equal(client.snapshot().active, false);
	assert.equal(notices.at(-1), "Connection Lost");
	assert.equal(socket.sent.at(-1).type, "leave");
});
