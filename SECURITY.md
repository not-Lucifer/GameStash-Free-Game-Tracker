# Security Practices - Game Stash

## Credential Management

### Supabase Credentials

**Current Approach (Secure):**
- Credentials stored in `supabase_config.json` (NOT in source code)
- File loaded at runtime via `load_supabase_config()`
- Anon key is public-facing (designed for client apps)
- Protected from git commits via `.gitignore`

**File Format:**
```json
{
  "supabase": {
    "url": "https://your-project.supabase.co",
    "anon_key": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9..."
  }
}
```

### Why This Approach is Secure

1. **Keys Not in Source Code**
   - No credentials in main.cpp, CMakeLists.txt, or build files
   - If GitHub repo is leaked, credentials are safe
   - No compiled binaries contain hardcoded secrets

2. **Easier Credential Rotation**
   - Update supabase_config.json locally
   - No recompilation needed
   - Distribute new config to users

3. **Git Protection**
   - `.gitignore` prevents supabase_config.json from being committed
   - Double-check before each commit

4. **Anon Key Safety**
   - Supabase anon keys ARE designed to be public
   - Client apps must use anon keys (not service role)
   - Data protection via Supabase Row Level Security (RLS)

## Best Practices

### ✅ DO:
- Keep supabase_config.json in distribution packages
- Use Row Level Security on all Supabase tables
- Rotate anon key periodically (Settings > API > Anon Key > Rotate)
- Test offline functionality (app should work without internet)
- Keep Supabase policies updated

### ❌ DON'T:
- Commit supabase_config.json to git
- Hardcode credentials in source files
- Share actual config files in public repositories
- Use service role key in client app (only anon key)
- Trust client-side validation alone (use RLS policies)

## Supabase Row Level Security (RLS)

**Critical:** Configure RLS policies on all tables to control data access.

Example: Only allow users to see their own claimed games
```sql
CREATE POLICY "Users can only see their own claimed games"
ON claimed_games
FOR SELECT
USING (auth.uid()::text = user_id);
```

## OAuth Credentials

Similarly handled via `oauth_config.json`:
- Not committed to git
- Contains Discord/OAuth provider keys
- Loaded at startup

## Distribution

**What gets distributed:**
- `supabase_config.json` (include)
- `oauth_config.json` (include)
- Source code (safe, no secrets)

**What never gets distributed:**
- Service role keys (from Supabase)
- OAuth client secrets (if you decide to use hidden keys)
- Private api keys

## Incident Response

If credentials are accidentally exposed:

1. **Immediate:**
   - Rotate the Supabase anon key (Settings > API)
   - Delete exposed credentials from git history

2. **Code:**
   - Update config locally
   - Rebuild and redistribute

3. **Prevention:**
   - Check .gitignore is working: `git check-ignore supabase_config.json`
   - Review git history to ensure no commits contain keys

## Testing Credential Loading

The app checks for `supabase_config.json` on startup:
- If found: Loads credentials, Supabase sync enabled
- If missing: Shows warning dialog, app operates offline only
- Required file location: Same directory as GameStash.exe

## Questions?

Review these related files:
- [BUILD.md](BUILD.md) - Build system configuration
- [DEPLOYMENT.md](DEPLOYMENT.md) - Distribution process
- [QUICKSTART.md](QUICKSTART.md) - User setup guide
