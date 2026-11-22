# AirBuzz

AirBuzz makes a Fitbit Air pulse by temporarily changing its haptic intensity over Bluetooth, Golden Gate Gattlink, DTLS and CoAP. A single tap restores the original setting in the same write. Double holds the alternate setting for 800 ms. Every operation attempts a refusing DTLS handoff before reporting its result so Google Health can reclaim the tracker channel.

## Build

Requires macOS, Xcode with an iOS 27 SDK, CMake, Ninja and XcodeGen. The Golden Gate core and mbedTLS are vendored; no package download is required.

```sh
bash scripts/check.sh
```

The gate runs the macOS fake-band tests under AddressSanitizer and UndefinedBehaviorSanitizer, builds the iOS static archive, generates the Xcode project and compiles the app without signing. Individual core builds use `bash scripts/build-core.sh macos` or `bash scripts/build-core.sh ios`. Build artifacts live under `build/`.

## Install and use

After building, open `App/AirBuzz.xcodeproj` in Xcode, select your iPhone and configure automatic signing for your account if necessary. Build and run to install. Allow Bluetooth access, keep the Fitbit Air nearby, then use **Test buzz** or **Read strength**. This repository's check script does not sign or install the app.

The core permits one operation per session and has a 45-second operation limit including handoff. AirBuzz caps each active app request at 25 seconds; a stalled operation is cancelled and disconnected at that limit. Requests arriving together run in order. Physical-band behavior and notification delivery require testing on an iPhone with the band.

## Shortcuts automations

In Shortcuts, choose **Automation**, **New**, then **Notification**. Select the app, such as Messages, WhatsApp or Slack. Set it to run immediately without asking if offered, then add **Buzz My Air**. Leave Mode empty for a single tap or select Double. Turning on **Notify When Run** is not needed.

WhatsApp and Telegram calls use the same Notification trigger. AirBuzz does not listen for notifications or detect calls itself; Shortcuts invokes its App Intent in the app process. The app also includes this setup guide.

## Disclaimer

AirBuzz is an independent project, not affiliated with or endorsed by Google or Fitbit. Golden Gate under `Vendor/golden-gate` is Apache 2.0; see its LICENSE and NOTICE.
