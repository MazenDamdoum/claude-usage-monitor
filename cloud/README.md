# Cloud relay (optional)

Reset alerts on your **iPhone and Apple Watch**, plus **Home Screen and Lock Screen widgets**,
that keep working when your laptop is **closed, asleep or offline**.

It runs free on your own Cloudflare account (Workers free plan). Your Claude login never
leaves your PC.

## How it works

```
Windows app (only place your Claude login exists)
   │  every check: percentages + reset times  (HTTPS, upload token)
   ▼
Cloudflare Worker + Durable Object (your account, free)
   ├─ alarm at the exact reset time ──Web Push──▶ iPhone ──▶ Apple Watch
   ├─ phone app (installable, loads anywhere)
   └─ /api/widget (read-only key) ──▶ Scriptable widgets
```

- **Alerts fire at the reset time.** The Windows app uploads when each limit resets, and the
  cloud's alarm sends the notification at that moment, whether or not your laptop is on.
- **Weekly alerts keep going on their own.** After each weekly reset the next one is scheduled
  7 days later, even if the laptop stays off for weeks.
- **No duplicates.** Every reset has an id, and when the laptop later confirms a reset the cloud
  already announced, it's ignored.
- **The cloud stores only** usage percentages, reset times, your phone's push address and hashed
  device tokens. It never contacts Claude and never sees your Claude login.

### One limit to know about

A 5-hour window starts with your **first message**. If you start one while the laptop is
off (for example on your phone), the cloud can't know when it will end, so that window's alert
is skipped. Weekly alerts and every window the laptop saw are always covered.

## Setup (about 5 minutes)

You need [Node.js](https://nodejs.org) 18+ and a free [Cloudflare account](https://dash.cloudflare.com/sign-up).

```powershell
cd cloud
.\setup.ps1        # sign in to Cloudflare in the browser when asked, then it deploys
.\pair.ps1         # your personal link: open it on your iPhone, the app signs in by itself
```

`setup.ps1` does five things:
1. Generates the push keys and an upload token, stored in `%APPDATA%\ClaudeUsageMonitor\cloud-secrets.json`.
2. Signs in to Cloudflare.
3. Deploys the Worker.
4. Stores the keys as Cloudflare secrets.
5. Writes `%APPDATA%\ClaudeUsageMonitor\cloud.json` so the Windows app uploads there.

Running it again redeploys with the same keys, so paired phones keep working.

> New Cloudflare accounts need a `workers.dev` subdomain. If the deploy asks for one or
> fails, open **Workers & Pages** in the Cloudflare dashboard once, then run `setup.ps1` again.

Then:

1. **Windows app:** Options → General → **Phone Notifications** → tick *Notify my iPhone / Apple Watch*.
2. **iPhone:** open your link in **Safari** → **Share → Add to Home Screen** → open **Claude Usage**
   from the Home Screen (it signs in by itself) → **Enable Notifications**.
3. **Apple Watch:** notifications are mirrored automatically when the iPhone is locked
   (Watch app → Notifications → Claude Usage).

## Several accounts (claude-swap)

With more than one account, your phone shows a tab per account (the one in use in orange) and
tells you when **any** account's 5-hour or weekly limit resets: "alice: 5-hour limit reset".

### Switching without stopping

When the account in use reaches the auto-switch point (95% by default), the Windows app moves to
the account with the most room left. Claude Code re-reads its login file on every request, so the
next request already goes out on the new account and the prompt carries on by itself. You're
notified on your PC and phone, and so are the owners of both accounts.

The usage API is only asked every minute or two (more often gets rate-limited), so between checks
the app adds up what Claude Code spends, from its transcripts, and switches on that live
estimate. That way it moves before the limit, not after Claude Code has already stopped.

As a safety net, `..\scripts\install-claude-hook.ps1` adds a Claude Code hook: if a turn does end
on a usage limit, Claude Code tells the app, which switches at once. (Claude Code ignores what
that hook says, so after such a stop you send one message to carry on.)

### Alerts, each one on or off per phone

- **Usage:** 25%, 50%, 75%, 85% and maxed out, for the 5-hour and weekly limits.
- **Reset reminders:** 1 hour, 30, 15 and 5 minutes before a limit resets.
- **Resets** and **account switches**.

### Your own caps

On an account's tab, set **your** 5-hour or weekly cap on it (say you agreed 40% with its owner in
person). The lower of your cap and the owner's applies, and the Windows app keeps you under it.

### Heads-up for the owner

When the account in use is down to **15% left**, the owner of the account the app would switch to
next is told "Sam may switch to your Claude account soon". On an account's tab, **Let alice know
I'm about to use it** sends the same kind of heads-up whenever you like.

### Prompts left

Each account also shows roughly how many more **Opus prompts** fit before its limit runs out, at
**low, medium and high** effort. The Windows app works it out on your PC:

- **What one prompt costs:** the median of your own prompts over the last 14 days, from Claude
  Code's transcripts (each reply records its model, effort and token use).
- **How much of the limit that is:** learnt from how each account's percentage moved while those
  tokens were spent. On first run it learns from the readings already in `app.log`.

It's an estimate. Usage outside Claude Code (claude.ai, the phone app) has no transcript, so it
errs on the low side. Only the resulting percentages go to the cloud, never the transcripts.

## Friends whose accounts you use

### Adding someone

1. **Sign in to their account once** in Claude Code (`/login`). The Windows app saves it to
   claude-swap by itself and it appears on the taskbar.
