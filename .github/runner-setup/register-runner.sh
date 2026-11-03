#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# T049 — register a GitHub Actions self-hosted runner with labels
#   self-hosted, Linux, X64, bar-engine
# so the self-hosted-only jobs in .github/workflows/ci.yml route
# here.
#
# Prerequisites (asserted up front):
#   - install-spring-headless.sh has been run (binary + pin match)
#   - hydrate-bar-assets.sh has been run (marker file present)
#
# Environment:
#   GH_RUNNER_URL         GitHub URL, e.g. https://github.com/<org>/<repo>
#   GH_RUNNER_TOKEN       registration token (from Settings → Actions → Runners)
#   GH_RUNNER_NAME        optional; defaults to $(hostname)-bar-engine
#   GH_RUNNER_DIR         optional; defaults to $HOME/actions-runner
#
# This script installs the runner tarball (if not already present),
# configures it with the bar-engine label, and starts it as a
# systemd-user service.  It intentionally does NOT run as root; BAR
# assets live under $HOME and the runner must have the same uid as
# the acceptance grid.

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)

# ---- preconditions ----------------------------------------------------

"$here/install-spring-headless.sh" >/dev/null || {
    echo "register-runner: install-spring-headless.sh failed" >&2
    exit 1
}

marker="$HOME/.local/state/Beyond All Reason/.highbar-assets-hydrated"
[ -f "$marker" ] || {
    echo "register-runner: run hydrate-bar-assets.sh before register-runner.sh" >&2
    exit 1
}

: "${GH_RUNNER_URL:?GH_RUNNER_URL is required (e.g. https://github.com/<org>/<repo>)}"
: "${GH_RUNNER_TOKEN:?GH_RUNNER_TOKEN is required (Settings > Actions > Runners > New)}"

runner_name=${GH_RUNNER_NAME:-$(hostname)-bar-engine}
runner_dir=${GH_RUNNER_DIR:-$HOME/actions-runner}
labels="self-hosted,Linux,X64,bar-engine"

# ---- download runner --------------------------------------------------

mkdir -p "$runner_dir"
cd "$runner_dir"

if [ ! -x ./config.sh ]; then
    # Pin an explicit runner version so registrations are reproducible.
    RUNNER_VERSION=2.338.2
    RUNNER_SHA=baebf204307bac6193480852732f0aba2ae97b6c5597c244f4d5558390147c40
    tarball="actions-runner-linux-x64-${RUNNER_VERSION}.tar.gz"
    echo "register-runner: fetching actions-runner ${RUNNER_VERSION}"
    curl -fL --retry 3 -o "$tarball" \
        "https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/${tarball}"
    echo "${RUNNER_SHA}  ${tarball}" | sha256sum -c -
    tar xzf "$tarball"
    rm -f "$tarball"
fi

# ---- configure --------------------------------------------------------

# Idempotent: if .runner exists the runner is already configured.
if [ ! -f "$runner_dir/.runner" ]; then
    ./config.sh \
        --unattended \
        --url "$GH_RUNNER_URL" \
        --token "$GH_RUNNER_TOKEN" \
        --name "$runner_name" \
        --labels "$labels" \
        --work "_work"
else
    echo "register-runner: runner already configured ($(jq -r .agentName "$runner_dir/.runner"))"
fi

# ---- install systemd-user service ------------------------------------

svc_dir="$HOME/.config/systemd/user"
mkdir -p "$svc_dir"
cat > "$svc_dir/github-runner-bar-engine.service" <<EOF
[Unit]
Description=GitHub Actions runner (bar-engine label, HighBarV3)
After=network-online.target

[Service]
Type=simple
WorkingDirectory=$runner_dir
ExecStart=$runner_dir/run.sh
Restart=always
RestartSec=5
Environment=HIGHBAR_RUNNER_LABEL=bar-engine

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
systemctl --user enable --now github-runner-bar-engine.service

echo "register-runner: runner '$runner_name' registered with labels [$labels]"
echo "register-runner: service status —"
systemctl --user --no-pager status github-runner-bar-engine.service | head -12 || true
