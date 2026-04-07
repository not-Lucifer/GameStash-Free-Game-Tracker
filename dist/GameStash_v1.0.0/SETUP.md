# Game Stash - Setup Guide

Welcome to **Game Stash**! 🎮

This guide will help you install and configure Game Stash on your Windows system.

## Installation Options

### Option 1: Quick Install (Recommended) ⚡

**For Windows 10/11 users who want automatic Start Menu integration:**

1. **Extract the ZIP file** to any folder
2. **Right-click `install.bat`** and select **"Run as administrator"**
3. Follow the on-screen prompts
4. Game Stash will appear in your **Windows Start Menu** and Desktop

**Uninstall:** Run `uninstall.bat` from `C:\Program Files\GameStash` folder

### Option 2: Portable Mode

**For users who prefer standalone operation:**

1. **Extract the ZIP file** to any folder
2. **Double-click `GameStash.exe`** to launch directly
3. No installation needed - run from anywhere

**Note:** No Start Menu shortcuts are created in this mode.

---

## System Requirements

- **OS:** Windows 10 or Windows 11 (64-bit)
- **Memory:** 512 MB minimum
- **.NET Framework:** WebView2 Runtime (auto-updated by Windows)
- **Internet:** Required for OAuth login and giveaway sync

---

## First Launch

1. Click **"Create New Account"** or use **Google/Discord OAuth**
2. Enter your email and create a password
3. Fill in your profile (optional):
   - Add a bio
   - Select your country
   - Choose your favorite gaming platform
4. **Browse giveaways** and claim free games!

---

## OAuth Setup (Optional but Recommended)

For **Google or Discord login**, you need OAuth credentials configured in `oauth_config.json`:

### For Development (Self-Hosted):
```json
{
  "google": {
    "client_id": "your-client-id.apps.googleusercontent.com",
    "client_secret": "your-secret"
  },
  "discord": {
    "client_id": "your-discord-app-id",
    "client_secret": "your-secret"
  }
}
```

See `oauth_config.json.example` for reference.

---

## Database Location

Your game data and profile are stored in a local SQLite database:

```
For Installed Version:
  C:\Program Files\GameStash\gamestash.db

For Portable Version:
  (Extracted folder)\gamestash.db
```

**Your database is never uploaded.** All data stays on your computer.

---

## Troubleshooting

### "HTML file not found" error
- ✅ **Fix:** Make sure `index.html` and `style.css` are in the same folder as `GameStash.exe`

### App crashes on startup
- ✅ **Fix:** Delete `gamestash.db` and restart the app (fresh database will be created)
- ✅ **Alt Fix:** Reinstall using `install.bat` with Administrator privileges

### OAuth login not working
- ✅ **Fix:** Verify `oauth_config.json` contains valid credentials
- ✅ **Alt:** Use email/password registration instead

### Start Menu shortcut missing after install
- ✅ **Fix:** Re-run `install.bat` as Administrator
- ✅ **Manual:** Navigate to `C:\Program Files\GameStash` and create desktop shortcut to `GameStash.exe`

---

## Advanced: Manual Installation

If `install.bat` fails:

1. Create folder: `C:\Program Files\GameStash`
2. Copy all files from extracted ZIP to that folder
3. Create a shortcut to `GameStash.exe` on your Desktop
4. (Optional) Create Start Menu shortcut:
   - Navigate to: `%APPDATA%\Microsoft\Windows\Start Menu\Programs`
   - Create new folder: `GameStash`
   - Create shortcut to `GameStash.exe` inside

---

## Version Information

- **Version:** 1.0
- **Build Date:** 2024
- **Architecture:** 64-bit (x64)
- **Release URL:** [Check GitHub Releases](https://github.com/yourusername/FreeGameStash)

---

## Getting Help

If you encounter issues:

1. Check **Troubleshooting** section above
2. Check `debug.log` file in app directory for error details
3. Open an issue on [GitHub Issues](https://github.com/yourusername/FreeGameStash/issues)

---

## Privacy & Security

✅ **No telemetry** - Game Stash doesn't track you  
✅ **Local data only** - Your database stays on your computer  
✅ **Open source** - Code is public for transparency  
✅ **No ads** - Completely free  

---

## Uninstall

### If installed with `install.bat`:
- Run `uninstall.bat` from `C:\Program Files\GameStash`
- OR use Windows Settings → Apps → Uninstall

### If using portable mode:
- Simply delete the extracted folder

---

**Enjoy finding amazing free games! 🎉**

Last updated: 2024
