"""Transport layer: get SID register writes onto the C64 Ultimate.

Transports implement write(address, data). The Flusher owns the SidStates,
batches their dirty registers and sends them from a background thread.

Hardware findings driving this design (see NOTES.md):
  * The C64U HTTP server answers every request with `Connection: close`,
    so keep-alive is impossible. Connect costs ~10 ms, the request ~10 ms.
    HttpTransport therefore keeps one *pre-opened* socket ready.
  * Payload size barely matters (1 vs 25 bytes), so we write whole ranges.
  * POST writemem is ~186 ms/call — only PUT is used.
  * DmaSocketTransport (Ultimate DMA Service, TCP 64) lands a write in ~1 ms, but needs
    acks for backpressure and a keepalive (it closes idle connections after 1 s).
"""
from __future__ import annotations

import errno
import json
import logging
import select
import socket
import threading
import time
from collections import deque
from contextlib import contextmanager
from typing import Callable, Iterable

from sid import SidState

log = logging.getLogger(__name__)

MAX_WRITE = 128  # bytes per PUT writemem (per API docs)


class TransportError(RuntimeError):
    pass


class Transport:
    def write(self, address: int, data: bytes) -> None:
        raise NotImplementedError

    # Flusher hooks; no-ops unless a transport needs pacing or keepalive (DmaSocketTransport).
    def begin_flush(self) -> None:
        """Called before each flush takes its snapshot; may block (backpressure)."""

    def end_flush(self) -> None:
        """Called after a flush has sent its writes."""

    def idle(self) -> None:
        """Called from the flush thread about every 100 ms while nothing is pending."""

    def close(self) -> None:
        pass


# ---------------------------------------------------------------- dry run

class DryRunTransport(Transport):
    """Records writes; optionally prints them and/or simulates latency."""

    def __init__(self, echo: bool = True, delay: float = 0.0,
                 out: Callable[[str], None] = print):
        self.echo, self.delay, self.out = echo, delay, out
        self.writes: list[tuple[int, bytes]] = []

    def write(self, address: int, data: bytes) -> None:
        self.writes.append((address, bytes(data)))
        if self.echo:
            self.out(f"W ${address:04X} <- {data.hex(' ').upper()}")
        if self.delay:
            time.sleep(self.delay)


# ---------------------------------------------------------------- HTTP

class HttpTransport(Transport):
    """PUT /v1/machine:writemem over raw HTTP/1.1 sockets.

    preconnect: after each response, immediately start a non-blocking connect
    for the next request so its handshake overlaps idle time.
    """

    def __init__(self, host: str, port: int = 80, password: str = "",
                 timeout: float = 1.0, connect_timeout: float = 0.3,
                 preconnect: bool = True, max_idle: float = 5.0, retries: int = 2):
        self.host, self.port = host, port
        self.password = password
        self.timeout, self.connect_timeout = timeout, connect_timeout
        self.preconnect, self.max_idle, self.retries = preconnect, max_idle, retries
        self._spare: tuple[socket.socket, float] | None = None
        self._addr = (host, port)
        self.latencies: deque[float] = deque(maxlen=2000)
        self.errors = 0
        self.spare_hits = 0      # requests served on the pre-opened socket
        self.spare_misses = 0    # spare unusable (stale/closed/not connected) -> fresh connect
        if preconnect:
            self._start_connect()

    # --- connection management
    def _new_socket(self) -> socket.socket:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return s

    def _start_connect(self) -> None:
        s = self._new_socket()
        s.setblocking(False)
        rc = s.connect_ex(self._addr)
        if rc not in (0, errno.EINPROGRESS, errno.EWOULDBLOCK):
            s.close()
            return
        self._spare = (s, time.monotonic())

    def _drop_spare(self) -> None:
        if self._spare:
            self._spare[0].close()
            self._spare = None

    def _take_socket(self) -> socket.socket:
        """A connected, blocking socket — the spare if usable, else a new one."""
        if self._spare:
            s, t0 = self._spare
            self._spare = None
            age = time.monotonic() - t0
            if age <= self.max_idle:
                remaining = max(0.0, self.connect_timeout - age)
                _, w, _ = select.select([], [s], [], remaining)
                if w and s.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR) == 0:
                    r, _, _ = select.select([s], [], [], 0)
                    if not r:  # readable now would mean the server closed it
                        s.setblocking(True)
                        s.settimeout(self.timeout)
                        self.spare_hits += 1
                        return s
            s.close()
            self.spare_misses += 1
        s = self._new_socket()
        s.settimeout(self.connect_timeout)
        s.connect(self._addr)
        s.settimeout(self.timeout)
        return s

    # --- request
    def _request(self, address: int, data: bytes) -> None:
        pw = f"X-Password: {self.password}\r\n" if self.password else ""
        req = (f"PUT /v1/machine:writemem?address={address:04X}&data={data.hex().upper()} HTTP/1.1\r\n"
               f"Host: {self.host}\r\n{pw}Content-Length: 0\r\n\r\n").encode()
        s = self._take_socket()
        try:
            s.sendall(req)
            status, body = _read_response(s)
        finally:
            s.close()
            if self.preconnect:
                self._start_connect()
        if status == 403:
            raise TransportError("403 Forbidden: wrong or missing network password")
        if status != 200:
            raise TransportError(f"HTTP {status}: {body[:200]!r}")
        if b'"errors"' in body:
            try:
                errs = json.loads(body).get("errors") or []
            except ValueError:
                errs = []
            if errs:
                raise TransportError(f"API errors: {errs}")

    def write(self, address: int, data: bytes) -> None:
        for i in range(0, len(data), MAX_WRITE):
            self._write_one(address + i, data[i:i + MAX_WRITE])

    def _write_one(self, address: int, data: bytes) -> None:
        last: Exception | None = None
        for attempt in range(self.retries + 1):
            t0 = time.perf_counter()
            try:
                self._request(address, data)
                self.latencies.append(time.perf_counter() - t0)
                return
            except TransportError:
                raise
            except (OSError, EOFError) as e:  # timeouts, resets, refused — writes are idempotent
                last = e
                self.errors += 1
                self._drop_spare()
                log.warning("write $%04X attempt %d failed: %s", address, attempt + 1, e)
        raise TransportError(f"write ${address:04X} failed after {self.retries + 1} attempts: {last}")

    def close(self) -> None:
        self._drop_spare()


