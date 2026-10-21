# Running the Mac stream as a LaunchAgent

The server only lives as long as its terminal. To keep it up (and restart it if it
dies), register it with `launchd`.

Create `~/Library/LaunchAgents/com.osmo.stream.plist`, replacing `YOUR_USER` and
the repo path:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>com.osmo.stream</string>
	<key>ProgramArguments</key>
	<array>
		<string>/opt/homebrew/bin/python3</string>
		<string>/Users/YOUR_USER/osmo-pocket-4-on-switch/mac/stream_server.py</string>
		<string>--port</string>
		<string>8765</string>
		<string>--size</string>
		<string>1920x1080</string>
		<string>--fps</string>
		<string>30</string>
		<string>--scale</string>
		<string>1280:720</string>
		<string>--quality</string>
		<string>5</string>
	</array>
	<key>WorkingDirectory</key>
	<string>/Users/YOUR_USER/osmo-pocket-4-on-switch</string>
	<key>RunAtLoad</key>
	<true/>
	<key>KeepAlive</key>
	<true/>
	<key>StandardOutPath</key>
	<string>/tmp/osmo-stream.log</string>
	<key>StandardErrorPath</key>
	<string>/tmp/osmo-stream.log</string>
	<key>EnvironmentVariables</key>
	<dict>
		<key>PATH</key>
		<string>/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin</string>
	</dict>
</dict>
</plist>
```

Load it:

```fish
launchctl bootstrap gui/(id -u) ~/Library/LaunchAgents/com.osmo.stream.plist
launchctl kickstart -k gui/(id -u)/com.osmo.stream    # restart
launchctl bootout gui/(id -u)/com.osmo.stream         # stop
```

`KeepAlive` restarts the server if it crashes, but the process still needs Camera
permission. The first run may show a macOS prompt — allow it.

If frames never arrive, check `/tmp/osmo-stream.log` and confirm no other app
(GimbalBar, Zoom, Photo Booth) is holding the UVC device.
