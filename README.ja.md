# Raspberry Pi Pico 2 W — Bluetooth PANUでスマホのインターネット回線を利用

**Pico 2 WをBluetooth ClassicのPANUクライアントとして動作させる、実機動作確認済みのサンプルです。** AndroidスマホのBluetoothテザリング（NAP）を経由して、Pico自身がIPv4のインターネット通信を行います。**Wi-Fi、SIM契約、スマホ専用アプリは不要**です。

おまけに、`example.com` へHTTPで到達した後、**APRS-IS（アマチュア無線関連のネットワーク）から実際のデータを無制限に受信**してUSBシリアルに表示します。無線機・電波送信・無線局免許は、この受信専用のサンプルを動かすためには必要ありません。

**実機確認環境：Pico 2 W、Pico SDK 2.3.1、Blackview BV9300（Android）。** 初回ペアリング、保存済みリンクキーによるPico単独リセット後の自動接続、BNEP、DHCP、HTTP、APRS-IS受信、およびスマホのBluetooth再有効化後の復旧まで確認しました。他のスマホや消費電力、長期間の安定性は未検証です。実機検証日：**2026年10月3〜4日**。

## 動作の流れ

```text
Pico 2 W                       Androidスマホ
 Bluetooth Classic PANU  ────  Bluetooth NAP (テザリング)
       │                          │
       └── BNEP / DHCP / lwIP ────┘── モバイル回線──インターネット
                                                      ├── example.com（HTTP）
                                                      └── APRS-IS（受信専用）
```

## Wi-FiテザリングよりBluetooth PANを選ぶ理由

**いちばんの狙いは、スマホ側のバッテリー負担を抑えてモバイルIoTを動かすことです。** Bluetoothテザリングは、一般にWi-Fiホットスポットより消費電力が少ない方式です。少量のセンサーデータや時々行うTCP/IP通信など、速度よりも長時間運用を重視する用途に向いています。[Sonyの比較説明](https://www.sony.com/electronics/support/articles/SX566701)や[Android公式のテザリング解説](https://www.android.com/intl/en_in/articles/tethering-hotspotting/)でも、Bluetooth接続は低消費電力の選択肢として紹介されています。

一方で、**通信速度はWi-Fiより遅く、スマホ側の接続操作はやや不便**です。特に、スマホのBluetoothをOFF→ONするとテザリングの設定がOFFに戻る機種があります。このサンプルはPico側から繰り返し接続を試みますが、スマホの設定を自動でONにすることはできません。

**実測上の注意：** 今回のPico 2 W／BV9300の組み合わせでは、スマホとPico双方の消費電流の比較測定はまだ実施していません。「何％省電力か」「全スマホで必ず省電力か」は未確認です。また、通信方式は省電力BLEではなく**Bluetooth Classic PAN**です。

## Windowsでの試し方

1. **VS Code** に [Raspberry Pi Pico公式拡張機能](https://marketplace.visualstudio.com/items?itemName=raspberry-pi.raspberry-pi-pico) をインストール。
2. このリポジトリのフォルダを開き、ボードは **`pico2_w`**、Pico SDKは動作確認済みの **2.3.1** を選択。
3. **Compile Project** でビルド。`build/pico2w_bt_pan_tether.uf2` が生成されます（出力場所は拡張機能の設定による）。
4. PicoをBOOTSELモードでUSB接続し、UF2を書き込み、USBシリアル端末を開きます。
5. スマホでBluetoothと**Bluetoothテザリング**をONにして、`Pico2W-PANU` とペアリングします。必要に応じて「インターネットアクセス」を許可してください。
6. `[DHCP] Lease acquired.` → `[RESULT] INTERNET PASS` → `[RESULT] APRS-IS PASS` が出れば成功です。

**2回目以降は、スマホ側のBluetoothテザリングを有効にしたままPicoをリセットするだけで、保存済みリンクキーを読んで自動接続します。** ペアリングのやり直しは不要です。ただしPico側のフラッシュを全消去すると、保存されたリンクキーも消える可能性があります。

## 大事な落とし穴：Bluetoothとテザリングのスイッチは別

実機テストの **BV9300では、スマホのBluetoothをOFF→ONするとBluetoothテザリングが自動的にOFFへ戻ります。** この状態でもPicoのSDP検索は成功し、BNEPが一瞬接続した後、数十ミリ秒で切れることがありました。**まずスマホの「Bluetoothテザリング」がONか確認**してください。

Pico側は失敗した接続をおおむね5秒間隔でリトライする設計なので、スマホ側をONに戻せば自動復帰を試みます。

## アマチュア無線の「おまけ」

ファームウェアは `rotate.aprs2.net:14580` に接続し、`pass -1` の**認証なし・受信専用**でログインします。ユーザー名はPicoのBluetooth MAC下位3バイトから `P123456` のように自動生成します。日本付近の矩形フィルタを指定しているため、外国のパケットが混ざることもあります。**APRSパケットの送信処理はありません。**

これによって、ソースを一切編集しなくても、購入した基板からアマチュア無線の気象局、I-Gate、移動局などの情報がUSBシリアルに流れてきます。APRS-ISデータをインターネットから受信するものであり、直接電波を受信しているわけではありません。

## 操作と設定

USBシリアルで `s` + Enter は統計表示、`t` はHTTP/APRS再試験、`a` はAPRS再接続、`r` はPAN接続リトライ、`x` はACL切断試験、`c XX:XX:XX:XX:XX:XX` は接続先MACの手動指定です。

初期設定では全APRSパケットをシリアル出力します。`main.c` の `APRS_PRINT_PACKETS` を `0` にするとカウンタのみになります。`DHCP_START_DELAY_MS=2000` は実機確認した経路を維持するため、診断用の2秒待機を残しています。

詳細： [English README](README.md) / [内部構成](docs/ARCHITECTURE.md) / [トラブルシューティング](docs/TROUBLESHOOTING.md) / [実機試験記録](docs/TEST_REPORT.md)

## ビルド、利用条件

- 手元でのコンパイルが難しい場合は、同梱のGitHub ActionsでUF2を生成できます。ただし公開整理版のワークフロー実行は未確認です。
- サンプル本体とドキュメントはMITライセンス（`LICENSE`）です。著作権表記：`Copyright (c) 2026 Daisuke JA1UMW / CQAKIBA.TOKYO`。二次配布時は著作権表示とライセンス文を保持してください。Pico SDK、BTstack、lwIPの再配布条件はそれぞれ別途確認してください。
- 一般公開時には、テスト時のスマホの実MACアドレスやペアリング情報を含むログを誤ってコミットしないようにしてください。
