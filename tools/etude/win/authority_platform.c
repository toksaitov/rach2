/* Native endpoint gates and protected operator files on Windows. */
#ifdef _WIN32
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "files.h"
#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include "../authority_platform.h"

const char *authority_installed_binding(void)
{
#ifdef RACH2_HOSTTEST
    const char *test = getenv("ETUDE_TEST_AUTHORITY");
    if (test)
        return test;
#endif
    return "C:/ProgramData/rach2/etude-authority";
}

int authority_file_id(const char *path, struct authority_file_id *id)
{
    struct etude_win_stat st;
    if (etude_win_stat(path, &st))
        return -1;
    id->device = st.st_dev;
    id->inode = st.st_ino;
    return 0;
}

static PSID operator_sid(const char *text)
{
    PSID sid = NULL;
    wchar_t *wide = etude_win_wide(text);
    if (wide) {
        ConvertStringSidToSidW(wide, &sid);
        free(wide);
    }
    return sid;
}

int authority_operator(const char *text)
{
    HANDLE token;
    DWORD size = 0;
    PSID sid = operator_sid(text);
    if (!sid || !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        LocalFree(sid);
        return 0;
    }
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    TOKEN_USER *user = malloc(size);
    int ok = user && GetTokenInformation(token, TokenUser, user, size, &size) &&
             EqualSid(sid, user->User.Sid);
    free(user);
    CloseHandle(token);
    LocalFree(sid);
    return ok;
}

static int trusted_sid(PSID sid, PSID operator_id)
{
    return EqualSid(sid, operator_id) || IsWellKnownSid(sid, WinLocalSystemSid) ||
           IsWellKnownSid(sid, WinBuiltinAdministratorsSid);
}

static int safe_acl(PACL acl, PSID operator_id, int private_file)
{
    if (!acl)
        return 0;
    DWORD forbidden = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
                      FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER |
                      GENERIC_WRITE | GENERIC_ALL;
    if (private_file)
        forbidden |= FILE_READ_DATA | GENERIC_READ;
    for (DWORD i = 0; i < acl->AceCount; i++) {
        ACE_HEADER *header;
        if (!GetAce(acl, i, (void **)&header))
            return 0;
        if (header->AceFlags & INHERIT_ONLY_ACE)
            continue;
        if (header->AceType == ACCESS_DENIED_ACE_TYPE)
            continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
            return 0;
        ACCESS_ALLOWED_ACE *ace = (ACCESS_ALLOWED_ACE *)header;
        if ((ace->Mask & forbidden) && !trusted_sid((PSID)&ace->SidStart, operator_id))
            return 0;
    }
    return 1;
}

int authority_protected(const char *path, const char *text, int private_file)
{
    wchar_t *wide = etude_win_long_path(path);
    PSID sid = operator_sid(text), owner = NULL;
    PACL acl = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    int ok = 0;
    if (!wide || !sid)
        goto done;
    DWORD attrs = GetFileAttributesW(wide);
    if (attrs == INVALID_FILE_ATTRIBUTES || attrs & FILE_ATTRIBUTE_REPARSE_POINT ||
        (private_file < 0 ? !(attrs & FILE_ATTRIBUTE_DIRECTORY) :
         (attrs & FILE_ATTRIBUTE_DIRECTORY)))
        goto done;
    if (GetNamedSecurityInfoW(wide, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION |
                            DACL_SECURITY_INFORMATION, &owner, NULL, &acl, NULL, &descriptor))
        goto done;
    ok = trusted_sid(owner, sid) && safe_acl(acl, sid, private_file > 0);
    if (private_file > 0)
        ok = ok && EqualSid(owner, sid);
 done:
    LocalFree(descriptor);
    LocalFree(sid);
    free(wide);
    if (!ok)
        errno = EACCES;
    return ok ? 0 : -1;
}

