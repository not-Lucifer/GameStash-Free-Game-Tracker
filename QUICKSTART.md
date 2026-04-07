# Quick Start Guide - Deployment in 15 Minutes

## ⚡ TL;DR - Get Live in 15 Minutes

### 1️⃣ Supabase Setup (5 min)

**Step 1:** Go to [supabase.com](https://supabase.com)

**Step 2:** Click **New Project**
- Name: `GameStash`
- Database: Keep defaults
- Create project

**Step 3:** Wait for project to be ready (grab coffee ☕)

**Step 4:** Get your credentials:
- Go to **Settings → API**
- Copy: `Project URL` (looks like `https://xxx.supabase.co`)
- Copy: `anon public` key

**Save these for step 2!**

---

### 2️⃣ Update Code (2 min)

Open `main.cpp` and go to line ~44:

Find:
```cpp
const std::string SUPABASE_URL = "https://dzhsobheihoickgvwyqt.supabase.co";
const std::string SUPABASE_ANON_KEY = "sb_anon_eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
```

Replace with YOUR credentials from step 1:
```cpp
const std::string SUPABASE_URL = "https://YOUR_PROJECT_ID.supabase.co";
const std::string SUPABASE_ANON_KEY = "your_anon_key_from_settings";
```

Save the file.

---

### 3️⃣ Create Database Tables (3 min)

In Supabase dashboard:
1. Click **SQL Editor** (left sidebar)
2. Click **New Query**
3. Paste this and click **Run:**

```sql
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

CREATE TABLE user_profiles (
  id BIGSERIAL PRIMARY KEY,
  user_id TEXT UNIQUE NOT NULL,
  bio TEXT,
  country TEXT,
  favorite_platform TEXT,
  created_at TIMESTAMPTZ DEFAULT NOW()
);
```

---

### 4️⃣ Build Release (3 min)

Open PowerShell in project folder and run:

```bash
.\build_release.bat
```

Outputs:
- ✅ `dist\GameStash_v1.0.0.zip` (portable)
- ✅ `dist\GameStash_v1.0.0\` (all files)

---

### 5️⃣ Build Installer (2 min)

```bash
iscc.exe setup.iss
```

Output:
- ✅ `dist\GameStash_Installer_v1.0.0.exe` (professional installer)

**Don't have Inno Setup?**
- Download: https://jrsoftware.org/isdl.php
- Install it
- Try again

---

### 6️⃣ Test It! (0 min, optional)

**Option A - Test Portable:**
```
1. Extract GameStash_v1.0.0.zip
2. Double-click GameStash.exe
3. Check: C:\Users\[YOU]\AppData\Roaming\GameStash\gamestash.db exists
```

**Option B - Test Installer:**
```
1. Run GameStash_Installer_v1.0.0.exe
2. Click Install
3. Launch from Start Menu
4. Verify works
5. Run uninstaller to test removal
```

---

## 🚀 Deploy to GitHub

### Create GitHub Repo

```bash
cd d:\Projects\FreeGameTracekr

git init
git add .
git commit -m "Game Stash v1.0.0 with Supabase backend"
git branch -M main
git remote add origin https://github.com/YOUR_USERNAME/FreeGameStash.git
git push -u origin main
```

### Create GitHub Release

1. Go to: https://github.com/YOUR_USERNAME/FreeGameStash/releases
2. Click **Draft a new release**
3. Fill in:
   - Tag: `v1.0.0`
   - Title: `Game Stash v1.0.0 - Release`
   - Description: (see template in [DEPLOYMENT.md](DEPLOYMENT.md))

4. Upload files:
   - `dist\GameStash_Installer_v1.0.0.exe`
   - `dist\GameStash_v1.0.0.zip`

5. Click **Publish**

**Done!** 🎉

---

## ✅ What You Now Have

```
✅ Professional Windows installer
✅ Cloud backup via Supabase
✅ Multi-user Windows support
✅ AppData storage
✅ GitHub-ready release
✅ 32+ games tracked
✅ User profiles
✅ Email validation
✅ OAuth support
```

---

## 🐛 Quick Troubleshooting

| Problem | Solution |
|---------|----------|
| "iscc.exe not found" | Install Inno Setup from jrsoftware.org |
| "Can't connect to Supabase" | Check credentials in main.cpp |
| "Database error on launch" | Delete `%APPDATA%\GameStash\gamestash.db` and retry |
| "Installer won't run" | Check `setup.iss` file exists and credentials are right |

---

## 📞 Need Help?

See detailed guides:
- **Full Setup:** [DEPLOYMENT.md](DEPLOYMENT.md)
- **Detailed Checklist:** [DEPLOYMENT_CHECKLIST.md](DEPLOYMENT_CHECKLIST.md)
- **GitHub:** [GITHUB_DEPLOYMENT.md](GITHUB_DEPLOYMENT.md)

---

**Time: ~15 minutes**  
**Result: Live production app on GitHub!** 🚀

Let's go! 💪
