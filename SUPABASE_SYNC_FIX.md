# 🔧 Supabase Sync Implementation - COMPLETE GUIDE

Your app now has **Cloud-First Supabase integration**! But you need to update the Supabase database schema. Follow these steps:

---

## ⚠️ CRITICAL: Update Supabase Tables (Required!)

Your current tables don't have the necessary **Row Level Security (RLS)** policies that allow writes via anon key.

### Step 1: Delete Old Tables

1. Go to: **https://dzhsobheihoickgvwyqt.supabase.co**
2. Click **SQL Editor** (left sidebar)
3. Click **+ New Query**
4. Paste and run:

```sql
DROP TABLE IF EXISTS claimed_games CASCADE;
DROP TABLE IF EXISTS user_profiles CASCADE;
```

5. Click **Run** (ignore any errors)

---

### Step 2: Create New Tables with RLS Policies

1. Click **+ New Query**
2. Copy **ENTIRE CONTENT** from: `supabase_setup.sql` in your project
3. Paste into SQL Editor
4. Click **Run** ✅

**Expected output:**
```
✅ CREATE TABLE
✅ CREATE INDEX
✅ ALTER TABLE
✅ CREATE POLICY
```

---

## ✅ Verify Setup

### Test from App

1. Run: `.\build\GameStash.exe`
2. Open DevTools (F12)
3. In console, call: `testSupabaseConnection()`
4. Check response for errors

### Manual Test (Optional)

In Supabase SQL Editor, run:

```sql
-- Check if tables exist
SELECT * FROM information_schema.tables 
WHERE table_name IN ('claimed_games', 'user_profiles');

-- Check Row Level Security
SELECT * FROM pg_stat_user_tables 
WHERE relname IN ('claimed_games', 'user_profiles');
```

---

## 🚀 How It Works Now

### Registration
- ✅ User creates account with email/password
- ✅ User profile automatically created in Supabase
- ✅ Stored locally in SQLite for offline use

### Login
- ✅ User logs in with email/password
- ✅ Games + profile fetched from Supabase
- ✅ Synced to local SQLite

### Claim Game
- ✅ Game claim saved to **Supabase FIRST**
- ✅ Then saved to local SQLite
- ✅ Data syncs across devices!

### Update Profile
- ✅ Changes saved to **Supabase FIRST**
- ✅ Then saved to local SQLite

---

## 🧪 Testing Checklist

- [ ] Run app: `.\build\GameStash.exe`
- [ ] **Register** new account (e.g., `test@example.com`)
- [ ] Check Supabase: https://dzhsobheihoickgvwyqt.supabase.co
  - [ ] New row in `user_profiles` table ✅
- [ ] **Add/Claim a game** in the app
- [ ] Check Supabase `claimed_games` table
  - [ ] New row appears ✅
- [ ] **Update profile** (bio, country, etc.)
- [ ] Check Supabase `user_profiles`
  - [ ] Profile updated ✅

---

## 🐛 Troubleshooting

### "Data still not in Supabase"

Check debug log: `%APPDATA%\GameStash\gamestash.db` or `build/debug.log`

Look for lines starting with:
- ✅ `Supabase config loaded successfully` - config OK
- ❌ `Missing credentials` - config not loading
- ❌ `Supabase add claimed game failed` - POST failed

### "Connection test failed"

1. Verify Supabase project exists: https://dzhsobheihoickgvwyqt.supabase.co
2. Verify credentials in `supabase_config.json`
3. Verify RLS policies created (Step 2 above)
4. Check Supabase status: https://status.supabase.io

### "POST returns error"

Most common cause: **Missing or incorrect RLS policies**

Re-run SQL from Step 2 above to recreate policies.

---

## 📝 Key Changes Made

| Component | Before | After |
|-----------|--------|-------|
| **User ID** | Database integer ID | Email (for Supabase) |
| **Data Write** | Local SQLite only | ✅ Supabase FIRST + local SQLite |
| **Data Read** | Local SQLite only | ✅ Supabase FIRST (fallback local) |
| **RLS Policies** | None | ✅ Allow anon key access |
| **Offline Support** | N/A | ✅ Full local SQLite fallback |

---

## 🎯 Architecture

```
┌─────────────────┐
│  GameStash App  │
└────────┬────────┘
         │
    ┌────┴─────┐
    │           │
┌───▼──┐  ┌────▼───────────┐
│SQLite│  │ Supabase Cloud  │
│(Local)  │ (Primary Source)│
└───────┘ └────────────────┘
    ▲           │
    └───────────┘
    Offline/Fallback
```

All operations now:
1. **Write to Supabase first** (cloud-first)
2. **Fallback to local SQLite** if offline
3. **Sync on login** to keep in sync

---

## ❓ Questions?

Check the debug log for detailed error messages:
```
Windows: %APPDATA%\GameStash\gamestash.db
Linux: ~/.gamestash/gamestash.db
```

Look for lines with 🔴❌ prefixes in `debug.log`.
