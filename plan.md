# stable_20260609 への Idein パッチ取り込み(v3: レビュー対応・実装手順確定)

## Context

Idein/linux では official (raspberrypi/linux) の stable タグに userqpu 等のパッチを当てた
カーネルをリリースしている(ブランチ `patched/stable_<日付>`、タグ `1.<日付>.<N>`)。
現行の `1.20250916.1` は `stable_20250916` (6.12.47) ベース。これを新しい official タグ
`stable_20260609` (**6.18.34**) に追従させる。

取り込むパッチは `stable_20250916..1.20250916.1` の3件:

| commit | 内容 | 6.18 への適用 |
|---|---|---|
| `83408a5c6eb3` | Apply userqpu patch (vc4/firmware) | テキストはクリーンに当たるが**コンパイル不可** → 6.18 API への移植が必要(下記) |
| `b223dc7c1813` | Change hang_limit_ms to 10sec (v3d) | コンフリクト → `.timeout` 1行変更で解消 |
| `88731b9779a9` | Copy overlays/README into deb package | クリーン適用可 |

決定事項(ユーザー確認済み):
- **push はしない**(ローカル作業のみ。push・タグ付けはユーザー自身が実施)
- **arm32 はスコープ外**(現行リリースフローは arm64 の v8/2712 のみ。パッチの
  arm64 用 `arch_memremap_wb` inline と ioremap.c の hunk は原型のまま維持)
- **PM 参照リークは今回修正する**(userqpu 移植とは分離した独立コミット。下記参照)

## 手順

作業リポジトリ: `/home/cutsea110/devel/idein/linux`
前提: `git status --short` の出力が `?? plan.md` のみであること(plan.md は未追跡のまま残してよい)。

### 1. ブランチ作成

```bash
git checkout -b patched/stable_20260609 stable_20260609   # = c8c7494100e9(ローカル取得済み)
```

### 2. userqpu パッチ適用 + 6.18 移植(単一コミット)

```bash
git cherry-pick 83408a5c6eb3   # テキスト上はコンフリクトなしで入る(ただしこのままではビルド不可)
```

続けて `drivers/gpu/drm/vc4/vc4_gem.c` に以下の 6.18 移植を加え、
`git commit --amend` でメッセージを **"Apply userqpu patch to stable_20260609"** に
書き換えつつ1コミットに統合する(過去リリースの慣例どおり)。

移植後は必ず staging してから amend する:

```bash
git add drivers/gpu/drm/vc4/vc4_gem.c
git commit --amend -m "Apply userqpu patch to stable_20260609"
```

移植内容 — 6.18 では BO 予約が `ww_acquire_ctx` から `struct drm_exec` に移行
(`vc4_lock_bo_reservations()` は `(exec, struct drm_exec *)`、`vc4_queue_submit()` は
第3引数に `struct drm_exec *` を取る)。`vc4_firmware_qpu_execute()` を次のように直す:

1. `struct ww_acquire_ctx acquire_ctx;` → `struct drm_exec exec_ctx;`
2. `vc4_lock_bo_reservations(dev, exec, &acquire_ctx)` →
   `vc4_lock_bo_reservations(exec, &exec_ctx)`。
   失敗時は 6.18 実装が内部で `drm_exec_fini()` 済みなので、従来どおり
   `vc4_complete_exec()` だけ呼べばよい。
3. lock 成功後〜`vc4_queue_submit()` 前の失敗経路(`arch_memremap_wb` が NULL を
   返した場合)は、6.18 の `vc4_submit_cl_ioctl()` の `fail_unreserve:`/`fail:` と同じ順で
   `drm_exec_fini(&exec_ctx)` → `vc4_complete_exec()` を呼ぶ。
4. `vc4_queue_submit(dev, exec, &exec_ctx, NULL)` に変更。失敗時も
   `drm_exec_fini(&exec_ctx)` → `vc4_complete_exec()`(成功時は内部で fini 済み)。
5. 旧パッチの `vc4_queue_submit` の戻り値型変更 hunk(`static int` → `static uint64_t`)は
   **破棄**し、upstream の `static int` のまま使う。seqno は従来どおり submit 成功後に
   `seqno = vc4->emit_seqno;` で取得し `vc4_wait_for_seqno(dev, seqno, ~0ull, true)`
   (6.18 にも同シグネチャで存在、vc4_gem.c:388)。

vc4_gem.c 以外の hunk(bcm270x.dtsi / bcm2835-rpi-common.dtsi / ioremap.c /
raspberrypi.c / vc4_drv.h / vc4_irq.c / vc4_regs.h / vc4_v3d.c /
raspberrypi-firmware.h)はクリーンに当たり、使用 API(rpi_firmware_property_list への
フィルタ挿入、V3D レジスタ、関数ポインタ登録)は 6.18 でも変化なし。コンパイルで最終確認する。

