Userspace macOS driver for the Dynacord CMS 600-3: 4 inputs and 4 outputs at 44.1, 48 and 96 kHz, plus USB-MIDI. Needs an Apple Silicon Mac on macOS 26.1 or later, and no SIP changes.

## Install with Homebrew

```bash
brew install --cask dominics/tap/ozzy-cms600
```

## Or install the package

Download the `.pkg` below. It is not signed with an Apple Developer ID, so macOS blocks it the first time you open it:

1. Double-click the package, then close the warning.
2. Open System Settings > Privacy & Security, scroll to Security, and click **Open Anyway** next to the message about the package.
3. Confirm the warnings that follow, and the installer opens.

Or skip Gatekeeper from Terminal: `sudo installer -pkg Ozzy-CMS600-*.pkg -target /`

## Uninstall

`brew uninstall --cask ozzy-cms600`, or `sudo "/Library/Application Support/Ozzy/uninstall.sh"`.

More in the [macOS README](https://github.com/dominics/Ozzy/blob/main/macos/README.md#install-dynacord-cms-600-3).
