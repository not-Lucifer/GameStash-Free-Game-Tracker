# Developer Reference - Credentials & Config Files

## Quick Start for Developers

### 1. Initial Setup
```bash
# Clone repository
git clone <repo>
cd FreeGameTracekr

# Create configs (copy examples)
copy oauth_config.json.example oauth_config.json
copy supabase_config.json.example supabase_config.json
```

### 2. Update Config Files
**oauth_config.json** - Add your Discord OAuth credentials
**supabase_config.json** - Add your Supabase details

### 3. Verify .gitignore
```bash
git check-ignore oauth_config.json     # Should return filename
git check-ignore supabase_config.json  # Should return filename
```

If not, these files will be committed accidentally!

## Config Files Reference

### supabase_config.json
```json
{
  "supabase": {
    "url": "https://your-project.supabase.co",
    "anon_key": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9..."
  }
}
```

**Get values from:** Supabase Dashboard > Settings > API > Project URL & Anon Key

### oauth_config.json
```json
{
  "discord": {
    "client_id": "YOUR_DISCORD_CLIENT_ID",
    "client_secret": "YOUR_DISCORD_CLIENT_SECRET",
    "redirect_uri": "http://localhost:8080/oauth/callback"
  }
}
```

**Get values from:** Discord Developer Portal > Applications > Your App > OAuth2

## Build System

### Build Locally
```bash
# Debug build
.\build.bat

# Release build with distribution
.\build_release.bat
```

**What build system does:**
- Compiles with MSVC (C++17)
- Links necessary libraries (SQLite3, WebView2, etc)
- **For release:** Copies configs to dist folder
- Creates portable ZIP package

### Important: config_path in main.cpp
```cpp
// When building with build_release.bat:
// 1. supabase_config.json must exist in project root BEFORE build
// 2. build_release.bat copies it to dist folder
// 3. At runtime, app looks for config in same directory as .exe

std::string exe_dir = get_exe_directory();
std::string config_path = exe_dir + "\\supabase_config.json";
```

## Making Changes to Credential Loading

### If you modify load_supabase_config():
1. Keep it AFTER read_file() function definition (line ordering matters)
2. Test locally: Ensure supabase_config.json loads correctly
3. Add error handling (MessageBox if missing)
4. Commit with message referencing security

### If you add new config options:
1. Add to supabase_config.json (example format)
2. Update load_supabase_config() to parse new values
3. Add corresponding global variable or struct field
4. Update documentation in this file

## Git Workflow

### Before Committing
1. Ensure supabase_config.json and oauth_config.json exist in .gitignore
   ```bash
   git status  # These files should NOT appear
   ```

2. Remove accidentally added configs
   ```bash
   git rm --cached supabase_config.json oauth_config.json
   git commit -m "remove: accidentally tracked credential files"
   ```

### Commit Message Format
```
security: Describe the change

- Specific detail 1
- Specific detail 2
- Why this matters
```

## Distribution Process

### Build for Release
```bash
.\build_release.bat
```

Output: `dist/GameStash_v1.0.0.zip`

**Includes:**
- GameStash.exe (optimized, GUI mode)
- All required DLLs
- index.html, style.css, assets
- supabase_config.json (credentials)
- install.bat (batch installer)
- setup.iss (Inno Setup script)
- Documentation

### Build Installer (if Inno Setup installed)
```bash
iscc.exe setup.iss
```

Output: `dist/GameStash_Setup_v1.0.0.exe`

## Credential Rotation (Emergency)

If credentials are exposed:

### Step 1: Rotate in Supabase
1. Go to Supabase Dashboard
2. Settings > API
3. Click "Rotate" next to Anon Key
4. New key generated immediately

### Step 2: Update Config File
1. Update supabase_config.json with new key
2. Rebuild: `.\build_release.bat`
3. Distribute new ZIP

### Step 3: Notify Users
- Provide new setup.exe or zip file
- Users replace supabase_config.json

### Step 4: Clean Git History
```bash
# If old key was ever committed
git filter-branch --force --index-filter ...  # Complex, consider re-creating repo
```

## Testing Offline Mode

The app works WITHOUT credentials:
1. Delete supabase_config.json
2. Run GameStash.exe
3. Shows warning dialog
4. Uses local SQLite only (no sync)

This is the expected behavior for disaster recovery.

## Troubleshooting

### "Could not load Supabase config" dialog
- supabase_config.json missing from same directory as .exe
- Invalid JSON format in config file
- Required fields missing (url, anon_key)

**Fix:** Verify supabase_config.json exists and is valid JSON

### Build fails with "identifier not found"
- Function definition after it's called (ordering issue)
- Header file missing (include for std::string, nlohmann/json)

**Fix:** Check #include statements and function declaration order

### Sync not working despite valid config
- Check if app can reach supabase_config_url (firewall?)
- Try manually: `curl https://your-project.supabase.co/rest/v1/claimed_games`
- Verify RLS policies allow anonymous access

**Fix:** Review Supabase logs and RLS policies

## Related Documentation
- [SECURITY.md](SECURITY.md) - Security overview
- [BUILD.md](BUILD.md) - Build system details
- [DEPLOYMENT.md](DEPLOYMENT.md) - Distribution guides
- [main.cpp](main.cpp) - Implementation reference