既知の制限(コミットメッセージまたはリリースノートに記載):
- arm32(`ARCH=arm`)では 6.18 の `arch_memremap_wb()` が3引数になったため
  vc4_gem.c の2引数呼び出しがコンパイル不可。arm64 のみサポート。

### 3. hang_limit_ms パッチ適用(コンフリクト解消)

```bash
git cherry-pick b223dc7c1813   # drivers/gpu/drm/v3d/v3d_sched.c でコンフリクト
```

6.18 ではキュー個別の `drm_sched_init()` が `v3d_queue_sched_init()` に共通化され、
タイムアウトは `struct drm_sched_init_args` の `.timeout = msecs_to_jiffies(500)`
(855行目付近)に移動している。ここを `msecs_to_jiffies(10000)` に変更して解消
(全キューに効くため旧パッチと意味は同一)。

### 4. dtbinst パッチ適用

```bash
git cherry-pick 88731b9779a9   # クリーンに入る
```

### 5. runtime PM 参照リークの修正(独立コミット)

下記「PM 参照リークの修正方法」のとおり、`vc4_exec_alloc()` から
`power_lock` / `power_refcount` による参照取得ブロックを削除する。userqpu 移植と
混ぜず、次の独立コミットとして記録する:

```bash
git add drivers/gpu/drm/vc4/vc4_gem.c
git commit -m "Fix v3d runtime PM refcount leak in userqpu path"
```

### 6. ビルド検証(rpi-kernel-builder / Docker、未 push のまま検証)

`build_kernel_deb.bash` は `linux/` ディレクトリが**既に存在すれば無条件に clone を
スキップする**ため、ローカルブランチを事前配置すれば未 push でも検証できる。
ビルドには 20GB 程度の空きディスクが必要(事前に `df -h` で確認)。

```bash
# 既存のローカル clone を使用する。想定 revision から変わっていれば内容を確認して止める。
BUILDER=/home/cutsea110/devel/idein/rpi-kernel-builder
test "$(git -C "$BUILDER" rev-parse HEAD)" = \
    3f2730772838850fd2d651a3e35793c95a5227ec

# `/tmp` は tmpfs で容量不足になり得るため、十分な空きがある作業リポジトリ配下に
# ビルド成果物を配置する（ビルド後は `?? .build/` が未追跡として残る）。
RESULT_PARENT=/home/cutsea110/devel/idein/linux/.build
mkdir -p "$RESULT_PARENT"
RESULT_DIR=$(mktemp -d "$RESULT_PARENT/rpi-kernel-build-20260609.XXXXXX")
df -h "$RESULT_DIR"
docker pull idein/rpi-kernel-deb-builder:bookworm-20251028
docker image inspect idein/rpi-kernel-deb-builder:bookworm-20251028 \
    --format '{{index .RepoDigests 0}}'  # 実行時に解決された digest を記録する
mkdir -p "$RESULT_DIR/custom-kernel-build"
git clone -b patched/stable_20260609 /home/cutsea110/devel/idein/linux \
    "$RESULT_DIR/custom-kernel-build/linux"

# 配置した tree が作業ブランチの HEAD と一致することを確認(古い tree の使い回し防止)
test "$(git -C "$RESULT_DIR/custom-kernel-build/linux" rev-parse HEAD)" \
   = "$(git -C /home/cutsea110/devel/idein/linux rev-parse patched/stable_20260609)"

docker run --rm -u "$(id -u):$(id -g)" -v "$RESULT_DIR:/work" -w /work \
    idein/rpi-kernel-deb-builder:bookworm-20251028 \
    build_kernel_deb.bash "1.20260609.0-0" v8
docker run --rm -u "$(id -u):$(id -g)" -v "$RESULT_DIR:/work" -w /work \
    idein/rpi-kernel-deb-builder:bookworm-20251028 \
    build_kernel_deb.bash "1.20260609.0-0" 2712

# v8 / 2712 の各 image deb に overlays/README があることを確認する。
for deb in "$RESULT_DIR"/custom-kernel-build/linux-image-*.deb; do
    dpkg-deb -c "$deb" | grep -q 'overlays/README$' || exit 1
done
```

※ 6.12→6.18 でツールチェーン要件(gcc/pahole 等)が上がっている可能性がある。
bookworm-20251028 イメージでビルドが通らない場合はその旨を報告し対処を相談する。

### 7. 完了報告

cherry-pick 結果・移植 diff・ビルド結果を報告。その後の作業(ユーザー自身が実施):

- `git push origin patched/stable_20260609`
- `git tag 1.20260609.0 && git push origin 1.20260609.0`
- rpi-kernel-builder の CI を workflow_dispatch で実行し、必須 input `tag` に
  **`1.20260609.0-0`** を指定する(**artifact が作られるだけで release は作成されない**)
