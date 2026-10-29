"""Public lobby directory for the Monopoly PC port: launchers list open lobbies here so anyone can
find and join them. The server records each host's internet address itself (so hosts don't need to
know it) and checks that the host's game port can be reached from outside.

  python3 lobby_directory.py [--host 0.0.0.0] [--port 27380] [--trust-proxy]
  python3 lobby_directory.py --self-test

--trust-proxy takes the client address from X-Forwarded-For; use it only behind a reverse proxy
(PythonAnywhere, Render, nginx), never when clients connect directly.

API, JSON over HTTP:
  GET    /v1/lobbies        -> {"lobbies": [{id, name, host, players, in_game, address, port, version, reachable}]}
  POST   /v1/lobbies        {name, host, players, in_game, port, version} -> {id, token, address, reachable}
  PUT    /v1/lobbies/<id>   same body, "Authorization: Bearer <token>"    -> {address, reachable}
  DELETE /v1/lobbies/<id>   "Authorization: Bearer <token>"
A lobby disappears TTL seconds after its last update. reachable is null until checked.
"""
import argparse
import json
import secrets
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TTL = 60.0
MAX_LOBBIES = 500
MAX_PER_ADDRESS = 4
MAX_BODY = 4096
RECHECK = 60.0  # seconds between reachability checks of one lobby


def clean_text(value, limit, fallback):
    if not isinstance(value, str):
        return fallback
    text = "".join(c for c in value if c >= " " and c not in "\x7f")[:limit].strip()
    return text or fallback


def clean_info(body):
    """The fields a launcher may set, validated; None if the body isn't usable."""
    if not isinstance(body, dict):
        return None
    port, players, version = body.get("port"), body.get("players", 1), body.get("version", 0)
    if not all(isinstance(v, int) and not isinstance(v, bool) for v in (port, players, version)):
        return None
    if not 1 <= port <= 65535:
        return None
    return {
        "name": clean_text(body.get("name"), 40, "Monopoly"),
        "host": clean_text(body.get("host"), 32, "Player"),
        "players": max(0, min(4, players)),
        "in_game": body.get("in_game") is True,
        "port": port,
        "version": version,
    }


class Directory:
    def __init__(self, ttl=TTL, probe_timeout=3.0):
        self.ttl = ttl
        self.probe_timeout = probe_timeout
        self.lock = threading.Lock()
        self.lobbies = {}  # id -> entry (entry["token"] is secret)

    def _prune(self):
        now = time.monotonic()
        for lobby_id in [i for i, e in self.lobbies.items() if now - e["updated"] > self.ttl]:
            del self.lobbies[lobby_id]

    @staticmethod
    def _public(entry):
        return {k: entry[k] for k in ("id", "name", "host", "players", "in_game", "address", "port", "version", "reachable")}

    def list(self):
        with self.lock:
            self._prune()
            return [self._public(e) for e in self.lobbies.values()]

    def add(self, address, info):
        with self.lock:
            self._prune()
            # Only one game can listen on an address and port: an older entry there is a host that
            # restarted (or crashed) without removing it.
            for stale in [i for i, e in self.lobbies.items() if e["address"] == address and e["port"] == info["port"]]:
                del self.lobbies[stale]
            if len(self.lobbies) >= MAX_LOBBIES:
                return None, "the directory is full"
            if sum(e["address"] == address for e in self.lobbies.values()) >= MAX_PER_ADDRESS:
                return None, "too many lobbies from your address"
            entry = dict(info, id=secrets.token_urlsafe(9), token=secrets.token_urlsafe(24), address=address,
                         updated=time.monotonic(), reachable=None, checked=0.0)
            self.lobbies[entry["id"]] = entry
        self._check_later(entry["id"])
        return entry, None

    def update(self, lobby_id, token, info):
        with self.lock:
            self._prune()
            entry = self.lobbies.get(lobby_id)
            if not entry or not secrets.compare_digest(entry["token"], token):
                return None
            moved = entry["port"] != info["port"]
            entry.update(info, updated=time.monotonic())
            due = moved or time.monotonic() - entry["checked"] > RECHECK
            result = {"address": entry["address"], "reachable": entry["reachable"]}
        if due:
            self._check_later(lobby_id)
        return result

    def remove(self, lobby_id, token):
        with self.lock:
            entry = self.lobbies.get(lobby_id)
            if not entry or not secrets.compare_digest(entry["token"], token):
                return False
            del self.lobbies[lobby_id]
            return True

    def _check_later(self, lobby_id):
        threading.Thread(target=self._check, args=(lobby_id,), daemon=True).start()

    def _check(self, lobby_id):
        """Can the internet reach the host's game port? Only ever connects back to the caller's own address."""
        with self.lock:
            entry = self.lobbies.get(lobby_id)
            if not entry:
                return
            entry["checked"] = time.monotonic()
            target = (entry["address"], entry["port"])
        try:
            with socket.create_connection(target, timeout=self.probe_timeout):
                ok = True
        except OSError:
            ok = False
        with self.lock:
            if lobby_id in self.lobbies:
                self.lobbies[lobby_id]["reachable"] = ok