def _read_response(s: socket.socket) -> tuple[int, bytes]:
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            raise EOFError("connection closed before response headers")
        buf += chunk
    head, _, body = buf.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    try:
        status = int(lines[0].split()[1])
    except (IndexError, ValueError):
        raise EOFError(f"bad status line {lines[0]!r}")
    length = None
    for line in lines[1:]:
        k, _, v = line.partition(b":")
        if k.strip().lower() == b"content-length":
            length = int(v.strip())
    while length is None or len(body) < length:
        chunk = s.recv(4096)
        if not chunk:
            break
        body += chunk
    return status, body


# ---------------------------------------------------------------- DMA socket (Phase 4)

DMA_PORT = 64
CMD_DMAWRITE = 0xFF06
CMD_AUTHENTICATE = 0xFF1F
CMD_IDENTIFY = 0xFF0E


class DmaSocketTransport(Transport):
    """Ultimate DMA Service (TCP 64): one persistent connection, binary commands.

    Framing (firmware software/network/socket_dma.cc): u16 LE command (high byte 0xFF),
    u16 LE length, payload. DMAWRITE payload = u16 LE address + data. Needs "Ultimate DMA
    Service" enabled on the C64U (Network Settings). If a network password is set, the
    first command must be AUTHENTICATE (reply: 1 byte, 1 = ok).

    Measured on the C64U (NOTES.md, Phase 4):
      * DMAWRITE has no reply, so writes are fire-and-forget. IDENTIFY (reply: length byte +
        text) is used as an in-order acknowledgement: end_flush() sends one after each flush,
        and begin_flush() waits until at most `max_in_flight` flushes are unacknowledged.
        That bounds the backlog (bursts are ~0.8 ms/command, but beyond ~150 commands/s the
        service drops to ~5 ms/command and a queue would build), and it is the only way to
        notice a dead connection: sends into a broken TCP link keep "succeeding" locally.
      * The service closes a connection after 1.0 s without a command, and a write sent into
        the closed socket is silently lost. idle() sends an IDENTIFY keepalive every
        `keepalive` s, and a connection that went quiet anyway is reopened before writing.
    """

    STALE = 0.9   # s without a command after which the service has (nearly) closed on us

    def __init__(self, host: str, port: int = DMA_PORT, password: str = "",
                 timeout: float = 1.0, retries: int = 1, max_in_flight: int = 1,
                 keepalive: float = 0.4):
        self.host, self.port, self.password = host, port, password
        self.timeout, self.retries = timeout, retries
        self.max_in_flight, self.keepalive = max_in_flight, keepalive
        self.sock: socket.socket | None = None
        self.errors = 0
        self.reconnects = 0
        self.keepalives_sent = 0
        self.last_reply = ""
        self._reset()

    def _reset(self) -> None:
        self._rx = bytearray()
        self._acks: deque[float] = deque()   # send time of each unanswered IDENTIFY
        self._unacked = False                # writes sent since the last IDENTIFY
        self._last_send = time.monotonic()

    @property
    def in_flight(self) -> int:
        return len(self._acks)

    def _connect(self) -> socket.socket:
        s = socket.create_connection((self.host, self.port), timeout=self.timeout)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if self.password:
            pw = self.password.encode()
            s.sendall(_dma_frame(CMD_AUTHENTICATE, pw))
            ok = s.recv(1)
            if ok != b"\x01":
                s.close()
                raise TransportError("DMA service: authentication failed")
        self._reset()
        return s

    def _send(self, frame: bytes) -> None:
        assert self.sock is not None
        self.sock.sendall(frame)
        self._last_send = time.monotonic()

    def _lossy(self) -> bool:
        """Could writes have been lost if the connection is gone now?"""
        return self._unacked or bool(self._acks)

    def _pump(self, wait: float = 0.0) -> None:
        """Read whatever replies have arrived (waiting up to `wait` s for the first bytes)."""
        assert self.sock is not None
        if self.sock.fileno() < 0:
            raise OSError(errno.EBADF, "socket closed")
        r, _, _ = select.select([self.sock], [], [], wait)
        if not r:
            return
        chunk = self.sock.recv(4096)
        if not chunk:
            lossy = self._lossy()
            self.close()
            if lossy:
                raise TransportError("DMA service closed the connection")
            return                                   # idle close; nothing was lost
        self._rx += chunk
        while self._rx and len(self._rx) >= 1 + self._rx[0]:
            n = self._rx[0]
            self.last_reply = bytes(self._rx[1:1 + n]).decode(errors="replace")
            del self._rx[:1 + n]
            if self._acks:
                self._acks.popleft()

    def _ensure(self) -> None:
        if self.sock is not None:
            if not self._lossy() and time.monotonic() - self._last_send > self.STALE:
                self.close()                         # the service is about to drop it
            else:
                self._pump()                         # notices an idle close
        if self.sock is None:
            self.sock = self._connect()
            self.reconnects += 1

    def _wait_acks(self, limit: int) -> None:
        deadline = time.monotonic() + self.timeout
        while self.sock is not None and len(self._acks) > limit:
            left = deadline - time.monotonic()
            if left <= 0:
                self.close()
                raise TransportError(f"DMA service: no reply for {self.timeout:.1f} s")
            self._pump(left)

    def write(self, address: int, data: bytes) -> None:
        frame = _dma_frame(CMD_DMAWRITE, address.to_bytes(2, "little") + bytes(data))
        for attempt in range(self.retries + 1):
            lossy = self.sock is not None and self._lossy()
            try:
                self._ensure()
                self._send(frame)
                self._unacked = True
                return
            except OSError as e:
                self.errors += 1
                self.close()
                # retrying is only safe if nothing earlier can have been lost with the socket
                if attempt == self.retries or lossy:
                    raise TransportError(f"DMA service write ${address:04X} failed: {e}") from e

    # --- Flusher hooks
    def begin_flush(self) -> None:
        try:
            if self.sock is not None:
                self._pump()
                self._wait_acks(self.max_in_flight - 1)
        except OSError as e:
            self.close()
            raise TransportError(f"DMA service: {e}") from e

    def end_flush(self) -> None:
        if self.sock is None or not self._unacked:
            return
        try:
            self._send(_dma_frame(CMD_IDENTIFY, b""))
        except OSError as e:
            self.close()
            raise TransportError(f"DMA service: {e}") from e
        self._acks.append(time.monotonic())
        self._unacked = False

    def idle(self) -> None:
        if self.sock is None:
            return
        try:
            self._pump()
            if self.sock is None:
                return
            now = time.monotonic()
            if self._acks and now - self._acks[0] > self.timeout:
                self.close()
                raise TransportError(f"DMA service: no reply for {self.timeout:.1f} s")
            if not self._acks and now - self._last_send > self.keepalive:
                self._send(_dma_frame(CMD_IDENTIFY, b""))
                self._acks.append(now)
                self.keepalives_sent += 1
        except OSError as e:
            self.close()
            raise TransportError(f"DMA service: {e}") from e

    def identify(self) -> str:
        self._ensure()
        self.end_flush()
        self._wait_acks(0)
        self._send(_dma_frame(CMD_IDENTIFY, b""))
        self._acks.append(time.monotonic())
        self._wait_acks(0)
        return self.last_reply

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None