- artifact の deb を動作確認後、GitHub UI で Release `1.20260609.0-0` を作成・publish
  する。この Release 作成で tag を push し、tag-trigger CI が既存 Release に deb asset を
  upload する。現 workflow に Release 作成処理はないため、この順序は必須。

## PM 参照リークの修正方法(手順5で適用)

**現象**: `vc4_exec_alloc()` が `power_refcount++`(0→1 なら `pm_runtime_get_sync()`)を
行った後、呼び出し元の `vc4_submit_cl_ioctl()` と `vc4_firmware_qpu_execute()` が
さらに `vc4_v3d_pm_get()` を呼ぶため、exec 1件につき参照を2回取得する。一方
`vc4_complete_exec()` の `vc4_v3d_pm_put()` は1回だけなので、**ジョブごとに参照が
1つ漏れ、初回ジョブ以降 V3D が runtime suspend しなくなる**。現行 6.12 リリース
(1.20250916.1)にも同一のバグが存在する(vc4_gem.c:1145 と 1217/1346)。

**修正**: `vc4_exec_alloc()` から `power_lock`/`power_refcount` ブロックを削除し、
`kcalloc` + `INIT_LIST_HEAD(&exec->unref_list)` だけの関数にする。

- 両呼び出し元は alloc 直後に `vc4_v3d_pm_get()` を呼んでおり(取得は1回になる)、
  その失敗時は `kfree(exec)` で抜けるため参照の取りこぼしはない。
- pm_get 成功後の全エラーパスは `vc4_complete_exec()` → `vc4_v3d_pm_put()` を通るので
  get/put が対称になる。
- 独立コミット **"Fix v3d runtime PM refcount leak in userqpu path"** として入れる
  (userqpu 移植コミットと分離し、diff で判断できるようにする)。
  同じ修正は現行 6.12 の `patched/stable_20250916` にもそのまま適用可能。

**修正の確認方法**(実機): ジョブ実行完了後に
`/sys/bus/platform/devices/*v3d*/power/runtime_status` が `suspended` に戻ること。
未修正だと初回ジョブ以降 `active` のまま。

## 検証

- 完了時の変更ファイルが以下の12ファイルのみであることを確認
  (`git diff --stat stable_20260609..HEAD`):
  - userqpu: `arch/arm/boot/dts/broadcom/bcm270x.dtsi`,
    `arch/arm/boot/dts/broadcom/bcm2835-rpi-common.dtsi`, `arch/arm/mm/ioremap.c`,
    `drivers/firmware/raspberrypi.c`, `drivers/gpu/drm/vc4/vc4_drv.h`,
    `drivers/gpu/drm/vc4/vc4_gem.c`, `drivers/gpu/drm/vc4/vc4_irq.c`,
    `drivers/gpu/drm/vc4/vc4_regs.h`, `drivers/gpu/drm/vc4/vc4_v3d.c`,
    `include/soc/bcm2835/raspberrypi-firmware.h`
  - hang_limit: `drivers/gpu/drm/v3d/v3d_sched.c`(timeout 1行のみ。
    `git diff stable_20260609..HEAD -- drivers/gpu/drm/v3d/` で確認)
  - dtbinst: `scripts/Makefile.dtbinst`
- `git diff --check stable_20260609..HEAD` で whitespace エラーなし
- Docker ビルドで v8 (bcm2711_defconfig) / 2712 (bcm2712_defconfig) 両方の bindeb-pkg が
  成功し、`linux-image-6.18.34-idein-rpi-{v8,2712}_1.20260609.0-0-bookworm_arm64.deb` が
  生成されること(userqpu は vc4/v3d/firmware に触るため、コンパイル成功が API 互換の確認)
- deb の内容検証: 手順6の loop で v8 / 2712 の各 image deb に
  overlays/README が格納されていることを確認(`88731b9779a9` の目的の直接確認)

## 実機動作テスト(userqpu / PM 修正)

テストスクリプト: **`/home/cutsea110/devel/idein/qmkl/test/userqpu-kernel-test.sh`**
(qmkl リポジトリに作成済み・未コミット)

**テスト経路の根拠**: userqpu パッチと直接やりとりするユーザ空間コードは
`mailbox` リポジトリの `mailbox_qpu_execute()` / `mailbox_qpu_enable()`
(`src/wrap_hello.c`。/dev/vcio に tag 0x00030011 / 0x00030012 を発行)で、
qmkl がそのエンドツーエンドの利用者(`src/launch_qpu_code.c` → libmailbox →
/dev/vcio → パッチ済みカーネルの `vc4_filter_property()` が横取り)。
そのため qmkl の実機テストバイナリ(sgemm / scopy / vsAbs)を数値検証つきで
回すことで、SET_ENABLE_QPU のフィルタ・EXECUTE_QPU の submit・完了割り込み
(DBQITC)・seqno wakeup のパッチ経路全体を検証できる。

