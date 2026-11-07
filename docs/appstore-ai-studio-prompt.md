Build "Sticky Apps": a lean, security-focused app store for small ESP32 e-ink devices such as the Seeed reTerminal Sticky (800x480, mostly black and white, touch, 8 MB PSRAM). Apps are small sandboxed Lua 5.4 programs. People can vibe code an app with AI, or submit one written elsewhere from GitHub. Every app goes through automated scanning and human review before publication.

Work in the milestones below, in order. Finish one, tell me how to test it, and wait for me to say "continue". If something in this brief conflicts with what you can do in this environment, tell me instead of silently changing the design.

# Hard rules

1. COST: this must cost close to nothing when idle. Use Cloud Run with 0 minimum instances and request-based billing. Do not add: always-on servers, background workers, cron jobs, queues, Redis, SQL databases, Cloud Storage, or paid third-party services. All long work happens inside the HTTP request that asked for it. The only database is Firestore, and only for what is listed in this brief.
2. AI SPENDING CAPS: Gemini is only called server-side, using the model named in the env var GEMINI_MODEL (default to the cheapest current Flash-Lite model). Enforce with atomic Firestore counters: sign-in required, USER_DAILY_LIMIT (default 10) generations per user per day and GLOBAL_DAILY_LIMIT (default 100) in total per day, returning HTTP 429 with a friendly message when reached. Cap output tokens. Each generation makes at most 3 model calls (first attempt plus 2 repairs). The Gemini key never reaches the browser or a device.
3. THE CATALOG IS STATIC: published apps live as static files on GitHub Pages. Devices read those files directly and never depend on this website. The website only reads the catalog; it cannot modify it. All publishing goes through a GitHub pull request.
4. NO BINARIES: apps are source only. Reject Lua bytecode (files starting with bytes 1B 4C 75 61) and any compiled file. CI builds each package itself.
5. ALL APP CODE AND TEXT IS UNTRUSTED: this covers generated code, submitted code, and catalog text. Never run it on the server outside the isolated emulator described below. Render its text as plain text, never as HTML. When it is sent to Gemini, mark it as data and instruct the model to ignore any instructions inside it.
6. AI NEVER APPROVES: AI review can add warnings for a human. Nothing about it can pass a check, merge, or approve an app.
7. SECRETS: read every secret from environment variables, never commit one, never log one, and never put one in client code. The app must still start when an optional secret is missing, with that feature showing "not configured".
8. Keep it small: few dependencies, no user-uploaded files other than what is described here, no payments, reviews, comments, or analytics.

# Repository layout

One GitHub repo:
- /web: the full-stack site and API, deployed to Cloud Run.
- /catalog/packages/<id>/: source of published apps (manifest.json, main.lua, LICENSE, optional assets/).
- /catalog/submissions/<id>.json: pointers to externally hosted apps: {"repo":"owner/name","commit":"<full 40-character SHA>","path":"subfolder","owner":"<github login>"}.
- /catalog/owners.json: which GitHub login owns each app id.
- /catalog/revoked.json: app id and version pairs that must be disabled.
- /tools: scanner and catalog builder, shared with /web where practical.
- /.github/workflows: scan.yml and publish.yml.

# App format (must match exactly)

Package: an uncompressed ("stored") ZIP with the extension .csp, containing manifest.json, main.lua and LICENSE. Manifest at most 4 KiB, main.lua at most 32 KiB, whole package at most 1 MiB and 32 files. Optional resources are listed in "assets" and live under assets/.

manifest.json example:
{
  "schema": 1,
  "id": "pomodoro",
  "version": "0.1.0",
  "name": {"en": "Pomodoro", "zh": "番茄钟"},
  "description": {"en": "A focus timer", "zh": "专注计时器"},
  "license": "MIT",
  "runtime": "crossmux-lua54",
  "api": 4,
  "entry": "main.lua",
  "memory_bytes": 65536,
  "requires": [],
  "permissions": [],
  "status": "ready"
}

- id: lowercase letters, digits and underscores, starts with a letter, at most 31 characters.
- memory_bytes: 16384 to 131072. Above 65536 must list "psram" in requires.
- requires may only contain: wifi, lora, esp_now, tox, imu, touch, frontlight, rtc, psram, storage, buzzer, audio, radio_audio. No duplicates.
- permissions may only contain: network, radio, notifications, background, reading_data, wallpaper, sound, device_info, files_export, keep_awake.
- api: integer from 1 to 8.

