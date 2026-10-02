# Local Environment Notes

## Spring Headless

- `SPRING_HEADLESS=/home/developer/.local/state/Beyond All Reason/engine/recoil_2026.07.04/spring-headless`
- Persisted for login shells in `/home/developer/.bash_profile`

## BAR Graphical Client

- Watched Itertesting runs use BAR's normal graphical `spring` client directly, without going through a lobby.
- `HIGHBAR_BAR_CLIENT_BINARY` optionally overrides the graphical client path used by Itertesting watch mode. If it is unset, watch mode derives `spring` from the pinned BAR engine install alongside `spring-headless`.
- `HIGHBAR_ITERTESTING_WATCH=true` enables launch-time viewer startup in `tests/headless/itertesting.sh`.
- `HIGHBAR_ITERTESTING_WATCH_PROFILE` selects the structured watch profile. `default` is windowed `1920x1080`, spectator-only, with mouse capture disabled.
- The default watched viewer speed target is `3x`.
- Watched launches rewrite the BAR startscript bounds to `MinSpeed=0` and `MaxSpeed=10`. Pause remains a separate control.
- `HIGHBAR_ITERTESTING_WATCH_SPEED` overrides the default target after the graphical client launches. The implementation applies it through the local AI Bridge widget rather than a raw CLI speed flag.
- `HIGHBAR_BAR_CLIENT_READY=false` and `HIGHBAR_BAR_CLIENT_REASON=...` can be used to simulate or diagnose host prerequisite failures before a live watched run starts.
- `HIGHBAR_BNV_BINARY`, `HIGHBAR_BNV_ENV_READY`, and `HIGHBAR_BNV_ENV_REASON` are legacy aliases kept for compatibility with the earlier draft.

## Notes

- This file is for machine-local environment prerequisites that affect repo workflows.
- Update it if the BAR engine install path changes.
