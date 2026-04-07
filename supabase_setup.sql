-- Game Stash Supabase Setup Script
-- Copy and paste this into Supabase SQL Editor

-- Create claimed_games table
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

-- Create user_profiles table
CREATE TABLE IF NOT EXISTS user_profiles (
  id BIGSERIAL PRIMARY KEY,
  user_id TEXT UNIQUE NOT NULL,
  bio TEXT,
  country TEXT,
  favorite_platform TEXT,
  created_at TIMESTAMPTZ DEFAULT NOW(),
  updated_at TIMESTAMPTZ DEFAULT NOW()
);

-- Create indexes for faster queries
CREATE INDEX IF NOT EXISTS idx_claimed_games_user_id ON claimed_games(user_id);
CREATE INDEX IF NOT EXISTS idx_user_profiles_user_id ON user_profiles(user_id);

-- Optional: Enable Row Level Security (RLS) for security
-- ALTER TABLE claimed_games ENABLE ROW LEVEL SECURITY;
-- ALTER TABLE user_profiles ENABLE ROW LEVEL SECURITY;

-- Optional: RLS Policies (uncomment if RLS enabled)
-- CREATE POLICY "Users can view their own games"
--   ON claimed_games FOR SELECT
--   USING (auth.uid()::text = user_id);
-- 
-- CREATE POLICY "Users can insert their own games"
--   ON claimed_games FOR INSERT
--   WITH CHECK (auth.uid()::text = user_id);
--
-- CREATE POLICY "Users can update their own profile"
--   ON user_profiles FOR UPDATE
--   USING (auth.uid()::text = user_id);
--
-- CREATE POLICY "Users can insert their own profile"
--   ON user_profiles FOR INSERT
--   WITH CHECK (auth.uid()::text = user_id);

-- Verification queries (run these to confirm tables created)
-- SELECT COUNT(*) FROM claimed_games;
-- SELECT COUNT(*) FROM user_profiles;
