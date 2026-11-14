"""The mesh (Meshtastic-compatible group-ride messaging) against the sim's
LoRa radio (sim/host/lora_sim.cpp): what the device hears and shows the
phone, what it puts on the air, and what it must not.

Every frame here is built — and everything the device sends is read — by
otpsim.mesh, a Meshtastic codec written from Meshtastic's own definitions,
not from src/mesh_proto.cpp. So these tests are interoperability tests at
the level of the radio, where tools/mesh_test (the "Meshtastic wire format"
CI job) checks the codec's functions one by one: hashes, frequencies, key
expansion, AES/CCM vectors and protobuf layouts are not re-checked here."""
import os
import struct
import time

import pytest

from conftest import TIME_SCALE
from otpsim import mesh as M
from mesh_kit import BROADCAST, Mesh, is_port, keep_artifacts, lock_warnings, mesh, start_device  # noqa: F401


ALEX = 0x1234ABCD
BEA = 0x0BEA0002


def alex(**kw):
    return M.VirtualNode(ALEX, "Alex Rivera", "ALX", **kw)


def bea(**kw):
    return M.VirtualNode(BEA, "Bea", "BEA", **kw)


# ---- the radio ---------------------------------------------------------------------------


def test_a_fresh_device_keeps_its_radio_silent_until_the_rider_turns_the_mesh_on(request):
    """Mesh is off out of the box: a rider who never asked for it must not be
    transmitting (and paying for idle receive). Turned on from the app, it
    listens exactly where an unconfigured stock node is — LongFast, US slot
    20, 906.875 MHz, SF11/250 kHz, sync word 0x2B — and introduces itself
    with a NodeInfo a stock node can read: its !id, names and public key,
    asking for replies so it relearns its neighbours' keys."""
    dev = start_device()
    try:
        m = Mesh(dev, radio_on=False)
        dev.set_speed(60)
        m.dev_sleep(40000)   # past the 20 s first announcement
        assert dev.lora()["up"] is False
        assert dev.lora_tx()[0] == [], "the radio transmitted while the mesh was off"
        assert not m.state().radio_ok

        dev.set_speed(1)
        m.radio(True, announce=False)
        tune = dev.lora()
        stock = M.tune_for("LongFast")
        assert abs(tune["freq_mhz"] - stock["freq_mhz"]) < 1e-3 and tune["freq_mhz"] == pytest.approx(906.875)
        assert (tune["sf"], tune["bw_khz"], tune["sync_word"]) == (11, 250, 0x2B)
        st = m.state()
        assert st.radio_ok and st.enabled and abs(st.freq_hz - 906875000) < 100

        hello = m.wait_sent(lambda h: is_port(h, M.PORT_NODEINFO), "a NodeInfo announcement", timeout=15)
        assert hello["dest"] == BROADCAST and hello["channel"] == M.Channel().hash
        assert hello["sender"] == m.num and hello["hop_start"] == hello["hop_limit"] > 0
        assert hello["data"]["want_response"], "the first announcement should ask neighbours to reply"
        user = M.decode_user(hello["data"]["payload"])
        assert user["id"] == "!%08x" % m.num
        assert user["long_name"] == st.long_name and user["short_name"] == st.short_name
        assert user["public_key"] == m.public_key and len(user["public_key"]) == 32
        assert user["hw_model"] == 0, "the board is not a Meshtastic hardware model and must not claim one"
        m.close()
    finally:
        keep_artifacts(request, dev)


