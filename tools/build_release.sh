#!/usr/bin/env bash
# 產生可發佈的韌體 —— 只編「當前分支負責的板子」。
#
# 用法:
#     git checkout main   && conda activate esphome        && tools/build_release.sh
#     git checkout lvgl9  && conda activate esphome-lvgl9  && tools/build_release.sh
#
# 刻意「不」自動切換 git 分支或 conda 環境:兩者都是全域狀態,而這個專案在
# 編譯途中被切走過好幾次(工具鏈找不到、拿錯分支的原始碼去編)。腳本只驗證
# 你已經切對了,切錯就直接拒絕跑。
#
# 產物放在 dist/,兩個分支各跑一次之後會湊齊四塊板子。dist/ 不進版控。
set -euo pipefail
cd "$(dirname "$0")/.."

# ---- 版本:git tag,沒有 tag 就退回 commit ----
# 可用 VERSION=... 覆寫,發 alpha 韌體時用得到:
#   VERSION=v1.2.0-alpha ONLY=s3-jc8048w550 tools/build_release.sh
VERSION="${VERSION:-$(git describe --tags --always --dirty 2>/dev/null || echo unknown)}"

# ---- 只編某一塊板(選用)----
# 給定 slug 就只編那一個入口,其餘跳過。單獨補發某塊板的韌體時免得整批重編。
ONLY="${ONLY:-}"

# ---- manifest 裡韌體的位址 ----
# 韌體「必須」和 manifest 同源(或由會送 CORS 標頭的主機供應),所以現在一律
# 複製到 docs/firmware/,由 GitHub Pages 和 manifest 一起供應,manifest 寫
# 相對路徑 firmware/<檔名>。
#
# >>> 不要再把 manifest 指向 GitHub Releases <<<
# esp-web-tools 是在瀏覽器裡 fetch() 韌體的。GitHub 把 release 資產改由
# release-assets.githubusercontent.com 供應之後就不再送 access-control-allow-origin,
# 跨來源請求被瀏覽器擋掉,安裝頁只會顯示一句 "Failed to fetch"(v1.0.0~v1.2.0
# 的安裝頁就是這樣靜默壞掉的,四塊板子全中)。GitHub Pages 則有送 `*`,所以
# 同源供應最省事 —— 根本不需要 CORS。
#
# 代價是韌體進版控(一顆 ~3.3MB)。所以每塊板只保留「當前版本」那一顆,發新版
# 就把舊的刪掉再放新的;歷史版本仍然上傳到 GitHub Releases 供手動下載/esptool。
#
# RELEASE_BASE_URL 留作逃生門:如果你把韌體放到別的、有送 CORS 標頭的主機,
# 設了它 manifest 就改寫絕對網址,也不會複製到 docs/firmware/。
RELEASE_BASE_URL="${RELEASE_BASE_URL:-}"
PAGES_FW_DIR="docs/firmware"

# ---- 分支 → 板子對照 ----
BRANCH="$(git rev-parse --abbrev-ref HEAD)"
case "$BRANCH" in
  main)
    WANT_ENV="esphome"
    # 入口:輸出檔名:晶片族(ESP Web Tools 的 chipFamily)
    ENTRIES=(
      "radar.yaml:s3-800x480-generic:ESP32-S3"
      "radar-jc8048w550.yaml:s3-jc8048w550:ESP32-S3"
      "radar-s3-5.yaml:s3-touch-lcd-5:ESP32-S3"
      "radar-s3-5b.yaml:s3-touch-lcd-5b:ESP32-S3"
      "radar-s3-7.yaml:s3-touch-lcd-7:ESP32-S3"
    ) ;;
  lvgl9)
    WANT_ENV="esphome-lvgl9"
    ENTRIES=( "radar-p4-7b.yaml:p4-touch-lcd-7b:ESP32-P4" ) ;;
  *)
    echo "錯誤:分支 '$BRANCH' 沒有對應的發佈板子(只認 main 與 lvgl9)。" >&2
    exit 1 ;;
esac

# ---- 前置檢查 ----
if [ -n "$(git status --porcelain)" ]; then
  echo "錯誤:工作目錄不乾淨。發佈的韌體必須對得回一個 commit。" >&2
  git status --short >&2
  exit 1
fi

CUR_ENV="${CONDA_DEFAULT_ENV:-<none>}"
if [ "$CUR_ENV" != "$WANT_ENV" ]; then
  echo "錯誤:分支 '$BRANCH' 需要 conda 環境 '$WANT_ENV',目前是 '$CUR_ENV'。" >&2
  echo "       先執行:conda activate $WANT_ENV" >&2
  exit 1
fi

command -v esphome >/dev/null || { echo "錯誤:找不到 esphome。" >&2; exit 1; }
echo "分支 $BRANCH / 環境 $WANT_ENV / 版本 $VERSION"
esphome version

mkdir -p dist

