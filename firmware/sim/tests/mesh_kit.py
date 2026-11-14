"""Shared helpers for the mesh tests: a device with its LoRa radio fitted and
switched on, the device's own view of it (the sim's `mesh` control), and a
stock Meshtastic node's view of
what it puts on the air (otpsim.mesh — an independent codec)."""
import base64
import os
import shutil
import time

import pytest

from types import SimpleNamespace

from conftest import ARTIFACTS, TIME_SCALE
from otpsim import SimDevice
from otpsim import mesh as M

BROADCAST = M.BROADCAST


def start_device(mac=None, args=()):
    a = ["--gps-baud", "115200", "--lora"] + list(args)
    if mac:
        a += ["--mac", mac]
    return SimDevice.start(args=a)


def node_num_for(mac):
    b = bytes(int(x, 16) for x in mac.split(":"))
    return int.from_bytes(b[2:6], "little")


class Mesh:
    """One device with its radio on, and helpers that read the mesh as the
    device holds it (the sim's `mesh` control command)."""

    def __init__(self, dev, radio_on=True):
        self.dev = dev
        self.num = self.state().node_num
        line = dev.wait_log(r"PKI key pair, public key", since=0, timeout=10)
        self.public_key = base64.b64decode(line.split("public key ")[1].strip())
        self.tx_seq = 0
        # Hears what the device sends, on the default channel, as a stock node.
        self.ear = M.VirtualNode(0x0EA20001, "Ear", "EAR")
        self.channels = [M.Channel()]
        if radio_on:
            self.radio(True)

    # -- the device's view ------------------------------------------------------------------

    def _mesh(self, **op):
        return self.dev.cmd("mesh", **op)

    def _view(self):
        return _ns(self._mesh()["mesh"])

    def state(self):
        return self._view().state

    def stats(self):
        return self._view().stats

    def nodes(self):
        return self._view().nodes

    def history(self):
        return self._view().history

    def device_channels(self):
        return self._view().channels

    def radio(self, on, announce=True):
        """Mesh on or off. Turned on, waits (unless `announce` is False) for
        the NodeInfo the device sends 5 s later, so a test's own frames don't
        land while it is transmitting (half-duplex)."""
        seq = self.dev.lora()["tx_seq"]
        self._mesh(op="radio", on=on)
        want = "rx" if on else "sleep"
        self.dev_wait(lambda: self.dev.lora()["mode"] == want, "radio " + want)
        if on and announce:
            self.dev_wait(lambda: self.dev.lora()["tx_seq"] > seq and self.dev.lora()["mode"] == "rx",
                          "the announcement after switching on", timeout=15)

    def send(self, text, dest=BROADCAST, channel=0):
        """As Doba's composer: the message queued on the UI task (console `mesh tx`)."""
        d = "all" if dest == BROADCAST else "%x" % dest
        lines = self.dev.console("mesh tx %s %d %s" % (d, channel, text), 400)
        ids = [int(l.split("id=")[1]) for l in lines if "mesh tx id=" in l]
        pid = ids[0] if ids else 0
        return _Queued(_Msg(packet_id=pid) if pid else None)

    def set_private(self, index, name, psk):
        self.dev.console("mesh chan %d %s %s" % (index, bytes(psk).hex() or "-", name), 400)
        self.dev_wait(lambda: any(c.index == index and c.name == name for c in self.device_channels()),
                      "private channel %d applied" % index)
        self.channels.append(M.Channel(name, psk))

    def command(self, preset=None, delete_channel=None, radio_on=None):
        """What the old phone link's MeshCommand did, through the sim's control."""
        if preset is not None:
            self.dev.console("mesh preset %d" % preset, 400)
        if delete_channel is not None:
            self.dev.console("mesh forget %d" % delete_channel, 400)
        if radio_on is not None:
            self._mesh(op="radio", on=radio_on)

    def share(self, index, on):
        self.dev.console("mesh share %d %s" % (index, "on" if on else "off"), 400)

    # -- the air ---------------------------------------------------------------------------

    def hear(self, frame, **kw):
        """Puts a frame on the air for the device."""
        return self.dev.lora_rx(frame, **kw)

    def deliver(self, frame, timeout=10, **kw):
        """Puts a frame on the air and waits until the device has read it out
        of the radio's FIFO, so the next one cannot overwrite it.

        For tests that put many frames on the air to fill a table, not to test
        reception under load. The air runs on device time, and on a slow or
        loaded host the mesh task's 250 ms poll can oversleep by more than a
        LongFast frame's ~600 ms on air — something no real ESP32 does — so a
        burst queued back to back loses frames to `overrun` in the sim alone."""
        self.hear(frame, **kw)
        self._wait_air_clear(timeout)

    def drain(self, timeout=10):
        """Waits until everything on the air has reached the device and the
        mesh task has had time to handle it (it polls every 250 ms)."""
        self._wait_air_clear(timeout)
        self.dev_sleep(400)

    def _wait_air_clear(self, timeout):
        deadline = time.time() + timeout * TIME_SCALE
        while True:
            st = self.dev.lora()
            if st["in_flight"] == 0 and not st["fifo"]:
                return
            assert time.time() < deadline, "frames never left the air: %s" % st
            time.sleep(0.02)

    def dev_sleep(self, device_ms):
        """Waits `device_ms` of device time."""
        t0 = self.dev.status()["uptime_ms"]
        while self.dev.status()["uptime_ms"] - t0 < device_ms:
            time.sleep(0.05)

    def dev_wait(self, pred, what, timeout=10):
        deadline = time.time() + timeout * TIME_SCALE
        while not pred():
            assert time.time() < deadline, "timed out waiting for " + what
            time.sleep(0.1)

    def sent(self, since=None, channels=None, pki_from=None, ear=None):
        """Frames the device transmitted since `since` (default: since the last
        call), decoded by a stock node (`ear`, default one that knows the
        device's channels). Each: header fields + data + `frame` + timing."""
        r = self.dev.cmd("lora_tx", since=self.tx_seq if since is None else since)
        # Only transmissions that have finished: one still going out is not
        # yet something a test may answer (the radio is deaf until it ends,
        # and until the mesh task — which polls every 250 ms — re-arms it).
        frames = []
        for f in r["frames"]:
            if f["end_ms"] + 300 > r["now_ms"]:
                break
            f["data"] = bytes.fromhex(f["hex"])
            frames.append(f)
        if since is None and frames:
            self.tx_seq = frames[-1]["seq"]
        out = []
        ear = ear or self.ear
        for f in frames:
            h = ear.decode(f["data"], channels=channels or self.channels,
                           pki_from=pki_from)
            h["frame"] = f["data"]
            h["tx"] = f
            out.append(h)
        return out

    def wait_sent(self, pred, what, timeout=15, **kw):
        """Waits for a transmission matching `pred` (on decoded frames)."""
        deadline = time.time() + timeout * TIME_SCALE
        seen = []
        while True:
            for h in self.sent(**kw):
                seen.append(h)
                if pred(h):
                    return h
            assert time.time() < deadline, "the device never sent %s; it sent %s" % (
                what, [(x["dest"], x["data"] and x["data"]["portnum"]) for x in seen])
            time.sleep(0.1)

    def close(self):
        pass


