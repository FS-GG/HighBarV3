#!/usr/bin/env bash
# T018 — shared spring-headless launcher used by BUILD.md step 9 + the
# acceptance suite. Validates the engine binary's SHA matches the pin
# in data/config/spring-headless.pin, exports SPRING_DATADIR +
# XDG_RUNTIME_DIR, launches the engine, optionally plumbs the
# coordinator endpoint, and writes the engine PID to a file the caller
# can wait on or kill.
#
# Usage:
#   _launch.sh \
#       --start-script <path> \
#       --plugin-so    <path>           # optional, install the .so first
#       --engine       <path>           # default: ~/.local/state/.../spring-headless
#       --log          <path>           # default: $TMPDIR/highbar-launch.log
#       --pid-file     <path>           # default: $TMPDIR/highbar-launch.pid
#       --coordinator  <uri>            # optional, sets HIGHBAR_COORDINATOR
#       --writedir     <path>           # default: $HOME/.local/state/Beyond All Reason
#       --runtime-dir  <path>           # default: $TMPDIR/hb-run
#       --window-mode  <mode>           # optional for graphical spring: windowed|borderless|fullscreen
#       --window-width <px>             # optional for graphical spring
#       --window-height <px>            # optional for graphical spring
#       --mouse-capture <bool>          # optional for graphical spring
#       --viewer-only <bool>            # optional for graphical spring join client; skips HighBar host bootstrap cleanup
#
# Exits 0 if the engine starts and writes a PID file; 77 if a
# prerequisite is missing (engine binary, pin file, mismatched SHA);
# 1 on any other failure.

set -euo pipefail

START_SCRIPT=""
PLUGIN_SO=""
ENGINE=""
LOG=""
PID_FILE=""
COORDINATOR=""
WRITEDIR="${HOME}/.local/state/Beyond All Reason"
RUNTIME_DIR=""
PHASE_MODE="1"
ENABLE_BUILTIN=""
WINDOW_MODE="windowed"
WINDOW_WIDTH="1920"
WINDOW_HEIGHT="1080"
MOUSE_CAPTURE="false"
VIEWER_ONLY="false"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --start-script) START_SCRIPT="$2"; shift 2 ;;
        --plugin-so)    PLUGIN_SO="$2";    shift 2 ;;
        --engine)       ENGINE="$2";       shift 2 ;;
        --log)          LOG="$2";          shift 2 ;;
        --pid-file)     PID_FILE="$2";     shift 2 ;;
        --coordinator)  COORDINATOR="$2";  shift 2 ;;
        --writedir)     WRITEDIR="$2";     shift 2 ;;
        --runtime-dir)  RUNTIME_DIR="$2";  shift 2 ;;
        --phase)        PHASE_MODE="$2";   shift 2 ;;
        --enable-builtin) ENABLE_BUILTIN="$2"; shift 2 ;;
        --window-mode)  WINDOW_MODE="$2"; shift 2 ;;
        --window-width) WINDOW_WIDTH="$2"; shift 2 ;;
        --window-height) WINDOW_HEIGHT="$2"; shift 2 ;;
        --mouse-capture) MOUSE_CAPTURE="$2"; shift 2 ;;
        --viewer-only) VIEWER_ONLY="$2"; shift 2 ;;
        -h|--help)
            sed -n '/^# Usage:/,/^$/p' "$0" >&2
            exit 0
            ;;
        *) echo "_launch.sh: unknown arg: $1" >&2; exit 1 ;;
    esac
done

REPO_ROOT="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/../.." && pwd)"
PIN_FILE="$REPO_ROOT/data/config/spring-headless.pin"
LUA_GADGET_SRC_DIR="$REPO_ROOT/tests/headless/LuaRules/Gadgets"
LUA_WIDGET_SRC_DIR="$REPO_ROOT/tests/headless/LuaUI/Widgets"
DEFAULT_PLUGIN_SO="$REPO_ROOT/build/libSkirmishAI.so"

# --- prerequisite checks (exit 77 on missing) --------------------------------

if [[ ! -f "$PIN_FILE" ]]; then
    echo "_launch.sh: pin file missing at $PIN_FILE" >&2
    exit 77
fi

