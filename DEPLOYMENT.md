# Deployment & Installer Guide

## Overview

Game Stash is now ready for production deployment with:
- ✅ **Centralized Supabase Backend** - All user claims and profiles sync to Supabase
- ✅ **AppData Storage** - Database stored in `%APPDATA%\GameStash\` for multi-user support
- ✅ **Professional Installer** - Inno Setup installer for Windows (creates Start Menu shortcuts)
- ✅ **GitHub Ready** - Complete release package for GitHub Releases

---

## Supabase Setup (Backend)

### 1. Create Supabase Project

1. Go to [supabase.com](https://supabase.com)
2. Create a new project (free tier supported)
3. Note your **Project URL** and **Anon Key** from Settings → API

### 2. Create Database Tables

In Supabase SQL Editor, run:

```sql
-- User profiles table
CREATE TABLE user_profiles (
  id BIGSERIAL PRIMARY KEY,
  user_id TEXT UNIQUE NOT NULL,
  bio TEXT,
  country TEXT,
  favorite_platform TEXT,
  created_at TIMESTAMPTZ DEFAULT NOW(),
  updated_at TIMESTAMPTZ DEFAULT NOW()
);

-- Claimed games table
CREATE TABLE claimed_games (
  id BIGSERIAL PRIMARY KEY,
  user_id TEXT NOT NULL,
  game_id INTEGER NOT NULL,
  title TEXT,
  platforms TEXT,
  worth_inr TEXT,
  open_giveaway_url TEXT,
  claimed_at TIMESTAMPTZ DEFAULT NOW(),
  UNIQUE(user_id, game_id)
);

-- Enable RLS (Row Level Security) - optional but recommended
ALTER TABLE claimed_games ENABLE ROW LEVEL SECURITY;
ALTER TABLE user_profiles ENABLE ROW LEVEL SECURITY;

-- Create policies to allow authenticated users to access only their data
CREATE POLICY "Users can read their own claims"
  ON claimed_games FOR SELECT
  USING (auth.uid()::text = user_id);

CREATE POLICY "Users can insert their own claims"
  ON claimed_games FOR INSERT
  WITH CHECK (auth.uid()::text = user_id);
```

### 3. Update Credentials in Code

Edit [main.cpp](main.cpp) and update line with Supabase credentials:

```cpp
const std::string SUPABASE_URL = "https://YOUR_PROJECT_ID.supabase.co";
const std::string SUPABASE_ANON_KEY = "your_anon_key_here";
```

Rebuild:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

---

## Building the Installer

### Prerequisites

1. **Inno Setup** - Download from [jrsoftware.org](https://jrsoftware.org/isdl.php)
   - Install latest version (6.2.2+)
   - Add `iscc.exe` to your PATH

2. **App Icon** (optional but recommended)
   - Create or download a professional icon (256x256 PNG)
   - Convert to `.ico` format
   - Save as `gamestash.ico` in project root

### Build Release Package

```bash
cd d:\Projects\FreeGameTracekr

# 1. Build in Release mode with all dependencies (already in build_release.bat)
.\build_release.bat

# 2. Compile Inno Setup installer
iscc.exe setup.iss

# Output: dist\GameStash_Installer_v1.0.0.exe
```

### What's Included

**ZIP Distribution:**
- `dist\GameStash_v1.0.0.zip` - Portable version (extract & run)

**Installer EXE:**
- `dist\GameStash_Installer_v1.0.0.exe` - Professional Windows installer
  - Installs to: `C:\Program Files\GameStash`
  - Creates: Start Menu shortcuts, Desktop shortcut
  - Database stored in: `%APPDATA%\GameStash\gamestash.db`
  - Uninstaller: Add/Remove Programs in Windows Settings

---

## Publishing to GitHub

### Step 1: Push Code

```bash
cd d:\Projects\FreeGameTracekr

git init
git add .
git commit -m "Game Stash v1.0.0 - Initial Release"
git branch -M main
git remote add origin https://github.com/YOUR_USERNAME/YOUR_REPO.git
git push -u origin main
```

### Step 2: Create GitHub Release

1. Go to GitHub: https://github.com/YOUR_USERNAME/YOUR_REPO/releases
2. Click **Draft a new release**
3. Set:
   - **Tag version:** `v1.0.0`
   - **Release title:** `Game Stash v1.0.0 - Initial Release`
   - **Description:** (see template below)

4. **Upload Assets:**
   - `dist\GameStash_v1.0.0.zip`
   - `dist\GameStash_Installer_v1.0.0.exe`

5. Click **Publish release**

### Release Description Template

```markdown
## 🎮 Game Stash v1.0.0

**A free, open-source game giveaway tracker for Windows**

### What's New
- Email validation and secure user accounts
- Personal profiles with bio and preferences
- Centralized backend sync via Supabase
- Professional Windows installer with Start Menu integration
- 32+ free games tracked from multiple platforms
- Local database backup with cloud sync

