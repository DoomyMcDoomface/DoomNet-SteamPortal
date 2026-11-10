"use strict";

const crypto = require("node:crypto");

const CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
const MAX_ROOM_SIZE = 5;
const ICE_URL = /^(stun|stuns|turn|turns):/i;

function normalizeIceServers(value) {
	if (value === undefined) {
		return undefined;
	}
	if (!Array.isArray(value) || value.length > 8) {
		throw new Error("ICE servers must be an array with at most eight entries");
	}
	return value.map((entry) => {
		if (!entry || typeof entry !== "object" || Array.isArray(entry)) {
			throw new Error("Each ICE server must be an object");
		}
		const urls = Array.isArray(entry.urls) ? entry.urls.slice() : [entry.urls];
		if (!urls.length || urls.length > 8 || urls.some((url) =>
			typeof url !== "string" || url.length > 2048 || !ICE_URL.test(url))) {
			throw new Error("ICE server URLs must use stun, stuns, turn, or turns");
		}
		const result = { urls: Array.isArray(entry.urls) ? urls : urls[0] };
		if (entry.username !== undefined) {
			if (typeof entry.username !== "string" || entry.username.length > 512) {
				throw new Error("ICE server usernames must be strings of at most 512 characters");
			}
			result.username = entry.username;
		}
		if (entry.credential !== undefined) {
			if (typeof entry.credential !== "string" || entry.credential.length > 512) {
				throw new Error("ICE server credentials must be strings of at most 512 characters");
			}
			result.credential = entry.credential;
		}
		return result;
	});
}

function safeName(value) {
	const name = String(value || "").trim();
	return name && name.length <= 64 ? name : "";
}

function safeCode(value) {
	return String(value || "").toUpperCase().replace(/[^A-Z0-9]/g, "");
}

class RoomRegistry {
	constructor(send, options) {
		options = options || {};
		this.send = send;
		this.iceServers = normalizeIceServers(options.iceServers);
		this.clients = new Map();
		this.rooms = new Map();
	}

	add(client) {
		const id = crypto.randomBytes(12).toString("hex");
		this.clients.set(client, { id, name: "", role: "none", room: null, pingMs: 0, kbps: 0 });
		return id;
	}

	code() {
		for (let attempt = 0; attempt < 100; attempt += 1) {
			const bytes = crypto.randomBytes(8);
			let raw = "";
			for (let i = 0; i < bytes.length; i += 1) {
				raw += CODE_ALPHABET[bytes[i] % CODE_ALPHABET.length];
			}
			if (!this.rooms.has(raw)) {
				return raw.slice(0, 4) + "-" + raw.slice(4);
			}
		}
		throw new Error("Could not allocate a room code");
	}

	reply(client, value) {
		this.send(client, value);
	}

	fail(client, code, message) {
		this.reply(client, { type: "error", code, message });
	}

	create(client, message) {
		const member = this.clients.get(client);
		const name = safeName(message.name);
		if (!member || !name) {
			this.fail(client, "invalid-name", "Steam persona name is required");
			return;
		}
		this.leave(client, false);
		const code = this.code();
		const room = { code, host: client, members: new Map() };
		member.name = name;
		member.role = "host";
		member.room = room;
		room.members.set(member.id, client);
		this.rooms.set(code.replace("-", ""), room);
		const created = { type: "created", code, peerId: member.id, count: 1, capacity: MAX_ROOM_SIZE };
		if (this.iceServers !== undefined) created.iceServers = this.iceServers;
		this.reply(client, created);
		this.publish(room);
	}

