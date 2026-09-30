#pragma once

#include <cstdint>

// BLE peripheral that presents the device to an Enhance Auto S3XY Commander
// as one S3XY Button ("ENH_BTN"). Protocol from Beat-YT/s3xy-virtual-button
// (MIT, reverse-engineered from real button <-> Commander traffic; tested
// against the Gen 2 Commander), ported from Bluedroid to NimBLE:
//
//   Service 00003d46-87d2-479e-7e45-8551415a6de1
//     00003d50-... READ | NOTIFY      button events to the Commander
//     00003d49-... READ/WRITE (encrypted) 10-byte ID + Commander commands:
//       B6 -> reply C7 00 01 (init), A1 -> unpair (disconnect),
//       A4 xx yy zz -> rename, reply A4 00 xx yy
//   Events: single = 01, 00, C1 01, 00; double = C1 02; long = C3 01.
//   Pairing: LE Secure Connections "Just Works" with bonding.
//
// The link owns the NimBLE stack while it runs, so the BLE keyboard host must
// be stopped first (the S3XY app does not keep Bluetooth alive for it).
namespace s3xy {

enum class LinkState : uint8_t { Off, Advertising, Connected, Ready };
enum class Press : uint8_t { Single, Double, Long };

// Starts advertising as a button. Returns false when BLE is unavailable in
// this build or the stack failed to start.
bool begin();
void end();
LinkState state();
// Sends one press; false when the Commander is not connected and subscribed.
bool send(Press press);
// Monotonic counter bumped by every Commander command (init, rename, unpair),
// so the UI can refresh without polling BLE state.
uint32_t commanderEvents();
bool available();

}  // namespace s3xy