2. **Name it:** right-click the taskbar readout → Rename → their account → e.g. `dave`.
3. **Send them their link:** right-click the taskbar readout → **Phone link** → **For dave's owner**
   copies a one-time link to paste to them (or run `.\pair.ps1 -Owner dave`).
   [FRIENDS.md](FRIENDS.md) explains the app from their side.

To remove an account from the PC: right-click → **Remove**. The plain-language
[guide](../docs/GUIDE.md) covers all of this with pictures.

To unpair someone's phone (or a duplicate install), run `.\status.ps1`, copy the phone's id and run
`.\status.ps1 -Remove <id>`.

If you use a friend's Claude account through claude-swap, they can follow it from their own iPhone:

```powershell
.\pair.ps1 -Owner alice    # alice = the name on your taskbar; prints a message to send them
```

Each link works once, within 24 hours, and installs an app that signs in by itself. On their phone they see **only their own account**: its
live 5-hour and weekly usage, the prompts left on it, and whether you're using it right now.
They get notified when:

- you may switch to their account soon (15% left on yours), or tap **Let them know**
- you start using their account, or switch away from it
- usage passes 25%, 50%, 75% and 85% ("48% left · resets in 2h 10m"), and when it's maxed out
- a limit resets in 1 hour, 30, 15 or 5 minutes, and when it resets

Each of these can be turned off on their phone. From the same screen they can **cap** your use
(5-hour and weekly, 20–90%) or turn off **Allow Sam to use it**. Your phone and PC are told right away. The Windows app then moves you
to another account when their cap is reached or they ask you to stop, even if you picked their
account by hand, and won't switch back to it until they allow it again.

Set the name they see in `%APPDATA%\ClaudeUsageMonitor\settings.ini`, in `[general]`:
`your_name=Sam`.

## Widgets (Scriptable)

1. Install [Scriptable](https://apps.apple.com/app/scriptable/id1405459188) (free).
2. In the Claude Usage phone app, tap **Create Widget Key** and copy the key. It's read-only and
   can only view usage.
3. In Scriptable, tap **+**, paste the contents of [`scriptable/ClaudeUsage.js`](scriptable/ClaudeUsage.js),
   name it *Claude Usage* and run it once. Enter your cloud URL and the key.
4. Add a **Scriptable** widget to the Home Screen or Lock Screen, edit it, and choose *Claude Usage*.

Sizes: small and medium for the Home Screen; rectangular, circular and inline for the Lock Screen.
iOS refreshes widgets roughly every 5–15 minutes; the script also asks for a refresh right after
each reset.

### Apple Watch: usage on tap (Shortcuts)

Watch complications need a native app, but a Shortcut on the watch can show usage when you tap it:

1. In **Shortcuts**, create a new shortcut:
   - **Get Contents of URL**: `https://<your-worker>.workers.dev/api/widget`, with the header
     `Authorization` set to `Bearer <widget key>`
   - **Get Dictionary Value**: `usage`, then a second one for `five_hour` and a third for `percent`
   - **Show Result**: `5h: <percent>%`
2. In the shortcut's details, turn on **Show on Apple Watch**.

## Everyday commands

```powershell
.\status.ps1          # devices, last upload, next scheduled alerts
.\status.ps1 -Test    # send a test notification to every enabled device
.\status.ps1 -Remove <id>  # unpair a phone (ids are in the status list)
.\pair.ps1            # pair another device
.\pair.ps1 -Owner alice  # pair the friend who owns the 'alice' account
.\setup.ps1           # redeploy after updating the code
```

## Security

- **Uploads.** `/ingest/*` needs the 256-bit upload token, which lives only in your `%APPDATA%` and
  in Cloudflare secrets, and is compared in constant time.
- **Pairing.** Links and codes are one-time and expire after 24 hours. Five
  wrong guesses void every pending code, and attempts are rate-limited. A paired phone gets a random
  256-bit token; only its SHA-256 hash is stored.
- **Friends.** A friend's phone can read only its own account's usage and change only its own
  account's rules. It never sees your other accounts or your own alerts.
- **Widget keys.** Read-only, separate from the device token. Making a new one revokes the old one.
- **Subscriptions.** Push endpoints must belong to Apple, Google, Mozilla or Microsoft push services.
  Subscriptions that are gone (HTTP 404/410) are removed automatically.
- **Encryption.** Web Push uses standard VAPID (RFC 8292) and aes128gcm encryption (RFC 8291),
  written with WebCrypto and tested against the reference libraries.
- **Headers.** Strict CSP, HSTS, `nosniff`, no framing, no third-party scripts.

## Cost

Free. One Durable Object, a few hundred requests a day (one upload per laptop check, plus the
phone app and widgets) and a few alarms a week are far below the Workers free-plan limits.

## Testing

```powershell
npm test                    # unit tests: WebCrypto Web Push interop + every Hub behaviour
npm run e2e                 # full flow in the real Workers runtime (wrangler dev), no account needed
..\scripts\test.ps1         # everything: Windows app, local push server, cloud
```

## Removing it

```powershell
.\node_modules\.bin\wrangler delete       # deletes the Worker and its data from Cloudflare
Remove-Item "$env:APPDATA\ClaudeUsageMonitor\cloud.json", "$env:APPDATA\ClaudeUsageMonitor\cloud-secrets.json"
```

Delete the Claude Usage app and the Scriptable widget from the iPhone. The Windows app keeps
working; it simply stops uploading once `cloud.json` is gone.
