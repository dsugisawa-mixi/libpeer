# Pico 2 W リアルタイム Opus 音声ストリーミング PoC

> RP2350 / Pico 2 W 上で I2S MIC + I2S DAC + LCD + CYW43 Wi-Fi を同時に動かし、Opus 圧縮した音声をクラウドへ送受信する低帯域リアルタイム音声端末 PoC。

教育用・低価格 MCU でも、PIO I2S・DMA・dual-core 分離・Opus 圧縮を組み合わせれば、クラウド接続型のリアルタイム音声端末を構成できる ── このファームウェアはそれを実証する。

---

## この PoC が証明すること

| 実証項目 | 詳細 |
|---|---|
| **I2S マイク入力** | PIO で I2S プロトコルを生成し、MCU 上でリアルタイム音声キャプチャ |
| **I2S DAC/スピーカ出力** | PIO + DMA ENDLESS ring で低レイテンシ再生 |
| **LCD ステータス UI** | ST7789 1.3" SPI LCD で状態表示 + 操作 UI |
| **CYW43 Wi-Fi 常時接続** | TLS 常時接続を維持しながら音声処理を並行実行 |
| **HTTPS クラウド POST / ポーリング** | 音声の送信・受信を HTTPS 上で実現 |
| **Opus 圧縮ペイロード** | PCM 直送ではなく Opus 16–24 kHz mono で帯域を大幅削減 |
| **Dual-core 完全分離** | Audio/UI と Network を mutex なしで分離、lock-free SPSC ring で接続 |
| **BLE プロビジョニング** | iOS アプリからゼロタッチで Wi-Fi 設定を書き込み |

**要するに:** RP2350 クラス（$7）の MCU 1 台で、Opus / TLS / Wi-Fi / I2S Audio が同時に回り、スマホも PC も不要でクラウド音声端末が成立する。

---

## なぜ Pico 2 W で難しいか

- **RAM 520 KB** — TLS バッファ、Opus デコーダ、オーディオ ring、Wi-Fi スタックが全て同居する
- **Wi-Fi と Audio の同時処理** — cyw43_arch_poll と I2S DMA を止めずに回し続ける必要がある
- **I2S を PIO で実装** — RP2350 には I2S ペリフェラルがないため PIO プログラムで生成
- **Opus デコード** — SILK fixed-point デコーダの VLA が ~3 KB のスタックを消費し、デフォルト 4 KB では溢れる
- **バックプレッシャー制御** — クラウド → TCP → ring buffer → DMA を跨ぐフロー制御を自前で構成


## システム構成

<div style="text-align:center; width:80%; box-sizing:border-box;">
    <img src="images/radio_mix_talk_architecture.svg" style="max-width:70%; height:auto;" />
</div>

3 ゾーンに分離される。

**Cloud (minimum)** — デバイス登録・WSS トンネル中継・Opus 音声の下り relay のみ。GPU も CDN キャッシュも持たない。

**Publisher (maximum)** — radio / MIC / music などの custom service を動的に登録。音声ミキシングとエンコードは Publisher 側で完結し、Cloud はパケットを通過させるだけ。

**MCU + Speaker** — RP2350 + CYW43 が Wi-Fi 経由で Cloud に TLS 常時接続し、Tunnel 経由で下りてきた Opus パケットを on-chip デコード → I2S DAC で再生する。


## Core 分離アーキテクチャ

<div style="text-align:center; width:80%; box-sizing:border-box;">
    <img src="images/core_architecture.svg" style="max-width:70%; height:auto;" />
</div>

RP2350 の 2 コアを完全に分離し、mutex なしで音声パイプラインを構成する。

| | Core 0 (20 KB stack) | Core 1 (4 KB stack) |
|---|---|---|
| **役割** | Audio / UI / Opus Encode & Decode | Network / Transport / Forwarding |
| **Stack 配置** | Main RAM top + SCRATCH_X (併合) | SCRATCH_Y |
| **重い処理** | opus_encode (SILK VLA peak 18.9 KB) | TLS handshake / cyw43_arch_poll |
| **Loop cadence** | 1 ms | 500 μs |
| **IC 方向** | Consumer (audio data) | Producer (opus pkt / PCM chunk) |

Core 間通信は **64 KB の SPSC ring buffer + HW FIFO notify (8 slot)**。バックプレッシャーは audio ring 充填率 75% → IC dequeue 停止 → IC ring 充填 → ic_send_avail==0 → forward_pump skip → g_https_resp 充填 → recv_cb ERR_MEM → TCP window close まで自動カスケードする。

<div style="page-break-before: always;"></div>

## 音声パイプライン

上りと下りは **独立した DMA ring を持ち、サイズ・方向・バッファリング戦略が非対称** である。

<div style="text-align:center; width:80%; box-sizing:border-box;">
    <img src="images/dma_asymmetry.png" style="max-width:70%; height:auto;" />
</div>

下りはリアルタイム連続再生のため大きな DMA ring (32 KB) で揺らぎを吸収し、上りは発話単位のバッチ送信のため ring は最小限 (4 KB) にして encoder + accumulator 側でバッファリングする。用途が違うので対称にする理由がない。

---

## メモリレイアウト

<div style="text-align:center; width:80%; box-sizing:border-box;">
    <img src="images/stack_layout.svg" style="max-width:70%; height:auto;" />
</div>

RP2350 は Main RAM (512 KB) とは別に **SCRATCH_X / SCRATCH_Y** という 4 KB × 2 の専用 SRAM バンクを持つ。SDK デフォルトでは Core 0 が SCRATCH_Y、Core 1 が SCRATCH_X をスタックに使い、バス競合なしに各コアが独立アクセスできる。

