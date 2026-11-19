/* autostart.c - registers the logon task with schtasks.exe and an XML
 * definition (the XML is the only way to set every option we need without
 * the much larger Task Scheduler COM API). */
#include "common.h"
#include "autostart.h"

#define TASK L"livewall"
#define XML_CAP 6000

static BOOL schtasks(const WCHAR *args)
{
    static WCHAR cmd[1400];
    WCHAR sys[MAX_PATH];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;

    GetSystemDirectoryW(sys, MAX_PATH);
    wsprintfW(cmd, L"\"%s\\schtasks.exe\" %s", sys, args);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return FALSE;
    WaitForSingleObject(pi.hProcess, 15000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

/* Append s to the XML, escaping the characters XML cares about. */
static void cat_esc(WCHAR *xml, const WCHAR *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': str_cat(xml, L"&amp;", XML_CAP); break;
        case '<': str_cat(xml, L"&lt;", XML_CAP); break;
        case '>': str_cat(xml, L"&gt;", XML_CAP); break;
        case '"': str_cat(xml, L"&quot;", XML_CAP); break;
        default: {
            WCHAR one[2] = {*s, 0};
            str_cat(xml, one, XML_CAP);
        }
        }
    }
}

BOOL autostart_query(void) { return schtasks(L"/Query /TN \"" TASK L"\""); }

BOOL autostart_apply(BOOL on)
{
    WCHAR exe[MAX_PATH], user[256], name[128], path[MAX_PATH], args[MAX_PATH + 64];
    WCHAR *xml;
    HANDLE f;
    DWORD w;
    BOOL ok;

    if (!on) {
        schtasks(L"/Delete /TN \"" TASK L"\" /F"); /* fails harmlessly if missing */
        return TRUE;
    }

    GetModuleFileNameW(NULL, exe, MAX_PATH);
    user[0] = name[0] = 0;
    GetEnvironmentVariableW(L"USERDOMAIN", user, 128);
    GetEnvironmentVariableW(L"USERNAME", name, 128);
    str_cat(user, L"\\", 256);
    str_cat(user, name, 256);

    xml = (WCHAR *)mem_alloc(XML_CAP * sizeof(WCHAR));
    if (!xml)
        return FALSE;
    str_cat(xml,
            L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
            L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
            L"  <RegistrationInfo><Description>Starts the livewall live wallpaper when you sign in."
            L"</Description></RegistrationInfo>\r\n"
            L"  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>", XML_CAP);
    cat_esc(xml, user);
    str_cat(xml,
            L"</UserId></LogonTrigger></Triggers>\r\n"
            L"  <Principals><Principal id=\"Author\"><UserId>", XML_CAP);
    cat_esc(xml, user);
    str_cat(xml,
            L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel>"
            L"</Principal></Principals>\r\n"
            L"  <Settings>\r\n"
            L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
            L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
            L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
            L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n"
            L"    <StartWhenAvailable>false</StartWhenAvailable>\r\n"
            L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\r\n"
            L"    <IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>\r\n"
            L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
            L"    <Enabled>true</Enabled>\r\n"
            L"    <Hidden>false</Hidden>\r\n"
            L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n"
            L"    <WakeToRun>false</WakeToRun>\r\n"
            L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
            L"    <Priority>4</Priority>\r\n"
            L"  </Settings>\r\n"
            L"  <Actions Context=\"Author\"><Exec><Command>", XML_CAP);
    cat_esc(xml, exe);
    str_cat(xml, L"</Command></Exec></Actions>\r\n</Task>\r\n", XML_CAP);

    app_dir(path);
    str_cat(path, L"\\task.xml", MAX_PATH);
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok = f != INVALID_HANDLE_VALUE;
    if (ok) {
        static const WCHAR bom = 0xFEFF;
        WriteFile(f, &bom, sizeof bom, &w, NULL);
        WriteFile(f, xml, (DWORD)(str_len(xml) * sizeof(WCHAR)), &w, NULL);
        CloseHandle(f);
        wsprintfW(args, L"/Create /TN \"" TASK L"\" /XML \"%s\" /F", path);
        ok = schtasks(args);
        DeleteFileW(path);
    }
    mem_free(xml);
    log_line(ok ? L"startup task registered for %s" : L"could not register startup task for %s", exe);
    return ok;
}