Catalog output (static, built by CI):
- index.json, page-2.json, ...: {"schema":1,"api":1,"packages":[...],"next":"page-2.json","previous":"","page":1,"pages":2,"total":20}. At most 16 packages and 24 KiB per page. Each entry is the full manifest plus "artifact" (relative path), "bytes" and "sha256" (lowercase hex).
- root.json: {"schema":1,"sequence":<integer that increases on every publish>,"pages":[{"path":"index.json","sha256":"..."}],"revoked":[{"id":"...","version":"..."}]}.
- root.sig: signature over the exact bytes of root.json, ECDSA P-256 with SHA-256, DER encoded (the output of `openssl dgst -sha256 -sign`).

# Lua API reference (API version 8)

main.lua returns a table of callbacks:

return {
  on_start = function()
    app.clear()
    app.title(app.tr("Hello", "你好"))
    app.canvas()
    local width, height = app.size()
    app.text(0, 20, width, 60, app.tr("Welcome", "欢迎"), 2, 1)
    app.button(1, 0, height - 60, width, 52, app.tr("Close", "关闭"), 0)
  end,
  on_select = function(action, gesture, x, y)
    if action == 1 then app.exit() end
  end,
}

Callbacks: on_start(), on_resize(), on_tick() (about every 500 ms), on_select(action, gesture, x, y) (gesture is "long" for a long press; x and y are -1 when chosen with a physical button), on_input(_, text), on_swipe(_, direction), on_network(success), on_back(), on_stop() (API 4; save data here), on_key(_, direction) (API 8; "up" or "down"; return true only to consume the press). Canvas apps must handle on_resize and must never assume a screen size.

Functions:
- app.title(text), app.clear(), app.tr(en, zh), app.exit()
- app.row(label, value), app.grid(columns) (1 to 6 columns, at most 32 items), app.caption(text)
- app.has(capability), app.allowed(permission), app.millis()
- app.read(key), app.write(key, value): private text storage, at most 2 KiB per value and 32 keys
- app.asset(path, offset, length): read package assets, at most 2048 bytes per call
- app.input(title, initial): system keyboard, result arrives in on_input, at most 191 bytes
- app.notify(text) (needs notifications), app.sound(cue) (needs sound; cue is "woodfish" or "click")
- app.system("device.time") returns ok, "HH:MM", weekday, seconds, unix, "YYYY-MM-DD"; returns false if the clock is invalid
- app.system("device.battery") returns ok, text, percent

Canvas:
- app.canvas() enables it and app.size() returns width, height. Origin is the canvas top-left. Gray: 0 is black, 15 is white.
- app.box(x,y,w,h,gray=0,radius=0,stroke=0) (stroke 0 means filled)
- app.circle(x,y,w,h,gray=0,reserved=0,stroke=0)
- app.line(x,y,w,h,gray=0,reverse=0,stroke=1)
- app.text(x,y,w,h,text,font=1,align=0,gray=0) (font: 0 small, 1 body, 2 title, 3 large numbers; align: 0 left, 1 center, 2 right)
- app.measure(text,maxWidth,font=1) returns width, height
- app.button(id,x,y,w,h,label,style=0) (style: 0 outline, 1 flat, 2 light gray, 3 black)
- app.hit(id,x,y,w,h): invisible touch area
- app.qr(x,y,size,text): one per frame, at most 384 bytes

Limits per frame: 512 drawing commands, 8192 bytes of text, 24 interactive areas. Interactive ids must be unique. Areas must not overlap or leave the canvas. Adjacent controls need at least 6 px between them.

Sandbox: only the base library, table, string, math and utf8 exist. Removed and unavailable: io, os, package, debug, coroutine, require, dofile, loadfile, load, collectgarbage, pcall, xpcall, print, string.dump, string.find, string.match, string.gmatch, string.gsub. Generated code must not use any of these. Each callback may run at most 100000 Lua instructions and about 100 ms. Do not busy-wait; use on_tick. E-ink refreshes slowly, so avoid animation and redraw only when something changes.

# Emulator (shared by the browser and the scanner)

