# ATC 模式改造計畫

## 架構決策(已與使用者確認)
雙模式並存:保留現有 plane_img 旋轉圖示的完整程式碼路徑不變;ATC 模式的方塊/向量線/軌跡/STCA
是另一條路徑。用全域 bool `atc_on` 切換兩者的顯示與隱藏,關閉時 100% 還原成目前畫面。
`atc_on` 沿用 `map_on`/`echo_on` 的 pattern(`restore_value: true`),按鈕沿用 `map_btn`/`echo_btn`/
`info_btn` 的樣式與 on_click pattern。

## 新增 ATC 按鈕
- 位置:`SYS`(info_btn, x=720 y=154)正上方 → `x=720 y=116 w=65 h=32`,樣式完全比照
  map_btn/echo_btn/info_btn(checkable、綠框、checked 反白)。
- on_click:切換 `id(atc_on)`,文字顏色反白邏輯同其他三顆鈕。不需要立即強制刷新——
  每秒的位置外插 lambda 本來就會在 ≤1s 內套用新狀態,跟現有 map/echo 切換的即時感一致
  (那兩顆會立即呼叫 rebuild_base 是因為底圖只在事件觸發時重繪,飛機標記本來就每秒都在重繪)。

## 新增 globals
- `atc_on` (bool, restore_value true, initial false)
- `ac_lc` (`std::vector<uint32_t>`):每架機的 OpenSky `last_contact`(epoch 秒),用於判斷訊號延遲。
- `trail_lat` / `trail_lon`(`std::vector<float>`,長度固定 30 = 10 槽 × 3 筆,索引 `slot*3+k`):
  軌跡歷史存經緯度而非像素座標,這樣範圍(range)或中心點改變時仍可用目前設定重新投影。
  沿用專案既有的 vector 慣例(ac_lat/ac_lon 也是 vector),不用 ESPHome 的原生固定陣列型別
  (`float[10][3]` 在 globals codegen 上有疑慣,vector 更穩)。
- `conflict_slot`(`std::vector<bool>` 或 `std::vector<uint8_t>`,長度 10):STCA 衝突旗標,
  由每秒 lambda 內的 O(shown²) 兩兩比對算出,提供顏色判斷用。

## radar_fetch.h 改動
- `AcInfo` 增加 `uint32_t lc;`(last_contact)。
- `do_states()` 解析 `a[4]` 存入 `ac.lc`(OpenSky states 陣列標準索引:4=last_contact,
  已用 5/6/7/8/9/10/11 對照確認索引無誤)。
- 主迴圈資料進來時同步 push 進新的 `ac_lc` global(比照 ac_lat/ac_lon 的 clear+push_back 模式)。

## 飛機符號(Target Symbol)
- ac0~ac9 容器內,`ai0~ai9`(image)保持不動、不刪除。
- 新增子物件 `sq0~sq9`(`obj:`,4x4,`bg_color: 0x00FF41`,`border_width:0 radius:0`),
  位置對齊圖示中心點(容器內約 (12,18),因為容器本身用 px-14,py-20 讓圖示中心對準座標點)。
  預設 `hidden: true`。
- 每秒 lambda 內:`atc_on` 為真時 `sq{i}` clear_flag(HIDDEN)、`ai{i}` add_flag(HIDDEN);
  為假時相反。既有的 rotate/recolor 呼叫維持不變(物件隱藏時空轉,成本可忽略,換取
  「關閉 = 100% 還原原邏輯」的保證,不冒改動既有路徑的風險)。
- 點擊選取(`clickable + on_click → select_slot`)完全不動,容器本身邏輯不變。

## 選取視覺反饋 / STCA 顏色
在每秒 lambda 內,對每個 shown 槽位算出顏色優先序(僅 `atc_on` 分支套用;非 ATC 模式沿用原本
`0xFFD24A`(選取)/`0x40FF9A`(一般)配色,不受此邏輯影響):
1. 選取 + 衝突 → 白/紅交替(用 `id(poll_tick) % 2` 或簡單的秒數奇偶做 blink)
2. 選取(無衝突) → 白 `0xFFFFFF`
3. 衝突(未選取) → 紅 `0xFF0033`
4. 一般 → 綠 `0x00FF41`
- 訊號延遲(`stale`,`now_epoch - ac_lc[idx] > 60`,需 `rtc_time().now().is_valid()` 才判斷)
  獨立於顏色優先序:一律在呼號後綴 `*`,顏色仍照上面規則(若同時 stale 又是一般狀態,
  規格要求 stale 顯示黃色 `0xFFCC00`——插入為第 3.5 順位,衝突 > stale > 一般)。
