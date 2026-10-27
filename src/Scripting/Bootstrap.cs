using System;

namespace MyeScripting
{
    // ネイティブの ManagedHost が hostfxr の load_assembly_and_get_function_pointer で呼ぶ
    // エントリポイント。arg = MyeBootstrapArgs* { MyeEngineApi* api; ManagedVTable* outVtable }。
    //  - api を Engine に保存 (C# → エンジンの Transform/Log/Input/Random 呼出)
    //  - outVtable に native から呼ぶ関数ポインタ群を書き込む (エンジン → C# の駆動)
    public static class Bootstrap
    {
        // ネイティブ (ManagedHost.cpp) が読む戻り値。0 以外は全て「C# レーン利用不可」へ縮退する
        public const int kResultOk = 0;
        public const int kResultBadArgs = 1;
        public const int kResultNoVtable = 2;
        public const int kResultApiVersionMismatch = 3; // M93a: 表の版がこの DLL の前提と違う

        // component_entry_point_fn の既定シグネチャ: int (IntPtr arg, int argLength)
        public static unsafe int Initialize(IntPtr arg, int argLength)
        {
            if (arg == IntPtr.Zero) return kResultBadArgs;
            var a = (BootstrapArgs*)arg;
            if (a->Api == null) return kResultBadArgs;
            // M93a: 版の照合。ネイティブが知らない欄 (旧エンジン) へは書かない
            if (argLength >= sizeof(BootstrapArgs)) a->ManagedApiVersion = MyeEngineApi.kExpectedApiVersion;
            // ★Engine.Init より前に弾く — ずれた表で Log スロットを呼ぶこと自体が別関数呼び出しになりうる
            if (a->Api->Version != MyeEngineApi.kExpectedApiVersion) return kResultApiVersionMismatch;
            Engine.Init(a->Api);

            ManagedVTable* vt = a->OutVtable;
            if (vt == null) return kResultNoVtable;
            vt->Compile = &ScriptRuntime.NativeCompile;
            vt->GetTypeCount = &ScriptRuntime.NativeGetTypeCount;
            vt->GetTypeName = &ScriptRuntime.NativeGetTypeName;
            vt->GetFieldCount = &ScriptRuntime.NativeGetFieldCount;
            vt->GetFieldInfo = &ScriptRuntime.NativeGetFieldInfo;
            vt->CreateInstance = &ScriptRuntime.NativeCreateInstance;
            vt->DestroyInstance = &ScriptRuntime.NativeDestroyInstance;
            vt->Invoke = &ScriptRuntime.NativeInvoke;
            vt->InvokeTrigger = &ScriptRuntime.NativeInvokeTrigger;
            vt->GetFieldValue = &ScriptRuntime.NativeGetFieldValue;
            vt->SetFieldValue = &ScriptRuntime.NativeSetFieldValue;
            vt->Serialize = &ScriptRuntime.NativeSerialize;
            vt->Deserialize = &ScriptRuntime.NativeDeserialize;
            vt->ResetInstances = &ScriptRuntime.NativeResetInstances;
            vt->InvokeCollision = &ScriptRuntime.NativeInvokeCollision;
            vt->InvokeBreak = &ScriptRuntime.NativeInvokeBreak;
            vt->BtTask = &ScriptRuntime.NativeBtTask;
            vt->BtTaskCatalog = &ScriptRuntime.NativeBtTaskCatalog;

            Engine.Log("[csharp] managed runtime ready (.NET " + Environment.Version + ", ABI v"
                       + MyeEngineApi.kExpectedApiVersion + ")");
            return kResultOk;
        }
    }
}
