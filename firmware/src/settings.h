#pragma once

// NVS-backed persistent settings: rider config + paired sensor addresses.
// config.h values are the first-boot defaults.

#include <cstddef>
#include <cstdint>

namespace settings {

void begin();

// Structured workout: hold at every block boundary until resumed.
bool panelScan();
void setPanelScan(bool on);

int tzMinutes();            // DOBA: from the GPS position when tzAuto()
void setTzMinutes(int m);   // the manual offset
int tzManualMinutes();
bool tzAuto();
void setTzAuto(bool on);
// DOBA: the mesh region (EU_868 / UA_868) from where the device is.
bool regionAuto();
void setRegionAuto(bool on);

int backlight();          // 0=off .. 3=bright
void setBacklight(int b);

bool useMiles();          // false = metric (km), true = imperial (mi/ft/mph)
void setUseMiles(bool m);

bool clock24h();          // true = 24-hour clock, false = 12-hour (AM/PM)
void setClock24h(bool h);

bool usbDrive();          // true = expose SD as a USB drive when plugged in
void setUsbDrive(bool on);
// DOBA: both radios default OFF. Bluetooth is brought up only at boot, so a
// change takes effect after a restart; WiFi is never started unless allowed.
// DOBA: the owner card on the power-off screen ("" = none).
// Bluetooth and Wi-Fi: switches kept for features to come (nothing uses
// either radio yet). Off by default.
bool bluetooth();
void setBluetooth(bool on);
bool wifi();
void setWifi(bool on);
const char* owner();
// The card as fields: 0 name, 1 born, 2 blood group, 3 allergies/illnesses, 4 contact.
constexpr int OWNER_FIELDS = 5;
const char* ownerField(int i);
void setOwnerField(int i, const char* t);
bool ownerSet();
void setOwner(const char* t);



// Last known GPS position (map center across reboots). Returns false if
// no position has ever been saved.
bool lastPosition(double& lat, double& lon);
void setLastPosition(double lat, double lon);

// Whether GPS has ever written UTC to the coin-cell RTC on this device. Until
// it has, the RTC may hold factory/local time (seen 8 h off UTC), so we must
// NOT seed GPS time-aiding from it — a grossly wrong time slows acquisition.
bool rtcTrusted();
void setRtcTrusted(bool ok);

// --- Meshtastic mesh messaging (see mesh_service.h) ------------------------

bool meshEnabled();               // false = the LoRa radio is never powered up
void setMeshEnabled(bool on);

// Channel name. Feeds both the channel hash and — via a second hash — the
// frequency slot, so two devices must agree on it exactly to hear each other.
const char* meshChannel();
// Which of Meshtastic's ten well-known channel keys, 1-10. 1 is the default
// channel's ("AQ==").
uint8_t meshChannelKey();
void setMeshChannel(const char* name, uint8_t keyIndex);

// The private channels, as meshtastic.ChannelSet bytes — the same encoding a
// share QR carries, so a channel has one representation rather than two.
// Returns the length written, 0 if none stored.
size_t meshPrivateChannels(uint8_t* out, size_t cap);
void setMeshPrivateChannels(const uint8_t* data, size_t len);

// Which channels we broadcast our own position on, as a bitmask over channel
// slots. 0 (the default) means we tell the mesh nothing about where we are.
uint8_t meshPositionChannels();
void setMeshPositionChannels(uint8_t mask);

// Modem preset: an index into mesh::kPresets, deciding how fast the radio talks.
// Separate from the channel, which decides where. MESH_PRESET_DEFAULT until set.
uint8_t meshPreset();
void setMeshPreset(uint8_t index);

// LoRa region: an index into mesh::kRegions. Persisted by NAME so the table can
// grow without moving an existing rider's band; an unknown stored name falls
// back to MESH_REGION_DEFAULT.
uint8_t meshRegion();
void setMeshRegion(uint8_t index);

// Transmit power in dBm. 0 (the default) means the most the region and the
// radio allow; mesh_service resolves that against the region's limit.
int8_t meshTxPower();
void setMeshTxPower(int8_t dbm);

// Frequency slot override, 1-based like Meshtastic's channel_num. 0 (the
// default) means the slot is derived from the channel name, which is what keeps
// two nodes that only agreed on a name on the same frequency.
uint8_t meshFreqSlot();
void setMeshFreqSlot(uint8_t slot);

// How this node introduces itself on the mesh. Empty until first set, at which
// point mesh_service derives a default from the node number.
const char* meshLongName();
const char* meshShortName();      // up to 4 glyphs, as Meshtastic apps show it
void setMeshNames(const char* longName, const char* shortName);

// This node's Curve25519 private key for Meshtastic PKI, generated once and then
// kept for good: stock nodes pin the first public key they hear for a node and
// drop every later NodeInfo that carries a different one, so a key that changed
// would make this device unreachable by DM until each peer forgot it by hand.
// False when none is stored yet.
bool meshPrivateKey(uint8_t out[32]);
void setMeshPrivateKey(const uint8_t key[32]);

// Compass mounting yaw, in degrees, learned from GPS course while riding (see
// aux_math::HeadingOffset). Persisted because the board does not move between
// rides, so re-learning it every time would leave the compass wrong for the
// first minutes of every ride. NAN when nothing has been learned yet.
// Cycling map layers the rider wants drawn (MAP_LAYER_* bits, map_view.h).
// All on by default. Applied at render time only — tiles keep everything.
uint16_t mapLayers();
void setMapLayers(uint16_t mask);

float compassOffsetDeg();
void setCompassOffsetDeg(float deg);

}
