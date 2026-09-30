# Hermes chat app

Chat with a [Hermes Agent](https://hermes-agent.nousresearch.com/) from the
Apps menu. Type on the on-screen keyboard or a paired BLE keyboard, or (on the
reTerminal Sticky) talk with the AI button.

## 1. Enable the Hermes API server

The app talks to Hermes' OpenAI-compatible API server, not to its webhooks
(webhooks deliver replies to other platforms, not back to the caller). On the
machine running Hermes, add to `~/.hermes/.env`:

```
API_SERVER_ENABLED=true
API_SERVER_KEY=<a long random secret>
API_SERVER_HOST=0.0.0.0   # listen on the LAN, not just localhost
API_SERVER_PORT=8642
```

Restart `hermes gateway` and allow TCP 8642 through that machine's firewall
for your LAN only. Anyone on the network who has the key can drive the agent
and its tools, so treat the key like a password.

## 2. Configure the app (Apps → Hermes → Setup)

| Setting | Default | Notes |
|---|---|---|
| Hermes URL | — | Base URL such as `http://<hermes-host>:8642`; `/v1/...` is appended. A full endpoint URL is used as-is. |
| API key | — | `API_SERVER_KEY`, sent as `Authorization: Bearer`. |
| Model / profile | `hermes-agent` | Profile name for multi-profile setups. |
| Conversation | `sticky` | Server-side conversation name; Hermes keeps the history. |
| API style | Responses | `POST /v1/responses` with `conversation`. Chat Completions sends `X-Hermes-Session-Id` instead. |
| Reply timeout | 180 s | Agent turns that run tools can take a while. |
| Speech-to-text URL | — | Full URL of an OpenAI-compatible `/v1/audio/transcriptions` endpoint. Empty disables voice. |
| Speech-to-text key | — | Optional Bearer token. |
| Speech-to-text model | `whisper-1` | Sent as the `model` form field. |
| Speech language | Auto | Optional ISO code (`en`, `nl`, ...). |
| Send voice immediately | On | Off puts the transcript in the draft line for editing first. |
| Max recording | 30 s | 15 / 30 / 60 / 120 s (16 kHz mono, ~32 KB/s in PSRAM). |
| Start new conversation | — | Switches to `<name>-N` and clears the on-device transcript. |

Settings live in `/.crosspoint/hermes.json` (keys obfuscated with the device
MAC, like KOReader/OPDS). The last 24 messages are kept in
`/.crosspoint/hermes_chat.json` for display only; Hermes owns the real history.

## 3. Speech-to-text contract

Voice clips are uploaded as `multipart/form-data`:

- `file`: `sticky.wav`, 16-bit mono PCM WAV at 16 kHz (DC-removed, normalized)
- `model`: the configured model (omitted if empty)
- `language`: the configured language (omitted if empty)
- `response_format`: `json`

The server must answer `{"text": "..."}` or a plain-text body. Any
OpenAI-compatible Whisper server works (e.g. faster-whisper-server /
speaches, LocalAI, whisper.cpp's server with an OpenAI-compatible route, or
OpenAI itself).

## AI button

On the Sticky the AI key is both OK and power. Settings → Controls:

| Setting | Options (default first) |
|---|---|
| AI button: click | **Lock screen**, Confirm, Sleep |
| AI button: hold | **Talk to Hermes**, Sleep |

| Gesture | Action |
|---|---|
| Hold ~1 s (anywhere) | Opens Hermes over the current screen and listens. High beep = speak now. |
| Release | Low beep; the clip is transcribed and sent (or put in the draft if "Send voice immediately" is off). |
| Click | Locks the screen (or Confirm / Sleep, per setting). A click never records. |
| Both page buttons ~1 s | Sleep. |
| Power+Down | Screenshot. |

Back in a chat opened by the hold returns to the screen you were on. The press
that wakes the device never records or locks.

## Lock screen

Settings → Controls → **Screen lock PIN** sets, changes or removes a 4-digit
PIN (current PIN first, then the new one twice; "Remove" on the new-PIN pad
clears it). With a PIN, the lock screen is a PIN pad and **Lock when waking**
(on by default) locks again after sleep. Without a PIN it is a tap-to-unlock
screen. While locked, tabs, the home swipe and push-to-talk are disabled; the
page-button sleep combo still works.

Five wrong PINs start a lockout of 30 s, doubling per further miss up to
15 min (a reboot restarts, not skips, the wait). The PIN is stored as
SHA-256(random salt + PIN) in internal flash (NVS), not on the SD card. It is a
privacy lock, not encryption: books and settings on the SD card stay readable
if the card is removed.

## Books from Hermes

When a Hermes reply contains a direct `http(s)` link to an `.epub`, `.txt`,
`.md`, `.xtc` or `.xtch` file, the app downloads it (up to 3 per reply) into
`/Books/Hermes/` on the SD card, then shows a bold, underlined
"Saved to Books/Hermes: …" line. Tap it to open the book. The book also appears
in Library. Existing files are never overwritten (`Title (2).epub`). Turn this
off with Setup → "Download book links".

The link only has to be reachable from the Sticky's Wi-Fi. The simplest setup
on the Hermes machine is a folder served over HTTP:

```bash
mkdir -p ~/sticky-books && cd ~/sticky-books && python3 -m http.server 8765
```

Then give Hermes a standing instruction (e.g. add it to its memory or SOUL.md):

> When I ask you to send a book to my Sticky, get the EPUB (convert other
> formats to EPUB with Calibre's `ebook-convert` if needed), save it into
> `~/sticky-books/` with a readable file name like `Author - Title.epub`, and
> reply with the direct link `http://<this-machine's-LAN-address>:8765/<file name>`
> on its own line. Percent-encode spaces in the link.

A public direct link (e.g. Project Gutenberg's `.epub` URLs) works the same way.

## Controls in the chat

| Input | Action |
|---|---|
| Talk / Stop / Cancel (touch) | Start / stop recording; cancels a pending request or download while busy. |
| Type button or tapping the draft line | On-screen keyboard; Done sends. |
| BLE keyboard | Types into the draft line. Enter sends, Backspace deletes, Esc clears or cancels, arrows / PgUp / PgDn scroll. |
| Side buttons, swipes | Scroll the transcript. |
| Back | Cancel the current request, or leave the app. |

## Radios

CrossMux never runs Wi-Fi and BLE at the same time. With Bluetooth enabled
(Settings → Bluetooth), the app connects Wi-Fi for each request and drops it
afterwards so the keyboard reconnects; expect a few seconds of radio
switching per message. With Bluetooth off, Wi-Fi stays connected while the app
is open. The app uses the last saved Wi-Fi network, or opens the Wi-Fi picker
if none is saved.

## Verifying on hardware

1. Build and flash: `pio run -e sticky -t upload`, then watch the serial log
   (`python3 scripts/debugging_monitor.py`) for `[HERMES]` lines — each request
   logs `job <kind> done: http=<status> err=<code>`.
2. Send a message; the header shows "Hermes is thinking… N s" until the reply.
3. Press the AI button, speak, press again. The PDM mic pins (GPIO19/20/38)
   come from the Sticky schematic and are not yet confirmed by a vendor demo;
   if transcripts come back empty, check `recorded N samples` and any
   `mic error` line in the log first.
