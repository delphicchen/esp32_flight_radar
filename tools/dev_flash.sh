#!/usr/bin/env bash
# 日常開發燒錄 —— 每個入口用「隔離的建置目錄」編譯+上傳,防止拿錯映像。
#
# 用法:
#     conda activate esphome
#     tools/dev_flash.sh radar.yaml                        # OTA 到 flight-radar.local
#     tools/dev_flash.sh radar.yaml --device /dev/ttyACM0  # USB 燒錄
#     COMPILE_ONLY=1 tools/dev_flash.sh radar-jc8048w550.yaml   # 只編譯不上傳
#
# 為什麼不能直接 `esphome run <entry>.yaml`:五個入口的 ESPHome `name:` 都是
# flight-radar,共用 .esphome/build/flight-radar/。2026.3.x 實測:編完 A 入口後
# 直接上傳 B 入口,ESPHome 認為建置已是最新、跳過程式碼生成,把 A 的映像燒進
# B 的板子(v1.3.6 當晚 Guition 映像被 OTA 到白牌板 → 背光亮但全黑)。
#
# 所以這個腳本強制 ESPHOME_BUILD_PATH=rel-dev-<slug>(與 tools/build_release.sh
# 同一套規則),並在上傳前驗證 main.cpp 確實是「這一次」生成的,不對就拒絕上傳。
# 腳本不幫你切分支/conda 環境,理由同 build_release.sh。
set -euo pipefail
cd "$(dirname "$0")/.."

ENTRY="${1:?用法: tools/dev_flash.sh <entry.yaml> [--device <host|port>] [其餘參數原樣傳給 esphome upload]}"
[ -f "$ENTRY" ] || { echo "錯誤:找不到入口 $ENTRY" >&2; exit 1; }
shift

command -v esphome >/dev/null || { echo "錯誤:找不到 esphome(先 pip install esphome 或啟用對應環境,見 README)。" >&2; exit 1; }

# 這個變數是相對於 .esphome/ 的,不要寫成 .esphome/xxx(同 build_release.sh)
SLUG="$(basename "$ENTRY" .yaml)"
export ESPHOME_BUILD_PATH="rel-dev-${SLUG}"
SRC=".esphome/${ESPHOME_BUILD_PATH}/flight-radar/src/main.cpp"

echo "== 編譯 $ENTRY(建置目錄 .esphome/$ESPHOME_BUILD_PATH)=="
MARKER="$(mktemp)"
trap 'rm -f "$MARKER"' EXIT
esphome compile "$ENTRY"

if [ ! -f "$SRC" ] || [ ! "$SRC" -nt "$MARKER" ]; then
  echo "錯誤:$SRC 不是這次生成的(建置目錄裡是舊貨),拒絕上傳。" >&2
  exit 2
fi

if [ "${COMPILE_ONLY:-0}" = "1" ]; then
  echo "== COMPILE_ONLY=1,跳過上傳 =="
  exit 0
fi

echo "== 上傳 $ENTRY =="
if [ "$#" -eq 0 ]; then set -- --device flight-radar.local; fi
esphome upload "$ENTRY" "$@"