	join(client, message) {
		const member = this.clients.get(client);
		const name = safeName(message.name);
		const room = this.rooms.get(safeCode(message.code));
		if (!member || !name) {
			this.fail(client, "invalid-name", "Steam persona name is required");
			return;
		}
		if (!room) {
			this.fail(client, "unknown-room", "Couldn't Join Party");
			return;
		}
		if (room.members.size >= MAX_ROOM_SIZE) {
			this.fail(client, "room-full", "Couldn't Join Party");
			return;
		}
		this.leave(client, false);
		member.name = name;
		member.role = "listener";
		member.room = room;
		room.members.set(member.id, client);
		const joined = {
			type: "joined",
			code: room.code,
			peerId: member.id,
			hostId: this.clients.get(room.host).id,
			count: room.members.size,
			capacity: MAX_ROOM_SIZE,
		};
		if (this.iceServers !== undefined) joined.iceServers = this.iceServers;
		this.reply(client, joined);
		this.reply(room.host, { type: "peer-joined", peerId: member.id, name });
		this.publish(room);
	}

	signal(client, message) {
		const member = this.clients.get(client);
		if (!member || !member.room) {
			return this.fail(client, "not-in-room", "Client is not in a party");
		}
		const target = member.room.members.get(String(message.to || ""));
		if (!target || target === client) {
			return this.fail(client, "unknown-peer", "Signal target is not in this party");
		}
		this.reply(target, { type: "signal", from: member.id, data: message.data || null });
	}

	stats(client, message) {
		const member = this.clients.get(client);
		if (!member || member.role !== "listener" || !member.room) {
			return;
		}
		member.pingMs = Math.max(0, Math.min(60000, Math.round(Number(message.pingMs) || 0)));
		member.kbps = Math.max(0, Math.min(100000, Math.round(Number(message.kbps) || 0)));
		this.publish(member.room);
	}

	end(client) {
		const member = this.clients.get(client);
		if (!member || member.role !== "host" || !member.room) {
			return this.fail(client, "not-host", "Only the host can end this party");
		}
		this.endRoom(member.room, "host-ended");
	}

	leave(client, notify = true) {
		const member = this.clients.get(client);
		if (!member || !member.room) {
			return;
		}
		const room = member.room;
		if (member.role === "host") {
			this.endRoom(room, notify ? "host-disconnected" : "host-ended");
			return;
		}
		room.members.delete(member.id);
		member.role = "none";
		member.room = null;
		if (notify) {
			this.reply(room.host, { type: "peer-left", peerId: member.id });
		}
		this.publish(room);
	}

	endRoom(room, reason) {
		this.rooms.delete(room.code.replace("-", ""));
		for (const client of room.members.values()) {
			const member = this.clients.get(client);
			if (client !== room.host) {
				this.reply(client, { type: "ended", reason });
			}
			if (member) {
				member.role = "none";
				member.room = null;
			}
		}
		room.members.clear();
	}

	publish(room) {
		if (!room || !room.members.size) {
			return;
		}
		const hostMember = this.clients.get(room.host);
		const peers = [];
		for (const client of room.members.values()) {
			const member = this.clients.get(client);
			if (!member) {
				continue;
			}
			peers.push({
				peerId: member.id,
				name: member.name,
				local: member.role === "host",
				pingMs: member.pingMs,
				kbps: member.kbps,
			});
			if (client !== room.host) {
				this.reply(client, { type: "room", count: room.members.size, capacity: MAX_ROOM_SIZE });
			}
		}
		if (hostMember) {
			this.reply(room.host, {
				type: "roster",
				code: room.code,
				count: room.members.size,
				capacity: MAX_ROOM_SIZE,
				peers,
			});
		}
	}

	handle(client, message) {
		if (!this.clients.has(client) || !message || typeof message !== "object") {
			return;
		}
		switch (message.type) {
			case "create": return this.create(client, message);
			case "join": return this.join(client, message);
			case "signal": return this.signal(client, message);
			case "stats": return this.stats(client, message);
			case "end": return this.end(client);
			case "leave": return this.leave(client, true);
			default: return this.fail(client, "unknown-message", "Unknown StreamTunes message");
		}
	}

	remove(client) {
		this.leave(client, true);
		this.clients.delete(client);
	}
}

module.exports = { RoomRegistry, MAX_ROOM_SIZE, normalizeIceServers, safeCode, safeName };
