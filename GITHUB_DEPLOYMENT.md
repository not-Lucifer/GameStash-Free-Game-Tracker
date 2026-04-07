# GitHub Deployment Guide

This guide explains how to deploy Game Stash to GitHub Releases so users can easily download and install it.

## Prerequisites

1. GitHub account with a repository for Game Stash
2. Git installed on your computer
3. A release ready (built using `build_release.bat`)

## Step 1: Prepare Your Repository

### On GitHub:
1. Go to your GitHub repository
2. Click **Settings**
3. Ensure your repository is **public** (under "Danger zone" → Repository visibility)
4. Your repository URL should be: `https://github.com/YOUR_USERNAME/YOUR_REPO_NAME`

### On Your Computer:
```bash
cd d:\Projects\FreeGameTracekr
git init
git add .
git commit -m "Initial commit: Game Stash v1.0"
git branch -M main
git remote add origin https://github.com/YOUR_USERNAME/YOUR_REPO_NAME.git
git push -u origin main
```

## Step 2: Create a GitHub Release

### Option A: Using GitHub Web Interface (Easiest)

1. Go to your GitHub repository: `https://github.com/YOUR_USERNAME/YOUR_REPO_NAME`
2. Click **Releases** (on the right sidebar)
3. Click **Draft a new release**
4. Fill in:
   - **Tag version:** `v1.0.0`
   - **Release title:** `Game Stash v1.0.0 - Initial Release`
   - **Description:** (see template below)
5. **Upload assets:** Drag and drop `dist/GameStash_v1.0.0.zip`
6. Click **Publish release**

### Option B: Using Git Command Line

```bash
# Tag the current commit
git tag -a v1.0.0 -m "Game Stash v1.0.0 Release"

# Push tag to GitHub
git push origin v1.0.0
```

Then use the web interface to create the release and upload the ZIP.

## Step 3: Release Description Template

Copy and paste this into your GitHub release description:

```markdown
## Game Stash v1.0.0

**A free, open-source game giveaway tracker for Windows**

Tired of missing free games? Game Stash tracks free giveaways from Steam, Epic Games, GOG, and Itch.io automatically.

### What's New

- Email validation and secure user accounts
- Personal profiles with bio and preferences  
- SQLite database for offline-capable tracking
- OAuth integration for Google and Discord login
- WebView2-based modern UI

### System Requirements

- Windows 10 or Windows 11 (64-bit)
- No additional software needed (WebView2 pre-installed on Windows 11)

### Installation

#### Quick Install (Recommended)
1. Download `GameStash_v1.0.0.zip`
2. Extract to any folder
3. Right-click `install.bat` → **Run as administrator**
4. Game Stash appears in Windows Start Menu

#### Portable Mode
1. Download `GameStash_v1.0.0.zip`
2. Extract to any folder
3. Double-click `GameStash.exe`

**See `SETUP.md` inside the ZIP for detailed instructions and troubleshooting.**

### Download

👉 **[GameStash_v1.0.0.zip](https://github.com/YOUR_USERNAME/YOUR_REPO_NAME/releases/download/v1.0.0/GameStash_v1.0.0.zip)**

### Features

✅ Track free games from multiple platforms  
✅ Personal profiles with customization  
✅ Email validation and OAuth support  
✅ Local SQLite database (all data stays on your computer)  
✅ Completely free and open-source  
✅ No ads, no tracking, no telemetry  

### Binary Files Included

- `GameStash.exe` - Main application (2.87 MB)
- `index.html`, `style.css` - Web UI
- `install.bat` - Windows 10/11 installer with Start Menu integration
- `SETUP.md` - Detailed setup guide
- `README.md` - Quick reference
- DLL libraries (WebView2, SQLite3)

### Uninstall

Run `uninstall.bat` from `C:\Program Files\GameStash` or use Windows Settings → Apps.

### Source Code

This is a compiled release. For source code, see the repository main branch.

### License

Free to use, modify, and distribute (see LICENSE file).

### Support

For issues or feature requests, open an issue on GitHub.

---

**Enjoy finding amazing free games!** 🎮
```

## Step 4: Update Releases

When you build a new version:

1. Run `build_release.bat` to generate `dist/GameStash_v1.0.1.zip`
2. Increment version in `build_release.bat` (change `1.0.0` to `1.0.1`)
3. Create new Git commit:
   ```bash
   git add .
   git commit -m "Version 1.0.1 release"
   git tag -a v1.0.1 -m "Game Stash v1.0.1"
   git push origin main
   git push origin v1.0.1
   ```
4. Create new GitHub Release with new ZIP file

## Step 5: Announce Your Release

### Social Media
- Reddit: r/gaming, r/FreeGames, r/pcgaming
- Twitter/X: #gamedev #freeware #opensource
- Discord communities for game trackers

### Other Platforms
- Product Hunt
- AlternativeTo.net
- GitHub Trending

## Versioning Strategy

Use **Semantic Versioning**: `MAJOR.MINOR.PATCH`

- **MAJOR** (1.0.0 → 2.0.0): Breaking changes, new features
- **MINOR** (1.0.0 → 1.1.0): New features, backwards compatible
- **PATCH** (1.0.0 → 1.0.1): Bug fixes only

## Example Release Checklist

- [ ] Run `build_release.bat` successfully
- [ ] Test the installer (run `install.bat` on clean Windows)
- [ ] Test portable mode (extract and run GameStash.exe)
- [ ] Verify all files in ZIP: exe, DLLs, HTML, install script, docs
- [ ] Update version number in code (if needed)
- [ ] Create Git tag
- [ ] Create GitHub Release with description
- [ ] Upload ZIPfile  
- [ ] Post release announcement
- [ ] Share on social media

## Database Notes

Game data is stored in `gamestash.db`:
- **Installed**: `C:\Program Files\GameStash\gamestash.db`
- **Portable**: Same folder as `GameStash.exe`

Each user gets their own local database. No syncing to cloud by default.

## Troubleshooting

### ZIP won't create
- Ensure you have PowerShell installed (Windows 10+ has it by default)
- Try running as Administrator

### GitHub upload fails
- Check your internet connection
- Ensure your GitHub authentication is configured
- Try uploading manually through the web interface

### Users report installer issues
- Provide a troubleshooting section in your SETUP.md
- Make install.bat require admin privileges
- Include uninstall script for clean removal

## Next Steps

1. Customize the repository README.md
2. Add a LICENSE file (MIT, GPL-3.0, etc.)
3. Monitor GitHub Issues for user feedback
4. Plan v1.1 features based on user requests
5. Set up CI/CD for automated builds (optional)

---

**Your app is now ready for distribution!** 🚀