@pytest.mark.parametrize("preset", M.PRESET_ORDER)
def test_each_modem_preset_tunes_where_stock_nodes_on_it_listen(mesh, preset):
    """With no channel name the preset's name IS the channel (a stock node
    leaves its primary unnamed), so each preset is its own frequency slot as
    well as its own modem. A device on any preset must hear a stock node on
    the same one."""
    idx = M.PRESET_ORDER.index(preset)
    if idx:
        mesh.command(preset=idx)
        mesh.dev_wait(lambda: mesh.dev.lora()["sf"] == M.PRESETS[preset][0] and mesh.dev.lora()["mode"] == "rx",
                      "retune to " + preset)
    stock = M.tune_for(preset)
    t = mesh.dev.lora()
    assert t["freq_mhz"] == pytest.approx(stock["freq_mhz"], abs=1e-3)
    assert (t["sf"], t["bw_khz"]) == (stock["sf"], stock["bw_khz"])
    ch = M.Channel(preset)
    node = alex(channel=ch)
    mesh.hear(node.text("on " + preset), **stock)
    mesh.drain()
    got = [x for x in mesh.history() if x.text == "on " + preset]
    assert got and got[0].channel == 0


def test_a_node_on_another_preset_or_channel_is_not_heard(mesh):
    """Same band, wrong slot or wrong modem: silence, as on the board — and
    the device must not count it as a damaged packet either."""
    node = alex(channel=M.Channel("MediumFast"))
    mesh.hear(node.text("wrong slot"), **M.tune_for("MediumFast"))
    lf = M.tune_for("LongFast")
    mesh.hear(alex().text("wrong modem"), **dict(lf, sf=9))
    mesh.drain()
    assert mesh.history() == []
    assert mesh.dev.lora()["mistuned"] == 2
    assert mesh.stats().rx_dropped == 0


# ---- hearing the mesh --------------------------------------------------------------------


def test_a_message_too_long_for_the_screen_is_cut_at_200_bytes_not_dropped(mesh):
    """Meshtastic's own limit is ~228 bytes; the device keeps 200. A longer
    one must still arrive, cut, rather than vanish."""
    text = ("Long ride report: " + "x" * 300)[:220]
    mesh.hear(alex().text(text))
    mesh.drain()
    got = mesh.history()
    assert len(got) == 1 and got[0].text == text[:200]


def test_the_node_list_shows_names_positions_and_when_each_was_last_heard(mesh):
    """The Nodes sheet and the node map are built from this: names from
    NodeInfo, a position only for nodes that sent one (and not for one at
    0,0 — that is "no fix"), the precision a node chose to blur to, and an
    age that grows until the node is heard again."""
    a, b = alex(), bea()
    c = M.VirtualNode(0x0C0C0C0C, "Cam", "CAM")
    mesh.hear(a.nodeinfo())
    mesh.hear(a.position(45.70531, -121.52147, alt=312, sats=9))
    mesh.hear(b.nodeinfo())
    mesh.hear(b.position(45.71, -121.50, precision_bits=13))
    mesh.hear(c.position(0.0, 0.0))
    mesh.drain()
    nodes = {n.num: n for n in mesh.nodes()}
    na = nodes[ALEX]
    assert na.HasField("position")
    assert na.position.lat_e7 == 457053100 and na.position.lon_e7 == -1215214700
    assert (na.position.alt_m, na.position.sats) == (312, 9)
    assert nodes[BEA].position.precision_bits == 13
    assert not nodes[0x0C0C0C0C].HasField("position"), "a 0,0 position is no fix, not a place"

    age1 = na.age_ms
    mesh.dev_sleep(3000)
    na2 = {n.num: n for n in mesh.nodes()}[ALEX]
    assert na2.age_ms >= age1 + 2500, "last heard must age with device time"
    assert na2.position.age_ms >= 2500
    mesh.hear(a.text("still here"))
    mesh.drain()
    na3 = {n.num: n for n in mesh.nodes()}[ALEX]
    assert na3.age_ms < 2000, "hearing a node again resets its age"
    assert na3.position.age_ms >= 2500, "a text is not a new position"


# ---- what is not ours --------------------------------------------------------------------


