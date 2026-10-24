# Phone notifications (optional)

Get a notification on your **iPhone** (and **Apple Watch**) when your Claude limits reset:

| Event | Notification |
|---|---|
| 5-hour window resets | **Claude 5-Hour Limit Reset**: Your usage window has reset. |
| Weekly window resets | **Claude Weekly Limit Reset**: Your weekly usage window has reset. |

The phone app also shows your live 5-hour and weekly usage with reset countdowns.

It's free: no Apple Developer account, paid push service or database. It works over
Wi-Fi and cellular, and no router ports are opened. It's built from a small Node.js server,
an installable web app (PWA) using standard Web Push, and Tailscale Funnel for HTTPS.

The Windows app works exactly the same with or without this feature.

## How it works

```
Claude Usage Monitor.exe (already polling Claude)
   │  confirmed reset / usage update
   │  http://127.0.0.1:47824/internal/*   localhost only, token from bridge.json
   ▼
push server (node server.js) ──Web Push──▶ Apple push service ──▶ iPhone ──▶ Apple Watch
   ▲
   │  http://127.0.0.1:47823  ◀── Tailscale Funnel https://<pc>.<tailnet>.ts.net:10000
   │                              (only the phone app + its API are published)
iPhone web app (Home Screen)
```

- **No second poller.** The Windows app reports what it already fetched. The push server
  never contacts Claude and never sees your Claude login.
- **Resets are confirmed, not guessed.** A notification is sent only when fresh API data
  arrives after the reset time and shows a new window. Nothing is sent at startup, for a reset
  that happened while the PC was off, or when a timestamp merely changes.
- **No duplicates.** Each reset has an id (`five_hour_reset:<time>`). The Windows app remembers
  what it announced (`push-state.ini`), and the server remembers what it sent (`state.json`).
  Both survive restarts, so polling, retries and reconnects can't double-notify.
- **Non-blocking.** Delivery runs on a background thread in the Windows app. If the push server
  is off, the app logs it once in `app.log` and carries on.

## Requirements

