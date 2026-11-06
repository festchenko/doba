#pragma once
// DOBA: stealth. Nothing that can give the device away: no LoRa transmission
// (beacons, acks, replies, queued messages wait), no Bluetooth advertising or
// scanning, no frontlight. GPS keeps working — it only listens. With "listen"
// on, the LoRa radio keeps receiving (receiving emits nothing); off, it sleeps
// too, which also saves the most battery.
//
// Services ask active()/transmitAllowed() at the point where they would emit;
// set() also tears down what is already running (BLE links, the radio).

namespace stealth {

bool active();
bool listen();                 // stealth with the LoRa receiver still on
void set(bool on);             // persisted; logs "[stealth] on|off"
void setListen(bool on);
bool transmitAllowed();        // false while stealth is on

}  // namespace stealth