def keep_artifacts(request, dev):
    failed = getattr(request.node, "rep_call", None) is not None and request.node.rep_call.failed
    if failed:
        out = os.path.join(ARTIFACTS, request.node.name + ("-" + os.path.basename(dev.dir)))
        os.makedirs(out, exist_ok=True)
        try:
            dev.screenshot(os.path.join(out, "panel.png"))
        except Exception:
            pass
    dev.stop()
    if failed:
        shutil.copy(dev.log_path, os.path.join(out, "console.log"))
    shutil.rmtree(dev.dir, ignore_errors=True)


@pytest.fixture
def mesh(request):
    """A device with its radio fitted and on (LongFast, the default channel),
    a paired phone on the link, and the phone's view of the mesh."""
    dev = start_device()
    m = None
    try:
        m = Mesh(dev)
        yield m
    finally:
        if m:
            m.close()
        keep_artifacts(request, dev)


def is_port(h, port):
    return h["data"] is not None and h["data"]["portnum"] == port


def lock_warnings(dev):
    return [l for l in dev.log() if "WITHOUT sdLock" in l or "LOCK ORDER INVERSION" in l]


class _Queued:
    """The old link's answer to a send: mesh_queued with the packet id."""

    def __init__(self, q):
        self.mesh_queued = q

    def WhichOneof(self, _):
        return "mesh_queued" if self.mesh_queued else "result"


class _Msg(SimpleNamespace):
    """Attribute access plus protobuf's HasField(), for tests written against
    the old phone-link messages."""

    def HasField(self, name):
        return getattr(self, name, None) is not None


def _ns(x):
    """JSON from the control channel -> attribute access, like the old protobufs."""
    if isinstance(x, dict):
        return _Msg(**{k: _ns(v) for k, v in x.items()})
    if isinstance(x, list):
        return [_ns(v) for v in x]
    return x