しかし Opus (SILK fixed-point) のエンコーダは内部で **VLA (Variable Length Array)** を多用し、これはヒープではなくスタック上に確保される。関数呼び出し中にスタックが一気に膨らみ、実測で `opus_encode` が **18,912 B** をピーク消費する（デコード側は 4,864 B）。デフォルトの 4 KB では到底足りない。

そこで Core 0 のスタックを **Main RAM top (0x2007c000) 〜 SCRATCH_X 末尾 (0x20081000) の 20 KB** に拡張。物理的に隣接しているため連続領域として併合でき、ヒープコストはゼロ。heap 上限は `__StackBottom` で切る。Core 1 は SCRATCH_X を明け渡し **SCRATCH_Y (4 KB)** に移動 — TLS / Wi-Fi 処理のみなので 4 KB で十分。

### スタックピークの計測方法

MMU もスタックガードページもない MCU では、スタックオーバーフローはヒープやリターンアドレスを無言で破壊する — ハングや見当違いのクラッシュとして現れ、原因特定が極めて困難。RP2350 にはメモリ範囲のハードウェアウォッチポイントもないため、ファームウェアに **canary-paint 方式のハイウォーターマーク計測** を組み込んでいる（`main.c`）:

1. **Paint** — ブート直後、スタックが浅いうちに `stackdiag_paint()` がスタック領域全体（+ `__StackBottom` 以下 16 KB のヒープギャップ）をカナリアワード (`0xC5C5C5C5`) で塗りつぶす。現在の SP 直下 256 B はガードとして残す。

2. **Scan** — メインループ内で 2 秒ごとに `stackdiag_hiwater()` が floor から上方スキャンし、最初の非カナリアワードを検出。`__StackTop` からの距離がそれまでのピーク使用量 — `opus_encode` と `opus_decode` 両方のピークを通算で捕捉する。

3. **オーバーフロー検出** — ハイウォーターマークがスタック領域サイズを超えた場合、溢れ量（"spill"）と最深部 8 ワードの hex dump を出力。Flash リターンアドレス (`0x10xxxxxx`) か RAM ポインタ (`0x20xxxxxx`) かサンプル値かで、破壊元がコールチェーンか DMA の暴走かを判別できる。

4. **Heap プローブ** — `stackdiag_heap_report()` が `sbrk(0)` で現在のヒープブレークを読み、`__StackBottom` までの残り容量を報告。逆方向の障害モード検出用 — `PICO_STACK_SIZE` を上げすぎるとヒープが 1:1 で縮み、mbedTLS の `altcp_tls_new` だけで ~16 KB のレコードバッファが必要なため、`0x6000` (24 KB) にしたら Wi-Fi が壊れた。

5. **静的サイズプローブ** — `opus_encoder_get_size(1)` / `opus_decoder_get_size(1)` を init 時に出力し、ヒープ確保される状態サイズ（VLA のスタックコストとは別）を把握。

(A) 静的サイズプローブ、(B) ヒープブレーク監視、(C) canary-paint ハイウォータースキャンの 3 点セットにより、`opus_encode` の実測ピーク 18,912 B を収めつつ TLS 用ヒープも確保できる最小値として 20 KB に収束させた。

---

## 帯域削減効果

PCM 直送と Opus 圧縮の比較:

| | PCM 16 kHz 16-bit mono | Opus 16 kbps |
|---|---|---|
| **ビットレート** | 256 kbps | 16 kbps |
| **削減率** | — | **93.75%** |
| **1 分あたり** | 1.92 MB | 120 KB |

CYW43 の Wi-Fi スループットと RP2350 の処理余力を考えると、Opus 圧縮は「あれば嬉しい」ではなく、**常時接続を安定させるために必須**の設計判断。

---

## Technical Stack

| Layer | What | Why |
|---|---|---|
| MCU | RP2350 dual-core 200 MHz | $7 クラス、Opus リアルタイムデコード可能 |
| Wi-Fi | CYW43 (Pico 2 W) | 常時 TLS 接続 |
| TLS | mbedTLS | HTTPS / WSS |
| Codec (decode) | Opus (SILK fixed-point) 24 kHz mono | Cloud → MCU ストリーミング再生 |
| Codec (encode) | Opus (SILK fixed-point) 16 kHz mono 16 kbps | MCU → Cloud push-to-talk 送信 |
| Audio Out | PIO I2S → PCM5101A DAC | DMA ring 32 KB, ENDLESS mode |
| Audio In | PIO I2S RX ← INMP441 MEMS mic | DMA ring 8 KB, Core 0 で Opus エンコード |
| IC Bus | SPSC ring + HW FIFO | lock-free, zero-copy |
| Provisioning | BLE (BTstack) | iOS app でゼロタッチ設定 |
| Display | ST7789 1.3" LCD | 状態表示 + 操作 UI |

---

## 配線図

<div style="text-align:center; width:80%; box-sizing:border-box;">
    <img src="images/rp2350-wired_bb.png" style="max-width:70%; height:auto;" />
</div>

---

## ビルド

### 前提

- [Pico SDK](https://github.com/raspberrypi/pico-sdk) (≥ 1.3.0)
- ARM GCC toolchain (`arm-none-eabi-gcc`)
- CMake ≥ 3.12

### ビルド手順

```bash
cd cmake.rp2350
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

---

## 依存ライブラリ

- [libopus](https://opus-codec.org/) — Opus audio decoding（SILK fixed-point）
- [cJSON](https://github.com/DaveGamble/cJSON) — JSON parsing
