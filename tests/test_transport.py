import socket
import threading
import time
from urllib.parse import parse_qs, urlparse

import pytest

import sid
from sid import SidState
from transport import DryRunTransport, Flusher, HttpTransport, TransportError


def make(strict=True):
    t = DryRunTransport(echo=False)
    st = SidState(0xD400)
    return t, st, Flusher(t, [st], strict_order=strict)


# ---------------------------------------------------------------- snapshot ordering

def test_knob_sweep_coalesces():
    t, st, fl = make()
    with fl.edit():
        for fc in range(0, 2048, 7):     # 293 changes, last = 2044
            st.set_cutoff(fc)
    assert fl.flush() == 1
    assert t.writes == [(0xD415, bytes([2044 & 7, 2044 >> 3]))]
    assert fl.flush() == 0  # nothing dirty


def test_note_on_single_write_freq_before_gate():
    t, st, fl = make()
    with fl.edit():
        st.set_note(0, 69)
    fl.gate(0, 0, True)
    fl.flush()
    assert len(t.writes) == 1
    addr, data = t.writes[0]
    assert addr == 0xD400 and data[0:2] == b"\x45\x1D" and data[4] == 0x01


def test_adsr_with_gate_splits_control_last():
    t, st, fl = make()
    with fl.edit():
        st.set_waveform(0, sid.SAW)
        st.set_adsr(0, a=2, d=3, s=4, r=5)
        st.set_gate(0, True)
    fl.flush()
    assert t.writes == [
        (0xD405, b"\x23\x45"),   # ADSR first...
        (0xD404, b"\x21"),       # ...then control with gate
    ]


def test_split_writes_old_control_inside_data_range():
    t, st, fl = make()
    with fl.edit():
        st.set_freq(0, 0x1234)
        st.set_adsr(0, a=1)
        st.set_control(0, 0x11)
    fl.flush()
    (a1, d1), (a2, d2) = t.writes
    assert a1 == 0xD400 and d1 == b"\x34\x12\x00\x00\x00\x10"  # control byte = old value 00
    assert (a2, d2) == (0xD404, b"\x11")


def test_no_split_when_not_strict():
    t, st, fl = make(strict=False)
    with fl.edit():
        st.set_adsr(0, a=2)
        st.set_gate(0, True)
    fl.flush()
    assert len(t.writes) == 1


def test_retrigger_within_one_window_is_not_lost():
    t, st, fl = make()
    with fl.edit():
        st.set_waveform(0, sid.TRI)
    fl.gate(0, 0, True)
    fl.flush()
    t.writes.clear()
    fl.gate(0, 0, False)
    fl.gate(0, 0, True)   # before any flush
    fl.flush()
    assert t.writes == [(0xD404, b"\x10"), (0xD404, b"\x11")]


def test_gate_off_then_on_other_voice_not_split():
    t, st, fl = make()
    fl.gate(0, 0, True); fl.flush(); t.writes.clear()
    fl.gate(0, 0, False)
    fl.gate(0, 1, True)   # different voice: no barrier needed
    fl.flush()
    assert len(t.writes) == 1


def test_two_sids_each_get_writes():
    t = DryRunTransport(echo=False)
    a, b = SidState(0xD400), SidState(0xD420)
    fl = Flusher(t, [a, b])
    with fl.edit():
        a.set_volume(15); b.set_volume(7)
    fl.flush()
    assert t.writes == [(0xD418, b"\x0f"), (0xD438, b"\x07")]


def test_background_thread_sends():
    t, st, fl = make()
    fl.start()
    try:
        with fl.edit():
            st.set_volume(9)
        deadline = time.time() + 1
        while not t.writes and time.time() < deadline:
            time.sleep(0.005)
    finally:
        fl.stop()
    assert t.writes == [(0xD418, b"\x09")]


def test_failed_flush_marks_resync():
    class Boom(DryRunTransport):
        def write(self, a, d):
            raise TransportError("down")
    st = SidState()
    fl = Flusher(Boom(echo=False), [st])
    with fl.edit():
        st.set_volume(3)
    with pytest.raises(TransportError):
        fl.flush()
    assert st.dirty == set(range(25))


# ---------------------------------------------------------------- HTTP against a fake C64U

class FakeC64U:
    """Minimal HTTP server mimicking the C64U: Connection: close on every reply."""

    def __init__(self, fail_first=0, status=200, body=b'{ "errors" : [  ] }'):
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(8)
        self.port = self.srv.getsockname()[1]
        self.requests: list[str] = []
        self.mem = bytearray(65536)
        self.fail_first, self.status, self.body = fail_first, status, body
        self._t = threading.Thread(target=self._serve, daemon=True)
        self._t.start()

    def _serve(self):
        while True:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            with c:
                buf = b""
                while b"\r\n\r\n" not in buf:
                    d = c.recv(4096)
                    if not d:
                        break
                    buf += d
                if not buf:
                    continue  # pre-opened socket closed unused
                line = buf.split(b"\r\n")[0].decode()
                self.requests.append(line)
                if self.fail_first > 0:
                    self.fail_first -= 1
                    continue  # drop connection without reply
                method, target, _ = line.split()
                q = parse_qs(urlparse(target).query)
                if method == "PUT" and self.status == 200:
                    addr = int(q["address"][0], 16)
                    data = bytes.fromhex(q["data"][0])
                    self.mem[addr:addr + len(data)] = data
                c.sendall(b"HTTP/1.1 %d X\r\nConnection: close\r\nContent-Type: application/json\r\n"
                          b"Content-Length: %d\r\n\r\n%s" % (self.status, len(self.body), self.body))

    def close(self):
        self.srv.close()


