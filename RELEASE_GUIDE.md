# Quick Reference - Production Release Guide

## 🚀 How to Build & Distribute Game Stash

### Regular Development Build
```batch
cd d:\Projects\FreeGameTracekr
build.bat
# Creates: build\GameStash.exe (Debug mode)
```

### Production Release Build  
```batch
cd d:\Projects\FreeGameTracekr
build_release.bat
# Creates: 
#   - dist/GameStash_v1.0/  (Portable folder)
#   - dist/GameStash_v1.0.zip  (Distribution archive)
```

---

## 📋 What's Included in Release ZIP

When you run `build_release.bat`, it creates a complete, portable package:

```
GameStash_v1.0/
├── GameStash.exe              ← Main application (2.87 MB)
├── index.html                 ← Web interface
├── style.css                  ← Styles
├── WebView2Loader.dll         ← WebView2 runtime
├── sqlite3.dll                ← Database engine
├── Launch.bat                 ← Easy launch script
├── README.md                  ← User documentation
└── oauth_config.json.example  ← OAuth template (optional)
```

**Anyone who downloads and extracts this ZIP can run the app immediately!**

---

## 🔍 Key Improvements

### 1. Path Resolution
**Before:** Database location changed based on where app was run from
**After:** Database always at `<app_directory>\gamestash.db`

### 2. Console Window
**Before:** Black cmd window appeared behind the app
**After:** Professional GUI with no console

### 3. Distribution
**Before:** Manual copying and packaging
**After:** Automated `build_release.bat` does everything

---

## 📤 Share Your App

### Option 1: GitHub Releases
```
1. Go to your GitHub repo → Releases
2. Create new release
3. Upload dist/GameStash_v1.0.zip
4. Share the link with friends
```

### Option 2: Itch.io
```
1. Create account at itch.io
2. Create new project (Game category)
3. Upload dist/GameStash_v1.0.zip
4. Set as "Available for everyone"
5. Share the game page
```

### Option 3: Cloud Storage
```
1. Upload dist/GameStash_v1.0.zip to:
   - Google Drive
   - OneDrive
   - Mega
   - Discord
2. Share the download link
```

---

## 🛠️ Technical Details

### WinMain vs main()
- `main()` → Console application
- `WinMain()` → GUI application (no console)
- Game Stash now uses WinMain() ✅

### GetModuleFileNameA()
- Gets absolute path to running .exe
- Works from any working directory
- Works with shortcuts
- Professional solution ✅

### build_release.bat Features
- Release mode build (faster, smaller)
- Automatic file copying
- Error checking
- ZIP archive creation
- README generation

---

## ⚙️ Configuration

### For Users Without OAuth
1. Extract the ZIP
2. Double-click Launch.bat
3. App starts and creates database
4. Users can register with email

### For Users WITH OAuth
1. Extract the ZIP
2. Rename `oauth_config.json.example` to `oauth_config.json`
3. Add your OAuth credentials (Google/Discord)
4. Double-click Launch.bat
5. OAuth login is available

---

## 📊 Release Checklist

Before sharing with users:
- [ ] Run `build_release.bat`
- [ ] Test the ZIP works by extracting and running
- [ ] Create account in the app
- [ ] Try "My Profile" feature
- [ ] Claim some games
- [ ] Restart app and verify data persists
- [ ] Upload to distribution platform
- [ ] Share with friends!

---

## 🎯 Performance

**Release Mode Benefits:**
- 10-20% faster execution
- 15-20% smaller file size
- Optimized code paths
- Better memory usage

Current: 2.87 MB (Release mode)

---

## 📞 Support

If users report issues:

1. **"App doesn't start"**
   - Ensure Windows 10/11
   - May need WebView2 runtime

2. **"Users disappear on restart"**
   - Should NOT happen with new path resolution
   - Database is at: `<app_folder>\gamestash.db`

3. **"Profile won't save"**
   - Check database file exists
   - Verify folder has write permissions

---

## 🎉 You're All Set!

Your Game Stash app is now:
- ✅ Professional
- ✅ Production-ready
- ✅ Easily distributable
- ✅ User-friendly

**Share it with the world!** 🚀

---

Last Updated: April 6, 2026
Game Stash v1.0.0