def _dma_frame(cmd: int, payload: bytes) -> bytes:
    if len(payload) > 0xFFFF:
        raise ValueError("DMA payload too long")
    return cmd.to_bytes(2, "little") + len(payload).to_bytes(2, "little") + payload


# ---------------------------------------------------------------- flusher

class Flusher:
    """Owns SidStates; sends their dirty registers from a background thread.

    Edit state inside `with flusher.edit() as sids:` (or holding flusher.lock).
    Changes made between flushes coalesce. Use flusher.gate() for gate changes:
    a gate-on that follows a not-yet-sent gate-off on the same voice first
    commits the gate-off as its own batch, so the retrigger isn't lost.
    """

    def __init__(self, transport: Transport, states: Iterable[SidState],
                 interval_ms: float = 5.0, strict_order: bool = True):
        self.transport = transport
        self.states = list(states)
        self.interval = interval_ms / 1000.0
        self.strict_order = strict_order
        self.lock = threading.RLock()      # guards states + queue
        self._io = threading.Lock()        # serialises sends; always taken before self.lock
        self._queue: list[list[tuple[int, bytes]]] = []
        self._wake = threading.Event()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self.batches_sent = 0
        self.writes_sent = 0
        self.errors = 0
        self.connected = True          # False after a failed flush, until one succeeds
        self.last_error: str = ""
        self.on_error: Callable[[Exception], None] | None = None
        self.on_reconnect: Callable[[], None] | None = None
        self.backoff_min, self.backoff_max = 0.25, 2.0

    # --- editing
    @contextmanager
    def edit(self):
        with self.lock:
            yield self.states
        self._wake.set()

    def gate(self, sid_index: int, voice: int, on: bool) -> None:
        with self.lock:
            st = self.states[sid_index]
            if on and st.gate_off_pending(voice):
                self._commit_locked()
            st.set_gate(voice, on)
        self._wake.set()

    def barrier(self) -> None:
        """Commit everything changed so far as its own batch (sent before later changes)."""
        with self.lock:
            self._commit_locked()
        self._wake.set()

    def _commit_locked(self) -> None:
        batch: list[tuple[int, bytes]] = []
        for st in self.states:
            batch += st.snapshot(self.strict_order)
        if batch:
            self._queue.append(batch)

    # --- sending
    def flush(self) -> int:
        """Send everything pending now (blocking). Returns number of writes."""
        with self._io:
            n = 0
            try:
                self.transport.begin_flush()
                with self.lock:
                    self._commit_locked()
                    batches, self._queue = self._queue, []
                for batch in batches:
                    for address, data in batch:
                        self.transport.write(address, data)
                        n += 1
                    self.batches_sent += 1
                if n:
                    self.transport.end_flush()
            except Exception as e:  # noqa: BLE001
                self._failed(e)
                raise
            else:
                if not self.connected and n:
                    self.connected = True
                    log.warning("C64U reachable again — all registers resynced")
                    if self.on_reconnect:
                        self.on_reconnect()
            finally:
                self.writes_sent += n
            return n

    def _failed(self, e: Exception) -> None:
        self.errors += 1
        self.last_error = str(e)
        if self.connected:
            log.error("C64U write failed: %s — retrying; registers will be resynced", e)
        self.connected = False
        with self.lock:
            for st in self.states:
                st.mark_all_dirty()
        if self.on_error:
            self.on_error(e)

    def idle(self) -> None:
        """Let the transport keep its connection alive (called while nothing is pending)."""
        with self._io:
            try:
                self.transport.idle()
            except Exception as e:  # noqa: BLE001
                self._failed(e)
                raise

    def _pending(self) -> bool:
        with self.lock:
            return bool(self._queue) or any(st.dirty for st in self.states)

    def _run(self) -> None:
        last = 0.0
        backoff = self.backoff_min
        while not self._stop.is_set():
            self._wake.wait(timeout=0.1)
            self._wake.clear()
            if self._stop.is_set():
                continue
            if not self._pending():
                try:
                    self.idle()
                except Exception:  # noqa: BLE001 — logged by _failed; the next flush resyncs
                    self._stop.wait(backoff)
                    backoff = min(self.backoff_max, backoff * 2)
                continue
            gap = self.interval - (time.monotonic() - last)
            if gap > 0:
                time.sleep(gap)
            last = time.monotonic()
            try:
                self.flush()
                backoff = self.backoff_min
            except Exception:  # noqa: BLE001 — logged in flush; back off, then retry
                self._stop.wait(backoff)
                backoff = min(self.backoff_max, backoff * 2)

    def start(self) -> "Flusher":
        if not self._thread:
            self._thread = threading.Thread(target=self._run, name="sid-flusher", daemon=True)
            self._thread.start()
        return self

    def stop(self, final_flush: bool = True) -> None:
        self._stop.set()
        self._wake.set()
        if self._thread:
            self._thread.join(timeout=2)
            self._thread = None
        if final_flush:
            try:
                self.flush()
            except Exception:  # noqa: BLE001
                pass
