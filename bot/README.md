# C3 Lichess Bot

Run C3 as a Lichess bot using the [lichess-bot](https://github.com/lichess-bot-devs/lichess-bot) bridge.

## Quick Start

Run the automated setup script:

```bash
cd bot
./setup.sh
```

This will:
1. Build C3 in release mode
2. Clone and install lichess-bot (using `uv`)
3. Configure everything
4. Prompt for your Lichess API token
5. Optionally start the bot

## Prerequisites

- [uv](https://docs.astral.sh/uv/) - install with `curl -LsSf https://astral.sh/uv/install.sh | sh`
- git
- cmake
- Lichess account (must not have played any human games)
- OAuth token with `bot:play` scope

## Manual Setup

### 1. Build C3

```bash
cmake --preset release && cmake --build --preset release
```

### 2. Clone lichess-bot

```bash
cd bot
git clone https://github.com/lichess-bot-devs/lichess-bot
cd lichess-bot
uv venv --python 3.12 .venv
uv pip install -r requirements.txt
```

### 3. Create Lichess Bot Account

1. Create a new Lichess account at https://lichess.org/signup
2. **Important**: Do NOT play any games on this account before upgrading
3. Generate an API token at https://lichess.org/account/oauth/token/create
   - Check the `bot:play` scope
   - Save the token securely

### 4. Configure

Copy the config template:

```bash
cp ../config.yml config.yml
```

Set your token via environment variable:

```bash
export LICHESS_BOT_TOKEN="lip_your_token_here"
```

### 5. Upgrade Account to Bot

This is irreversible - the account can never play as a human again:

```bash
.venv/bin/python lichess-bot.py -u
```

### 6. Run the Bot

```bash
.venv/bin/python lichess-bot.py
```

Add `-v` for verbose output (shows engine thinking).

## Configuration

Edit `config.yml` to customize behavior. Key settings:

| Setting | Description | Default |
|---------|-------------|---------|
| `engine.uci_options.Hash` | Transposition table size (MB), 1-4096 | 256 |
| `move_overhead` | Time buffer for communication (ms) | 1000 |
| `challenge.time_controls` | Time controls to accept | bullet, blitz, rapid, classical |
| `challenge.modes` | casual, rated, or both | both |

`Hash` is paid for once, not once per move. The transposition table belongs to
the engine and lives across moves, so it is allocated when the bridge sends
`setoption name Hash` and cleared (not reallocated) at `ucinewgame`. A large
table therefore costs startup time rather than thinking time, and what it holds
about the position carries over into the next move's search.

## Troubleshooting

### Bot times out

Increase `move_overhead` in config.yml — network latency is what this buffer is
for, and it varies.

C3's own time handling is two limits rather than one. The soft limit is what the
move is worth and is only checked between search iterations; the hard limit is
three times that, capped by what is left on the clock, and is the one the search
polls while it is running. On top of that the search aims to stop 5 ms before the
hard limit (`TIME_SAFETY_MARGIN`), because it consults the clock only every few
hundred nodes and still has to write its `bestmove` afterwards. That margin
covers the engine's own overshoot, not the round trip to Lichess, which is why
`move_overhead` still has a job to do.

### Engine not found

Ensure the engine binary exists:

```bash
ls -la ../build-release/c3
```

### Token issues

Set the token via environment variable:

```bash
export LICHESS_BOT_TOKEN="lip_your_token_here"
.venv/bin/python lichess-bot.py
```

## Running in Production

For a persistent bot, use a process manager:

```bash
# Using nohup
cd bot/lichess-bot
nohup .venv/bin/python lichess-bot.py &

# Using screen/tmux for interactive monitoring
screen -S c3bot
.venv/bin/python lichess-bot.py -v
# Ctrl+A, D to detach
```

## Links

- [lichess-bot documentation](https://github.com/lichess-bot-devs/lichess-bot/wiki)
- [Lichess Bot API](https://lichess.org/api#tag/Bot)
- [uv documentation](https://docs.astral.sh/uv/)
