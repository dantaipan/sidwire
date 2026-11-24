import http.server
import threading

import pytest

from finder import FoundDevice, find_ultimates, local_address, parse_info

REAL_INFO = ('{ \n  "product" : "C64 Ultimate",\n  "firmware_version" : "1.2.1",\n'
             '  "hostname" : "C64-Ultimate-ABC123",\n  "errors" : [  ]\n}')


def serve(status: int, body: str):
    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):  # noqa: N802
            self.server.seen.append((self.path, self.headers.get("X-Password")))
            self.send_response(status)
            self.end_headers()
            self.wfile.write(body.encode())

        def log_message(self, *a):
            pass

    srv = http.server.HTTPServer(("127.0.0.1", 0), H)
    srv.seen = []
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def test_parse_info():
    d = parse_info(REAL_INFO)
    assert d == FoundDevice("", "C64-Ultimate-ABC123", "C64 Ultimate", "1.2.1")
    assert parse_info('{"product": "Router"}') is None
    assert parse_info("nope") is None


@pytest.mark.parametrize("status, body, expect", [
    (200, REAL_INFO, "found"), (403, "{}", "locked"), (200, '{"product": "Router"}', "none")])
def test_find_on_localhost(status, body, expect):
    srv = serve(status, body)
    try:
        prog = []
        found = find_ultimates(hosts=["127.0.0.1", "127.0.0.2"], port=srv.server_port, timeout=0.3,
                               password="pw", progress=prog.append)
        assert prog[-1] == 100
        assert srv.seen[0] == ("/v1/info", "pw")
        if expect == "found":
            assert [d.hostname for d in found] == ["C64-Ultimate-ABC123"] and found[0].ip == "127.0.0.1"
        elif expect == "locked":
            assert len(found) == 1 and found[0].locked
        else:
            assert found == []
    finally:
        srv.shutdown()


def test_local_address_shape():
    ip = local_address()
    assert ip == "" or ip.count(".") == 3