int authority_gate_open(const char *path, int exclusive, struct authority_gate *gate)
{
    wchar_t *wide = etude_win_long_path(path);
    gate->value = -1;
    if (!wide)
        return -1;
    HANDLE file = CreateFileW(wide, GENERIC_READ | (exclusive ? GENERIC_WRITE : 0),
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                               FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    free(wide);
    if (file == INVALID_HANDLE_VALUE)
        return etude_win_file_error();
    OVERLAPPED at = {0};
    if (!LockFileEx(file, LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0),
                    0, 1, 0, &at)) {
        DWORD error = GetLastError();
        CloseHandle(file);
        SetLastError(error);
        return etude_win_file_error();
    }
    gate->value = (intptr_t)file;
    return 0;
}

void authority_gate_close(struct authority_gate *gate)
{
    if (gate->value != -1)
        CloseHandle((HANDLE)gate->value);
    gate->value = -1;
}

static PSECURITY_DESCRIPTOR own_descriptor(int private_file)
{
    HANDLE token;
    DWORD size = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return NULL;
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    TOKEN_USER *user = malloc(size);
    LPWSTR sid = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    if (user && GetTokenInformation(token, TokenUser, user, size, &size) &&
        ConvertSidToStringSidW(user->User.Sid, &sid)) {
        wchar_t sddl[1024];
        int n = swprintf(sddl, 1024, L"D:P(A;;FA;;;%ls)(A;;FA;;;SY)(A;;FA;;;BA)%ls", sid,
                         private_file ? L"" : L"(A;;GR;;;BU)");
        if (n > 0 && n < 1024)
            ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1,
                                                                   &descriptor, NULL);
    }
    LocalFree(sid);
    free(user);
    CloseHandle(token);
    return descriptor;
}

static PSECURITY_DESCRIPTOR previous_descriptor(const wchar_t *path, int private_file)
{
    DWORD attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? own_descriptor(private_file) : NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    if (GetNamedSecurityInfoW((wchar_t *)path, SE_FILE_OBJECT,
                              OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                              NULL, NULL, NULL, NULL, &descriptor))
        return NULL;
    return descriptor;
}

int authority_publish(const char *path, const void *data, size_t size, int private_file)
{
    char temporary[4096];
    int n = snprintf(temporary, sizeof temporary, "%s.route-%lu", path,
                     (unsigned long)GetCurrentProcessId());
    if (n < 0 || (size_t)n >= sizeof temporary) {
        errno = ENAMETOOLONG;
        return -1;
    }
    wchar_t *wide = etude_win_long_path(temporary), *target = etude_win_long_path(path);
    PSECURITY_DESCRIPTOR descriptor = target ? previous_descriptor(target, private_file) : NULL;
    if (!wide || !target || !descriptor) {
        free(wide);
        free(target);
        LocalFree(descriptor);
        errno = ENOMEM;
        return -1;
    }
    SECURITY_ATTRIBUTES security = {sizeof security, descriptor, FALSE};
    HANDLE file = CreateFileW(wide, GENERIC_WRITE, 0, &security, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD error = file == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    size_t offset = 0;
    while (!error && offset < size) {
        DWORD wrote = 0, chunk = size - offset > 1048576 ? 1048576 : (DWORD)(size - offset);
        if (!WriteFile(file, (const char *)data + offset, chunk, &wrote, NULL) || !wrote)
            error = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
        offset += wrote;
    }
    if (!error && !FlushFileBuffers(file))
        error = GetLastError();
    if (file != INVALID_HANDLE_VALUE && !CloseHandle(file) && !error)
        error = GetLastError();
    if (!error && !MoveFileExW(wide, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        error = GetLastError();
    if (error)
        DeleteFileW(wide);
    free(wide);
    free(target);
    LocalFree(descriptor);
    if (!error)
        return 0;
    SetLastError(error);
    return etude_win_file_error();
}
int authority_nonce(char out[65])
{
    unsigned char bytes[32];
    if (BCryptGenRandom(NULL, bytes, sizeof bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) {
        errno = EIO;
        return -1;
    }
    for (size_t i = 0; i < sizeof bytes; i++)
        snprintf(out + i * 2, 3, "%02x", bytes[i]);
    return 0;
}

#endif
