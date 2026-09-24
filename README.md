# 🎮 Game Stash - Free Game Tracker

[![Latest release](https://img.shields.io/github/v/release/not-Lucifer/GameStash-Free-Game-Tracker?include_prereleases&label=latest)](https://github.com/not-Lucifer/GameStash-Free-Game-Tracker/releases)

A lightweight Windows desktop app that lists every free game giveaway running right now — on Steam, Epic, GOG, Ubisoft Connect, EA, consoles and more — and keeps a stash of the ones you've claimed, so you never grab the same game twice.

## ✨ Features

- 🎯 **Every store** - Giveaways from Steam, Epic, GOG, Ubisoft Connect, EA, Battle.net, Amazon/Prime Gaming, Itch.io, DRM-free, Xbox, PlayStation, Switch, mobile and VR. A filter appears for each store that has something on offer, and **Other** catches giveaways that don't name a store.
- ⏳ **Find the good ones fast** - A spotlight on the next giveaway to end, search, and sorting by newest, ending soon or highest value (worth shown in ₹).
- 📚 **Your stash** - After you claim a game in its store, Game Stash asks you to confirm and adds it to your stash, with a breakdown by store and your recent claims.
- 🔔 **Update alerts** - Tells you when a new version is released and opens the download. You can also check any time from the profile menu → **Check for updates**.
- 👤 **Accounts and profiles** - Sign in with email and password, add a bio, country and favourite store. Google sign-in needs setup (see **Configuration** below); Discord sign-in is coming soon.
- 💾 **Local-first** - Everything is stored on your PC. There is no cloud sync and no account server.

## 🚀 Install

1. Download **`GameStash-<version>-Setup.exe`** from the [Releases page](https://github.com/not-Lucifer/GameStash-Free-Game-Tracker/releases).
2. Run it. It installs for your Windows user only, so it doesn't need administrator rights. You get a Start menu shortcut, an optional desktop shortcut, and an entry in **Settings → Apps** to uninstall it.
3. If Windows shows **"Windows protected your PC"**: Game Stash isn't code-signed yet, so SmartScreen doesn't recognise it. Click **More info → Run anyway**. Only do this for installers downloaded from this repository's Releases page.

**Updating:** from version 1.0.2 onwards, the app tells you when a new version is out. Run the new installer over the old one; your stash and account are kept. Versions 1.0.1 and older don't have update alerts, so install 1.0.2 manually once.

## 📋 System Requirements

- **OS:** Windows 10 or 11, 64-bit
- **Microsoft Edge WebView2 Runtime:** built into Windows 11 and present on most up-to-date Windows 10 PCs. If the window opens blank, [install the WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/).
- **Internet:** needed to load giveaways and check for updates. Your stash is available offline.
- **Disk:** about 10 MB

## 🔧 Build from Source

**Prerequisites**

- **Visual Studio 2026 Community** with the *Desktop development with C++* workload. `build.bat` loads the compiler from `C:\Program Files\Microsoft Visual Studio\18\Community\`; if you have a different edition or version, edit that path in `build.bat`.
- **vcpkg** in a `vcpkg\` folder at the repository root. It is **not** included in the repository (the folder is gitignored), so set it up once:

  ```powershell
  git clone https://github.com/microsoft/vcpkg vcpkg
  .\vcpkg\bootstrap-vcpkg.bat
  .\vcpkg\vcpkg install nlohmann-json cpp-httplib[brotli] sqlite3 webview2 zserge-webview --triplet x64-windows
  ```

  `build.bat` uses the CMake that vcpkg downloads to `vcpkg\downloads\tools\cmake-3.31.10-windows\`. If your vcpkg fetched a different CMake version, update that path in `build.bat`.
- **Inno Setup 6 or 7** ([download](https://jrsoftware.org/isinfo.php)), only needed to build the installer.

**Build**

```powershell
.\build.bat        # Release build -> build\GameStash.exe
.\build_dist.bat   # Release build + dist\GameStash-<ver>-win64\ + .zip + dist\GameStash-<ver>-Setup.exe
```

Run `build\GameStash.exe` to try your changes. When started from `build\`, the app loads `index.html` and `style.css` from the repository root, and it only checks for updates when you ask it to (or when `GAMESTASH_UPDATE_CHECK=1` is set).

Always build the installer with `build_dist.bat`. It compiles `installer.iss` straight after staging fresh files; compiling `installer.iss` by hand packages whatever happens to be in `dist\`, which may be out of date.

## 🛠️ Configuration (optional)

Email and password sign-in works without any setup. Google sign-in needs an OAuth client of your own:

1. In Google Cloud Console, create an OAuth client ID of type **Desktop app**.
2. Copy `oauth_config.json.example` to `oauth_config.json` and fill in `client_id` and `client_secret`. Keep the redirect URI as `http://localhost:9876/oauth/google/callback`.
3. Put `oauth_config.json` next to `GameStash.exe` (for a dev build, in the repository root).

The app signs in with PKCE. Google still asks desktop clients for their client secret, but doesn't treat it as confidential for installed apps. `oauth_config.json` is gitignored and `build_dist.bat` never packages it, so **release builds ship without one, and Google sign-in in a release build reports that it isn't configured.**

## 🔐 Security

- Giveaway data is fetched over HTTPS, and every value from it is escaped before it reaches the page, which runs under a Content Security Policy.
- Links from the giveaway feed can only open `http`/`https` pages, never local files, network paths or other programs.
- Passwords are hashed with PBKDF2-HMAC-SHA256 (310,000 iterations) and a random salt. Accounts created by older versions are upgraded automatically the next time you sign in.
- Google sign-in uses PKCE and a one-time state token.
- The update check only offers downloads from this repository's Releases and never runs anything itself; the download opens in your browser.
- No credentials are packaged in builds, and `debug.log` masks tokens and e-mail addresses.

Found a security problem? Please [open an issue](https://github.com/not-Lucifer/GameStash-Free-Game-Tracker/issues) describing it, without including exploit details.

## 💾 Data and Files

| What | Where |
|---|---|
| Your stash, accounts and profile | `%APPDATA%\GameStash\gamestash.db` (plus `-wal` / `-shm` files) |
| The installed app | `%LOCALAPPDATA%\Programs\Game Stash\` |
| Log file | `debug.log` in the app folder |

Uninstalling keeps your stash. To remove it as well, delete `%APPDATA%\GameStash`.

## 🚢 Releasing a New Version

The in-app update check reads this repository's GitHub Releases, so releases must follow this pattern:

1. Bump the version in `version.h`: the three numbers **and** both strings. It is the only place the version lives; the exe, `build_dist.bat` and `installer.iss` all read it.
2. Commit the bump.
3. Run `.\build_dist.bat` to produce `dist\GameStash-<version>-Setup.exe`.
4. Tag **the commit that contains the bump** as `v<version>` (for example `v1.0.3`), publish a GitHub release from that tag, and attach the Setup file.

Pre-releases are offered to users too; draft releases are ignored.

## 📁 Project Structure

```
FreeGameTracker/
├── main.cpp                   # App backend: window, local database, GamerPower and GitHub API calls, sign-in
├── index.html                 # The whole user interface (markup and script)
├── style.css                  # Styles
├── version.h                  # App version: the single place to change it
├── app.rc, resource.h         # Windows resources: version info and icon
├── app.manifest               # Windows manifest: no admin prompt, DPI awareness
├── assets/gamestash.ico       # App icon
├── CMakeLists.txt             # Build configuration
├── build.bat                  # Release build -> build\GameStash.exe
├── build_dist.bat             # Release build + dist folder + zip + installer
├── installer.iss              # Inno Setup installer script
├── oauth_config.json.example  # Template for Google sign-in
└── main_cli.cpp               # Legacy console prototype (not part of the build)
```

Not in the repository: `vcpkg\` (set up as above), and the generated `build\` and `dist\` folders.

## 🐛 Troubleshooting

**"Windows protected your PC" when installing**
The installer isn't code-signed yet. Click **More info → Run anyway**, but only for files from this repository's Releases page.

**The window is blank**
Install the [WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/).

**The giveaway list is empty**
Check your internet connection. The list comes from the GamerPower API, which may be briefly unavailable; press refresh after a moment. `debug.log` in the app folder shows the error.

**Google sign-in says it isn't configured**
Release builds don't include Google credentials. Sign in with email, or set up your own client as described in **Configuration** above.

**"Couldn't check for updates"**
GitHub allows 60 update checks per hour from one network. Try again later, or check the [Releases page](https://github.com/not-Lucifer/GameStash-Free-Game-Tracker/releases) directly.

## 📝 License

Released under the MIT License.

## 🤝 Contributing

Contributions are welcome: report bugs, suggest features, or open a pull request.

## 📧 Support

For issues and questions, please [open an issue](https://github.com/not-Lucifer/GameStash-Free-Game-Tracker/issues).

---

**Made with ❤️ by Aman Singh**
