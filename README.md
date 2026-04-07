# 🎮 Game Stash - Free Game Tracker

A lightweight Windows desktop app to track your claimed free games across multiple platforms with **Supabase cloud sync**. Never lose track of your game library again!

## ✨ Features

- 🎯 **Track Claimed Games** - Add games you've claimed from Epic, Steam, GOG, etc.
- ☁️ **Cloud Sync** - Automatic Supabase synchronization across devices
- 👤 **User Profiles** - Customize your profile with bio and favorite platform
- 📱 **Multi-Device** - Access your game library from any device
- 💾 **Offline Mode** - Local SQLite database fallback when offline
- 🔐 **Secure** - Authentication via Supabase

## 🚀 Quick Start

### Download Latest Release
1. Go to **[Releases](../../releases)**
2. Download `GameStash_v1.0.0.zip`
3. Extract the ZIP file
4. Run `GameStash.exe`
5. Sign up with your email
6. Start tracking games!

### Build from Source

**Prerequisites:**
- Windows 10 or later
- CMake 3.20+
- Visual Studio 2022 (with C++ tools)
- vcpkg (included in repo)

**Build:**
```bash
cd FreeGameTracekr
.\build_release.bat
```

Output: `build\Release\GameStash.exe`

## 📋 System Requirements

- **OS:** Windows 10 or later
- **RAM:** 50 MB minimum
- **Storage:** 100 MB (including database)
- **Internet:** Required for cloud sync (optional for offline use)

## 🔧 Technologies

- **Frontend:** WebView2 (Chromium-based)
- **Backend:** C++ with WinHTTP
- **Database:** 
  - Cloud: PostgreSQL (Supabase)
  - Local: SQLite3
- **Dependencies:** nlohmann/json, Brotli compression

## 📁 Project Structure

```
FreeGameTracekr/
├── main.cpp              # Main application logic
├── main_cli.cpp          # CLI variant
├── index.html            # WebView UI
├── style.css             # UI styling
├── supabase_setup.sql    # Database schema
├── supabase_config.json  # Configuration
├── CMakeLists.txt        # Build configuration
└── build/                # Build artifacts (generated)
```

## 🛠️ Configuration

### Supabase Setup

1. Create a [Supabase](https://supabase.com) account
2. Create a new project
3. Run `supabase_setup.sql` in SQL Editor to create tables:
   - `claimed_games` - Game library tracking
   - `user_profiles` - User information

4. Update `supabase_config.json`:
```json
{
  "supabase": {
    "url": "https://your-project.supabase.co",
    "anon_key": "your-anon-key"
  }
}
```

## 💾 Data Storage

- **Windows User Profile:** `%APPDATA%\GameStash\gamestash.db`
- **Cloud:** Supabase PostgreSQL database

## 🐛 Troubleshooting

### App won't start
- Check Windows Defender isn't blocking it
- Ensure WebView2 is installed (auto-installs on first run)

### Data not syncing
- Check internet connection
- Verify Supabase credentials in `supabase_config.json`
- Check `debug.log` for error details

### Can't create account
- Use a valid email address
- Ensure Supabase project exists

## 📝 License

This project is open source and available under the MIT License.

## 🤝 Contributing

Contributions are welcome! Feel free to:
- Report bugs
- Suggest features
- Submit pull requests

## 📧 Support

For issues and questions, please open an [Issue](../../issues) on GitHub.

---

**Made with ❤️ by Game Stash Community**
