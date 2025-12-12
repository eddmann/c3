#!/bin/bash
#
# Download Syzygy tablebase files from lichess mirror
#
# Usage:
#   ./download_syzygy.sh [output_dir] [pieces]
#
# Examples:
#   ./download_syzygy.sh ./syzygy 345    # Download 3-4-5 piece TBs (~1GB)
#   ./download_syzygy.sh ./syzygy 3456   # Download 3-4-5-6 piece TBs (~155GB)
#

set -euo pipefail

OUTPUT_DIR="${1:-./syzygy}"
PIECES="${2:-345}"

MIRROR="https://tablebase.lichess.ovh/tables/standard"

mkdir -p "$OUTPUT_DIR"
cd "$OUTPUT_DIR"

download_file() {
    local url="$1"
    local file="$2"

    if [[ -f "$file" ]]; then
        echo "  [skip] $file (exists)"
        return 0
    fi

    echo "  [download] $file"
    if ! curl -fsSL -o "$file" "$url"; then
        echo "  [error] Failed to download $file"
        rm -f "$file"
        return 1
    fi
    return 0
}

echo "Downloading Syzygy tablebases to: $OUTPUT_DIR"
echo "Piece counts: $PIECES"
echo ""

# Download 3-4-5 piece tablebases
if [[ "$PIECES" =~ [345] ]]; then
    echo "Fetching 3-4-5 piece tablebase file list..."

    # Download WDL files
    echo "Downloading WDL files (.rtbw)..."
    curl -fsSL "${MIRROR}/3-4-5-wdl/" | \
        grep -oE 'K[A-Za-z]+\.rtbw' | \
        sort -u | \
        while read -r file; do
            download_file "${MIRROR}/3-4-5-wdl/${file}" "$file"
        done

    # Download DTZ files
    echo "Downloading DTZ files (.rtbz)..."
    curl -fsSL "${MIRROR}/3-4-5-dtz/" | \
        grep -oE 'K[A-Za-z]+\.rtbz' | \
        sort -u | \
        while read -r file; do
            download_file "${MIRROR}/3-4-5-dtz/${file}" "$file"
        done
fi

# Download 6 piece tablebases (optional, very large)
if [[ "$PIECES" == *"6"* ]]; then
    echo ""
    echo "WARNING: 6-piece tablebases are ~150GB total."
    echo "This may take several hours..."
    echo ""

    echo "Downloading 6-piece WDL files..."
    curl -fsSL "${MIRROR}/6-wdl/" | \
        grep -oE 'K[A-Za-z]+\.rtbw' | \
        sort -u | \
        while read -r file; do
            download_file "${MIRROR}/6-wdl/${file}" "$file"
        done

    echo "Downloading 6-piece DTZ files..."
    curl -fsSL "${MIRROR}/6-dtz/" | \
        grep -oE 'K[A-Za-z]+\.rtbz' | \
        sort -u | \
        while read -r file; do
            download_file "${MIRROR}/6-dtz/${file}" "$file"
        done
fi

echo ""
echo "Download complete!"
ls -lh *.rtb* 2>/dev/null | head -20 || echo "(no files downloaded)"
echo ""
echo "To use with c3 engine:"
echo "  setoption name SyzygyPath value $(pwd)"
