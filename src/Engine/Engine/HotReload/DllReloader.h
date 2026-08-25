#pragma once
#include <string>

namespace mye {

class ScriptHost;

// GameLogic.dll のリロード制御 (engine_spec.md 8.4)。
//
// 手順:
//   1. ビルド出力 (bin\x64\{cfg}\GameLogic.dll) のタイムスタンプをポーリング
//   2. 共有読みオープン (FILE_SHARE_READ) でリンカの書き込み完了を確認 — 書き手が
//      居れば失敗 = 次フレーム再試行。読み手 (もう片方のエンジンプロセス) とは共存
//   3. DLL + PDB を cache\hot\v{N}\ に「元のファイル名のまま」コピー
//      → /PDBALTPATH:GameLogic.pdb と組み合わせて、デバッガはコピー先の PDB を
//        ロードする。ビルド出力はロックされず、ブレークポイントも維持される
//   4. フェーズ 2 (スクリプト非実行中) で ScriptHost::LoadModule → 旧 DLL 解放
class DllReloader {
public:
    void Init(ScriptHost* host, const std::wstring& buildDllPath, const std::wstring& cacheDir);

    // 起動時の初回ロード (DLL が無ければ false — スクリプト無しで続行)
    bool LoadInitial();

    // フェーズ 2 で毎フレーム呼ぶ。リロードを実行したら true
    bool Update();

    uint32_t Version() const { return counter_; }

    // 書き手 (リンカ / コピー) の不在確認。0 = 書き込み完了 / それ以外 = Win32 エラーコード。
    // stateless な static にしてあるのはセルフテストから直接叩くため。
    // 戻り値が unsigned long (= DWORD) なのはヘッダに Windows.h を入れないため
    static unsigned long ProbeWritable(const std::wstring& path);

    // ProbeWritable が共有違反 (32) を返す間だけ待つ有界版。最後のエラーコードを返す
    // (不在 (2/3) 等は待っても無駄なので即返す)
    static unsigned long WaitUntilWritable(const std::wstring& path, uint32_t timeoutMs);

private:
    bool TryCopyAndLoad();

    ScriptHost* host_ = nullptr;
    std::wstring dllPath_;
    std::wstring pdbPath_;
    std::wstring cacheDir_;
    uint64_t lastWriteTime_ = 0; // FILETIME (ロード済み DLL のもの)
    uint32_t counter_ = 0;
    uint64_t lastPollMs_ = 0;
};

} // namespace mye
