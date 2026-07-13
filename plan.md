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
- `build_kernel_deb.bash "1.20260609.0-0" v8` はコンパイル開始前の Debian build dependency
  確認で停止した。使用した `bookworm-20251028` image には、6.18 が
  `Build-Depends-Arch` に要求する native 側の `libdw-dev` と `python3` が入っていない。
  `bindeb-pkg` はこれらの依存確認を省略しないため、原因は再現・確定している。
  最小の builder 修正は Dockerfile の native apt パッケージ一覧へ `libdw-dev python3` を
  追加して新しいローカル image tag で再ビルドすることだが、手順6の「失敗時は相談」に従い
  **この builder/image の変更および v8/2712 の package 再実行は未実施**とする。

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

ビルド検証の状況: 6.18 の native 依存(`libdw-dev` `python3`)を追加したローカル image
`idein/rpi-kernel-deb-builder:bookworm-20251028-kernel618deps` で
`build_kernel_deb.bash "1.20260609.0-0" v8` を実行中(レビュー時点で未完)。
v8 完了後に deb 生成と overlays/README 格納を確認し、2712 も同様に実行する。