**実行環境**: Pi Zero 2 / 2 / 3(VideoCore IV)+ パッチ済み arm64 カーネル
(`6.18.34-idein-rpi-v8`)+ `dtoverlay=vc4-kms-v3d`。qmkl とその依存
(libmailbox / librpimemmgr / vcsm)をビルド・インストール済みであること。
root または `video` グループで実行。

**使い方**:

```bash
test/userqpu-kernel-test.sh [反復回数]   # 既定3周。失敗が1件でもあれば非ゼロ終了
```

**チェック内容**:

1. 前提: カーネル名(idein ビルドか)、`/dev/vcio` の存在と権限、
   **`vc4_v3d` ドライバが bind されているか**(未 bind だと tag がファームウェアに
   素通りしパッチを全くテストしないため、ハードエラー)。
2. QPU 実行: `scopy` / `vsAbs` / `sgemm` を反復実行。qmkl のテストバイナリは
   **失敗しても exit 0** のため出力解析で判定する(scopy/vsAbs は
   「GPU and CPU differ」の有無、sgemm は「Maximum absolute error」を閾値
   `SGEMM_MAX_ABS_ERR`(既定 0.1)と比較)。パッチが壊れていると EXECUTE_QPU の
   ioctl が永久待ちになるため各テストは `timeout`(既定 180 秒)配下で実行し、
   タイムアウトは HANG として報告する。
3. runtime PM: 全ジョブ完了後に v3d の `runtime_status` が `suspended` に戻ること
   (手順5の PM 参照リーク修正の直接検証。未修正カーネルでは初回ジョブ以降
   `active` のまま)。コンポジタ等が GPU を使う環境では `SKIP_PM_CHECK=1` で回避。
4. dmesg: 実行中に vc4/v3d のエラーログが出ていないこと(root 時のみ)。

**カバー範囲**: このスクリプトは userqpu(vc4 = Pi Zero 2〜3)と PM 修正の検証用。
`hang_limit_ms`(v3d = Pi 4/5)の 10 秒 timeout はカバーしない(下記リスク欄参照)。

## リスク・注意点

- コンパイル成功は API 互換の最低限の確認。リリース前の既存実機フローでは、Pi 4 相当で
  (1) userqpu の既知ワークロードが完走すること、(2) 通常 GPU 利用と併用できること、
  (3) ジョブ完了後に `/sys/bus/platform/devices/*v3d*/power/runtime_status` が
  `suspended` に戻ること、を必須ゲートとして記録する。(1)(3) は上記
  「実機動作テスト」のスクリプトで確認できる。2712 のコンパイルは userqpu の
  機能確認の代替にはならない。
- `vc4_firmware_qpu_execute()` は firmware の `timeout` / `noflush` を利用せず、user QPU
  実行中は VC4 hangcheck も停止する。したがって `v3d_sched.c` の 10 秒 timeout 検証は
  userqpu のハング保護確認ではない。timeout の実機確認は通常の V3D scheduler workload を
  対象にして別途記録する。
- arm32 はコンパイル不可のまま(スコープ外と決定済み)。将来 32-bit 出荷が必要になったら
  `memremap(paddr, size, MEMREMAP_WB)` 直呼びへの書き換え(arm32/arm64 両対応になり
  ioremap.c の hunk も不要になる)+ `ARCH=arm` ビルド検証を追加する。

## Codex レビューへの対応(2026-07-13 検証結果)

| 指摘 | 検証結果 | 対応 |
|---|---|---|
| vc4_gem.c の drm_exec 移行でコンパイル不可 | **正しい**(6.18 の両関数シグネチャを確認) | 手順2に移植内容を明記 |
| arch_memremap_wb 3引数化 | **正しい**(ただし arm32 のみ影響。v8/2712 は arm64) | arm32 スコープ外と決定。リスク欄に将来対応を記載 |
| PM 参照リーク | **正しい**(現行 6.12 リリースにも存在する既存バグ) | 手順5で独立コミットとして修正 |
| 「3パッチ分のみ」の完了条件 | 妥当 | 期待ファイル一覧(12件)+ `git diff --check` に更新 |
| amend 前の staging | **必要** | 手順2に `git add` とメッセージ付き amend を明記 |
| deb 内の overlays/README 確認 | 妥当 | 手順6で各 image deb を正しいパスから loop 検証 |
| builder の固定・clone スキップ | すべて事実 | 既存 builder の SHA を検証し、成果物を一時ディレクトリへ配置 |
| `${GID}` / Docker image の再現性 | すべて事実 | `id -u:id -g` を使用し、pull 後の image digest を記録 |
| `/tmp` のビルド容量 | この環境では空き約4.5GBで不足 | 手順6では容量のある `.build/` 配下へ成果物を配置 |
| workflow_dispatch input / Release の順序 | **必須** | tag input と既存 Release への asset upload 手順を明記 |
| userqpu の timeout / hangcheck 制限 | **正しい** | 実機の userqpu 検証と V3D timeout 検証を別ゲートとして記録 |