PIN_SHA="$(grep -E '^sha256[[:space:]]*=' "$PIN_FILE" | sed 's/.*"\([^"]*\)".*/\1/')"
PIN_RELEASE="$(grep -E '^release_id[[:space:]]*=' "$PIN_FILE" | sed 's/.*"\([^"]*\)".*/\1/')"

if [[ -z "$ENGINE" ]]; then
    ENGINE="$WRITEDIR/engine/$PIN_RELEASE/spring-headless"
fi
if [[ ! -x "$ENGINE" ]]; then
    echo "_launch.sh: engine binary not found at $ENGINE" >&2
    exit 77
fi

ENGINE_BASENAME="$(basename "$ENGINE")"
PIN_CHECK_ENGINE="$ENGINE"
if [[ "$ENGINE_BASENAME" != "spring-headless" ]]; then
    PIN_CHECK_ENGINE="$(dirname "$ENGINE")/spring-headless"
    if [[ ! -x "$PIN_CHECK_ENGINE" ]]; then
        echo "_launch.sh: pinned spring-headless sibling not found at $PIN_CHECK_ENGINE" >&2
        exit 77
    fi
fi

ACTUAL_SHA="$(sha256sum "$PIN_CHECK_ENGINE" | cut -d' ' -f1)"
if [[ "$ACTUAL_SHA" != "$PIN_SHA" ]]; then
    echo "_launch.sh: engine SHA mismatch: pin=$PIN_SHA actual=$ACTUAL_SHA" >&2
    exit 77
fi

if [[ -z "$START_SCRIPT" ]]; then
    echo "_launch.sh: --start-script required" >&2
    exit 1
fi
if [[ ! -f "$START_SCRIPT" ]]; then
    echo "_launch.sh: start script missing at $START_SCRIPT" >&2
    exit 77
fi

# --- optional plugin install --------------------------------------------------

HIGHBAR_DIR="$WRITEDIR/engine/$PIN_RELEASE/AI/Skirmish/highBar/stable"
BARB_SEED_DIR="$WRITEDIR/engine/$PIN_RELEASE/AI/Skirmish/BARb/stable"
PLUGIN_SRC="$PLUGIN_SO"
if [[ -z "$PLUGIN_SRC" && -f "$DEFAULT_PLUGIN_SO" ]]; then
    PLUGIN_SRC="$DEFAULT_PLUGIN_SO"
fi

if [[ -n "$PLUGIN_SRC" ]]; then
    if [[ ! -f "$PLUGIN_SRC" ]]; then
        echo "_launch.sh: plugin file missing: $PLUGIN_SRC" >&2
        exit 1
    fi
    mkdir -p "$HIGHBAR_DIR/config" "$HIGHBAR_DIR/script"
    if [[ -d "$BARB_SEED_DIR/config" ]]; then
        cp -a "$BARB_SEED_DIR/config/." "$HIGHBAR_DIR/config/"
    fi
    if [[ -d "$BARB_SEED_DIR/script" ]]; then
        cp -a "$BARB_SEED_DIR/script/." "$HIGHBAR_DIR/script/"
    fi
    cp "$REPO_ROOT/data/AIInfo.lua" "$HIGHBAR_DIR/AIInfo.lua"
    cp "$REPO_ROOT/data/AIOptions.lua" "$HIGHBAR_DIR/AIOptions.lua"
    cp -a "$REPO_ROOT/data/config/." "$HIGHBAR_DIR/config/"
    cp -a "$REPO_ROOT/data/script/." "$HIGHBAR_DIR/script/"
    cp "$PLUGIN_SRC" "$HIGHBAR_DIR/libSkirmishAI.so"
elif [[ "$VIEWER_ONLY" != "true" && ! -f "$HIGHBAR_DIR/libSkirmishAI.so" ]]; then
    echo "_launch.sh: highBar AI is not installed at $HIGHBAR_DIR and no plugin artifact was provided" >&2
    exit 77
fi

if [[ "$VIEWER_ONLY" != "true" ]]; then
    HIGHBAR_GAME_CFG_DIR="$WRITEDIR/LuaRules/Configs/highBar/stable"
    mkdir -p "$HIGHBAR_GAME_CFG_DIR/config" "$HIGHBAR_GAME_CFG_DIR/script"
    if [[ -d "$BARB_SEED_DIR/config" ]]; then
        cp -a "$BARB_SEED_DIR/config/." "$HIGHBAR_GAME_CFG_DIR/config/"
    fi
    if [[ -d "$BARB_SEED_DIR/script" ]]; then
        cp -a "$BARB_SEED_DIR/script/." "$HIGHBAR_GAME_CFG_DIR/script/"
    fi
    cp -a "$REPO_ROOT/data/config/." "$HIGHBAR_GAME_CFG_DIR/config/"
    cp -a "$REPO_ROOT/data/script/." "$HIGHBAR_GAME_CFG_DIR/script/"
fi

# --- environment + launch ----------------------------------------------------

if [[ -z "$RUNTIME_DIR" ]]; then
    RUNTIME_DIR="${TMPDIR:-/tmp}/hb-run"
fi
mkdir -p "$RUNTIME_DIR"

if [[ -z "$LOG" ]];      then LOG="$RUNTIME_DIR/highbar-launch.log"; fi
if [[ -z "$PID_FILE" ]]; then PID_FILE="$RUNTIME_DIR/highbar-launch.pid"; fi

export SPRING_DATADIR="${HIGHBAR_DATA_DIRS:-$WRITEDIR}"
export XDG_RUNTIME_DIR="$RUNTIME_DIR"
if [[ "$VIEWER_ONLY" != "true" ]]; then
    [[ -n "$COORDINATOR" ]] && export HIGHBAR_COORDINATOR="$COORDINATOR"
    export HIGHBAR_AUDIT_PHASE="$PHASE_MODE"
    # Built-in BARb/Circuit behavior is permanently disabled in the proxy.
    # Keep the environment key for older helper compatibility, but never
    # allow launch arguments or phase defaults to re-enable it.
    export HIGHBAR_ENABLE_BUILTIN="false"

    # Clear stale state files from prior runs.
    rm -f "$RUNTIME_DIR/highbar-0.sock" \
          "$WRITEDIR/highbar.token" \
          "$WRITEDIR/highbar.health" \
          "$WRITEDIR/engine/$PIN_RELEASE/highbar.token" \
          "$WRITEDIR/engine/$PIN_RELEASE/highbar.health"
    if [[ -n "${HIGHBAR_TOKEN_PATH:-}" ]]; then
        rm -f "$HIGHBAR_TOKEN_PATH" "$(dirname "$HIGHBAR_TOKEN_PATH")/highbar.health"
    fi

    if [[ -d "$LUA_GADGET_SRC_DIR" ]]; then
        mkdir -p "$WRITEDIR/LuaRules/Gadgets"
        cp "$LUA_GADGET_SRC_DIR"/*.lua "$WRITEDIR/LuaRules/Gadgets/"
        mkdir -p "$WRITEDIR/engine/$PIN_RELEASE/LuaRules/Gadgets"
        cp "$LUA_GADGET_SRC_DIR"/*.lua "$WRITEDIR/engine/$PIN_RELEASE/LuaRules/Gadgets/"
    fi
    if [[ -d "$LUA_WIDGET_SRC_DIR" ]]; then
        mkdir -p "$WRITEDIR/LuaUI/Widgets"
        cp "$LUA_WIDGET_SRC_DIR"/*.lua "$WRITEDIR/LuaUI/Widgets/"
        mkdir -p "$WRITEDIR/engine/$PIN_RELEASE/LuaUI/Widgets"
        cp "$LUA_WIDGET_SRC_DIR"/*.lua "$WRITEDIR/engine/$PIN_RELEASE/LuaUI/Widgets/"
        widget_cfg="$WRITEDIR/engine/$PIN_RELEASE/LuaUI/Config/BYAR.lua"
        if [[ -f "$widget_cfg" ]]; then
            sed -i 's/\["HighBar Admin Speed"\][[:space:]]*=[[:space:]]*0/\["HighBar Admin Speed"\] = 12345/' "$widget_cfg"
        fi
    fi
fi

LAUNCH_ARGS=()
if [[ "$ENGINE_BASENAME" == "spring" ]]; then
    CONFIG_PATH="$RUNTIME_DIR/highbar-watch.springsettings.cfg"
    if [[ -f "$WRITEDIR/springsettings.cfg" ]]; then
        cp "$WRITEDIR/springsettings.cfg" "$CONFIG_PATH"
    else
        : > "$CONFIG_PATH"
    fi

    case "$WINDOW_MODE" in
        windowed)
            fullscreen_value=0
            borderless_value=0
            ;;
        borderless)
            fullscreen_value=0
            borderless_value=1
            ;;
        fullscreen)
            fullscreen_value=1
            borderless_value=0
            ;;
        *)
            echo "_launch.sh: unsupported --window-mode $WINDOW_MODE" >&2
            exit 1
            ;;
    esac

    if [[ "$MOUSE_CAPTURE" == "true" || "$MOUSE_CAPTURE" == "1" ]]; then
        hardware_cursor=0
        relative_mode_warp=1
    else
        hardware_cursor=1
        relative_mode_warp=0
    fi

    cat >> "$CONFIG_PATH" <<EOF
Fullscreen = $fullscreen_value
WindowBorderless = $borderless_value
WindowPosX = 0
WindowPosY = 0
XResolution = $WINDOW_WIDTH
YResolution = $WINDOW_HEIGHT
XResolutionWindowed = $WINDOW_WIDTH
YResolutionWindowed = $WINDOW_HEIGHT
HardwareCursor = $hardware_cursor
MouseRelativeModeWarp = $relative_mode_warp
EOF

    LAUNCH_ARGS+=(--write-dir "$WRITEDIR" --config "$CONFIG_PATH")
    if [[ "$WINDOW_MODE" != "fullscreen" ]]; then
        LAUNCH_ARGS+=(--window)
    fi
fi

"$ENGINE" "${LAUNCH_ARGS[@]}" "$START_SCRIPT" > "$LOG" 2>&1 &
SPRING_PID=$!
echo "$SPRING_PID" > "$PID_FILE"
echo "_launch.sh: started spring pid=$SPRING_PID log=$LOG"
echo "$SPRING_PID"
