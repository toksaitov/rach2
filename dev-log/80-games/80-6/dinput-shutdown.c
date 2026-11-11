/* Reproduce a DirectInput release from a process-detach callback. */
#define COBJMACROS
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <stdio.h>

static IDirectInput8A *input;
static IDirectInputDevice8A *keyboard;

/**
 * Acquire a keyboard; return 0, or the failed setup step from 1 to 5.
 */
__declspec(dllexport) int start(void)
{
    HRESULT hr;
    hr = DirectInput8Create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION,
                           &IID_IDirectInput8A, (void **)&input, NULL);
    printf("dinput-probe: create: %#lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 1;
    hr = IDirectInput8_CreateDevice(input, &GUID_SysKeyboard, &keyboard, NULL);
    printf("dinput-probe: keyboard: %#lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 2;
    hr = IDirectInputDevice8_SetDataFormat(keyboard, &c_dfDIKeyboard);
    printf("dinput-probe: format: %#lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 3;
    hr = IDirectInputDevice8_SetCooperativeLevel(keyboard, GetDesktopWindow(),
                                               DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    printf("dinput-probe: cooperative: %#lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 4;
    hr = IDirectInputDevice8_Acquire(keyboard);
    printf("dinput-probe: acquire: %#lx\n", (unsigned long)hr);
    fflush(stdout);
    return FAILED(hr) ? 5 : 0;
}

/**
 * Release the interfaces during detach, both for unload and process exit.
 */
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    (void)instance;
    if (reason == DLL_PROCESS_DETACH) {
        printf("dinput-probe: detach: reserved=%d\n", reserved != NULL);
        fflush(stdout);
        if (keyboard) IDirectInputDevice8_Release(keyboard);
        if (input) IDirectInput8_Release(input);
        puts("dinput-probe: detached");
        fflush(stdout);
    }
    return TRUE;
}