def test_a_channel_with_the_wrong_key_is_ignored_not_shown_as_garbage(mesh):
    """Two ways to hold the wrong key: a different key (the channel byte
    differs, so the frame is recognisably someone else's), and — 1 in 256
    for random keys — a different key with the SAME channel byte, which
    decrypts to noise. Neither may put a message, a phantom node or a
    position in front of the rider."""
    other = alex(channel=M.Channel("LongFast", b"\x02"))           # well-known key #2
    assert other.channel.hash != M.Channel().hash
    same_hash_key = bytearray(M.DEFAULT_PSK)
    same_hash_key[0] ^= 0x5A
    same_hash_key[1] ^= 0x5A                                       # xor unchanged
    clash = M.Channel("LongFast", bytes(same_hash_key))
    assert clash.hash == M.Channel().hash
    for i in range(10):
        mesh.deliver(other.text("private %d" % i, packet_id=0x100 + i))
    mesh.drain()
    spy = M.VirtualNode(0x5B15B15B, "Spy", "SPY", channel=clash)
    mesh.dev.set_speed(4)
    for i in range(60):
        frame = [spy.text("secret %d" % i, packet_id=0x200 + i), spy.nodeinfo(packet_id=0x300 + i),
                 spy.position(45.7, -121.5, packet_id=0x400 + i)][i % 3]
        mesh.deliver(frame)
    mesh.drain()
    mesh.dev.set_speed(1)
    assert mesh.history() == []
    assert mesh.nodes() == [], "a frame we could not decrypt created a node"
    s = mesh.stats()
    assert s.rx == 0 and s.rx_other_channel == 10 and s.rx_dropped == 60


def test_a_private_channel_whose_hash_collides_with_the_primary_is_still_heard(mesh):
    """The channel byte is an 8-bit hint, not an identity: two of the
    rider's channels can share one. The primary winning a tie is fine;
    the other channel's traffic must still be tried with its own key."""
    # A name whose hash, with a fresh 16-byte key, equals the primary's 0x08.
    key = bytearray(16)
    key[0] = M.xor_hash(b"Gravel Crew") ^ M.Channel().hash
    key = bytes(key)
    crew = M.Channel("Gravel Crew", key)
    assert crew.hash == M.Channel().hash
    mesh.set_private(1, "Gravel Crew", key)
    mesh.hear(alex(channel=crew).text("crew only"))
    mesh.drain()
    assert [(x.text, x.channel) for x in mesh.history()] == [("crew only", 1)]


# ---- flooding, duplicates and acks ----------------------------------------------------------


def test_a_flooded_message_is_shown_once_and_never_relayed_or_acked(mesh):
    """The mesh floods: a broadcast arrives once directly and again from each
    relay (hop_limit decremented, relay byte rewritten). The rider sees it
    once; the device, a leaf, never puts it back on the air — not as a
    rebroadcast and not as an ack even though it asked for one (acking a
    broadcast would make every listener transmit at once)."""
    a = alex()
    pid = 0xA11CE001
    original = a.text("Flat tire at mile 12", packet_id=pid, hop_limit=3, want_ack=True)
    mesh.hear(original)
    for hl, relay in ((2, 0x11), (1, 0x22), (2, 0x33)):
        f = bytearray(original)
        f[12] = (f[12] & ~7) | hl
        f[15] = relay
        mesh.hear(bytes(f))
    mesh.drain()
    mesh.dev_sleep(4000)
    texts = [x.text for x in mesh.history()]
    assert texts == ["Flat tire at mile 12"]
    assert mesh.stats().rx_duplicate == 3
    for h in mesh.sent(since=0):
        assert h["id"] != pid, "the device rebroadcast someone else's packet"
        assert not is_port(h, M.PORT_ROUTING), "the device acked a broadcast"


