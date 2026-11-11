"use strict";

const http = require("node:http");
const { WebSocket, WebSocketServer } = require("ws");
const { RoomRegistry, normalizeIceServers } = require("./relay-core");

function log(level, sentence) {
	process.stdout.write(new Date().toISOString() + " " + level + " pid=" + process.pid + " party: " + sentence + "\n");
}

function parseIceServers(value) {
	if (!value) return undefined;
	try {
		return normalizeIceServers(JSON.parse(value));
	} catch (error) {
		throw new Error("STREAMTUNES_ICE_SERVERS_JSON is invalid; TURN credentials were not logged");
	}
}

function createRelayServer(options) {
	options = options || {};
	const writeLog = options.log || log;
	const server = http.createServer((request, response) => {
		if (request.url === "/health") {
			response.writeHead(200, { "content-type": "application/json", "cache-control": "no-store" });
			response.end(JSON.stringify({ ok: true, service: "StreamTunes relay" }));
			return;
		}
		response.writeHead(404, { "content-type": "text/plain; charset=utf-8" });
		response.end("Not found");
	});

	const sockets = new WebSocketServer({
		server,
		path: "/streamtunes",
		maxPayload: 32 * 1024,
		perMessageDeflate: false,
	});

	const registry = new RoomRegistry((client, message) => {
		if (client.readyState === WebSocket.OPEN) {
			client.send(JSON.stringify(message));
		}
	}, { iceServers: options.iceServers });

	sockets.on("connection", (socket, request) => {
		const peerId = registry.add(socket);
		socket.isAlive = true;
		writeLog("info", "Accepted peer " + peerId + " from '" + (request.socket.remoteAddress || "unknown") + "'.");

		socket.on("pong", () => {
			socket.isAlive = true;
		});
		socket.on("message", (bytes, isBinary) => {
			if (isBinary) {
				socket.close(1003, "Text messages only");
				return;
			}
			try {
				registry.handle(socket, JSON.parse(bytes.toString("utf8")));
			} catch (error) {
				writeLog("warn", "Rejected malformed message from peer " + peerId + ": " + error.message + ".");
				socket.close(1007, "Invalid JSON");
			}
		});
		socket.on("close", () => {
			registry.remove(socket);
			writeLog("info", "Removed peer " + peerId + ".");
		});
		socket.on("error", (error) => {
			writeLog("error", "Peer " + peerId + " failed: " + error.message + ".");
		});
	});

	const heartbeat = setInterval(() => {
		for (const socket of sockets.clients) {
			if (!socket.isAlive) {
				socket.terminate();
				continue;
			}
			socket.isAlive = false;
			socket.ping();
		}
	}, 30000);
	if (typeof heartbeat.unref === "function") heartbeat.unref();

	sockets.on("close", () => clearInterval(heartbeat));
	return { server, sockets, registry };
}

if (require.main === module) {
	const port = Number(process.env.PORT || 8787);
	try {
		const relay = createRelayServer({
			iceServers: parseIceServers(process.env.STREAMTUNES_ICE_SERVERS_JSON),
		});
		relay.server.listen(port, "0.0.0.0", () => {
			log("info", "Listening on port " + port + " at '/streamtunes'.");
		});
	} catch (error) {
		log("error", "Startup failed because " + error.message + ".");
		process.exitCode = 1;
	}
}

module.exports = { createRelayServer, parseIceServers };
