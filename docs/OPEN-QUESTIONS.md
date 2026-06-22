# Open Questions & Research

This document tracks items that need research or clarification before full implementation. As you gather information, fill in the answers here.

---

## 1. Backup File (`.qcb`) Format

### Status
**OPEN** — need to examine a real `.qcb` file

### What we need
- The exact JSON schema inside a `.qcb` file
- Field names for: preset name, scene name, bank index, slot index, MIDI program number
- Whether all presets are always present or if there can be gaps
- Whether the `.qcb` format changes between firmware versions

### How to research
1. Export a backup from your Quad Cortex:
   - On QC: Menu → Settings → System → Backup → Export
   - Transfer to Mac
2. Unzip the `.qcb` file:
   ```bash
   unzip my-backup.qcb
   ls -la  # see what's inside
   cat manifest.json | jq .  # pretty-print if JSON
   ```
3. Examine the structure and document below

### Findings
```
File structure of .qcb:
[document your findings here]

Example preset entry:
[paste a sample JSON entry]

Scene structure:
[describe how scenes are stored]

Bank/slot mapping:
[explain how bank indices and slot indices are stored]
```

---

## 2. Cortex Cloud API

### Status
**OPEN** — need to capture API traffic from cloud.neuraldsp.com

### What we need
- Authentication endpoint (how to log in)
- Presets endpoint (how to fetch preset list)
- Response schema (what fields are returned)
- Auth token format (Bearer token? API key?)
- Rate limiting / throttling rules
- Whether tokens expire and need refresh

### How to research
1. Open browser Developer Tools (F12)
2. Go to cloud.neuraldsp.com
3. Open Network tab
4. Log in with your Neural DSP account
5. Interact with the app (click "Presets", etc.)
6. Observe the network requests
7. Right-click → Copy as cURL → document below

### Findings
```
Login endpoint:
POST [endpoint]
Headers: [relevant headers]
Body: { email: "...", password: "..." }
Response: { token: "...", ... }

Presets endpoint:
GET [endpoint]
Headers: Authorization: Bearer [token]
Response: [example response]

Error handling:
[what happens on wrong credentials, network error, etc.]
```

---

## 3. Quad Cortex MIDI Configuration

### Status
**OPEN** — verify which settings we need to know

### What we need
- Can the scene change CC (currently assumed to be CC 34) be configured on the QC?
- Can the MIDI channel be configured?
- Are there any MIDI quirks or special messages we should know about?
- How does bank select work for >128 presets?

### How to research
1. On your Quad Cortex, check Settings:
   - Menu → Settings → MIDI
   - Document what's configurable
2. Check the QC manual or community forums (Neural DSP forums, geargods, etc.)

### Findings
```
QC MIDI Configuration Options:
- Scene change CC: [default/configurable?]
- MIDI channel: [default/configurable?]
- Bank select support: [yes/no, and how?]
- Any special messages: [list any]

Tested with QC firmware version: [your version]
```

---

## 4. Bank Select for >128 Presets

### Status
**OPEN** — only relevant if you have >128 presets

### What we need
- Do you have more than 128 presets?
- If yes, how does the QC organize them (multiple banks of 128, or some other scheme)?
- What MIDI bank select messages does it use?

### How to research
1. On QC: Check total number of presets in library
2. If >128: Check MIDI documentation for bank select (CC0 + CC32 + PC pattern)
3. Capture real MIDI output when switching between banks

### Findings
```
Total presets: [number]
Bank organization: [e.g., "4 presets per bank, 32 banks = 128 presets"]
Bank select messages: [CC numbers and mapping]
```

---

## 5. WIDI Jack BLE Stability

### Status
**OPEN** — real-world testing needed

### What we need
- How often does the WIDI Jack disconnect?
- What causes disconnections?
- How long does it take to reconnect?
- Does it auto-reconnect or require manual re-pairing?

### How to research
- Use the app on stage or in a real environment
- Monitor connection status and log any disconnections
- Test with the app running for extended periods

### Findings
```
Disconnection frequency: [observed]
Common causes: [list any patterns]
Reconnection behavior: [auto/manual]
Stable range: [distance, interference, etc.]
```

---

## 6. Cortex Control USB Protocol (Future)

### Status
**OUT OF SCOPE for Phase 1-7** — but worth documenting for the USBDirectProvider stub

### What we might need (for Phase 8)
- How does Cortex Control read the preset library via USB?
- What protocol/messages does it use?
- Can we replicate this in our app?

### How to research
- Reverse-engineer Cortex Control's USB communication
- Capture USB packets (requires specialized tools like Wireshark or USB sniffing hardware)
- Community might have hints on QC forums

### Findings
```
USB protocol (reverse-engineered):
[to be filled in if we implement USBDirectProvider]
```

---

## 7. Keychain Storage for Cloud Credentials

### Status
**PENDING IMPLEMENTATION** — but we know what we need

### What we know
- On iOS/macOS, we use the native Keychain via `KeychainAccess` or Security framework
- Email & password will be stored securely
- Tokens should be kept in memory only, cleared on sign out

### Implementation notes
- Use Security framework's `SecItemAdd`, `SecItemUpdate`, `SecItemCopyMatching`, `SecItemDelete`
- Or use `KeychainAccess` CocoaPod (simpler API, adds dependency)
- Store under service identifier: "com.izerion.qcremote"

---

## 8. Scene Grid Layout on Different Devices

### Status
**PENDING VERIFICATION** — layout should adapt, but may need tweaking

### What we know
- iPhone: 2×4 grid (2 columns, 4 rows)
- iPad/Mac: 4×2 grid (4 columns, 2 rows)
- Used `@Environment(\.horizontalSizeClass)` to detect device

### Testing needed
- Verify layout on actual devices (iPhone 15, iPad Air, Mac)
- Check if tap targets are big enough (>=44pt)
- Verify text is readable from a distance (stage use)

### Findings
```
iPhone layout: [works / needs adjustment]
iPad layout: [works / needs adjustment]
Mac layout: [works / needs adjustment]
Text size for stage readability: [estimate distance/visibility]
```

---

## 9. Simulator Limitations

### Status
**CONFIRMED** — affects testing strategy

### What we know
- iOS Simulator can't access real MIDI devices
- We can test UI without MIDI
- We need a physical device to test WIDI Jack connection

### Workarounds
- Develop UI on simulator
- Test MIDI logic with mock data
- Test real device connection on iPhone/iPad connected to WIDI Jack

---

## 10. Performance: Preset Lookup Speed

### Status
**PENDING BENCHMARKING** — need to verify lookup is fast enough

### What we need
- SwiftData query speed for `preset(for programNumber:)`
- Whether 128 presets is fast enough
- Whether 256+ presets is still acceptable

### How to test
- Implement Phase 4, import a full preset library
- Time the query: `let start = Date(); let _ = presetLibrary.preset(for: 42); let elapsed = Date().timeIntervalSince(start)`
- Run multiple queries and check for slowness

### Findings
```
Average lookup time (1 preset): [ms]
Average lookup time (128 presets): [ms]
Average lookup time (256+ presets): [ms]
Acceptable threshold: [based on MIDI event frequency]
```

---

## Summary

Update this file as you research each topic. Once you have answers, move the item to **RESOLVED** and integrate the findings into the appropriate phase documentation.

---

**Legend:**
- **OPEN:** No research done yet
- **IN PROGRESS:** Currently investigating
- **PENDING IMPLEMENTATION:** Research done, waiting for code
- **PENDING VERIFICATION:** Code exists, needs real-world testing
- **RESOLVED:** Research complete, integrated into design