def test_a_direct_message_is_acked_once_with_a_reply_that_can_get_back(mesh):
    """A DM that wants an ack gets exactly one ROUTING ack, addressed to the
    sender, carrying its packet id, on its channel, with enough hops to get
    back (hops it used + 2, Meshtastic's getHopLimitForResponse). Copies of
    it from relays must not trigger more acks."""
    a = alex()
    mesh.hear(a.nodeinfo(with_key=False))
    mesh.drain()
    pid = 0xD1EC7001
    dm = a.text("you there?", dest=mesh.num, packet_id=pid, hop_limit=5, hop_start=6, want_ack=True)
    mesh.hear(dm)
    f = bytearray(dm)
    f[12] = (f[12] & ~7) | 4
    mesh.hear(bytes(f))
    mesh.drain()
    ack = mesh.wait_sent(lambda h: is_port(h, M.PORT_ROUTING), "an ack")
    assert ack["dest"] == ALEX and ack["data"]["request_id"] == pid
    assert M.decode_routing(ack["data"]["payload"]) == M.ROUTING_NONE
    assert ack["hop_limit"] == 3 and not ack["want_ack"]
    mesh.dev_sleep(4000)
    more = [h for h in mesh.sent() if is_port(h, M.PORT_ROUTING)]
    assert more == [], "a relayed copy of the DM was acked again"
    got = [x for x in mesh.history() if x.text == "you there?"]
    assert len(got) == 1 and got[0].to == mesh.num


def test_the_senders_own_resend_of_a_direct_message_is_acked_again(mesh):
    """A second copy of a DM straight from its sender — hop counts untouched —
    means our ack did not reach it, so it gets another. (A relay's copy does
    not: the acked-once test above.) The message is still shown once."""
    a = alex()
    mesh.hear(a.nodeinfo(with_key=False))
    mesh.drain()
    pid = 0xD1EC7002
    dm = a.text("still there?", dest=mesh.num, packet_id=pid, hop_limit=3, hop_start=3, want_ack=True)
    for what in ("the ack", "the ack again"):
        mesh.hear(dm)
        mesh.drain()
        ack = mesh.wait_sent(lambda h: is_port(h, M.PORT_ROUTING) and h["data"]["request_id"] == pid, what)
        assert ack["dest"] == ALEX
    assert len([x for x in mesh.history() if x.text == "still there?"]) == 1


def test_a_dm_to_a_node_without_its_key_goes_out_the_legacy_way_and_asks_for_the_key(mesh):
    """Before the device knows a node's public key it can only send the old
    channel-encrypted DM; it also asks that node for its NodeInfo (directly,
    want_response) so the next DM can be PKI-encrypted as current
    Meshtastic requires."""
    mesh.hear(alex().nodeinfo(with_key=False))
    mesh.drain()
    # At most one NodeInfo a minute, and the device has just announced itself.
    mesh.dev.set_speed(10)
    mesh.dev_sleep(61 * 1000)
    mesh.dev.set_speed(1)
    pid = mesh.send("hello Alex", dest=ALEX).mesh_queued.packet_id
    dm = mesh.wait_sent(lambda h: h["id"] == pid, "the DM")
    assert dm["channel"] == M.Channel().hash and not dm["pki"]
    ask = mesh.wait_sent(lambda h: is_port(h, M.PORT_NODEINFO) and h["dest"] == ALEX, "a NodeInfo request")
    assert ask["data"]["want_response"]


