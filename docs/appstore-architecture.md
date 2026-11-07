# App store architecture

Status: proposal, 2026-11-06. Nothing here is built yet. The prompt for Google AI
Studio is in [appstore-ai-studio-prompt.md](appstore-ai-studio-prompt.md).

## Summary

Reuse the CrossStore app format (sandboxed Lua plus a static JSON catalog), run your
own signed catalog, and put a small website in front of it. Everyone gets three ways
to make an app, and all of them end in the same scanner and review:

| Source | Where it starts | Path |
|---|---|---|
| Vibe coded on the web | Storefront, with an in-browser emulator | Draft → scan → review → catalog |
| Vibe coded on the Sticky | App Lab on the device, by voice or text | Draft → test on device → scan → review → catalog |
| Built elsewhere | A public GitHub repo at a tag | Pinned commit → scan → review → catalog |

The system has three separated parts:

1. **Catalog:** static signed files on GitHub Pages. This is the only part devices
   need for browsing and installing.
2. **Device store:** firmware that checks signatures, installs and runs apps.
3. **Website and API:** built in AI Studio, hosted on Cloud Run. It never holds the
   catalog signing key, so hacking it can't push an app to every device.

## What already exists

| Option | How apps run | Status | Fit |
|---|---|---|---|
| Built-in C++ apps (your fork today) | Compiled into the firmware | Working, but every new app needs a reflash | Not an app store |
| [Upstream SD plugins](https://github.com/0x1abin/crossmux/blob/main/docs/sd-plugins.md) | No code on the device: browser JS, catalog screens, events | In upstream, but not in your fork (476 commits behind) | Good for connectors, can't make a real on-device app |
| [CrossStore](https://github.com/CrossStore/CrossStore) | Sandboxed Lua 5.4 on the device | Experimental, MIT, one maintainer | **Best fit** |

The runtime is on the `codex/crossstore-apps` branch of
[zhuzhe1983/crossmux](https://github.com/zhuzhe1983/crossmux/tree/codex/crossstore-apps),
which has a `sticky_store` build target.

**Three risks to accept knowingly:**

- That branch's [device matrix](https://github.com/zhuzhe1983/crossmux/blob/codex/crossstore-apps/docs/engineering/crossstore-device-matrix.md)
  marks the Sticky **Alpha, not physically tested**.
- Its [API docs](https://github.com/CrossStore/CrossStore/blob/main/docs/lua-api.md)
  call the sandbox a development-preview fault isolation measure that **hasn't had a
  security audit**. Keep the store private or invite-only until that is resolved.
- The API went through 8 versions quickly, so expect apps to need updates.

## Security

Scanning can't prove an app is safe, so it is one layer of several.

**Layer 1: the sandbox is the real boundary.** Lua apps have no `io`, `os`, `package`,
`debug` or `coroutine`, no `load`, `pcall` or `string.dump`, no sockets and no arbitrary
file access. They are limited to 100,000 instructions per callback and a 16-128 KiB
memory budget. Never expose `HermesStore` or any key to Lua.

**Layer 2: source only.** The format forbids compiled bytecode, and CI builds every
package itself from pinned source. Nobody can upload a binary, which removes a whole
class of hidden code. "Built outside" therefore means written outside, not compiled.

**Layer 3: the scanner**, running on every pull request:

1. Structure: safe paths, size limits, bytecode rejected, manifest valid.
2. Static analysis: banned names, unknown `app.*` calls, permissions used but not declared.
3. Behaviour: run in a headless emulator with scripted taps, and record what it does.
4. Content: leaked secrets, embedded URLs, QR codes, text that asks for passwords.
5. AI review: Gemini writes a risk report. **It can raise flags but never approve**,
   because a comment in the code could try to talk it into approving.

**Layer 4: a human merges.** Every first publish and every permission increase needs
your approval.

**Layer 5: signing and revocation.**

- CI signs one root file, which covers each catalog page's hash. Each page already
  covers each package's hash. Devices verify with a key pinned in firmware.
- A `sequence` number only goes up, so an old catalog can't be replayed.
- A signed `revoked` list lets you switch off a bad app on every device.
- CrossStore currently has no signatures and doesn't validate HTTPS certificates on
  downloads, so both must be added to the firmware.
- ECDSA P-256 is specified because mbedTLS supports it. I'm not sure it supports Ed25519.

**CI rules that matter:** the scan runs on `pull_request` with no secrets, never
`pull_request_target`. Signing runs only after merge, in a protected environment that
requires your approval.

## Vibe coding on the Sticky (App Lab)

This follows what worked on Hako: a chat designer that produces drafts, a real preview,
and layout problems sent back to the AI to fix.

1. **Pair once.** The Sticky shows a short code, and you enter it on the website after
   signing in. The device gets its own revocable token, not a GitHub or Gemini key.
2. **Describe it.** Hold the AI button and talk, using the speech-to-text the Hermes app
   already has, or type. The transcript goes to the API, and Gemini writes the app.
3. **Check it.** The server validates and runs the draft in the emulator, sending
   problems back to Gemini up to twice. It answers in the same request, because Cloud
   Run doesn't reliably run work after a response and there's no job queue to pay for.
4. **Try it on the device.** The draft installs to a separate drafts area and opens.
5. **Refine it.** "Make the numbers bigger" produces a new version of the same draft.
6. **Publish it.** Tap Publish, and the draft enters the normal scan and review.

**Why testing a draft is safe:** drafts run with **no permissions at all**, and they use
a second signing key, tied to one device and with an expiry. That key lives on Cloud
Run, so if it leaks, an attacker can only get a permission-less draft onto a device
that is already paired, never into the catalog.

The firmware side is new work: App Lab doesn't exist yet, and the prompt only builds
the server half. Until then, the web emulator can be used to test the same flow.

## Keeping it cheap

No component should cost anything while idle:

| Part | Choice | Cost when idle |
|---|---|---|
| Catalog and packages | GitHub Pages | Free |
| Scanner, build, signing | GitHub Actions, public repo | Free |
| Website and API | Cloud Run: 0 minimum instances, 2 maximum, request-based billing | None |
| Data | Firestore free tier, with automatic expiry of drafts after 7 days | None |
| App preview | Runs in the visitor's browser (Lua compiled to WebAssembly) | None |
| AI | Cheapest current Flash-Lite model, only on request | None |

Gemini is the one cost that scales with use, so it has hard limits: sign-in required, 10
generations per user per day, 100 in total per day, and capped output length.
**Google Cloud budget alerts only notify you and don't stop spending**, so the app
enforces its own daily cap. Check current Cloud Run, Firestore and Gemini pricing when
you set this up. I haven't measured the real cost per generated app.

## Things that shaped the design

- **The website can't install directly to a device.** It's HTTPS and the device serves
  plain HTTP on the LAN, so browsers block the call. The device always pulls.
- **One repo:** `/web` for Cloud Run and `/catalog` for Pages. The GitHub App the website
  uses can only open pull requests, and branch protection stops it merging.
- **Outside apps must be public repos,** so their source can be audited. Ownership is
  checked by the repo's owner matching the signed-in GitHub user, with no user tokens stored.
- **Importing CrossStore's apps** is possible later, after checking each app's licence.

## Build order

1. **Prove the runtime on the Sticky.** Flash `sticky_store` and try a few apps. If it
   isn't stable on this hardware, stop and rethink before spending more. The website can
   start first, but nothing it publishes will run until this works.
2. **Website, catalog and scanner** with the prompt, milestones 1 to 4.
3. **Merge firmware.** Move your Hermes work onto the runtime branch, then add signature
   checks and certificate validation.
4. **App Lab firmware,** against the API from milestone 5.
5. **Security review of the sandbox,** then consider opening the store to others.

## Setup only you can do

AI Studio can't do these. They take about 30 minutes, once milestone 1 exists:

1. Create a GitHub App with read and write access to contents and pull requests, plus
   user sign-in. Install it on the store repo only.
2. Protect `main`: require a pull request, the `scan` check, and your review.
3. Enable GitHub Pages from Actions, and create a `signing` environment that requires your
   approval.
4. Generate the signing key and save it as a secret in that environment only:
   ```bash
   openssl ecparam -name prime256v1 -genkey -noout -out catalog-signing.pem
   ```
   ```bash
   openssl ec -in catalog-signing.pem -pubout -out catalog-signing.pub
   ```
   Keep a second offline key for rotation. The `.pub` file goes into the firmware.
5. In Google Cloud: enable Firestore, add a TTL policy on the drafts' `expiresAt`, set
   Cloud Run's maximum instances to 2, and add a budget alert.

## Sources

- [CrossStore README](https://github.com/CrossStore/CrossStore), [package protocol](https://github.com/CrossStore/CrossStore/blob/main/docs/protocol.md), [Lua API](https://github.com/CrossStore/CrossStore/blob/main/docs/lua-api.md)
- [Runtime branch](https://github.com/zhuzhe1983/crossmux/tree/codex/crossstore-apps), at commit `8000d1b4`
- [Upstream SD plugins](https://github.com/0x1abin/crossmux/blob/main/docs/sd-plugins.md)
- [AI Studio Build mode](https://ai.google.dev/gemini-api/docs/aistudio-build-mode)
