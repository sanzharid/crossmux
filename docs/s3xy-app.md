# S3XY app (reTerminal Sticky)

Turns the Sticky into one **S3XY Button** for an Enhance Auto S3XY Commander
(tested protocol: Gen 2). While the app is open, the Sticky advertises over
Bluetooth as `ENH_BTN`, exactly like a real button.

## Pairing

1. Open **Apps → S3XY** on the Sticky and leave it on that screen.
2. In the S3XY (Enhance) phone app, add a new button as you would a physical
   one. The Commander finds `ENH_BTN` and bonds (no PIN).
3. The Sticky shows "Connected to the Commander". Assign car actions to the
   button's **single**, **double** and **long** press in the S3XY app.

The bond is remembered; next time the app is opened the Commander reconnects
by itself. Removing the button in the S3XY app unpairs it.

## Using it

| Input | Sends |
|---|---|
| Top tile / Up page button | Single press |
| Middle tile / Down page button | Double press |
| Bottom tile | Long press |

**Edit labels** lets you rename the tiles (e.g. "Frunk", "Vent windows");
labels are stored in `/.crosspoint/s3xy.json`. A tile flashes when the press
was delivered; "Not connected" means it was not sent.

## Notes and limits

- One virtual button (three actions): each Bluetooth identity is one button.
- The Commander link only exists while the S3XY app is open. It uses the
  radio the BLE keyboard normally uses; the keyboard reconnects after you leave.
- Auto-sleep is paused while the Commander is connected, so the screen stays
  usable in the car (power the Sticky from USB there).
- Hold-to-talk to Hermes is disabled on this screen (it would start Wi-Fi next
  to the car link). The lock screen still works.
- Set up the tiles while parked.

Protocol from [Beat-YT/s3xy-virtual-button](https://github.com/Beat-YT/s3xy-virtual-button)
(MIT, see `src/activities/apps/s3xy/LICENSE.s3xy-virtual-button`), ported to NimBLE.