## 実装・検証結果 (2026-07-13)

- `patched/stable_20260609` を作成し、userqpu 移植、hang timeout、dtbinst、PM 参照リーク修正を
  ローカル4コミットとして適用した。push・tag 作成は行っていない。
- `git diff --check stable_20260609..HEAD` は成功し、変更ファイルは検証節の12ファイルのみ。
- builder は `/home/cutsea110/devel/idein/rpi-kernel-builder` の
  `3f2730772838850fd2d651a3e35793c95a5227ec` を使用した。pull した Docker image の digest は
  `idein/rpi-kernel-deb-builder@sha256:8397c3720690af3a84184848b2b7d46ecd80739fde13b7dcd588fd3e5e4e7c1b`。
- `bcm2711_defconfig` (v8) と `bcm2712_defconfig` (2712) の `prepare modules_prepare`、および
  変更対象の VC4/V3D/firmware オブジェクトのクロスコンパイルは両方で成功した。単独の
  `M=drivers/gpu/drm/vc4 modules` は VC4 の全オブジェクト生成後、全体カーネル未ビルドのため
  `Module.symvers` が存在しないことによる modpost の未解決シンボルで停止した（コンパイルエラーではない）。
- v8 では `make dtbs` と `make dtbs_install` も成功し、ログで
  `INSTALL overlays/README ... -> /work/dtbs-install-v8/overlays/README` を確認した。これは
  `scripts/Makefile.dtbinst` の修正がステージングに反映されることの直接確認である。
- 元の `bookworm-20251028` image では、6.18 が `Build-Depends-Arch` に要求する native 側の
  `libdw-dev` と `python3` がなく、`bindeb-pkg` の Debian build dependency 確認で停止した。
  `rpi-kernel-builder` のローカル branch `codex/kernel-6.18-build-deps` に commit
  `378d661` を作成し、Dockerfile の native apt パッケージ一覧へこの2パッケージだけを追加した。
  その Dockerfile からローカル image
  `idein/rpi-kernel-deb-builder:bookworm-20251028-kernel618deps`
  (`sha256:43c96effa33551494dc99815b2695ec80638ebf9b92fa3842241a8f9419e48cc`) を作成した。builder の
  remote への push や既存 image の変更は行っていない。
- 上記 image と新しいビルド作業ディレクトリで、`build_kernel_deb.bash "1.20260609.0-0" v8`
  (38分22秒) と `build_kernel_deb.bash "1.20260609.0-0" 2712` (27分20秒) をともに終了コード0で
  完走した。両 image deb の `Package` / `Version` / `Architecture` と `overlays/README` の格納を
  検査し、各 deb に README が1件だけ含まれることを確認した。
  - v8: `linux-image-6.18.34-idein-rpi-v8_1.20260609.0-0-bookworm_arm64.deb` —
    `linux-image-6.18.34-idein-rpi-v8` / `1.20260609.0-0-bookworm` / `arm64`、
    SHA-256 `f63e4b74af31a91a80248ffb075cb335be369f82a4e0b624315037df0f6959b9`
  - 2712: `linux-image-6.18.34-idein-rpi-2712_1.20260609.0-0-bookworm_arm64.deb` —
    `linux-image-6.18.34-idein-rpi-2712` / `1.20260609.0-0-bookworm` / `arm64`、
    SHA-256 `084e5b186102c8de2e5b2d16bd107a9fcb8ebd7b7abe3c1314c252b362c3a74f`

## パッチ適用のレビュー結果 (2026-07-13, Claude によるレビュー)

Codex が適用した `patched/stable_20260609`(4コミット)を独立にレビューした。
**結論: 問題なし。** ソース上の懸念は見つからなかった。

| コミット | 内容 | 判定 |
|---|---|---|
| `e1efe88dfbf8` | Apply userqpu patch to stable_20260609 | OK(下記) |
| `0955bcd80ee7` | Change hang_limit_ms to 10sec | OK(`.timeout` 500→10000ms の1行のみ、計画どおり) |
| `529a973916f7` | Copy overlays/README into deb package | OK(range-diff で原型と完全一致) |
| `a8eec37d3dec` | Fix v3d runtime PM refcount leak in userqpu path | OK(下記) |

確認方法と結果:

