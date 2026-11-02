#pragma once
#include <windows.h>
#include <sddl.h>
#include <taskschd.h>
#include <oleauto.h>
#include <string>
#include <vector>

namespace StartupTask {
template<class T> struct Com {
    T* value{};
    ~Com() { if (value) value->Release(); }
    T* operator->() const { return value; }
    T** out() { return &value; }
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
};
struct Text {
    BSTR value;
    explicit Text(const wchar_t* text) : value(SysAllocString(text)) {}
    ~Text() { SysFreeString(value); }
    operator BSTR() const { return value; }
};
struct Apartment {
    HRESULT result = CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
inline std::wstring Sid() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) return {};
    DWORD size{}; GetTokenInformation(token,TokenUser,nullptr,0,&size);
    std::vector<BYTE> buffer(size);
    bool ok = size && GetTokenInformation(token,TokenUser,buffer.data(),size,&size);
    CloseHandle(token);
    if (!ok) return {};
    LPWSTR text{};
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,&text)) return {};
    std::wstring sid(text); LocalFree(text); return sid;
}
inline std::wstring Path() {
    std::vector<wchar_t> buffer(32768);
    DWORD size = GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    return size && size < buffer.size() ? std::wstring(buffer.data(),size) : L"";
}
struct Connection {
    Apartment apartment;
    Com<ITaskService> service;
    Com<ITaskFolder> folder;
    std::wstring sid = Sid();
    std::wstring name = L"WindowOpacity-Logon-" + sid;
    HRESULT Open() {
        if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE) return apartment.result;
        if (sid.empty()) return E_ACCESSDENIED;
        HRESULT result = CoCreateInstance(CLSID_TaskScheduler,nullptr,CLSCTX_INPROC_SERVER,IID_ITaskService,reinterpret_cast<void**>(service.out()));
        if (FAILED(result)) return result;
        VARIANT empty{};
        result = service->Connect(empty,empty,empty,empty);
        if (FAILED(result)) return result;
        return folder.value ? S_OK : service->GetFolder(Text(L"\\"),folder.out());
    }
};
inline bool Enabled(bool allowLegacy = false) {
    Connection connection;
    if (FAILED(connection.Open())) return false;
    Com<IRegisteredTask> task;
    if (FAILED(connection.folder->GetTask(Text(connection.name.c_str()),task.out()))) return false;
    VARIANT_BOOL enabled = VARIANT_FALSE;
    if (FAILED(task->get_Enabled(&enabled)) || enabled != VARIANT_TRUE) return false;
    Com<ITaskDefinition> definition; Com<IActionCollection> actions; Com<IAction> action; Com<IExecAction> executable;
    if (FAILED(task->get_Definition(definition.out())) || FAILED(definition->get_Actions(actions.out()))) return false;
    LONG count{};
    if (FAILED(actions->get_Count(&count)) || count != 1 || FAILED(actions->get_Item(1,action.out())) ||
        FAILED(action->QueryInterface(IID_IExecAction,reinterpret_cast<void**>(executable.out())))) return false;
    BSTR path{}, args{};
    HRESULT first = executable->get_Path(&path), second = executable->get_Arguments(&args);
    bool match = SUCCEEDED(first) && SUCCEEDED(second) && path && args &&
        _wcsicmp(path,Path().c_str()) == 0 && (wcscmp(args,L"--startup-supervisor") == 0 || (allowLegacy && wcscmp(args,L"--background") == 0));
    SysFreeString(path); SysFreeString(args);
    Com<ITriggerCollection> triggers; Com<ITrigger> trigger;
    if (!match || FAILED(definition->get_Triggers(triggers.out())) || FAILED(triggers->get_Count(&count)) || count != 1 ||
        FAILED(triggers->get_Item(1,trigger.out()))) return false;
    TASK_TRIGGER_TYPE2 type{};
    return SUCCEEDED(trigger->get_Type(&type)) && type == TASK_TRIGGER_LOGON &&
        SUCCEEDED(trigger->get_Enabled(&enabled)) && enabled == VARIANT_TRUE;
}
inline HRESULT Configure(bool enabled) {
    Connection connection;
    HRESULT result = connection.Open();
    if (FAILED(result)) return result;
    Text name(connection.name.c_str());
    if (!enabled) {
        result = connection.folder->DeleteTask(name,0);
        return result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ? S_OK : result;
    }
    std::wstring path = Path();
    if (path.empty()) return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    Com<ITaskDefinition> definition;
    if (FAILED(result = connection.service->NewTask(0,definition.out()))) return result;
    Com<IRegistrationInfo> registration;
    Com<IPrincipal> principal;
    Com<ITaskSettings> settings;
    Com<ITriggerCollection> triggers;
    Com<ITrigger> trigger;
    Com<ILogonTrigger> logon;
    Com<IActionCollection> actions;
    Com<IAction> action;
    Com<IExecAction> executable;
    // Check every operation so a partially configured definition is never registered.
    if (FAILED(result = definition->get_RegistrationInfo(registration.out())) ||
        FAILED(result = registration->put_Author(Text(L"WindowOpacity"))) ||
        FAILED(result = registration->put_Description(Text(L"WindowOpacity: delayed interactive logon startup; retry abnormal exits; managed by the app's startup setting."))) ||
        FAILED(result = definition->get_Principal(principal.out())) ||
        FAILED(result = principal->put_UserId(Text(connection.sid.c_str()))) ||
        FAILED(result = principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN)) ||
        FAILED(result = principal->put_RunLevel(TASK_RUNLEVEL_LUA)) ||
        FAILED(result = definition->get_Settings(settings.out())) ||
        FAILED(result = settings->put_Enabled(VARIANT_TRUE)) ||
        FAILED(result = settings->put_StartWhenAvailable(VARIANT_TRUE)) ||
        FAILED(result = settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE)) ||
        FAILED(result = settings->put_StopIfGoingOnBatteries(VARIANT_FALSE)) ||
        FAILED(result = settings->put_ExecutionTimeLimit(Text(L"PT0S"))) ||
        FAILED(result = settings->put_RestartInterval(Text(L"PT1M"))) ||
        FAILED(result = settings->put_RestartCount(3)) ||
        FAILED(result = settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW)) ||
        FAILED(result = settings->put_AllowDemandStart(VARIANT_TRUE)) ||
        FAILED(result = definition->get_Triggers(triggers.out())) ||
        FAILED(result = triggers->Create(TASK_TRIGGER_LOGON,trigger.out())) ||
        FAILED(result = trigger->QueryInterface(IID_ILogonTrigger,reinterpret_cast<void**>(logon.out()))) ||
        FAILED(result = logon->put_UserId(Text(connection.sid.c_str()))) ||
        FAILED(result = logon->put_Delay(Text(L"PT10S"))) ||
        FAILED(result = logon->put_Enabled(VARIANT_TRUE)) ||
        FAILED(result = definition->get_Actions(actions.out())) ||
        FAILED(result = actions->Create(TASK_ACTION_EXEC,action.out())) ||
        FAILED(result = action->QueryInterface(IID_IExecAction,reinterpret_cast<void**>(executable.out()))) ||
        FAILED(result = executable->put_Path(Text(path.c_str()))) ||
        FAILED(result = executable->put_Arguments(Text(L"--startup-supervisor"))) ||
        FAILED(result = executable->put_WorkingDirectory(Text(path.substr(0,path.find_last_of(L"\\/")).c_str())))) return result;
    Text sid(connection.sid.c_str());
    VARIANT user{}, empty{}; user.vt = VT_BSTR; user.bstrVal = sid.value;
    Com<IRegisteredTask> registered;
    return connection.folder->RegisterTaskDefinition(name,definition.value,TASK_CREATE_OR_UPDATE,user,empty,
        TASK_LOGON_INTERACTIVE_TOKEN,empty,registered.out());
}
// Run only for the scheduled logon action. An ordinary tray exit returns zero
// and ends supervision; an abnormal exit gets at most three delayed retries.
inline int Supervise() {
    HANDLE mutex = CreateMutexW(nullptr,FALSE,L"Local\\WindowOpacity.StartupSupervisor.8CF3470F");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return 0; }
    DWORD code = 1;
    for (int attempt = 0; attempt <= 3; ++attempt) {
        if (!Enabled()) { code = 0; break; }
        std::wstring path = Path(), command = L"\"" + path + L"\" --background";
        std::vector<wchar_t> writable(command.begin(),command.end()); writable.push_back(0);
        std::wstring directory = path.substr(0,path.find_last_of(L"\\/"));
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (CreateProcessW(path.c_str(),writable.data(),nullptr,nullptr,FALSE,0,nullptr,directory.c_str(),&startup,&process)) {
            CloseHandle(process.hThread);
            DWORD wait = WaitForSingleObject(process.hProcess,INFINITE);
            if (wait != WAIT_OBJECT_0 || !GetExitCodeProcess(process.hProcess,&code)) code = 1;
            CloseHandle(process.hProcess);
            if (code == 0) break;
        } else code = GetLastError();
        if (attempt < 3) Sleep(60000);
    }
    CloseHandle(mutex);
    return static_cast<int>(code);
}
} // namespace StartupTask