for spec in "${ENTRIES[@]}"; do
  IFS=: read -r ENTRY SLUG CHIP <<< "$spec"
  # 注意要用 if,不能寫 `[ ... ] && continue` —— set -e 下條件不成立就會中止整個腳本
  if [ -n "$ONLY" ] && [ "$ONLY" != "$SLUG" ]; then continue; fi
  OUT="flight-radar-${SLUG}-${VERSION}"
  echo
  echo "==== $ENTRY  →  $OUT ===="

  # 每個入口用自己的建置目錄。四個入口的 ESPHome `name:` 都是 flight-radar,
  # 共用 .esphome/build/flight-radar/ 會互相覆蓋、每次都得全編;分開放才能沿用
  # 各自的快取。也與日常開發用的 build/ 與 build9/ 分開,不干擾手上的工作。
  # (這個變數是相對於 .esphome/ 的,不要寫成 .esphome/xxx。)
  export ESPHOME_BUILD_PATH="rel-${SLUG}"

  esphome compile "$ENTRY"

  SRC=".esphome/${ESPHOME_BUILD_PATH}/flight-radar/.pioenvs/flight-radar/firmware.factory.bin"
  [ -f "$SRC" ] || { echo "錯誤:找不到 $SRC" >&2; exit 1; }
  cp "$SRC" "dist/${OUT}.factory.bin"

  # ESP Web Tools 的 manifest。factory bin 是「含 bootloader + 分割表 + app」
  # 的完整映像,一律燒在 offset 0 —— S3 的 bootloader 就在檔案開頭,P4 的檔案
  # 開頭是 0xFF 填充、bootloader 落在 0x2000,兩者都是從 0 寫進去才正確。
  if [ -n "$RELEASE_BASE_URL" ]; then
    BIN_URL="${RELEASE_BASE_URL%/}/${OUT}.factory.bin"
  else
    # 預設:同源供應。韌體複製進 docs/firmware/,manifest 寫相對路徑
    # (esp-web-tools 會以 manifest 自己的網址為基準解析),舊版本先刪掉,
    # 這樣每塊板在版控裡永遠只有一顆當前韌體。
    mkdir -p "$PAGES_FW_DIR"
    rm -f "$PAGES_FW_DIR/flight-radar-${SLUG}-"*.factory.bin
    cp "$SRC" "$PAGES_FW_DIR/${OUT}.factory.bin"
    BIN_URL="firmware/${OUT}.factory.bin"
  fi
  # ESP Web Tools / esptool-js 的文件列出 ESP32-P4 為支援的 chipFamily,
  # 但我們沒有實際用瀏覽器燒過 P4;S3 是確定沒問題的。
  # manifest 用「不帶版本號」的固定檔名:docs/install.html 直接引用它,這樣發新版
  # 只要覆蓋 manifest、不用改網頁。版本資訊在 manifest 內容與韌體檔名裡。
  # 直接寫進 docs/(下面再複製一份到 dist/):以前要人工把 dist/*.manifest.json
  # 複製過去,漏掉就會出現「manifest 指向舊版韌體」這種只有使用者才踩得到的錯。
  cat > "docs/flight-radar-${SLUG}.manifest.json" <<JSON
{
  "name": "ESP32 Flight Radar (${SLUG})",
  "version": "${VERSION}",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "${CHIP}",
      "parts": [
        { "path": "${BIN_URL}", "offset": 0 }
      ]
    }
  ]
}
JSON
  cp "docs/flight-radar-${SLUG}.manifest.json" "dist/flight-radar-${SLUG}.manifest.json"
  echo "→ dist/${OUT}.factory.bin  ($(du -h "dist/${OUT}.factory.bin" | cut -f1))"
  if [ -z "$RELEASE_BASE_URL" ]; then
    echo "→ $PAGES_FW_DIR/${OUT}.factory.bin(供 GitHub Pages 同源取用)"
  fi
done

# 校驗碼:每次只更新這一輪產出的檔案,另一個分支的產物保留
( cd dist && sha256sum flight-radar-*.factory.bin > SHA256SUMS )

echo
echo "完成。dist/ 目前的內容:"
ls -1 dist/
echo
echo "另一個分支的板子請切過去再跑一次:"
[ "$BRANCH" = main ] && echo "  git checkout lvgl9 && conda activate esphome-lvgl9 && tools/build_release.sh" \
                     || echo "  git checkout main  && conda activate esphome       && tools/build_release.sh"

# GitHub Pages 是從 main 的 /docs 供應的,所以 lvgl9 上編出來的 P4 韌體與 manifest
# 留在 lvgl9 的 docs/ 沒有用,一定要帶回 main 才會被網頁抓到。
if [ "$BRANCH" = lvgl9 ]; then
  echo
  echo "注意:Pages 是從 main 的 /docs 供應的。P4 的韌體與 manifest 要帶回 main:"
  echo "  git checkout main"
  echo "  git checkout lvgl9 -- docs/firmware/flight-radar-p4-touch-lcd-7b-*.factory.bin \\"
  echo "                        docs/flight-radar-p4-touch-lcd-7b.manifest.json"
fi