- **構成**: 変更ファイルは検証節の12ファイルのみ(`git diff --stat`)。
  `git diff --check stable_20260609..patched/stable_20260609` はクリーン。
- **userqpu 移植**(`git range-diff stable_20250916..1.20250916.1
  stable_20260609..patched/stable_20260609` で旧シリーズと突き合わせ):
  旧パッチとの差分は意図した移植内容に正確に限定されている。
  - `ww_acquire_ctx` → `struct drm_exec` 変換、6.18 シグネチャの
    `vc4_lock_bo_reservations(exec, &exec_ctx)` / `vc4_queue_submit(dev, exec, &exec_ctx, NULL)`
  - エラーパス: lock 失敗時は fini 不要(6.18 側が内部で `drm_exec_fini()` 済み)、
    memremap 失敗時・queue_submit 失敗時は `drm_exec_fini()` → `vc4_complete_exec()`
    (upstream `vc4_submit_cl_ioctl()` の `fail_unreserve:`/`fail:` と同一順序)
  - 旧パッチの `vc4_queue_submit` 戻り値型変更 hunk(`int`→`uint64_t`)は正しく破棄
  - raspberrypi.c / vc4_irq.c / vc4_regs.h / Makefile.dtbinst の hunk は
    +/- 行の機械的比較で原型と完全一致
  - `vc4->firmware` は upstream 既存メンバ(vc4_drv.h:100)の利用で重複定義なし
- **PM リーク修正**: `vc4_exec_alloc()` から refcount ブロックを削除し、未使用になった
  `dev` 引数も除去。最終ツリーで `vc4_v3d_pm_get()` は vc4_gem.c:1096
  (submit_cl_ioctl)と :1227(firmware_qpu_execute)の各1回、`vc4_v3d_pm_put()` は
  :918(complete_exec)の1回で、**get/put が対称**。
- **ビルド対象ツリーの照合**: `.build/rpi-kernel-build-20260609-final.*/custom-kernel-build/linux`
  の HEAD が `patched/stable_20260609` と一致することを確認。

## 実機テスト状況と引き継ぎ (2026-07-14, Claude → Codex)

### 実機環境(構築済み)

- 対象機: **Raspberry Pi 3B+**, `pi@192.168.11.45`(パスワード raspberry、
  鍵 `~/.ssh/raspi_userqpu_test` 登録済み)。OS は RPiOS **trixie** arm64。
- カーネル `linux-image-6.18.34-idein-rpi-v8` **1.20260609.0-0-trixie** インストール・起動済み
  (`uname -r` = 6.18.34-idein-rpi-v8)。DTB はパッチ入り
  (`/proc/device-tree/soc/v3d@7ec00000/firmware` あり)。`vc4_v3d` bind 済み。
- `~/src/` に qmkl / mailbox / librpimemmgr / qpu-assembler2 / qpu-bin-to-hex /
  py-videocore を配置し、**全スタックをビルド済み**。生成 deb(回収して再利用可能):
  - `~/src/mailbox/build/libmailbox_3.1.1_arm64.deb`
  - `~/src/librpimemmgr/build/librpimemmgr_5.0.0_arm64.deb`(vcsm 依存のため
    `dpkg -i --force-depends` でインストール済み)
  - `~/src/qmkl/qmkl-1.0.0-Linux.deb`
- vcsm: **aarch64 の userland ビルドは libvcsm を生成しない**ため手動ビルドして
  `/opt/vc/lib/libvcsm.so` に配置済み(userland の
  `host_applications/linux/libs/sm/user-vcsm.c` を gcc -shared でビルド、
  ヘッダは `/opt/vc/include/interface/vcsm/user-vcsm.h` に配置。
  `/etc/ld.so.conf.d/00-vc.conf` 作成済み)。deb 化は未実施(残作業)。
- py-videocore は `~/venv-pyvc`(qmkl ビルド時のみ必要)。`rpi-vcsm` は PyPI に
  ないため `git+https://github.com/Idein/rpi-vcsm.git` から導入。
- WSL 側リポジトリへの未コミット修正(rsync 済み、要 commit):
  - `qmkl/CMakeLists.txt`: `cmake_minimum_required` 3.0→3.10(CMake 4 対応)、
    aarch64 分岐追加(`-mfloat-abi`/`-mfpu` は 32-bit 専用のため)
  - `qmkl/src/include/local/error.h`: `exit_handler` に extern 付与(GCC 10+ の
    -fno-common 対応)
  - `qmkl/test/userqpu-kernel-test.sh`: 実機テストスクリプト(新規)

### 発見したカーネルバグ(修正済み・再ビルド中)

**テスト実行で全ケース HANG する実バグを発見・修正した。**

