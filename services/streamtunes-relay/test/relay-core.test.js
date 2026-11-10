"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const { RoomRegistry, normalizeIceServers } = require("../relay-core");

function fixture(options) {
	const messages = new Map();
	const registry = new RoomRegistry((client, message) => {
		if (!messages.has(client)) {
			messages.set(client, []);
		}
		messages.get(client).push(message);
	}, options);
	function client(name) {
		const value = { name };
		messages.set(value, []);
		registry.add(value);
		return value;
	}
	function last(clientValue, type) {
		return messages.get(clientValue).filter((message) => message.type === type).at(-1);
	}
	return { registry, messages, client, last };
}

test("a room is the host plus four listeners", () => {
	const f = fixture();
	const host = f.client("host");
	f.registry.handle(host, { type: "create", name: "Host Persona" });
	const created = f.last(host, "created");
	assert.match(created.code, /^[A-Z2-9]{4}-[A-Z2-9]{4}$/);
	assert.equal(created.count, 1);

	const listeners = [];
	for (let index = 0; index < 4; index += 1) {
		const listener = f.client("listener-" + index);
		listeners.push(listener);
		f.registry.handle(listener, { type: "join", code: created.code, name: "Listener " + index });
		assert.equal(f.last(listener, "joined").capacity, 5);
	}
	const roster = f.last(host, "roster");
	assert.equal(roster.count, 5);
	assert.equal(roster.peers.length, 5);
	assert.equal(roster.peers[0].name, "Host Persona");

	const extra = f.client("extra");
	f.registry.handle(extra, { type: "join", code: created.code, name: "Extra Listener" });
	assert.equal(f.last(extra, "error").code, "room-full");
	assert.equal(f.last(host, "roster").count, 5);
});

test("direct-only and TURN configurations are distributed when a room is entered", () => {
	const direct = fixture({ iceServers: [] });
	const directHost = direct.client("direct-host");
	direct.registry.handle(directHost, { type: "create", name: "Direct Host" });
	assert.deepEqual(direct.last(directHost, "created").iceServers, []);

	const iceServers = [{
		urls: ["stun:stun.example.net", "turns:turn.example.net:5349"],
		username: "temporary-user",
		credential: "temporary-secret",
	}];
	const relayed = fixture({ iceServers });
	const host = relayed.client("host");
	const listener = relayed.client("listener");
	relayed.registry.handle(host, { type: "create", name: "Host" });
	const created = relayed.last(host, "created");
	relayed.registry.handle(listener, { type: "join", code: created.code, name: "Listener" });
	assert.deepEqual(created.iceServers, iceServers);
	assert.deepEqual(relayed.last(listener, "joined").iceServers, iceServers);

	assert.throws(() => normalizeIceServers([{ urls: "https://not-an-ice-server.example" }]));
});

test("signals stay inside a room and listener stats reach only its host roster", () => {
	const f = fixture();
	const host = f.client("host");
	const listener = f.client("listener");
	f.registry.handle(host, { type: "create", name: "Host" });
	const created = f.last(host, "created");
	f.registry.handle(listener, { type: "join", code: created.code, name: "Listener" });
	const joined = f.last(listener, "joined");

	f.registry.handle(host, { type: "signal", to: joined.peerId, data: { description: { type: "offer", sdp: "test" } } });
	assert.equal(f.last(listener, "signal").from, created.peerId);
	assert.equal(f.last(listener, "signal").data.description.type, "offer");

	f.registry.handle(listener, { type: "stats", pingMs: 32.4, kbps: 127.6 });
	const listenerRow = f.last(host, "roster").peers.find((peer) => peer.peerId === joined.peerId);
	assert.equal(listenerRow.pingMs, 32);
	assert.equal(listenerRow.kbps, 128);
});

test("a clean end and a host crash have different listener reasons", () => {
	const clean = fixture();
	const cleanHost = clean.client("host");
	const cleanListener = clean.client("listener");
	clean.registry.handle(cleanHost, { type: "create", name: "Host" });
	clean.registry.handle(cleanListener, { type: "join", code: clean.last(cleanHost, "created").code, name: "Listener" });
	clean.registry.handle(cleanHost, { type: "end" });
	assert.equal(clean.last(cleanListener, "ended").reason, "host-ended");

	const crash = fixture();
	const crashHost = crash.client("host");
	const crashListener = crash.client("listener");
	crash.registry.handle(crashHost, { type: "create", name: "Host" });
	crash.registry.handle(crashListener, { type: "join", code: crash.last(crashHost, "created").code, name: "Listener" });
	crash.registry.remove(crashHost);
	assert.equal(crash.last(crashListener, "ended").reason, "host-disconnected");
});

test("a listener leaving updates the host without ending the room", () => {
	const f = fixture();
	const host = f.client("host");
	const listener = f.client("listener");
	f.registry.handle(host, { type: "create", name: "Host" });
	f.registry.handle(listener, { type: "join", code: f.last(host, "created").code, name: "Listener" });
	f.registry.handle(listener, { type: "leave" });
	assert.equal(f.last(host, "peer-left").peerId, f.last(listener, "joined").peerId);
	assert.equal(f.last(host, "roster").count, 1);
	assert.equal(f.last(listener, "ended"), undefined);
});
