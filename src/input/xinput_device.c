/**
 * Xbox Input Compatibility Layer (Unified SDL3 Gamepad Backend)
 * =============================================================================
 * Translates the Xbox XPP controller API to SDL3's SDL_Gamepad API
 * across Windows, Linux, and macOS.
 */

#include "xinput_xbox.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#ifndef ERROR_SUCCESS
#define ERROR_SUCCESS 0u
#endif

#ifndef ERROR_DEVICE_NOT_CONNECTED
#define ERROR_DEVICE_NOT_CONNECTED 1167u
#endif

static SDL_Gamepad *g_pads[XBOX_MAX_CONTROLLERS] = { NULL };
static BOOL         g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD        g_packet[XBOX_MAX_CONTROLLERS] = { 0 };

/* Scan and open up to XBOX_MAX_CONTROLLERS connected gamepads.
 * Safe to call periodically to support hotplugging. */
static void refresh_gamepads(void)
{
    int count = 0;
    SDL_JoystickID *ids;

    /* First, clean up any disconnected handles */
    for (int slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++) {
        if (g_pads[slot] && !SDL_GamepadConnected(g_pads[slot])) {
            SDL_CloseGamepad(g_pads[slot]);
            g_pads[slot] = NULL;
            g_controller_connected[slot] = FALSE;
        }
    }

    ids = SDL_GetGamepads(&count);
    if (!ids)
        return;

    for (int i = 0; i < count; i++) {
        SDL_JoystickID id = ids[i];

        /* Skip if this gamepad is already open in one of our 4 slots */
        if (SDL_GetGamepadFromID(id) != NULL)
            continue;

        /* Find the first empty slot */
        for (int slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++) {
            if (!g_pads[slot]) {
                g_pads[slot] = SDL_OpenGamepad(id);
                if (g_pads[slot]) {
                    SDL_SetGamepadPlayerIndex(g_pads[slot], slot);
                    g_controller_connected[slot] = TRUE;
                }
                break;
            }
        }
    }

    SDL_free(ids);
}

void xbox_InputInit(void)
{
    if (!SDL_WasInit(SDL_INIT_GAMEPAD))
        SDL_InitSubSystem(SDL_INIT_GAMEPAD);

    refresh_gamepads();

    if (!g_controller_connected[0] && getenv("RECOMP_SIMULATE_INPUT"))
        g_controller_connected[0] = TRUE;
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    SDL_Gamepad *c;
    WORD btn = 0;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_UpdateGamepads();

    c = g_pads[dwPort];
    if (!c || !SDL_GamepadConnected(c)) {
        refresh_gamepads();
        c = g_pads[dwPort];
    }

    if (!c || !SDL_GamepadConnected(c)) {
        g_controller_connected[dwPort] = FALSE;

        /* Headless synthetic "Press Start" nudge when RECOMP_SIMULATE_INPUT is set */
        if (dwPort == 0 && getenv("RECOMP_SIMULATE_INPUT")) {
            static DWORD sim_packet = 0;
            static DWORD sim_frame = 0;
            BOOL press = ((sim_frame++ % 120) < 6);
            memset(pState, 0, sizeof(XBOX_INPUT_STATE));
            pState->dwPacketNumber = ++sim_packet;
            if (press) {
                pState->Gamepad.wButtons = XBOX_GAMEPAD_START;
                pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
            }
            g_controller_connected[0] = TRUE;
            g_packet[0] = sim_packet;
            return ERROR_SUCCESS;
        }

        return ERROR_DEVICE_NOT_CONNECTED;
    }

    g_controller_connected[dwPort] = TRUE;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++g_packet[dwPort];

    /* Digital buttons */
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_DPAD_UP))     btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_DPAD_DOWN))   btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_DPAD_LEFT))   btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))  btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_START))       btn |= XBOX_GAMEPAD_START;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_BACK))        btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_LEFT_STICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = btn;

    /* Face & shoulder analog buttons (0 or 255 on digital gamepads) */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_SOUTH) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_EAST) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_WEST) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_NORTH) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) ? 255 : 0;

    /* Triggers: SDL3 range 0..32767 -> Xbox range 0..255 */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);

    /* Thumbsticks: invert Y axis to match Xbox convention (-1 - v avoids -32768 overflow) */
    pState->Gamepad.sThumbLX = SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFTX);
    pState->Gamepad.sThumbLY = (SHORT)(-1 - SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_LEFTY));
    pState->Gamepad.sThumbRX = SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHTX);
    pState->Gamepad.sThumbRY = (SHORT)(-1 - SDL_GetGamepadAxis(c, SDL_GAMEPAD_AXIS_RIGHTY));

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    SDL_Gamepad *c;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    c = g_pads[dwPort];
    if (!c || !SDL_GamepadConnected(c))
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_RumbleGamepad(c, pVibration->wLeftMotorSpeed, pVibration->wRightMotorSpeed, 1000);
    return ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS)
        return FALSE;

    if (!g_pads[dwPort] || !SDL_GamepadConnected(g_pads[dwPort]))
        refresh_gamepads();

    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    if (!xbox_InputIsConnected(dwPort))
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1; /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1; /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    return ERROR_SUCCESS;
}