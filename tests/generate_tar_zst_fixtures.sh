#!/usr/bin/env bash
# Generate .tar.zst test fixtures for KEYSTONE tar.zst streaming tests

set -e

FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)/fixtures_tar_zst"
mkdir -p "$FIXTURE_DIR"

echo "Generating tar.zst test fixtures in $FIXTURE_DIR..."

# Generate a simple text file with one int64_t per line
TEXT_FILE="$FIXTURE_DIR/numbers.txt"
echo "Creating $TEXT_FILE..."
for i in $(seq 0 2 2000); do
    echo "$i"
done > "$TEXT_FILE"

# Generate a CSV file
CSV_FILE="$FIXTURE_DIR/numbers.csv"
echo "Creating $CSV_FILE..."
{
    echo "value"
    for i in $(seq 0 3 3000); do
        echo "$i"
    done
} > "$CSV_FILE"

# Generate a JSON file with flat array
JSON_FILE="$FIXTURE_DIR/numbers.json"
echo "Creating $JSON_FILE..."
{
    printf "["
    first=1
    for i in $(seq 0 5 5000); do
        if [ "$first" -eq 1 ]; then
            first=0
        else
            printf ","
        fi
        printf "%d" "$i"
    done
    printf "]\n"
} > "$JSON_FILE"

# Generate corrupt test files
CORRUPT_TXT="$FIXTURE_DIR/corrupt.txt"
echo "Creating $CORRUPT_TXT..."
echo "not a number" > "$CORRUPT_TXT"
echo "still not a number" >> "$CORRUPT_TXT"

CORRUPT_CSV="$FIXTURE_DIR/corrupt.csv"
echo "Creating $CORRUPT_CSV..."
echo "value" > "$CORRUPT_CSV"
echo "123" >> "$CORRUPT_CSV"
echo "bad,data" >> "$CORRUPT_CSV"
echo "456" >> "$CORRUPT_CSV"

CORRUPT_JSON="$FIXTURE_DIR/corrupt.json"
echo "Creating $CORRUPT_JSON..."
echo "[1, 2, \"bad\", 4]" > "$CORRUPT_JSON"

# Create tar archive and compress with zstd
ARCHIVE="$FIXTURE_DIR/test_data.tar.zst"
echo "Creating $ARCHIVE..."
if command -v tar >/dev/null 2>&1 && tar --zstd -cf /dev/null /dev/null 2>/dev/null; then
    tar --zstd -cf "$ARCHIVE" -C "$FIXTURE_DIR" numbers.txt numbers.csv numbers.json corrupt.txt corrupt.csv corrupt.json
elif command -v zstd >/dev/null 2>&1; then
    # Manual approach: create tar then compress
    TAR_TMP="$FIXTURE_DIR/tmp.tar"
    tar -cf "$TAR_TMP" -C "$FIXTURE_DIR" numbers.txt numbers.csv numbers.json corrupt.txt corrupt.csv corrupt.json
    zstd -f -19 "$TAR_TMP" -o "$ARCHIVE"
    rm -f "$TAR_TMP"
else
    echo "Error: neither tar --zstd nor zstd command available" >&2
    exit 1
fi

echo "Fixtures generated:"
ls -lh "$FIXTURE_DIR"
