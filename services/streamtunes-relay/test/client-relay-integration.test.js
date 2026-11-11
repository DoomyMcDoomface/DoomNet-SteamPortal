"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const { WebSocket } = require("ws");
const { createRelayServer } = require("../server");
const { StreamTunesClient } = require("../../../src/streamtunes_client");

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
	addTrack(track, stream) { this.tracks.push({ track, stream }); }
	async createOffer() { return { type: "offer", sdp: "offer-sdp" }; }
	async createAnswer() { return { type: "answer", sdp: "answer-sdp" }; }
	async setLocalDescription(value) { this.localDescription = value; }
	async setRemoteDescription(value) { this.remoteDescription = value; }
	async addIceCandidate(value) { this.candidate = value; }
	async getStats() {
		return [
			{ type: "candidate-pair", state: "succeeded", currentRoundTripTime: 0.04 },
			{ type: "inbound-rtp", kind: "audio", bytesReceived: 8000, isRemote: false },
		];
	}
	close() { this.closed = true; this.connectionState = "closed"; }
}

function audioStream() {
	const track = { kind: "audio" };
	return { track, getTracks: () => [track], getAudioTracks: () => [track] };
}

function tick() {
	return new Promise((resolve) => setImmediate(resolve));
}

async function waitFor(predicate, label) {
	const started = Date.now();
	while (Date.now() - started < 2000) {
		if (predicate()) return;
		await new Promise((resolve) => setTimeout(resolve, 15));
	}
	throw new Error("Timed out waiting for " + label);
}

function withTimeout(promise, ms, label) {
	return Promise.race([
		promise,
		new Promise((_, reject) => setTimeout(() => reject(new Error(label + " timed out")), ms)),
	]).catch(() => {});
}

async function shutDown(relay, clients) {
	for (const client of clients) {
		try {
			if (client.socket) {
				client.closedByUser = true;
				try { client.socket.terminate(); } catch (error) {}
				client.socket = null;
			}
			client.active = false;
			client.role = "none";
		} catch (error) {}
	}
	await withTimeout(new Promise((resolve) => relay.sockets.close(() => resolve())), 1000, "sockets.close");
	await withTimeout(new Promise((resolve) => relay.server.close(() => resolve())), 1000, "server.close");
}

test.beforeEach(() => {
	MockPeer.instances.length = 0;
});

test("StreamTunesClient create/join/offer/answer and leave work against the real relay", async () => {
	const relay = createRelayServer({ log: () => {} });
	await new Promise((resolve) => relay.server.listen(0, "127.0.0.1", resolve));
	const url = "ws://127.0.0.1:" + relay.server.address().port + "/streamtunes";
	const notices = [];
	const host = new StreamTunesClient({
		url,
		name: "Host Persona",
		WebSocket,
		RTCPeerConnection: MockPeer,
		reconnectMs: 0,
		onNotice: (value) => notices.push("host:" + value),
	});
	const listener = new StreamTunesClient({
		url,
		name: "Listener Persona",
		WebSocket,
		RTCPeerConnection: MockPeer,
		reconnectMs: 0,
		onNotice: (value) => notices.push("listener:" + value),
		onRemoteStream: () => {},
		onRemoteStop: () => {},
	});
	try {
		const started = await host.start(audioStream());
		assert.equal(started.role, "host");
		assert.match(started.code, /^[A-Z2-9]{4}-[A-Z2-9]{4}$/);

		const joined = await listener.join(started.code);
		assert.equal(joined.role, "listener");
		assert.equal(joined.code, started.code);

		await waitFor(() => MockPeer.instances.length >= 2, "offer and answer peers");
		await tick();
		await tick();
		assert.equal(MockPeer.instances[0].tracks.length, 1);
		assert.equal(MockPeer.instances[1].remoteDescription.type, "offer");
		assert.equal(MockPeer.instances[1].localDescription.type, "answer");

		await waitFor(() => host.snapshot().count === 2, "host count 2");
		assert.equal(host.snapshot().active, true);
		assert.equal(listener.snapshot().active, true);

		await listener.leave();
		await waitFor(() => host.snapshot().count === 1, "host count after leave");
		assert.equal(listener.snapshot().active, false);
		assert.equal(host.snapshot().active, true);
		assert.equal(notices.length, 0);
	} finally {
		await shutDown(relay, [listener, host]);
	}
});

test("listener Connection Lost leaves the real relay room for the host", async () => {
	const relay = createRelayServer({ log: () => {} });
	await new Promise((resolve) => relay.server.listen(0, "127.0.0.1", resolve));
	const url = "ws://127.0.0.1:" + relay.server.address().port + "/streamtunes";
	const notices = [];
	const host = new StreamTunesClient({
		url,
		name: "Host Persona",
		WebSocket,
		RTCPeerConnection: MockPeer,
		reconnectMs: 0,
	});
	const listener = new StreamTunesClient({
		url,
		name: "Listener Persona",
		WebSocket,
		RTCPeerConnection: MockPeer,
		reconnectMs: 0,
		onNotice: (value) => notices.push(value),
	});
	try {
		const started = await host.start(audioStream());
		await listener.join(started.code);
		await waitFor(() => MockPeer.instances.length >= 2, "both peers");
		await tick();
		await tick();

		const listenerPeer = MockPeer.instances[1];
		listenerPeer.connectionState = "failed";
		listenerPeer.onconnectionstatechange();
		await tick();
		await tick();
		assert.equal(notices.at(-1), "Connection Lost");
		assert.equal(listener.snapshot().active, false);
		await waitFor(() => host.snapshot().count === 1, "host recovered capacity");
		assert.equal(host.snapshot().active, true);
		assert.equal(host.snapshot().code, started.code);
	} finally {
		await shutDown(relay, [listener, host]);
	}
});
