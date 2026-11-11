/* dinputprobe: list the devices DirectInput 8 enumerates, as a game does. */
#define COBJMACROS
#define DIRECTINPUT_VERSION 0x0800
#include "probe.h"
#include <dinput.h>

/** Print one enumerated device: its type, its subtype and its names. */
static BOOL CALLBACK device(LPCDIDEVICEINSTANCEA inst, LPVOID count)
{
    (*(int *)count)++;
    printf("dinput: device type 0x%02lx subtype 0x%02lx %s / %s\n",
           (unsigned long)GET_DIDEVICE_TYPE(inst->dwDevType),
           (unsigned long)GET_DIDEVICE_SUBTYPE(inst->dwDevType), inst->tszInstanceName,
           inst->tszProductName);
    return DIENUM_CONTINUE;
}

/**
 * Enumerate every attached device class with DirectInput8Create and
 * EnumDevices, printing each, and say whether a keyboard and a mouse were
 * among them. Returns 0 when both were, 1 when a call fails, 2 otherwise.
 */
int main(void)
{
    IDirectInput8A *di;
    IDirectInputDevice8A *dev;
    int count = 0, keyboard, mouse;
    HRESULT hr;

    probe_output();
    hr = DirectInput8Create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8A,
                            (void **)&di, NULL);
    if (FAILED(hr)) {
        printf("dinput: DirectInput8Create: hr=0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    hr = IDirectInput8_EnumDevices(di, DI8DEVCLASS_ALL, device, &count, DIEDFL_ATTACHEDONLY);
    if (FAILED(hr)) {
        printf("dinput: EnumDevices: hr=0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    keyboard = SUCCEEDED(IDirectInput8_CreateDevice(di, &GUID_SysKeyboard, &dev, NULL));
    if (keyboard)
        IDirectInputDevice8_Release(dev);
    mouse = SUCCEEDED(IDirectInput8_CreateDevice(di, &GUID_SysMouse, &dev, NULL));
    if (mouse)
        IDirectInputDevice8_Release(dev);
    IDirectInput8_Release(di);
    printf("dinput: %d devices, keyboard %s, mouse %s\n", count, keyboard ? "yes" : "no",
           mouse ? "yes" : "no");
    if (!keyboard || !mouse)
        return 2;
    puts("DINPUT OK");
    return 0;
}
