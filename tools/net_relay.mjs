#!/usr/bin/env node
// Kern network relay: forwards Kern datagrams between browsers over WebSockets.
//
//   node tools/net_relay.mjs [port]          (default 8790; needs the "ws" package)
//
// Browsers cannot send UDP, so Kern games (kern/web/studio/game.mjs, KernNet)
// talk through this relay when the players are on different machines. The
// relay is dumb on purpose: Kern's own stack on top (acks, resends, channels,
// connection handshake, timeouts - kern/src/net) does everything else, exactly
// as it would over UDP. Same-machine tabs use a BroadcastChannel instead and
// need no relay.
//
// Protocol (per WebSocket):
//   text  {"join": "<room>"}              -> {"id": <peer id>, "host": <host id or 0>}
//   text  {"host": "<room>"}              -> {"id": <peer id>, "host": <own id>}   (becomes the room's host)
//   binary [8 bytes: target id, LE][datagram]  -> delivered as [8 bytes: sender id][datagram]
// The host leaving closes the room for new joiners; datagrams to unknown ids are dropped.
import {createServer} from "node:http";
import {createRequire} from "node:module";

const require = createRequire(import.meta.url);
let WebSocketServer;
try { ({WebSocketServer} = require("ws")); } catch {
  console.error('The relay needs the "ws" package: npm install ws');
  process.exit(1);
}

const port = Number(process.argv[2] || process.env.PORT || 8790);
const MAX_DATAGRAM = 1400;
const peers = new Map();   // id -> socket
const rooms = new Map();   // room -> host id
let nextId = 1;

const http = createServer((req, res) => { res.writeHead(200, {"content-type": "text/plain"}); res.end("Kern relay\n"); });
const wss = new WebSocketServer({server: http, maxPayload: MAX_DATAGRAM + 8});

wss.on("connection", ws => {
  const id = nextId++;
  peers.set(id, ws);
  ws.binaryType = "nodebuffer";
  let room = null;
  ws.on("message", (data, binary) => {
    if (!binary) {
      let msg;
      try { msg = JSON.parse(String(data)); } catch { return; }
      if (typeof msg.host === "string" && msg.host.length < 64) {
        room = msg.host;
        rooms.set(room, id);
        ws.send(JSON.stringify({id, host: id}));
      } else if (typeof msg.join === "string" && msg.join.length < 64) {
        room = msg.join;
        ws.send(JSON.stringify({id, host: rooms.get(room) || 0}));
      }
      return;
    }
    if (data.length < 9 || data.length > MAX_DATAGRAM + 8) return;
    const to = Number(data.readBigUInt64LE(0));
    const target = peers.get(to);
    if (!target || target.readyState !== 1) return;
    const out = Buffer.allocUnsafe(data.length);
    out.writeBigUInt64LE(BigInt(id), 0);
    data.copy(out, 8, 8);
    target.send(out, {binary: true});
  });
  ws.on("close", () => {
    peers.delete(id);
    if (room && rooms.get(room) === id) rooms.delete(room);
  });
});

http.listen(port, () => console.log(`Kern relay on ws://localhost:${port}`));
