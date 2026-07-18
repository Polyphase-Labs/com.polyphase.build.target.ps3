/**
 * @file Input_PS3.cpp
 * @brief PS3 controller input via PSL1GHT's io/pad. Maps port 0 (the primary
 *        DualShock3 / Sixaxis) into the engine's GamepadState slot 0.
 *
 * Face-button mapping uses Xbox-position convention to match engine intent
 * for GAMEPAD_A/B/X/Y:
 *   CROSS    -> GAMEPAD_A   (bottom of the face diamond, "confirm")
 *   CIRCLE   -> GAMEPAD_B   (right, "cancel")
 *   SQUARE   -> GAMEPAD_X   (left)
 *   TRIANGLE -> GAMEPAD_Y   (top)
 *
 * The DualShock3 has two full analog sticks and pressure-sensitive L2/R2, so
 * unlike the PSP port we populate both thumb axes and both trigger axes.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Input/Input.h"
#include "Input/InputUtils.h"
#include "Engine.h"
#include "Log.h"
#include "Maths.h"

#include <io/pad.h>
#include <string.h>

namespace
{
    constexpr u32 kMaxPads = 7;

    // DS3 analog nubs report 0..255 with ~128 centre. Sticks drift; a 0.30
    // normalised deadzone (~38 raw) absorbs it while still letting a
    // deliberate push cross the engine's 0.5 virtual-button threshold.
    constexpr float kAnalogDeadzone = 0.30f;

    float ApplyDeadzone(float v)
    {
        if (v >  kAnalogDeadzone) return (v - kAnalogDeadzone) / (1.0f - kAnalogDeadzone);
        if (v < -kAnalogDeadzone) return (v + kAnalogDeadzone) / (1.0f - kAnalogDeadzone);
        return 0.0f;
    }
}

void INP_Initialize()
{
    ioPadInit(kMaxPads);

    InputState& input = GetEngineState()->mInput;
    input.mGamepads[0].mType = GamepadType::Standard;
    input.mGamepads[0].mConnected = true;
    input.mNumControllers = 1;

    InputInit();
    LogDebug("Input_PS3: ioPad initialised");
}

void INP_Shutdown()
{
    InputShutdown();
    ioPadEnd();
}

void INP_Update()
{
    InputAdvanceFrame();

    InputState& input = GetEngineState()->mInput;
    GamepadState& gp = input.mGamepads[0];

    padInfo info;
    bool connected = false;
    if (ioPadGetInfo(&info) == 0)
    {
        connected = (info.status[0] != 0);
    }
    gp.mConnected = connected;

    padData pad;
    if (connected && ioPadGetData(0, &pad) == 0 && pad.len > 0)
    {
        // Face buttons (Xbox-position mapping — see file comment)
        gp.mButtons[GAMEPAD_A] = pad.BTN_CROSS    ? 1 : 0;
        gp.mButtons[GAMEPAD_B] = pad.BTN_CIRCLE   ? 1 : 0;
        gp.mButtons[GAMEPAD_X] = pad.BTN_SQUARE   ? 1 : 0;
        gp.mButtons[GAMEPAD_Y] = pad.BTN_TRIANGLE ? 1 : 0;

        // Shoulders
        gp.mButtons[GAMEPAD_L1] = pad.BTN_L1 ? 1 : 0;
        gp.mButtons[GAMEPAD_R1] = pad.BTN_R1 ? 1 : 0;
        gp.mButtons[GAMEPAD_L2] = pad.BTN_L2 ? 1 : 0;
        gp.mButtons[GAMEPAD_R2] = pad.BTN_R2 ? 1 : 0;

        // D-pad
        gp.mButtons[GAMEPAD_UP]    = pad.BTN_UP    ? 1 : 0;
        gp.mButtons[GAMEPAD_DOWN]  = pad.BTN_DOWN  ? 1 : 0;
        gp.mButtons[GAMEPAD_LEFT]  = pad.BTN_LEFT  ? 1 : 0;
        gp.mButtons[GAMEPAD_RIGHT] = pad.BTN_RIGHT ? 1 : 0;

        // System + stick clicks
        gp.mButtons[GAMEPAD_START]  = pad.BTN_START  ? 1 : 0;
        gp.mButtons[GAMEPAD_SELECT] = pad.BTN_SELECT ? 1 : 0;
        gp.mButtons[GAMEPAD_HOME]   = 0; // PS button is consumed by the XMB overlay
        gp.mButtons[GAMEPAD_THUMBL] = pad.BTN_L3 ? 1 : 0;
        gp.mButtons[GAMEPAD_THUMBR] = pad.BTN_R3 ? 1 : 0;

        // Right-stick virtual buttons are derived from the axes in
        // InputPostUpdate; clear them here so no stale values persist.
        gp.mButtons[GAMEPAD_R_LEFT]  = 0;
        gp.mButtons[GAMEPAD_R_RIGHT] = 0;
        gp.mButtons[GAMEPAD_R_UP]    = 0;
        gp.mButtons[GAMEPAD_R_DOWN]  = 0;

        // Analog sticks — 0..255, centre ~128. Vertical grows DOWNWARD; engine
        // convention is +Y = up, so invert Y.
        const float lx = (float(pad.ANA_L_H) - 128.0f) / 128.0f;
        const float ly = (float(pad.ANA_L_V) - 128.0f) / 128.0f;
        const float rx = (float(pad.ANA_R_H) - 128.0f) / 128.0f;
        const float ry = (float(pad.ANA_R_V) - 128.0f) / 128.0f;
        gp.mAxes[GAMEPAD_AXIS_LTHUMB_X] = glm::clamp(ApplyDeadzone( lx), -1.0f, 1.0f);
        gp.mAxes[GAMEPAD_AXIS_LTHUMB_Y] = glm::clamp(ApplyDeadzone(-ly), -1.0f, 1.0f);
        gp.mAxes[GAMEPAD_AXIS_RTHUMB_X] = glm::clamp(ApplyDeadzone( rx), -1.0f, 1.0f);
        gp.mAxes[GAMEPAD_AXIS_RTHUMB_Y] = glm::clamp(ApplyDeadzone(-ry), -1.0f, 1.0f);

        // Triggers — L2/R2 are pressure-sensitive on DS3; use the pressure
        // reading (0..255) normalised, falling back to the digital bit.
        const float l2 = pad.PRE_L2 ? (float(pad.PRE_L2) / 255.0f) : (pad.BTN_L2 ? 1.0f : 0.0f);
        const float r2 = pad.PRE_R2 ? (float(pad.PRE_R2) / 255.0f) : (pad.BTN_R2 ? 1.0f : 0.0f);
        gp.mAxes[GAMEPAD_AXIS_LTRIGGER] = glm::clamp(l2, 0.0f, 1.0f);
        gp.mAxes[GAMEPAD_AXIS_RTRIGGER] = glm::clamp(r2, 0.0f, 1.0f);
    }

    InputPostUpdate();
}

// PS3 has no cursor / mouse / soft keyboard concept exposed through the
// engine's interface, but the symbols are required for link.
void INP_SetCursorPos(int32_t /*x*/, int32_t /*y*/) {}
void INP_ShowCursor(bool /*show*/) {}
void INP_LockCursor(bool /*lock*/) {}
void INP_TrapCursor(bool /*trap*/) {}
void INP_TrapCursorToRect(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/) {}

const char* INP_ShowSoftKeyboard(bool /*show*/) { return nullptr; }
bool INP_IsSoftKeyboardShown() { return false; }

#endif // POLYPHASE_PLATFORM_ADDON
