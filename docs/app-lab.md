# App Lab

Describe an app and Gemini writes it; the Sticky checks it and runs it.
Apps → **App Lab**.

## Setup

1. Get a Gemini API key at [aistudio.google.com](https://aistudio.google.com).
2. Enter it on the Sticky (App Lab → App Lab settings → Gemini API key) or, easier,
   from a phone: open **File Transfer** on the device, browse to the address it
   shows and open **App Lab**. Saved keys are never shown again.
3. The model defaults to `gemini-3.5-flash`. Change it in the same places if Google
   retires it; any Gemini model id that answers `generateContent` works.

## Making an app

- **New app**: type a description on the on-screen or a Bluetooth keyboard.
- **Make the app sent from your phone**: long descriptions are easier to type on
  the phone page; the row appears in App Lab once one is saved.
- An app's menu offers **Open**, **Change it** (describe a change; Gemini rewrites
  the whole app) and **Delete**.

Each request connects Wi-Fi, sends the description to Gemini, then runs the
result headless in the sandbox: start, resize, ticks, a tap on every control and
row, a swipe and the page keys. Lua errors and drawing that breaks the canvas
rules (controls outside the canvas or overlapping, frame limits) go back to
Gemini for up to two fixes. The app is saved even if problems remain, so it can
still be tried and changed.

## The sandbox

Apps use the [CrossStore](https://github.com/CrossStore/CrossStore) Lua 5.4
runtime (`lib/CrossStoreLua`, `lib/CrossStoreRuntime`, MIT; Lua checked byte for
byte against the official 5.4.9 release). App Lab apps get **no permissions**:
no network, sound, notifications, file access, other apps' data or system routes.
They can draw, read buttons, touch and swipes, use the keyboard, keep up to 32
values of 2 KiB each, and read the clock and battery. Each callback is limited to
100,000 Lua instructions and the VM to 128 KiB (in PSRAM). The full API is the
system instruction in `src/activities/apps/applab/AppLabPrompt.h`.

## Files

| Path | Contents |
|---|---|
| `/.crosspoint/applab.json` | Model and the obfuscated key |
| `/.crosspoint/applab/<id>/main.lua` | The app |
| `/.crosspoint/applab/<id>/app.json` | Name and the requests that made it |
| `/.crosspoint/applab/<id>/data/` | The app's saved values |
| `/.crosspoint/applab/pending.txt` | The description sent from the phone |

## Privacy and security

- The request goes straight from the Sticky to `generativelanguage.googleapis.com`
  over HTTPS with ESP-IDF's HTTP client, which checks the certificate against the
  built-in root bundle and the hostname, so the key cannot be sent elsewhere.
  (The SDK's wolfSSL client used elsewhere cannot verify hostnames.)
- Only the description, the current app code (for changes) and sandbox problems
  are sent. Google's terms for the key's project apply to that data.
- Generated code never runs outside the sandbox, including during the check.
