using System;

namespace MyeScripting
{
    // ネイティブの ManagedHost が hostfxr の load_assembly_and_get_function_pointer で呼ぶ
    // エントリポイント。arg = MyeBootstrapArgs* { MyeEngineApi* api; ManagedVTable* outVtable }。
    //  - api を Engine に保存 (C# → エンジンの Transform/Log/Input/Random 呼出)
    //  - outVtable に native から呼ぶ関数ポインタ群を書き込む (エンジン → C# の駆動)
    public static class Bootstrap
    {
        // component_entry_point_fn の既定シグネチャ: int (IntPtr arg, int argLength)
        public static unsafe int Initialize(IntPtr arg, int argLength)
        {
            if (arg == IntPtr.Zero) return 1;
            var a = (BootstrapArgs*)arg;
            Engine.Init(a->Api);

            ManagedVTable* vt = a->OutVtable;
            if (vt == null) return 2;
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

            Engine.Log("[csharp] managed runtime ready (.NET " + Environment.Version + ")");
            return 0;
        }
    }
}