@pytest.fixture
def fake():
    f = FakeC64U()
    yield f
    f.close()


@pytest.mark.parametrize("preconnect", [True, False])
def test_http_put_format_and_memory(fake, preconnect):
    t = HttpTransport("127.0.0.1", port=fake.port, preconnect=preconnect)
    for i in range(5):
        t.write(0xD400, bytes([i, 0xAB]))
    t.close()
    assert fake.requests[-1] == "PUT /v1/machine:writemem?address=D400&data=04AB HTTP/1.1"
    assert fake.mem[0xD400:0xD402] == b"\x04\xab"
    assert len(t.latencies) == 5


def test_http_chunks_over_128_bytes(fake):
    t = HttpTransport("127.0.0.1", port=fake.port)
    payload = bytes(range(200))
    t.write(0x1000, payload)
    t.close()
    assert len(fake.requests) == 2
    assert "address=1080" in fake.requests[1]
    assert bytes(fake.mem[0x1000:0x1000 + 200]) == payload


def test_http_retries_dropped_connection():
    f = FakeC64U(fail_first=1)
    try:
        t = HttpTransport("127.0.0.1", port=f.port, retries=2)
        t.write(0xD418, b"\x0f")
        t.close()
        assert f.mem[0xD418] == 0x0F
        assert t.errors == 1
    finally:
        f.close()


def test_http_api_error_raises():
    f = FakeC64U(status=200, body=b'{"errors": ["bad address"]}')
    try:
        t = HttpTransport("127.0.0.1", port=f.port)
        with pytest.raises(TransportError, match="bad address"):
            t.write(0xD418, b"\x0f")
        t.close()
    finally:
        f.close()


def test_http_403_raises():
    f = FakeC64U(status=403, body=b"")
    try:
        t = HttpTransport("127.0.0.1", port=f.port)
        with pytest.raises(TransportError, match="403"):
            t.write(0xD418, b"\x0f")
        t.close()
    finally:
        f.close()


def test_stale_spare_is_replaced(fake):
    t = HttpTransport("127.0.0.1", port=fake.port, max_idle=0.05)
    time.sleep(0.1)  # spare is now too old
    t.write(0xD418, b"\x01")
    t.close()
    assert fake.mem[0xD418] == 1


# ---------------------------------------------------------------- DMA socket (port 64) against a fake

class FakeDmaService:
    """Speaks the socket_dma.cc framing: u16 cmd, u16 len, payload; DMAWRITE has no reply."""

    def __init__(self, password="", idle_close=None, delay=0.0):
        # idle_close: drop a connection after this many s without a command (C64U: 1.0 s)
        # delay: s per command (the C64U slows to ~5 ms/command under sustained load)
        self.idle_close, self.delay, self.frozen = idle_close, delay, False
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 0)); self.srv.listen(4)
        self.port = self.srv.getsockname()[1]
        self.mem = bytearray(65536); self.connections = 0; self.password = password.encode()
        threading.Thread(target=self._serve, daemon=True).start()

    def _read(self, c, n):
        b = b""
        while len(b) < n:
            d = c.recv(n - len(b))
            if not d:
                raise EOFError
            b += d
        return b

    def _serve(self):
        while True:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            self.connections += 1
            authed = not self.password
            c.settimeout(self.idle_close)
            try:
                while True:
                    cmd = int.from_bytes(self._read(c, 2), "little")
                    n = int.from_bytes(self._read(c, 2), "little")
                    payload = self._read(c, n)
                    if self.delay:
                        time.sleep(self.delay)
                    if self.frozen:
                        continue
                    if cmd == 0xFF1F:
                        authed = payload == self.password
                        c.sendall(b"\x01" if authed else b"\x00")
                        if not authed:
                            break
                    elif not authed:
                        break
                    elif cmd == 0xFF06:
                        a = int.from_bytes(payload[:2], "little")
                        self.mem[a:a + n - 2] = payload[2:]
                    elif cmd == 0xFF0E:
                        t = b"C64U-FAKE"; c.sendall(bytes([len(t)]) + t)
            except (EOFError, OSError):   # includes the idle timeout
                pass
            c.close()

    def close(self):
        self.srv.close()


