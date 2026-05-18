# GameOS v2 — what changed

## File layout (Arduino IDE auto-includes siblings)
```
GameOS/
├── GameOS.ino       # core: boot, screens, registry-driven launcher, PONG, SNAKE
├── games_pack1.h    # easy:    TETRIS, BREAKOUT, 2048, FLAPPY, TIC-TAC-TOE,
│                    #          WHACK-A-MOLE, SIMON, DINO, PONG-2P, REACTION
├── games_pack2.h    # medium:  MINESWEEPER, MEMORY, LIGHTS-OUT, CONNECT-4,
│                    #          15-PUZZLE, INVADERS, ASTEROIDS, DOODLE-DODGE,
│                    #          FROGGER, MAZE
└── games_pack3.h    # show:    TRON, MATCH-3, PINBALL, WORDLE, SUDOKU,
                     #          CHECKERS, AIR-HOCKEY, PLATFORMER, TOWER-STACK, CROSSY
```
Put all 4 files in the same Arduino sketch folder named `GameOS`, open
`GameOS.ino`, and compile as before.

## PONG — what's new (was "dookie")
- **Spin physics**: ball's vertical velocity is bent by where it hits the paddle
  (`rel = (ball - paddle_center)/half`), so you can angle shots.
- **Speed-up volleys**: ball speed +0.35 each paddle hit, capped at 9.0 px/frame.
- **Smart AI with reaction lag**: re-targets every 3 frames + jitter so it's
  beatable, but predictive enough to be fun.
- **Trail**: 8-frame ghost trail (4 brightness steps) — pure neon CRT feel.
- **Score flash**: cyan/magenta strip at top+bottom on every hit/score.
- **Sharper paddles** (rounded), centre dotted line, deeper black bg.
- Drag anywhere on the canvas (not just left half) — paddle follows finger.

## Launcher — what's new
- **Registry-driven** (`GAMES[]` in `GameOS.ino`). Adding a 31st game is now
  one struct line + one `open_*` function.
- **Categories**: `BUILT-IN`, `QUICK PLAY`, `PUZZLES & ARCADE`, `SHOWPIECES`
  with thin colored bars + uppercase section labels.
- **Card upgrades**: shadow glow on press (uses each game's accent color),
  zoom on press, larger icon disc, chevron right.
- **Smooth momentum scroll** with thin cyan scrollbar that auto-hides.
- **Game-count badge** in top-right (`32 GAMES`).
- **Bottom fade overlay** so the last card doesn't hard-cut.

## Game helpers introduced
All 30 games use the same 5 helpers (see `GameOS.ino` top section):
- `game_screen_base(title, accent)` — black screen + title pill + score label
- `game_add_canvas(s, w, h)` — allocs LVGL canvas in PSRAM (falls back to DRAM)
- `game_add_exit(s, accent)` — bottom-pinned exit pill that cleans up timer+buf
- `game_canvas_rect / circle / clear` — drawing shortcuts
- `game_set_score(fmt, ...)` — printf into header label
- `game_cleanup()` — frees timer + canvas buffer (single source of truth)

Net result: each game is ~30–80 lines, no leaks, identical UX.

## Notes
- Touch swipe detection at the screen level is now suppressed while a game
  is active (see the `g_transient_screen==nullptr` guard in `lv_touch_gesture`),
  so in-game gestures don't accidentally page back to the watch face.
- Wordle dictionary is 16 words baked in (no SPIFFS dependency). Easy to
  swap for an SD-card list later by reading `/WATCH/games/wordle.txt`.
- Sudoku ships with one fixed puzzle (the classic) — clean tap-to-select +
  number pad. Validation is left simple on purpose.