def test_a_pki_dm_from_a_node_whose_key_the_device_lost_asks_for_it_and_then_gets_through(mesh):
    """Neighbours' keys live in RAM, so after a reboot a peer's PKI DM can't
    be read. The device must say so (a PKI_UNKNOWN_PUBKEY NAK, only because
    the DM wanted an ack), show nothing garbled, and read the retry once
    the peer has answered with its NodeInfo."""
    a = alex()
    mesh.hear(a.text("can you read this?", dest=mesh.num, pki_to=mesh.public_key, want_ack=True,
                     packet_id=0x0BAD0001))
    mesh.hear(a.text("no ack wanted", dest=mesh.num, pki_to=mesh.public_key, packet_id=0x0BAD0002))
    mesh.drain()
    nak = mesh.wait_sent(lambda h: is_port(h, M.PORT_ROUTING), "a NAK")
    assert nak["dest"] == ALEX and nak["data"]["request_id"] == 0x0BAD0001
    assert M.decode_routing(nak["data"]["payload"]) == M.ROUTING_PKI_UNKNOWN_PUBKEY
    mesh.dev_sleep(3000)
    assert not [h for h in mesh.sent() if is_port(h, M.PORT_ROUTING)], "NAKed a DM that wanted no ack"
    assert mesh.history() == []
    assert mesh.stats().rx_dropped == 2

    mesh.deliver(a.nodeinfo(dest=mesh.num))
    mesh.hear(a.text("can you read this?", dest=mesh.num, pki_to=mesh.public_key, want_ack=True,
                     packet_id=0x0BAD0003))
    mesh.drain()
    assert [x.text for x in mesh.history()] == ["can you read this?"]


def test_a_node_cannot_take_over_another_nodes_identity_or_ours(mesh):
    """Keys are pinned on first sight, as NodeDB::updateUser does: a later
    NodeInfo for the same node with a different key is ignored whole (it
    can't rename the node either), and a NodeInfo claiming the DEVICE's own
    public key is never adopted. DMs keep going to the real key."""
    a = alex()
    mesh.hear(a.nodeinfo())
    mesh.drain()
    impostor = M.VirtualNode(ALEX, "Not Alex", "BAD", private_key=os.urandom(32))
    mesh.hear(impostor.nodeinfo())
    copycat = M.VirtualNode(0xC0C0C0C0, "Copycat", "CC")
    mesh.hear(copycat.nodeinfo(public_key=mesh.public_key))
    mesh.drain()
    nodes = {n.num: n for n in mesh.nodes()}
    assert nodes[ALEX].long_name == "Alex Rivera"
    log = "\n".join(mesh.dev.log())
    assert "changed its public key" in log and "advertises our own public key" in log
    pid = mesh.send("only for Alex", dest=ALEX).mesh_queued.packet_id
    dm = mesh.wait_sent(lambda h: h["id"] == pid, "the DM", ear=a, pki_from=mesh.public_key)
    assert dm["pki"] and dm["data"]["payload"] == b"only for Alex"
    assert impostor.decode(dm["frame"], pki_from=mesh.public_key)["data"] is None
    # A DM to the copycat cannot be PKI (we never took its "key").
    pid2 = mesh.send("hi copycat", dest=0xC0C0C0C0).mesh_queued.packet_id
    dm2 = mesh.wait_sent(lambda h: h["id"] == pid2, "the DM to the copycat")
    assert not dm2["pki"] and dm2["channel"] == M.Channel().hash