- Windows PC that stays on, with Claude Usage Monitor running
- [Node.js](https://nodejs.org) 18 or newer
- [Tailscale](https://tailscale.com/download/windows) (free), signed in, with Funnel allowed in your tailnet
- iPhone on **iOS 16.4 or later**. Apple Watch notifications come through iPhone mirroring.

## 1. Start the server

```powershell
cd push
.\start.ps1 -Background      # installs dependencies on first run, then runs hidden
```

The server keeps running when you close the terminal or VS Code. Other commands:

```powershell
.\start.ps1                  # run in the foreground instead (Ctrl+C to stop)
.\stop.ps1                   # stop the background server
.\autostart.ps1 -Enable      # optional: start it whenever you sign in to Windows
.\autostart.ps1 -Disable
```

On first start the server creates its keys in `%APPDATA%\ClaudeUsageMonitor\push`.

## 2. Publish it with Tailscale Funnel

```powershell
.\funnel.ps1 -Enable         # serves the phone app on https://<pc>.<tailnet>.ts.net:10000
.\funnel.ps1                 # show the URL / status
.\funnel.ps1 -Disable        # stop publishing
```

This uses Funnel port **10000** only. Routes you already have on 443 or 8443 are left alone.
If Tailscale says Funnel isn't enabled for your tailnet, open the link it prints, allow
Funnel (and HTTPS certificates) for this machine, and run the command again.

The command it runs, if you prefer to do it by hand:

```powershell
tailscale funnel --bg --https=10000 http://127.0.0.1:47823
```

## 3. Install the app on your iPhone

1. On the PC, run `.\pair.ps1`. It prints a one-time code (valid 10 minutes) and the URL.
2. On the iPhone, open the URL in **Safari**.
3. Tap **Share → Add to Home Screen → Add**.
4. Open **Claude Usage** from the Home Screen, not from Safari, and enter the code.
5. Tap **Enable Notifications**, then **Allow**.
6. Tap **Send Test Notification**. It should arrive within seconds.

## 4. Turn it on in the Windows app

Right-click the taskbar readout → **Options... → General Settings → Phone Notifications →
Notify my iPhone / Apple Watch when a limit resets**.

## Apple Watch

Notifications appear on the watch when the iPhone is locked and the watch is on your wrist.
In the **Watch** app on the iPhone, go to **Notifications** and check that **Claude Usage**
mirrors iPhone alerts (it's on by default).

## Testing

```powershell
.\test-notification.ps1                  # test notification to every enabled device
.\test-notification.ps1 -Reset FiveHour  # preview the real 5-hour reset notification
.\test-notification.ps1 -Reset Weekly
..\scripts\test.ps1                      # full automated suite (C++ + server)
```

Each device can turn 5-hour and weekly alerts on or off in the app.

## Security

- **Pairing.** Codes are one-time, expire after 10 minutes, and lock after 5 wrong guesses.
  A paired phone gets a random 256-bit token; only its SHA-256 hash is stored.
- **API access.** Every phone API call needs that token. Test notifications are rate-limited.
- **Internal endpoint.** `/internal/*` runs on its own port that Funnel never exposes. It accepts
  loopback connections only, checks the `Host` header (DNS-rebinding protection), and requires
  the 256-bit token in `bridge.json`.
- **Subscriptions.** Push endpoints must belong to a real push service (Apple, Google, Mozilla,
  Microsoft), so the server can't be used to make requests elsewhere. Expired subscriptions
  (HTTP 404/410) are removed automatically.
- **Storage.** VAPID keys, tokens and subscriptions live in `%APPDATA%\ClaudeUsageMonitor\push`,
  outside the repository. Logs never include tokens, keys or full endpoints.
- **Hardening.** Strict Content-Security-Policy, no third-party scripts, both ports bound to
  `127.0.0.1`.

## Files

| Path | |
|---|---|
| `server.js`, `lib/` | Server: storage, Web Push, HTTP routes, logging |
| `public/` | The iPhone app: page, script, styles, service worker, manifest, icons |
| `test/` | Automated tests (`npm test`) |
| `*.ps1` | Start, stop, pair, test, Funnel and autostart helpers |
| `%APPDATA%\ClaudeUsageMonitor\push\` | `config.json` (keys), `devices.json`, `state.json`, `bridge.json`, `server.log` |
| `%APPDATA%\ClaudeUsageMonitor\push-state.ini` | Resets the Windows app has already announced |

## Troubleshooting

| Symptom | Fix |
|---|---|
| No "Enable Notifications" prompt | The app must be opened from the Home Screen, not from Safari, on iOS 16.4+. |
| "Blocked in Settings" | iPhone **Settings → Notifications → Claude Usage → Allow Notifications**. |
| Test notification works, but no reset alerts | Check the Windows setting in step 4, and that both apps are running. `%APPDATA%\ClaudeUsageMonitor\app.log` shows `push:` lines. |
| "PC monitor: Not connected" | The Windows app hasn't reported yet: phone notifications are off in Options, or the server was started after the last check (it updates within ~2 min). |
| The page doesn't load on the phone | `.\funnel.ps1` should show the URL, and `.\start.ps1` should say the server is running. |
| "This iPhone is no longer paired" | Run `.\pair.ps1` again and re-enter a code. |
| Nothing arrives on the watch | Lock the iPhone and check the Watch app's notification mirroring for Claude Usage. |

Server log: `%APPDATA%\ClaudeUsageMonitor\push\server.log`

## Turning it off or removing it

- **Pause:** untick the option in the Windows app, or run `.\stop.ps1`.
- **Remove completely:**
  ```powershell
  .\autostart.ps1 -Disable
  .\funnel.ps1 -Disable
  .\stop.ps1
  Remove-Item -Recurse "$env:APPDATA\ClaudeUsageMonitor\push", "$env:APPDATA\ClaudeUsageMonitor\push-state.ini"
  ```
  Then delete the Claude Usage app from the iPhone's Home Screen. The Windows app keeps working
  as before, and the `push\` folder can be deleted.
