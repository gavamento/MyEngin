# Epic Online Services SDK (C) の置き方

M93 のオンライン機能 (ロビー・P2P 接続・ボイス) は EOS SDK を使う。
SDK の再配布の可否は Epic の開発者契約に依るので、**このフォルダの中身はコミットしない** (`.gitignore` で除外。この BUILD.md だけ版管理する)。

SDK が無くてもソリューションはビルドできる。そのときオンラインは `local` (同じ PC の開発用の代役) と `none` だけになる。

## 手順

1. [Epic Developer Portal](https://dev.epicgames.com/portal) に組織を作り、プロダクトを 1 つ作る。EOS Developer Agreement に同意する。
2. ポータルの「SDK & Release Notes」から **C SDK (Windows)** をダウンロードして展開する。
3. 展開した `SDK\` の中身を、次の配置でこのフォルダへコピーする。

```
external\eos-sdk\
  Include\eos_sdk.h ほか        (SDK\Include\ の中身すべて)
  Lib\EOSSDK-Win64-Shipping.lib (SDK\Lib\ から)
  Bin\EOSSDK-Win64-Shipping.dll (SDK\Bin\ から)
  ThirdPartyNotices\            (SDK に同梱の告知。配布物のクレジットに含める)
```

4. Visual Studio でソリューションを開き直してビルドする。`build\Common.props` が `Include\eos_sdk.h` の有無を見て `MYE_HAS_EOS=1` を立て、exe へ lib を渡し、DLL を `bin\x64\<構成>\` へコピーする。
5. `assets\project_settings.json` (ゲームのプロジェクトなら `<project>\assets\project_settings.json`) の `online` 節を埋める。値の取り方とポータルの設定は `docs\eos-setup.md`。

## 使っている版

| 版 | 取得日 | 備考 |
|---|---|---|
| (未取得) | | 取得したらここに版と日付を書く。Epic は古い SDK を段階的に非対応にするので、年 1 回程度は上げる |
