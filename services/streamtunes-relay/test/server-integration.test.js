"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const { WebSocket } = require("ws");
const { createRelayServer, parseIceServers } = require("../server");

function openSocket(url) {
	return new Promise((resolve, reject) => {
		const socket = new WebSocket(url);
		socket.once("open", () => resolve(socket));
		socket.once("error", reject);
	});
}

function nextMessage(socket, type) {
	return new Promise((resolve, reject) => {
		const timeout = setTimeout(() => {
			socket.off("message", onMessage);
			reject(new Error("Timed out waiting for " + type));
		}, 2000);
		function onMessage(bytes) {
			const message = JSON.parse(bytes.toString("utf8"));
			if (message.type !== type) return;
			clearTimeout(timeout);
			socket.off("message", onMessage);
			resolve(message);
		}
		socket.on("message", onMessage);
	});
}

test("deployment ICE configuration is validated without echoing credentials", () => {
	assert.deepEqual(parseIceServers("[]"), []);
	assert.deepEqual(parseIceServers(JSON.stringify([{
		urls: "turns:turn.example.net:5349",
		username: "temporary-user",
		credential: "temporary-secret",
	}])), [{
		urls: "turns:turn.example.net:5349",
		username: "temporary-user",
		credential: "temporary-secret",
	}]);
	assert.throws(
		() => parseIceServers('[{"urls":"https://bad","credential":"do-not-print-this"}]'),
		(error) => !error.message.includes("do-not-print-this"),
	);
});

test("the real WebSocket wrapper creates, joins, signals, and cleanly ends a room", async () => {
	const iceServers = [{
		urls: "turns:turn.example.net:5349",
		username: "temporary-user",
		credential: "temporary-secret",
	}];
	const relay = createRelayServer({ log: () => {}, iceServers });
	await new Promise((resolve) => relay.server.listen(0, "127.0.0.1", resolve));
	const address = relay.server.address();
	const base = "ws://127.0.0.1:" + address.port + "/streamtunes";
	const host = await openSocket(base);
	const listener = await openSocket(base);
	try {
		const createdPromise = nextMessage(host, "created");
		host.send(JSON.stringify({ type: "create", name: "Host Persona" }));
		const created = await createdPromise;
		assert.match(created.code, /^[A-Z2-9]{4}-[A-Z2-9]{4}$/);
		assert.deepEqual(created.iceServers, iceServers);

		const joinedPromise = nextMessage(listener, "joined");
		const peerJoinedPromise = nextMessage(host, "peer-joined");
		listener.send(JSON.stringify({ type: "join", code: created.code, name: "Listener Persona" }));
		const joined = await joinedPromise;
		const peerJoined = await peerJoinedPromise;
		assert.equal(peerJoined.peerId, joined.peerId);
		assert.deepEqual(joined.iceServers, iceServers);

		const signalPromise = nextMessage(listener, "signal");
		host.send(JSON.stringify({
			type: "signal",
			to: joined.peerId,
			data: { description: { type: "offer", sdp: "integration-offer" } },
		}));
		const signal = await signalPromise;
		assert.equal(signal.data.description.sdp, "integration-offer");

		const endedPromise = nextMessage(listener, "ended");
		host.send(JSON.stringify({ type: "end" }));
		assert.equal((await endedPromise).reason, "host-ended");
	} finally {
		host.close();
		listener.close();
		await new Promise((resolve) => relay.sockets.close(resolve));
		await new Promise((resolve) => relay.server.close(resolve));
	}
});
