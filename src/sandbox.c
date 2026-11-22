/* sandbox.c - see sandbox.h. */
#include "common.h"
#include "sandbox.h"
#include <userenv.h>
#include <aclapi.h>

#define CONTAINER_NAME L"livewall.decoder"

/* Enough for a 4K video's frame queue with a wide margin; a file built to
   make the decoder grab memory hits this and the helper ends. */
#define MEMORY_LIMIT ((SIZE_T)1536 << 20)

typedef HRESULT(WINAPI *DeriveFn)(PCWSTR, PSID *);
typedef HRESULT(WINAPI *CreateProfileFn)(PCWSTR, PCWSTR, PCWSTR, PSID_AND_ATTRIBUTES, DWORD, PSID *);

/* The container has to be registered with Windows before a process can run
   in it (without that, CreateProcess fails). Registering makes a small
   folder, %LOCALAPPDATA%\Packages\livewall.decoder, once; after that it
   already exists and its SID is derived from the name.
   userenv.dll is loaded only when a helper is first started, so the engine
   doesn't carry it while showing a shader. */
static PSID container_sid(void)
{
    HMODULE ue = LoadLibraryExW(L"userenv.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    CreateProfileFn make;
    DeriveFn derive;
    PSID sid = NULL;
    if (!ue)
        return NULL;
    make = (CreateProfileFn)(void *)GetProcAddress(ue, "CreateAppContainerProfile");
    derive = (DeriveFn)(void *)GetProcAddress(ue, "DeriveAppContainerSidFromAppContainerName");
    if (make && SUCCEEDED(make(CONTAINER_NAME, L"livewall video decoder",
                               L"Decodes videos for livewall, with no network and no file access.", NULL, 0, &sid)))
        return sid;
    if (derive && SUCCEEDED(derive(CONTAINER_NAME, &sid)))
        return sid;
    return NULL;
}

/* An AppContainer process can only start from an exe it may read. In
   Program Files that is already so; in a user folder, give the container
   read and run rights on livewall.exe itself (nothing else). */
static BOOL allow_exe(const WCHAR *exe, PSID sid)
{
    PACL old = NULL, acl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    EXPLICIT_ACCESSW ea;
    DWORD err = GetNamedSecurityInfoW(exe, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &old, NULL, &sd);
    if (err == ERROR_SUCCESS) {
        memset(&ea, 0, sizeof ea);
        ea.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
        ea.grfAccessMode = GRANT_ACCESS;
        ea.grfInheritance = NO_INHERITANCE;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea.Trustee.ptstrName = (LPWSTR)sid;
        err = SetEntriesInAclW(1, &ea, old, &acl);
    }
    if (err == ERROR_SUCCESS)
        err = SetNamedSecurityInfoW((LPWSTR)exe, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, acl, NULL);
    if (acl)
        LocalFree(acl);
    if (sd)
        LocalFree(sd);
    if (err != ERROR_SUCCESS)
        log_line(L"sandbox: could not let the helper read livewall.exe (%u)", err);
    return err == ERROR_SUCCESS;
}

static HANDLE make_job(void)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui;
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job)
        return NULL;
    memset(&lim, 0, sizeof lim);
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
                                           JOB_OBJECT_LIMIT_PROCESS_MEMORY |
                                           JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    lim.BasicLimitInformation.ActiveProcessLimit = 1;
    lim.ProcessMemoryLimit = MEMORY_LIMIT;
    ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
                             JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_GLOBALATOMS |
                             JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                             JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim) ||
        !SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof ui)) {
        CloseHandle(job);
        return NULL;
    }
    return job;
}

static BOOL start(const WCHAR *exe, WCHAR *cmd, PSID sid, HANDLE *inherit, UINT n, PROCESS_INFORMATION *pi)
{
    static BYTE attr_buf[512];
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attr_buf;
    SIZE_T size = sizeof attr_buf;
    SECURITY_CAPABILITIES caps;
    STARTUPINFOEXW si;
    DWORD64 mitigate = PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON |
                       PROCESS_CREATION_MITIGATION_POLICY_FONT_DISABLE_ALWAYS_ON |
                       PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON |
                       PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON;
    DWORD child = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
    BOOL ok;

    if (!InitializeProcThreadAttributeList(attrs, 4, 0, &size))
        return FALSE;
    memset(&caps, 0, sizeof caps);
    caps.AppContainerSid = sid; /* and no capabilities: no network, no libraries */
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.lpAttributeList = attrs;
    ok = UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &caps, sizeof caps, NULL, NULL) &&
         UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, n * sizeof(HANDLE), NULL, NULL) &&
         UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &mitigate, sizeof mitigate, NULL, NULL) &&
         UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY, &child, sizeof child, NULL, NULL) &&
         CreateProcessW(exe, cmd, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, NULL, NULL,
                        &si.StartupInfo, pi);
    DeleteProcThreadAttributeList(attrs);
    return ok;
}

BOOL sandbox_start(const WCHAR *args, HANDLE *inherit, UINT n, Sandboxed *out)
{
    static WCHAR exe[MAX_PATH], cmd[MAX_PATH + 128];
    static PSID sid;
    static BOOL tried_acl;
    PROCESS_INFORMATION pi;
    DWORD err;

    memset(out, 0, sizeof *out);
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wsprintfW(cmd, L"\"%s\" -%s", exe, args);
    if (!sid)
        sid = container_sid();
    if (!sid) {
        log_line(L"sandbox: no app container");
        return FALSE;
    }
    for (;;) {
        if (start(exe, cmd, sid, inherit, n, &pi))
            break;
        err = GetLastError();
        if (err == ERROR_ACCESS_DENIED && !tried_acl) {
            tried_acl = TRUE;
            if (allow_exe(exe, sid))
                continue;
        }
        log_line(L"sandbox: could not start the helper (%u)", err);
        return FALSE;
    }
    out->process = pi.hProcess;
    out->job = make_job();
    if (!out->job || !AssignProcessToJobObject(out->job, pi.hProcess)) {
        log_line(L"sandbox: could not limit the helper (%u)", GetLastError());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        sandbox_end(out);
        return FALSE;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return TRUE;
}

void sandbox_end(Sandboxed *s)
{
    if (s->job)
        CloseHandle(s->job); /* ends the process if it is still running */
    else if (s->process)
        TerminateProcess(s->process, 1);
    if (s->process)
        CloseHandle(s->process);
    s->job = s->process = NULL;
}