- 修正後優先序:選取+衝突(白紅交替) > 選取(白) > 衝突(紅) > stale(黃) > 一般(綠)。
- 方塊 `sq{i}` 與標籤 `ad{i}` 套用同一個顏色。

## 速度向量線 + 歷史軌跡
- 新增 10 條 top-level `line:`(`vec0~vec9`,仿 `tail0~tail7`/`sweep_line` 寫法,注意這類
  compound widget 要用 `id(vecN)->obj` 存取,不能直接轉型——踩過這個坑,見專案記憶)。
- 向量終點:用目前已算出的 e/n(相對 home,公里)加上 `vel*120/1000` 沿 `trk` 方向的位移,
  換算回像素;若落在半徑 226px 之外就沿方向 clamp 到圓周上,避免畫出雷達圈外。
- 新增 30 個 top-level `obj:` 當軌跡尾跡點(`tr{slot}_0~tr{slot}_2`,2x2,遞減 `bg_opa`
  做漸淡:255/170/85),平常 hidden,`atc_on` 時視 `trail_lat/lon` 是否有效(非 NAN)決定
  clear_flag。
- 軌跡歷史寫入時機:「收到新資料」那個事件區塊(`g_states_ready` 分支,在 clear ac_lat/ac_lon
  之前),用呼號比對舊 `ac_cs`/`ac_lat`/`ac_lon` 找出對應舊位置,推入 `trail_lat/lon`(shift
  0→1→2)。
- (審查後修正)軌跡槽位一律用**資料索引**(g_result 第 j 架、顯示端同樣用 i)定址,不能用顯示
  槽位 shown——圓形雷達會跳過方形 bbox 角落的飛機,shown 與 i 經常錯位,混用會讀到別架的歷史。
  另外每槽記錄呼號(`trail_cs`),排序變動導致該格換機時把舊歷史作廢(NAN),避免把前任的
  軌跡點接在新飛機後面。

## 標籤格式化(ad0~ad9,僅 ATC 模式)
兩行:
- Line1:呼號(stale 則加 `*` 後綴)
- Line2:`FL{alt/100:03.0f}` + 爬升箭頭(`ac_vr>0.5→↑ / <-0.5→↓ / 其他→=`)+ `{vel_kt:.0f}kt`
非 ATC 模式維持現有格式(`%s\n%03.0f %.0f`)不變。

### 字型 glyph 風險
`↑`/`↓` 是 Unicode 箭頭,`font_tiny`(ad{i} 用的字型)目前沒有限制 glyphs,ESPHome 預設只內建
ASCII;如果直接寫 `glyphs:` 在 font_tiny 上會整個「取代」預設字集(font_clock 就是故意這樣做
來縮小 64px 字型體積),直接加會讓數字/呼號的其他字元消失。改用 `extras:` 追加箭頭 glyph
(比照 cjk_glyphs 的 extras 寫法),保留原本預設 ASCII 集合不被取代。若編譯後箭頭仍顯示空白
(google font 檔案本身沒收錄該碼位),退而求其次改用 ASCII 替代(`^`/`v`/`-`)。

## 執行順序(分階段,每階段都跑 esphome compile 驗證)
1. Stage A:globals(atc_on/ac_lc/trail_lat/trail_lon/conflict_slot)+ radar_fetch.h last_contact
   欄位 + ATC 按鈕(先不接任何顯示邏輯,只讓它能切換 `atc_on` 且編譯過)。
2. Stage B:sq0~sq9 方塊物件 + 每秒 lambda 內 atc_on 顯示/隱藏切換(ai↔sq)。
3. Stage C:vec0~vec9 向量線 + 30 個軌跡點物件 + 對應計算與資料寫入時機。
4. Stage D:STCA 兩兩比對 + stale 判斷 + 顏色優先序 + 標籤兩行格式化 + 箭頭字型。
5. Stage E:README(中英)補一段 ATC 模式說明;最終整體編譯確認。

## 不動的東西
`select_slot`、`show_weather`、`show_sysinfo`、掃描線 30ms glow lambda(既有 recolor 邏輯不變,
只是 atc_on 時該圖示是隱藏的)、鬧鐘/喇叭/天氣等既有功能全部不碰。