def test_nodeinfo_requests_are_answered_but_cannot_make_the_device_chatter(mesh):
    """How a node that just booted learns our name and key: it asks, we
    answer (to it, request_id = its packet id). But a busy mesh must not be
    able to make us transmit on demand: one answer per asker per 12 h to a
    broadcast ask, at most one reply every 10 min to broadcasts, and every
    60 s to a request addressed to us."""
    mesh.dev_sleep(500)
    mesh.wait_sent(lambda h: is_port(h, M.PORT_NODEINFO), "the boot announcement", timeout=15)
    mesh.dev.set_speed(200)
    # The announcement counts as our last NodeInfo: wait out the 10 min gap.
    mesh.dev_sleep(11 * 60 * 1000)
    mesh.dev.set_speed(1)
    a, b = alex(), bea()
    mesh.hear(a.nodeinfo(want_response=True, packet_id=0xAAAA0001))
    mesh.drain()
    r = mesh.wait_sent(lambda h: is_port(h, M.PORT_NODEINFO), "an answer to Alex")
    assert r["dest"] == ALEX and r["data"]["request_id"] == 0xAAAA0001
    mesh.hear(a.nodeinfo(want_response=True, packet_id=0xAAAA0002))
    mesh.hear(b.nodeinfo(want_response=True, packet_id=0xBBBB0001))
    mesh.drain()
    mesh.dev_sleep(5000)
    assert not [h for h in mesh.sent() if is_port(h, M.PORT_NODEINFO)], "answered inside the rate limit"
    # Bea asks us directly after a minute: answered.
    mesh.dev.set_speed(30)
    mesh.dev_sleep(61 * 1000)
    mesh.dev.set_speed(1)
    mesh.hear(b.nodeinfo(dest=mesh.num, want_response=True, packet_id=0xBBBB0002, want_ack=True))
    mesh.drain()
    r = mesh.wait_sent(lambda h: is_port(h, M.PORT_NODEINFO), "a direct answer to Bea")
    assert r["dest"] == BEA and r["data"]["request_id"] == 0xBBBB0002
    mesh.dev_sleep(3000)
    assert not [h for h in mesh.sent() if is_port(h, M.PORT_ROUTING)], \
        "the reply is the ack; a separate ack doubles the airtime"
    assert mesh.stats().rx_duplicate == 0


# ---- damaged and hostile frames -----------------------------------------------------------


def test_malformed_frames_neither_crash_the_device_nor_invent_nodes(mesh):
    """At the edge of range, in a noisy band, or from a buggy or hostile
    node, frames arrive that pass the radio's CRC but are nonsense. None may
    take the device down, create a phantom neighbour or a blank message —
    and the next good frame must still get through."""
    ch = M.Channel()
    a = alex()

    def enc(data, pid, sender=ALEX, **kw):
        return M.header(BROADCAST, sender, pid, channel=ch.hash, **kw) + ch.encrypt(sender, pid, data)

    long_name = "N" * 120
    frames = [
        b"\x01",                                                        # runt
        bytes(15),                                                      # one short of a header
        M.header(BROADCAST, ALEX, 1, channel=ch.hash),                  # header, no payload
        enc(M.data_msg(M.PORT_TEXT, b"from nobody"), 2, sender=0),      # sender 0
        enc(b"\x0a\xff\xff\xff\xff\x0f", 3),                            # length past the end
        enc(b"\x08", 4),                                                # truncated varint
        enc(b"\x08\x01\x12\x05ab", 5),                                  # payload cut short
        enc(b"\x0f\x01\x02\x03", 6),                                    # wire type 7
        enc(b"\x08" + b"\xff" * 12, 7),                                 # varint > 64 bits
        enc(M.data_msg(0, b"no port"), 8),                              # portnum 0
        enc(M.data_msg(M.PORT_NODEINFO, M.user_msg(ALEX, long_name, "S" * 30, public_key=b"\x01" * 31)), 10),
        enc(M.data_msg(M.PORT_NODEINFO, b"\x12\xff\x01"), 11),          # User truncated
        enc(M.data_msg(M.PORT_POSITION, M.position_msg(123.0, 500.0)), 12),   # out of range
        enc(M.data_msg(M.PORT_POSITION, b"\x0d\x01\x02"), 13),          # sfixed32 cut short
        enc(M.data_msg(M.PORT_ROUTING, b"\x18"), 14),                   # routing truncated
        enc(M.data_msg(M.PORT_TELEMETRY, os.urandom(40)), 15),          # an app we don't speak
        enc(os.urandom(200), 16),                                       # noise on our channel
        M.header(mesh.num, ALEX, 17, channel=0) + os.urandom(64),       # "PKI" noise for us
        M.header(mesh.num, ALEX, 18, channel=0) + os.urandom(5),        # PKI shorter than its overhead
        os.urandom(255),                                                # the longest frame, all noise
        M.header(BROADCAST, ALEX, 21, channel=ch.hash) + bytes(239),     # the longest, zeros
    ]
    for f in frames:
        mesh.hear(f)
    mesh.hear(a.text("damaged", packet_id=19), crc_error=True)
    mesh.drain(timeout=60)
    assert mesh.dev.proc.poll() is None, "the device died"
    assert mesh.dev.lora()["crc"] == 1
    hist = mesh.history()
    assert [x.text for x in hist] == [], "a malformed frame became a message: %s" % hist
    nodes = mesh.nodes()
    assert all(n.num in (ALEX,) for n in nodes), "phantom nodes: %s" % [hex(n.num) for n in nodes]
    for n in nodes:
        assert len(n.long_name) <= 39 and len(n.short_name) <= 7
        assert not n.HasField("position")
    mesh.hear(a.text("good frame after all that", packet_id=20))
    mesh.drain()
    assert [x.text for x in mesh.history()] == ["good frame after all that"]


