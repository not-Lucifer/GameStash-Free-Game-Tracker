# 🎮 Game Stash - Free Game Tracker

A lightweight Windows desktop app to track your claimed free games across multiple platforms. Never lose track of your game library again!

## ✨ Features

- 🎯 **Track Claimed Games** - Add games you've claimed from Epic, Steam, GOG, etc.
- 👤 **User Profiles** - Customize your profile with bio and favorite platform
- 💾 **Fully Offline** - All data lives in a local SQLite database; no account server required
- 🔐 **Secure** - Authentication support

## 🚀 Quick Start

### Download Latest Release
1. Download the latest release ZIP from the repository **Releases** page, or build a distribution locally with `.\build_dist.bat` and find the ZIP under `dist/`.
2. Extract the ZIP
3. Run `GameStash.exe`
4. Sign up or configure authentication and start tracking games

### Build from Source

**Prerequisites:**
- Windows 10 or later
- CMake 3.20+
- Visual Studio 2022 (with C++ tools)
- vcpkg (included in repo)

**Build (from repository root):**
```powershell
.\build.bat
```

Output: `build\GameStash.exe`

## 📋 System Requirements

- **OS:** Windows 10 or later
- **RAM:** 50 MB minimum
- **Storage:** 100 MB (including database)
- **Internet:** Optional (only required for external services)

## 🔧 Technologies

- **Frontend:** WebView2 (Chromium-based)
- **Backend:** C++ with WinHTTP
- **Database:** Local SQLite3
- **Dependencies:** nlohmann/json, Brotli compression

## 📁 Project Structure

```
FreeGameTracker/
├── .git/                 # Git repo metadata
├── build/                # Build artifacts (generated)
├── dist/                 # Distribution packages
├── vcpkg/                # vcpkg submodule / packages
├── ZIP_TEST_EXTRACT/     # Sample ZIP extraction
├── main.cpp              # Main application logic
├── main_cli.cpp          # CLI variant
├── index.html            # WebView UI
├── style.css             # UI styling
├── CMakeLists.txt        # Build configuration
├── build.bat             # Build script (release)
├── build_dist.bat        # Build distribution script
├── run_out.txt           # Last run output
├── debug.log             # Debug log
├── gamestash.db          # Local SQLite database (example)
├── GameStash_v1.0.0.zip  # Packaged release (example)
├── oauth_config.json     # OAuth config (example)
├── oauth_config.json.example
└── README.md             # Project README
```

## 🛠️ Configuration

This project includes a few example configuration and helper files. Remove or update them if you're not using the corresponding services.

- `oauth_config.json` / `oauth_config.json.example`: OAuth settings for third-party auth providers.

Adjust any local paths or service credentials before building or running the app.

## 💾 Data Storage

- **Windows User Profile:** `%APPDATA%\GameStash\gamestash.db`
- All data is stored locally on your machine. There is no cloud sync or remote database.

## 🐛 Troubleshooting

### App won't start
- Check Windows Defender isn't blocking it
- Ensure WebView2 is installed (auto-installs on first run)

### Games list won't load
- Check your internet connection (the giveaway list is fetched from the GamerPower API)
- Check `debug.log` for error details

### Can't create account
- Use a valid email address

## 📝 License

This project is open source and available under the MIT License.

## 🤝 Contributing

Contributions are welcome! Feel free to:
- Report bugs
- Suggest features
- Submit pull requests

## 📧 Support

For issues and questions, please open an [Issue](./issues) on GitHub.

---

**Made with ❤️ by Aman Singh**