### Installation Options

#### 🚀 Fast Install (Recommended)
Download `GameStash_Installer_v1.0.0.exe` and run it. Creates Start Menu shortcut automatically.

#### 📦 Portable Mode
Download `GameStash_v1.0.0.zip`, extract, and run `GameStash.exe`. No installation needed.

### System Requirements
- Windows 10 or Windows 11 (64-bit)
- WebView2 Runtime (Windows 11 has it built-in, Windows 10 auto-installs)
- ~50 MB disk space

### Features
✅ Track free games from Steam, Epic, GOG, Itch.io  
✅ Personal profiles with customization  
✅ Email & OAuth authentication  
✅ Cloud sync to Supabase  
✅ Offline-capable local database  
✅ Completely free & open-source  

### Download
- [GameStash_Installer_v1.0.0.exe](https://github.com/YOUR_USERNAME/YOUR_REPO/releases/download/v1.0.0/GameStash_Installer_v1.0.0.exe) - **Recommended for most users**
- [GameStash_v1.0.0.zip](https://github.com/YOUR_USERNAME/YOUR_REPO/releases/download/v1.0.0/GameStash_v1.0.0.zip) - Portable version

### Data Storage
- **Local database:** `%APPDATA%\GameStash\gamestash.db`
- **Cloud sync:** Optional, via Supabase
- **Privacy:** Your data is yours. No tracking, no ads.

### Support
Found a bug? [Report an issue](https://github.com/YOUR_USERNAME/YOUR_REPO/issues)

---

**Enjoy finding free games!** 🎉
```

---

## Database Storage

### Local Storage
```
%APPDATA%\GameStash\gamestash.db
```

Example on Windows:
```
C:\Users\JohnDoe\AppData\Roaming\GameStash\gamestash.db
```

### Cloud Storage (Supabase)
- **claimed_games** table - Automatically syncs when user logs in
- **user_profiles** table - Syncs profile changes
- **Offline mode** - Works without internet, syncs when online

---

## Development & Updates

### Updating Version

1. Update version in `build_release.bat`:
   ```batch
   set VERSION=1.0.1
   ```

2. Update version in `setup.iss`:
   ```
   AppVersion=1.0.1
   OutputBaseFilename=GameStash_Installer_v1.0.1
   ```

3. Rebuild everything:
   ```bash
   .\build_release.bat
   iscc.exe setup.iss
   ```

4. Commit and push:
   ```bash
   git add .
   git commit -m "Version 1.0.1 - Bug fixes"
   git tag -a v1.0.1 -m "Release 1.0.1"
   git push origin main v1.0.1
   ```

5. Create new GitHub Release with new files

### CI/CD (Optional - Future)

You can set up GitHub Actions to auto-build releases:

```yaml
name: Build Release
on:
  push:
    tags:
      - 'v*'

jobs:
  build:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v2
      - name: Build Release
        run: .\build_release.bat
      - name: Build Installer
        run: iscc.exe setup.iss
      - name: Create Release
        uses: softprops/action-gh-release@v1
        with:
          files: |
            dist/GameStash_v*.zip
            dist/GameStash_Installer_*.exe
```

---

## Troubleshooting

### "Failed to create installer"
- Ensure Inno Setup is installed and in PATH
- Check `setup.iss` file exists in project root
- Verify `dist\GameStash_v1.0.0\` folder has all files

### "GameStash.exe failed to start"
- Check `%APPDATA%\AppData\Roaming\GameStash\debug.log` for errors
- Verify WebView2 runtime is installed (Windows 11 has it built-in)
- For Windows 10, WebView2 installer runs automatically

### "Database errors on install"
- Ensure `%APPDATA%\GameStash\` directory exists
- Delete `gamestash.db` and restart app for clean database

### "Supabase sync not working"
- Check credentials in code match your Supabase project
- Verify internet connection
- Check `debug.log` for API errors
- Ensure tables exist in Supabase

---

## Rollback / Uninstall

Users can uninstall via Windows Settings:
```
Settings → Apps → Apps & features → Game Stash → Uninstall
```

Or manually delete:
- Program: `C:\Program Files\GameStash\`
- Data: `%APPDATA%\GameStash\`

Admin can remove installer files after deployment.

---

## Security Notes

1. **Supabase Anon Key** - Currently public in code (OK for read-only client apps)
   - Consider using policies (`RLS`) to restrict data access
   - Never commit real secrets - use environment variables in production

2. **Database Passwords** - Password hashes use SHA-256 (consider bcrypt/argon2)

3. **OAuth** - Requires valid credentials in `oauth_config.json`

---

## Support & Updates

- **Bug Reports:** GitHub Issues
- **Features:** GitHub Discussions
- **Documentation:** README.md, SETUP.md in release
- **Updates:** Check GitHub Releases for new versions

---

**Your Game Stash app is production-ready!** 🚀

For questions or issues, see the GitHub repository.