def make_handler(directory, trust_proxy):
    class Handler(BaseHTTPRequestHandler):
        server_version = "MonopolyLobbies/1"
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):  # one short line per request
            print(f"{self.client_address[0]} {fmt % args}", flush=True)

        def _address(self):
            address = self.client_address[0]
            if trust_proxy and self.headers.get("X-Forwarded-For"):
                address = self.headers["X-Forwarded-For"].split(",")[0].strip()
            return address[7:] if address.startswith("::ffff:") else address

        def _send(self, status, payload=None):
            data = b"" if payload is None else json.dumps(payload).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def _body(self):
            length = int(self.headers.get("Content-Length") or 0)
            if length <= 0 or length > MAX_BODY:
                return None
            try:
                return clean_info(json.loads(self.rfile.read(length)))
            except (ValueError, UnicodeDecodeError):
                return None

        def _token(self):
            auth = self.headers.get("Authorization", "")
            return auth[7:] if auth.startswith("Bearer ") else ""

        def _lobby_id(self):
            prefix = "/v1/lobbies/"
            return self.path[len(prefix):] if self.path.startswith(prefix) and len(self.path) > len(prefix) else None

        def do_GET(self):
            if self.path.split("?")[0] != "/v1/lobbies":
                return self._send(404, {"error": "not found"})
            self._send(200, {"lobbies": directory.list()})

        def do_POST(self):
            if self.path != "/v1/lobbies":
                return self._send(404, {"error": "not found"})
            info = self._body()
            if info is None:
                return self._send(400, {"error": "bad lobby"})
            entry, error = directory.add(self._address(), info)
            if error:
                return self._send(429, {"error": error})
            self._send(201, {k: entry[k] for k in ("id", "token", "address", "reachable")})

        def do_PUT(self):
            lobby_id, info = self._lobby_id(), self._body()
            if lobby_id is None or info is None:
                return self._send(400, {"error": "bad lobby"})
            result = directory.update(lobby_id, self._token(), info)
            if result is None:
                return self._send(404, {"error": "no such lobby"})
            self._send(200, result)

        def do_DELETE(self):
            lobby_id = self._lobby_id()
            if lobby_id is None or not directory.remove(lobby_id, self._token()):
                return self._send(404, {"error": "no such lobby"})
            self._send(204)

    return Handler


def serve(host, port, trust_proxy, directory=None):
    server = ThreadingHTTPServer((host, port), make_handler(directory or Directory(), trust_proxy))
    server.daemon_threads = True
    return server


def self_test():
    import urllib.request

    directory = Directory(ttl=3.0, probe_timeout=1.0)
    server = serve("127.0.0.1", 0, False, directory)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_address[1]}/v1/lobbies"

    def call(method, url, body=None, token=None):
        req = urllib.request.Request(url, method=method, data=None if body is None else json.dumps(body).encode())
        req.add_header("Content-Type", "application/json")
        if token:
            req.add_header("Authorization", "Bearer " + token)
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read() or b"null")
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"null")

    game = socket.socket()  # stands in for a host's open lobby port
    game.bind(("127.0.0.1", 0))
    game.listen()
    port = game.getsockname()[1]

    status, made = call("POST", base, {"name": "Test\x01 lobby", "host": "Ann", "players": 1, "port": port, "version": 1})
    assert status == 201 and made["address"] == "127.0.0.1", (status, made)
    for _ in range(50):
        lobbies = call("GET", base)[1]["lobbies"]
        if lobbies and lobbies[0]["reachable"] is not None:
            break
        time.sleep(0.05)
    assert lobbies[0]["name"] == "Test lobby" and lobbies[0]["reachable"] is True, lobbies
    assert "token" not in lobbies[0]

    assert call("PUT", base + "/" + made["id"], {"name": "x", "port": port, "players": 2}, "wrong")[0] == 404
    status, upd = call("PUT", base + "/" + made["id"], {"name": "Renamed", "port": port, "players": 2, "in_game": True},
                       made["token"])
    assert status == 200 and upd["reachable"] is True
    lobby = call("GET", base)[1]["lobbies"][0]
    assert lobby["name"] == "Renamed" and lobby["players"] == 2 and lobby["in_game"] is True

    assert call("POST", base, {"name": "bad", "port": 0})[0] == 400
    assert call("POST", base, {"name": "bad", "port": "27360"})[0] == 400
    for i in range(MAX_PER_ADDRESS - 1):
        assert call("POST", base, {"port": 40001 + i})[0] == 201
    assert call("POST", base, {"port": 40100})[0] == 429
    # Listing again on an address and port that's already listed replaces the old entry.
    status, again = call("POST", base, {"name": "Restarted", "port": 40001})
    assert status == 201 and sum(l["port"] == 40001 for l in call("GET", base)[1]["lobbies"]) == 1

    game.close()
    closed_port = port
    status, dead = call("DELETE", base + "/" + made["id"], token=made["token"])
    assert status == 204
    time.sleep(3.2)  # everything else expires
    assert call("GET", base)[1]["lobbies"] == []
    assert call("POST", base, {"port": closed_port})[0] == 201
    for _ in range(50):
        lobbies = call("GET", base)[1]["lobbies"]
        if lobbies and lobbies[0]["reachable"] is not None:
            break
        time.sleep(0.05)
    assert lobbies[0]["reachable"] is False, lobbies
    server.shutdown()
    print("lobby directory self-test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=27380)
    parser.add_argument("--trust-proxy", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    server = serve(args.host, args.port, args.trust_proxy)
    print(f"lobby directory on http://{args.host}:{args.port}/v1/lobbies", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
