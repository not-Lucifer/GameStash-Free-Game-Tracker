-- Game Stash Supabase Setup Script
-- Copy and paste this into Supabase SQL Editor

-- ✅ STEP 1: Create claimed_games table
CREATE TABLE IF NOT EXISTS claimed_games (
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

-- ✅ STEP 2: Create user_profiles table
CREATE TABLE IF NOT EXISTS user_profiles (
  id BIGSERIAL PRIMARY KEY,
  user_id TEXT UNIQUE NOT NULL,
  bio TEXT,
  country TEXT,
  favorite_platform TEXT,
  created_at TIMESTAMPTZ DEFAULT NOW(),
  updated_at TIMESTAMPTZ DEFAULT NOW()
);

-- ✅ STEP 3: Create indexes for faster queries
CREATE INDEX IF NOT EXISTS idx_claimed_games_user_id ON claimed_games(user_id);
CREATE INDEX IF NOT EXISTS idx_user_profiles_user_id ON user_profiles(user_id);

-- ✅ STEP 4: Enable Row Level Security (RLS)
ALTER TABLE claimed_games ENABLE ROW LEVEL SECURITY;
ALTER TABLE user_profiles ENABLE ROW LEVEL SECURITY;

-- ✅ STEP 5: Create RLS Policies for claimed_games
-- Allow anyone (including anon) to INSERT
CREATE POLICY "Allow anyone to insert claimed games"
  ON claimed_games FOR INSERT
  WITH CHECK (true);

-- Allow anyone to SELECT their own games (or all if user_id is available)
CREATE POLICY "Allow anyone to select claimed games"
  ON claimed_games FOR SELECT
  USING (true);

-- ✅ STEP 6: Create RLS Policies for user_profiles
-- Allow anyone to INSERT user profiles
CREATE POLICY "Allow anyone to insert user profiles"
  ON user_profiles FOR INSERT
  WITH CHECK (true);

-- Allow anyone to SELECT user profiles
CREATE POLICY "Allow anyone to select user profiles"
  ON user_profiles FOR SELECT
  USING (true);

-- Allow anyone to UPDATE user profiles
CREATE POLICY "Allow anyone to update user profiles"
  ON user_profiles FOR UPDATE
  USING (true)
  WITH CHECK (true);

-- ✅ Verification queries (run these to confirm tables created)
-- SELECT COUNT(*) FROM claimed_games;
-- SELECT COUNT(*) FROM user_profiles;