# ---- the transmitter ------------------------------------------------------------------------


def turnaround(mesh):
    """For a failure message: interrupts fired, and how late (device ms) the
    receiver came back after our own frames — the number these tests are about."""
    r = mesh.dev.lora()
    return {k: r[k] for k in ("irqs", "turn_max_ms", "turn_last_ms", "tx")}


def test_back_to_back_packets_on_a_fast_preset_are_both_received(mesh):
    """Two riders answering at once on ShortFast: their packets follow each
    other on the channel within tens of milliseconds. Both messages must
    reach the rider."""
    mesh.command(preset=M.PRESET_ORDER.index("ShortFast"))
    mesh.dev_wait(lambda: mesh.dev.lora()["sf"] == 7 and mesh.dev.lora()["mode"] == "rx", "retune")
    sf = M.Channel("ShortFast")
    for i in range(4):
        mesh.hear(alex(channel=sf).text("alex %d" % i))
        mesh.hear(bea(channel=sf).text("bea %d" % i))
        mesh.drain()
    got = sorted(x.text for x in mesh.history())
    assert got == sorted(["alex %d" % i for i in range(4)] + ["bea %d" % i for i in range(4)]), \
        "lost to overrun: %d; radio: %s" % (mesh.dev.lora()["overrun"], turnaround(mesh))


def test_doba_starts_on_the_eu_868_band(monkeypatch):
    """DOBA: a fresh device is an EU node (MESH_REGION_DEFAULT "EU_868"): once
    the mesh is on it listens where a stock EU LongFast node does, 869.525 MHz."""
    monkeypatch.setenv("DOBA_MESH_REGION", "")
    dev = start_device()
    try:
        m = Mesh(dev, radio_on=False)
        m.radio(True, announce=False)
        assert dev.lora()["freq_mhz"] == pytest.approx(869.525, abs=1e-3)
        assert abs(m.state().freq_hz - 869525000) < 100
    finally:
        dev.stop()


def test_stealth_holds_every_transmission_and_sends_the_queue_when_it_ends(mesh):
    """DOBA stealth: nothing goes on the air — not a queued message, not a
    NodeInfo, not an ack — while the receiver keeps listening (LISTEN). What
    was queued waits, and goes out once stealth ends."""
    mesh.dev.lora(busy=True)                     # hold the outbox
    assert mesh.send("held in stealth").WhichOneof("body") == "mesh_queued"
    mesh.dev.console("stealth listen", 800)
    mesh.drain()                                 # a frame already on the air finishes
    mesh.sent()                                  # forget what went before
    mesh.dev.lora(busy=False)
    mesh.dev_sleep(8000)
    assert mesh.sent() == [], "the device transmitted in stealth"
    assert mesh.dev.lora()["mode"] != "sleep", "LISTEN must keep the receiver on"
    mesh.dev.console("stealth off", 800)
    mesh.dev_wait(lambda: any(is_port(h, M.PORT_TEXT) and h["data"]["payload"] == b"held in stealth"
                              for h in mesh.sent(since=0)), "the held message after stealth")