Write one small emulator that runs apps using Lua 5.4 compiled to WebAssembly (wasmoon or equivalent).
- It removes the same globals as the sandbox above and implements app.* against a recording of draw commands.
- It draws those commands on an 800x480 canvas in high contrast, and accepts clicks as on_select.
- In the browser it runs inside a Web Worker, so bad code cannot touch the page, and the worker is killed if a callback takes longer than 2 seconds.
- It produces a problems list: Lua errors, calls to unknown functions, frame limits exceeded, duplicate ids, overlapping or off-canvas areas, gaps under 6 px, text that does not fit (using app.measure's approximation).
- Headless mode: start the app, tap each interactive area once, send a few ticks and a resize to 480x800, then return the problems and a log of every app.* call.

# Milestone 1: Catalog and storefront

/tools/build_catalog: builds packages and the paged catalog from /catalog, deterministically (fixed ZIP timestamps).

Website (read-only, reads the catalog on the server with a 5-minute in-memory cache, CATALOG_URL env var):
- Browse: search and filter by hardware and permissions.
- App page: description, each permission explained in plain language, hardware needed, version, licence, size, SHA-256, source link, a "Run in browser" button using the emulator, and a "Report a problem" link that opens a GitHub issue.
- How to install: installs happen in the Store app on the device. HTTPS sites cannot reach devices on a local network, so do not build any "send to device" button or call any local network address.

Include one sample app in /catalog/packages to test with. Style: high contrast, sharp, paper-like, mobile first, fast, with no heavy UI framework beyond what the stack needs.

# Milestone 2: Scanner and CI

/tools/scan runs on one package and produces a Markdown report plus a pass or fail. Fail on:
1. Structure: path traversal, absolute paths, symlinks, undeclared or oversized files, invalid manifest, bytecode or binary files, missing LICENSE.
2. Static analysis on a real Lua parser: any removed name from the sandbox list, _G or _ENV access, getmetatable or setmetatable used on anything but the app's own tables, unknown app.* functions, unknown app.system commands, permission-gated calls without the permission declared.
3. Behaviour: headless emulator run; fail on Lua errors or timeouts.
4. Secrets: API key and token patterns.
5. Ownership: the id is new, or the author matches /catalog/owners.json. New versions must have a higher version number.

Warn, without failing, on: embedded URLs and QR codes, text asking for passwords, codes, or payment, permissions requested but apparently unused, any permission added since the previous version, and names that closely imitate an existing app.

AI review: if GEMINI_API_KEY is available, add an advisory risk summary to the report, with structured JSON output. It cannot change pass or fail. Skip it without failing if the key is absent or the daily cap is reached.

scan.yml: triggered by `pull_request` only. NEVER use `pull_request_target`. Use `permissions: contents: read`, no repository secrets, pinned action versions (full commit SHAs), and a time limit. For a submission pointer it fetches that exact commit from the public repo into a temp folder, treats it purely as data, and never installs or runs its dependencies or scripts. Post the report as the job summary and as an artifact. A separate workflow triggered by `workflow_run` may post the report as a PR comment; it must not check out or execute PR code.

publish.yml: triggered on push to main. Builds the catalog, increments sequence (read the current value from the live root.json), signs root.json in a job using `environment: signing` with the key from the CATALOG_SIGNING_KEY secret, then deploys to GitHub Pages. If signing fails, nothing is deployed. Nothing from a PR ever runs in this workflow.

Write /catalog/README.md explaining how a human should review a submission.

# Milestone 3: Submit from GitHub

Sign-in with GitHub through a GitHub App (env vars GITHUB_APP_ID, GITHUB_APP_PRIVATE_KEY, GITHUB_CLIENT_ID, GITHUB_CLIENT_SECRET, SESSION_SECRET). Use a signed, HttpOnly, Secure, SameSite=Lax session cookie holding only the login. Do not store user tokens. Protect all state-changing requests against CSRF.

"Submit an app" form: public GitHub repo URL, tag, optional subfolder. The server:
1. Checks the repo is public and its owner equals the signed-in login. Organization repos and private repos are refused for now, with a clear message.
2. Resolves the tag to a full commit SHA. The SHA is what gets reviewed, so later changes to the tag have no effect.
3. Fetches manifest.json and main.lua through the GitHub API, within the size limits, and runs the scanner for instant feedback.
4. If it passes, opens a PR adding /catalog/submissions/<id>.json. Only allow URLs on github.com. Never fetch a user-supplied URL directly, to prevent server-side request forgery.
5. Shows the PR link and the report.

The GitHub App's token is used only to create submission branches and PRs. Merging is always done by a human on GitHub.

# Milestone 4: Vibe coding on the web

"Create an app" page: a chat on the left and the emulator on the right (stacked on mobile).
- POST /api/drafts takes a description, and optionally an existing draft id to refine. The server calls Gemini with a system prompt containing the full API reference and rules above, and structured JSON output: {name_en, name_zh, description_en, description_zh, main_lua}.
- The server forces id (from the name, made unique), runtime, entry, license "MIT", api 4 and memory_bytes 65536, with requires and permissions empty. The model cannot choose these.
- Validation runs in the same request: scanner static checks plus the headless emulator in an isolated worker with a time limit. If there are problems, send them back to the model to fix, at most twice. Show the remaining problems honestly.
- The user sees the app running, can keep chatting to refine, and each step becomes a new draft version.
- Show generations left today.
- "Publish" scans the draft, then opens a PR adding /catalog/packages/<id>/, with the author's login in the PR and in owners.json. The user confirms they accept the MIT licence first. Tell them a human reviews every app.

Firestore, the only collections allowed:
- drafts: id, owner, version, manifest, main_lua, problems, createdAt, expiresAt (7 days out; a TTL policy deletes these).
- usage: daily counters per user and global, with an expiresAt field.
- devices (used in Milestone 5).

# Milestone 5: Device API for vibe coding on the Sticky

The device firmware will be written separately, against this exact contract. The device talks to the server over HTTPS and always initiates. All requests after pairing send `Authorization: Bearer <device token>`.

Pairing (device token stored only as a SHA-256 hash; a code expires after 10 minutes and allows 5 attempts):
- POST /api/device/pair: {"name":"Sticky"} returns {"code":"7 characters, no ambiguous letters","poll_secret":"...","expires_in":600}.
- The user opens /pair on the website, signed in, types the code, and confirms.
- GET /api/device/pair/<poll_secret> returns {"status":"pending"} or, once only, {"status":"paired","token":"...","device_id":"..."}.
- An account page lists paired devices and lets the user revoke them.

Drafts:
- POST /api/device/drafts: {"prompt":"...","draft_id":"optional, to refine","screen":{"width":800,"height":480},"capabilities":["touch","buzzer"],"api":8}. This is synchronous (up to 90 seconds) and uses the same generation, validation and spending caps as the web. Returns {"id","version","name":{"en","zh"},"problems":[...],"package_url","bytes","sha256","draft_token"}. Responses must stay under 4 KiB, because the device has little memory.
- GET package_url returns the .csp. Authenticated.
- POST /api/device/drafts/<id>/publish: {"version":n} returns {"pr_url":"...","report":"short summary"}, or a clear reason it cannot be published. It uses the paired account as author and needs that account's previous MIT acceptance; otherwise it returns a message asking the user to finish on the website.

draft_token: a compact signature using ECDSA P-256 with SHA-256 from env var DRAFT_SIGNING_KEY, over the UTF-8 string "draft|<device_id>|<id>|<version>|<sha256>|<expires unix time>", returned as {"expires":<unix time>,"sig":"base64 of DER"}. The expiry is 7 days out. Drafts must always have empty permissions, because the device will refuse any draft with permissions. DRAFT_SIGNING_KEY must be a different key from the catalog signing key, and the catalog signing key must never be available to this service.

Rate limit pairing attempts per IP using a lightweight check. Document all of this in /web/DEVICE_API.md, with curl examples, so I can hand it to the firmware developer. Also build a /dev/device-simulator page, enabled only when DEV_TOOLS=true, that runs the same flow in the browser with the emulator.

# At the end

Give me:
1. A list of every environment variable and secret: where each one goes and which milestone needs it.
2. The exact Cloud Run settings to use: minimum instances 0, maximum instances 2, request-based billing, memory and CPU as low as works, and request timeout 120 seconds.
3. Anything in this brief you could not do in this environment.
