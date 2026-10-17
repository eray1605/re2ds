#!/bin/sh
# Installs everything re2ds needs on Ubuntu / WSL in one go:
#   - the Ubuntu packages (compiler, Python, FFmpeg and its libraries),
#   - devkitPro (devkitARM + libnds) with devkitPro's pacman, or, when
#     devkitpro.org cannot be reached (its Cloudflare blocks some networks with
#     "403 Forbidden"), devkitPro's Docker image from Docker Hub - make then
#     builds inside that image by itself.
# Run it from the repository: tools/setup_wsl.sh   (it asks for your password)
set -e

echo "== Ubuntu packages"
sudo apt-get update
sudo apt-get install -y git build-essential pkg-config python3 ffmpeg wget \
    libavcodec-dev libavutil-dev libswscale-dev

if [ -f /opt/devkitpro/devkitARM/ds_rules ] && [ -d /opt/devkitpro/libnds ]; then
    echo "== devkitPro is already installed in /opt/devkitpro"
    exit 0
fi

echo "== devkitPro (devkitARM + libnds)"
tmp=$(mktemp -d)
if wget -q -O "$tmp/install-devkitpro-pacman" https://apt.devkitpro.org/install-devkitpro-pacman &&
    chmod +x "$tmp/install-devkitpro-pacman" &&
    sudo "$tmp/install-devkitpro-pacman" &&
    sudo dkp-pacman -S --needed --noconfirm nds-dev; then
    rm -rf "$tmp"
    echo
    echo "Done. Close and reopen this terminal (for DEVKITPRO / DEVKITARM), then go on"
    echo "with '1. Extract your disc' in the README."
    exit 0
fi
rm -rf "$tmp"

echo
echo "== devkitpro.org cannot be reached (blocked): using devkitPro's Docker image"
sudo apt-get install -y docker.io
# WSL without systemd does not start services by itself
sudo service docker start 2>/dev/null || sudo systemctl start docker
sudo docker pull devkitpro/devkitarm
# docker without sudo after the next login
sudo usermod -aG docker "$(id -un)" || true
echo
echo "Done. make builds in the Docker image now (no local devkitPro needed)."
echo "After restarting Windows or WSL, start Docker again first: sudo service docker start"
echo "Go on with '1. Extract your disc' in the README."