def test_dma_socket_writes_on_one_connection():
    from transport import DmaSocketTransport
    f = FakeDmaService()
    try:
        t = DmaSocketTransport("127.0.0.1", port=f.port)
        for i in range(50):
            t.write(0xD400, bytes([i, 0x11]))
        t.write(0xD418, b"\x0f")
        assert t.identify() == "C64U-FAKE"       # also flushes ordering: identify replies after writes
        assert f.mem[0xD400:0xD402] == bytes([49, 0x11]) and f.mem[0xD418] == 0x0F
        assert f.connections == 1
        t.close()
    finally:
        f.close()


def test_dma_socket_password_and_reconnect():
    from transport import DmaSocketTransport
    f = FakeDmaService(password="pw")
    try:
        bad = DmaSocketTransport("127.0.0.1", port=f.port, password="nope", retries=0)
        with pytest.raises(TransportError, match="authentication"):
            bad.write(0xD418, b"\x01")
        t = DmaSocketTransport("127.0.0.1", port=f.port, password="pw")
        t.write(0xD418, b"\x05")
        t.end_flush(); t.begin_flush()            # acknowledged: nothing can be lost now
        t.sock.close()                            # connection dropped under us
        t.write(0xD418, b"\x07")                  # safe to reconnect + retry
        assert t.identify() == "C64U-FAKE"
        assert f.mem[0xD418] == 0x07 and t.reconnects == 2
        t.write(0xD418, b"\x09")                  # not acknowledged yet ...
        t.sock.close()
        with pytest.raises(TransportError):       # ... so a silent retry could hide a lost write
            t.write(0xD418, b"\x0a")
    finally:
        f.close()


def test_dma_keepalive_holds_connection_across_idle_close():
    from transport import DmaSocketTransport
    f = FakeDmaService(idle_close=0.3)
    try:
        t = DmaSocketTransport("127.0.0.1", port=f.port, keepalive=0.1)
        fl = Flusher(t, [SidState()], interval_ms=1).start()
        with fl.edit() as (s,):
            s.set_volume(5)
        time.sleep(1.0)                      # > 3x the idle close: only keepalives keep it open
        with fl.edit() as (s,):
            s.set_volume(9)
        time.sleep(0.1)
        fl.stop()
        assert f.mem[0xD418] & 0x0F == 9 and f.connections == 1 and t.keepalives_sent >= 3
        assert fl.connected and fl.errors == 0
    finally:
        f.close()


def test_dma_idle_closed_connection_is_reopened_without_loss():
    from transport import DmaSocketTransport
    f = FakeDmaService(idle_close=0.1)
    try:
        t = DmaSocketTransport("127.0.0.1", port=f.port)
        t.write(0xD418, b"\x03"); t.end_flush(); t.begin_flush()
        time.sleep(0.3)                      # service drops us (before STALE)
        t.write(0xD418, b"\x04")             # EOF noticed before sending -> reopen, not lost
        assert t.identify() == "C64U-FAKE" and f.mem[0xD418] == 4 and f.connections == 2
        t.STALE = 0.2
        time.sleep(0.25)                     # past STALE: reopened proactively
        t.write(0xD418, b"\x06")
        assert t.identify() == "C64U-FAKE" and f.mem[0xD418] == 6 and f.connections == 3
    finally:
        f.close()


def test_dma_backpressure_bounds_backlog_and_coalesces():
    from transport import DmaSocketTransport
    f = FakeDmaService(delay=0.01)            # 10 ms per command: slower than the edits
    try:
        t = DmaSocketTransport("127.0.0.1", port=f.port)
        fl = Flusher(t, [SidState()], interval_ms=1).start()
        for i in range(200):                 # a fast knob sweep: one edit per ms
            with fl.edit() as (s,):
                s.set_cutoff(i * 10)
            time.sleep(0.001)
        t0 = time.monotonic()
        while (f.mem[0xD415] & 7) | (f.mem[0xD416] << 3) != 1990 and time.monotonic() - t0 < 1:
            time.sleep(0.002)
        settle = time.monotonic() - t0
        fl.stop()
        assert settle < 0.06                 # at most ~two commands behind, not 200
        assert fl.batches_sent < 120         # edits coalesced while waiting for acks
    finally:
        f.close()


def test_dma_dead_link_is_detected():
    from transport import DmaSocketTransport
    f = FakeDmaService()
    try:
        t = DmaSocketTransport("127.0.0.1", port=f.port, timeout=0.2)
        fl = Flusher(t, [SidState()], interval_ms=1).start()
        with fl.edit() as (s,):
            s.set_volume(1)
        time.sleep(0.05)
        f.frozen = True                      # link up, but nothing is processed any more
        with fl.edit() as (s,):
            s.set_volume(2)
        time.sleep(0.6)
        assert fl.errors >= 1 and "no reply" in fl.last_error
        f.frozen = False                     # link recovers: everything is rewritten
        t0 = time.monotonic()
        while f.mem[0xD418] & 0x0F != 2 and time.monotonic() - t0 < 3:
            time.sleep(0.01)
        fl.stop(final_flush=False)
        assert f.mem[0xD418] & 0x0F == 2
    finally:
        f.close()
