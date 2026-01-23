#!/usr/bin/env bash
# C3 Lichess Bot Setup Script
# This script builds C3, installs lichess-bot, and configures everything

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
C3_ROOT="$(dirname "$SCRIPT_DIR")"
BOT_DIR="$SCRIPT_DIR/lichess-bot"

echo "=== C3 Lichess Bot Setup ==="
echo ""

# Check prerequisites
command -v uv >/dev/null 2>&1 || { echo "Error: uv is required. Install with: curl -LsSf https://astral.sh/uv/install.sh | sh"; exit 1; }
command -v git >/dev/null 2>&1 || { echo "Error: git is required but not installed."; exit 1; }
command -v cmake >/dev/null 2>&1 || { echo "Error: cmake is required but not installed."; exit 1; }

echo "Found uv $(uv --version)"

# Step 1: Build C3
echo ""
echo "=== Step 1: Building C3 (release mode) ==="
cd "$C3_ROOT"
if [ ! -f "build-release/c3" ]; then
    cmake --preset release
    cmake --build --preset release
    echo "C3 built successfully at build-release/c3"
else
    echo "C3 already built at build-release/c3"
fi

# Step 2: Clone lichess-bot
echo ""
echo "=== Step 2: Setting up lichess-bot ==="
if [ ! -d "$BOT_DIR" ]; then
    git clone https://github.com/lichess-bot-devs/lichess-bot "$BOT_DIR"
    echo "Cloned lichess-bot"
else
    echo "lichess-bot already exists, updating..."
    cd "$BOT_DIR"
    git pull
fi

# Step 3: Set up venv and install dependencies with uv
echo ""
echo "=== Step 3: Installing Python dependencies with uv ==="
cd "$BOT_DIR"
uv venv --python 3.12 .venv
uv pip install -r requirements.txt
echo "Dependencies installed"

# Step 4: Copy config
echo ""
echo "=== Step 4: Configuring ==="
if [ ! -f "$BOT_DIR/config.yml" ]; then
    cp "$SCRIPT_DIR/config.yml" "$BOT_DIR/config.yml"
    echo "Config copied to $BOT_DIR/config.yml"
else
    echo "Config already exists at $BOT_DIR/config.yml"
fi

# Step 5: Token setup
echo ""
echo "=== Step 5: API Token ==="
if [ -z "$LICHESS_BOT_TOKEN" ]; then
    echo "No LICHESS_BOT_TOKEN environment variable found."
    echo ""
    echo "To get a token:"
    echo "  1. Go to https://lichess.org/account/oauth/token/create"
    echo "  2. Check 'Play as a bot' (bot:play scope)"
    echo "  3. Generate and copy the token"
    echo ""
    read -p "Enter your Lichess API token (or press Enter to skip): " TOKEN
    if [ -n "$TOKEN" ]; then
        export LICHESS_BOT_TOKEN="$TOKEN"
        echo ""
        echo "Token set for this session."
        echo "To persist, add to your shell profile:"
        echo "  export LICHESS_BOT_TOKEN=\"$TOKEN\""
    fi
else
    echo "LICHESS_BOT_TOKEN is set"
fi

echo ""
echo "=== Setup Complete ==="
echo ""
echo "Files created:"
echo "  - $C3_ROOT/build-release/c3 (engine binary)"
echo "  - $BOT_DIR/ (lichess-bot installation)"
echo "  - $BOT_DIR/config.yml (bot configuration)"
echo ""

# Helper function to run lichess-bot with the venv
run_bot() {
    cd "$BOT_DIR"
    .venv/bin/python lichess-bot.py "$@"
}

if [ -z "$LICHESS_BOT_TOKEN" ]; then
    echo "Next steps:"
    echo "  1. Set LICHESS_BOT_TOKEN environment variable"
    echo "  2. Run: cd $BOT_DIR && .venv/bin/python lichess-bot.py -u  (to upgrade account)"
    echo "  3. Run: cd $BOT_DIR && .venv/bin/python lichess-bot.py     (to start bot)"
else
    echo "Ready to run!"
    echo ""
    read -p "Upgrade Lichess account to bot? (IRREVERSIBLE) [y/N]: " UPGRADE
    if [ "$UPGRADE" = "y" ] || [ "$UPGRADE" = "Y" ]; then
        run_bot -u
    fi
    echo ""
    read -p "Start the bot now? [y/N]: " START
    if [ "$START" = "y" ] || [ "$START" = "Y" ]; then
        echo "Starting bot... (Ctrl+C to stop)"
        run_bot -v
    else
        echo ""
        echo "To start the bot later:"
        echo "  cd $BOT_DIR && .venv/bin/python lichess-bot.py"
    fi
fi