- 症状: scopy/vsAbs/sgemm すべて EXECUTE_QPU の ioctl から返らない。
  ハング中スタックは `vc4_wait_for_seqno ← vc4_firmware_qpu_execute ←
  rpi_firmware_property_list`。
- 証拠: `V3D_SRQCS = 0x00010100`(QPU 要求1・**完了1** = プログラム自体は完走)、
  v3d IRQ(/proc/interrupts の irq 52)カウント 0、
  `/dev/mem` 直読みで **`V3D_DBCFG = 0`**(DBQITE=0xfff は生きている)。
- 原因: `V3D_DBCFG`(QPU→ホスト割り込み有効)は bind 時に一度書くだけで、
  runtime PM による v3d 電源断でレジスタが消え、resume 経路
  (`vc4_v3d_init_hw`/`vc4_irq_enable`)では誰も書き直さない。
  **PM 参照リーク(a8eec37d で修正)がこのバグを隠していた**(リークすると
  v3d が suspend しないため DBCFG が消えない)。6.12 の現行リリースにも同じ
  潜在バグがある(リークで顕在化しないだけ)。
- 修正: **`cccbe495a89c` "Fix QPU host interrupt enable lost across V3D runtime
  suspend"**(patched/stable_20260609 の新 tip)。`V3D_WRITE(V3D_DBCFG, 1)` を
  bind から `vc4_v3d_init_hw()`(bind + 全 runtime resume で実行)へ移動。

### 進行中・残作業(Codex への引き継ぎ)

1. **カーネル再ビルド(進行中)**: container `208fc8d6aebb`
   (image `idein/rpi-kernel-deb-builder:trixie-20260714-kernel618deps`)が
   `.build/rpi-kernel-build-20260609-trixie.3sVqJO` で v8 を再ビルド中。
   ビルドツリーは cccbe495a89c に更新済み。`docker wait 208fc8d6aebb` で完了待ち →
   `custom-kernel-build/linux-image-*v8*.deb` のタイムスタンプ更新を確認。
   deb 名は既存と同一(1.20260609.0-0-trixie)なので上書きに注意。
2. 新しい v8 deb を Pi へ scp → `sudo dpkg -i`(再インストール)→
   /boot/firmware への配線を確認(config.txt の `kernel=` 行の指す先に
   `/boot/vmlinuz-6.18.34-idein-rpi-v8` をコピーし直す。DTB は変更なしのため
   そのままで可)→ **再起動**(現在 v3d のジョブキューがハングした exec で
   詰まっており、再起動は必須。ハング中の scopy プロセスが残っている場合あり)。
3. 再起動後にテスト実行: `cd ~/src/qmkl && sudo ./test/userqpu-kernel-test.sh`
   (/dev/vcio が root 専用のため sudo 必須)。
   合格条件: scopy/vsAbs/sgemm 3周 PASS + runtime PM が suspended に戻る + dmesg クリーン。
   検証ポイント: 修正が正しければ「v3d が一度 suspend した後の最初のジョブ」も
   完了する(数分置いて2回目を実行すると suspend→resume 経路を確実に踏める)。
4. 2712 の再ビルド(同 image で FLAVOR=2712)と bookworm 側
   (`.build/rpi-kernel-build-20260609-final.pDmExF`、image
   `bookworm-20251028-kernel618deps`)の v8/2712 再ビルドも DBCFG 修正込みで
   やり直すこと(既存 deb は cccbe495a89c を含まない)。
5. 注意: この Pi は **低電圧警告(Undervoltage detected)が頻発**している。
   テストが不安定な場合は電源を疑うこと。

### 実機テスト結果 (2026-07-14, DBCFG 修正カーネルで合格)

DBCFG 修正(cccbe495a89c)入りカーネル `1.20260609.0-1-trixie` (v8) を Pi 3B+ に
インストール・再起動後、`sudo ./test/userqpu-kernel-test.sh` を **2回**実行
(2回目は1〜2分アイドル後 = v3d が完全に runtime suspend した状態からの復帰を検証)。

- **2回とも 11 passed / 0 failed**。
  - scopy / vsAbs: GPU=CPU 完全一致 ×3周
  - sgemm: max abs error 0.0009〜0.0010(fp32 として正常)×3周
  - runtime PM: 全ジョブ後 `suspended`(PM リーク修正の実証)
  - dmesg: vc4/v3d エラーなし
- v3d の autosuspend は約 40ms のため、テストバイナリ間でも suspend→resume を
  繰り返しており、DBCFG 修正が壊れていれば1回目から HANG する。2回目(長時間
  アイドル後)も PASS したことで修正の検証は完了。
- 修正前カーネル(a8eec37d 時点)では全ケース HANG することを確認済み(再現性あり)。

これで検証節の全項目 + 実機動作テストがクリア。残りは push・タグ付け(ユーザー実施)と
deb 再ビルドの完了確認のみ。

