#!/usr/bin/env bash
set -euo pipefail

out="$1"
shift

cat > "$out" <<'EOF'
#include "src/server/assets.h"

#include <array>

namespace gitboard::server {
namespace {

EOF

asset_index=0
entries_file="${out}.entries"
: > "$entries_file"

content_type() {
  case "$1" in
    *.html|*.htm) echo "text/html; charset=utf-8" ;;
    *.css) echo "text/css; charset=utf-8" ;;
    *.js) echo "application/javascript; charset=utf-8" ;;
    *.json) echo "application/json; charset=utf-8" ;;
    *.svg) echo "image/svg+xml" ;;
    *.png) echo "image/png" ;;
    *.jpg|*.jpeg) echo "image/jpeg" ;;
    *.gif) echo "image/gif" ;;
    *.webp) echo "image/webp" ;;
    *.ico) echo "image/x-icon" ;;
    *.txt) echo "text/plain; charset=utf-8" ;;
    *) echo "application/octet-stream" ;;
  esac
}

url_path() {
  local src="$1"
  local rel="${src#src/server/assets/}"
  case "$rel" in
    *.html|*.htm)
      if [[ "$rel" == "index.html" || "$rel" == "index.htm" ]]; then
        echo "/"
      else
        echo "/$rel"
      fi
      ;;
    *)
      echo "/static/$rel"
      ;;
  esac
}

emit_bytes() {
  local src="$1"
  od -An -v -t u1 "$src" | awk '
    {
      for (i = 1; i <= NF; ++i) {
        if (count % 16 == 0) {
          if (count > 0) printf "\n";
          printf "  ";
        }
        printf "%s,", $i;
        ++count;
      }
    }
    END { printf "\n"; }
  '
}

for src in "$@"; do
  [[ -f "$src" ]] || continue
  symbol="kAsset${asset_index}"
  path="$(url_path "$src")"
  type="$(content_type "$src")"

  {
    echo "constexpr unsigned char ${symbol}[] = {"
    emit_bytes "$src"
    echo "};"
    echo
  } >> "$out"

  printf '    {"%s", "%s", %s, sizeof(%s)},\n' \
    "$path" "$type" "$symbol" "$symbol" >> "$entries_file"

  if [[ "$path" == "/" ]]; then
    printf '    {"/index.html", "%s", %s, sizeof(%s)},\n' \
      "$type" "$symbol" "$symbol" >> "$entries_file"
  fi

  asset_index=$((asset_index + 1))
done

{
  echo "constexpr std::array<asset, $(wc -l < "$entries_file" | tr -d ' ')> kAssets = {{"
  cat "$entries_file"
  echo "}};"
  echo
  echo "}  // namespace"
  echo
  echo "const asset* find_asset(std::string_view path) {"
  echo "  for (const asset& item : kAssets) {"
  echo "    if (item.path == path) return &item;"
  echo "  }"
  echo "  return nullptr;"
  echo "}"
  echo
  echo "}  // namespace gitboard::server"
} >> "$out"

rm -f "$entries_file"