ビルド検証の結果(DBCFG 修正前の記録): 6.18 の native 依存(`libdw-dev` `python3`)を追加したローカル image
`idein/rpi-kernel-deb-builder:bookworm-20251028-kernel618deps` で
`build_kernel_deb.bash "1.20260609.0-0"` の **v8 / 2712 が両方完走**(2026-07-13)。
`.build/rpi-kernel-build-20260609-final.pDmExF/custom-kernel-build/` に
`linux-image-6.18.34-idein-rpi-{v8,2712}_1.20260609.0-0-bookworm_arm64.deb`
(+ headers / libc-dev)が生成された。両 image deb に
`/usr/lib/linux-image-6.18.34-idein-rpi-{v8,2712}/overlays/README` が格納されていることを
`dpkg-deb -c` で確認済み(検証節の全項目クリア)。

残作業: 実機動作テスト(qmkl スクリプト、上記「実機動作テスト」節)→ push・タグ付け
(手順7、ユーザー実施)。builder の image 変更(Dockerfile への `libdw-dev python3` 追加)は
ローカル image のみで、rpi-kernel-builder リポジトリへの反映は別途必要。

## Trixie 向け builder と package 検証結果 (2026-07-14)

- **Bookworm は保持した。** 既存の `Dockerfile`、ローカル image
  `idein/rpi-kernel-deb-builder:bookworm-20251028-kernel618deps`、および
  `.build/rpi-kernel-build-20260609-final.pDmExF/` の Bookworm 成果物は変更・削除していない。
  Trixie は独立した Dockerfile・image tag・成果物ディレクトリで扱う。
- `rpi-kernel-builder` のローカル branch `codex/kernel-6.18-trixie` に commit `b1b0da8`
  (`Add trixie kernel deb builder`)を作成した。`Dockerfile.trixie` は
  `debian:trixie-slim` を基に、6.18 の native build dependency である `libdw-dev` と
  `python3` を含める。Trixie では `libncurses-dev` を使用し、`adduser` が slim image に
  ないため既存ユーザーを sudo グループへ加える処理を `usermod` で行う。
- `build_kernel_deb.bash` は `DEBIAN_SUITE` を受け取るようにし、未指定時は従来どおり
  `bookworm` とする。許可する値は `bookworm` / `trixie` のみで、package version は
  `${PKG_VERSION}-${DEBIAN_SUITE}` となる。従来の Bookworm 呼び出しとの互換性を維持する。
- ローカルで作成した Trixie image は
  `idein/rpi-kernel-deb-builder:trixie-20260714-kernel618deps`
  (`sha256:c5710447aaa38629b5a521331a53bde458063af62f959147cbe69c536688a581`)。
  コンテナ内で `VERSION_CODENAME=trixie`、`DEBIAN_SUITE=trixie`、`libdw-dev=0.192-4`、
  `python3=3.13.5-1` を確認した。
- ソース HEAD `33a595afca710845f493a4c37745dae13f91e471` を新しい作業ディレクトリ
  `.build/rpi-kernel-build-20260609-trixie.3sVqJO/custom-kernel-build/` へ複製し、上記 image で
  `build_kernel_deb.bash "1.20260609.0-0" v8` (37分38秒) と
  `build_kernel_deb.bash "1.20260609.0-0" 2712` (35分12秒) をともに終了コード0で完走した。
  両 image deb の `Package` / `Version` / `Architecture` と `overlays/README` を検査し、
  README は各 package に1件だけ格納されることを確認した。
  - v8: `linux-image-6.18.34-idein-rpi-v8_1.20260609.0-0-trixie_arm64.deb` —
    `linux-image-6.18.34-idein-rpi-v8` / `1.20260609.0-0-trixie` / `arm64`、
    SHA-256 `268c460b6eec7ac55507eba0a4d1fed5cb878b8874bb015f8a052445a203c559`、
    raw kernel `build-v8/arch/arm64/boot/Image`
  - 2712: `linux-image-6.18.34-idein-rpi-2712_1.20260609.0-0-trixie_arm64.deb` —
    `linux-image-6.18.34-idein-rpi-2712` / `1.20260609.0-0-trixie` / `arm64`、
    SHA-256 `cb9039e31c8f787a88ca2abb97a3d2ccc9f77e030d063b22e997426f0c92311e`、
    raw kernel `build-2712/arch/arm64/boot/Image`
- 変更はローカルのみであり、builder の remote への push、既存 Docker image の置換、CI/release
  workflow の変更は行っていない。CI は明示的に切り替えない限り従来の Bookworm image を使う。
  Bookworm/Trixie の image package は同名で version だけが異なるため、同一 rootfs へ同居させず、
  利用する Debian suite に対応した方を選んで導入する。
