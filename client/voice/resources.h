#pragma once

// Resource IDs embedded into l2voice.dll via voice/resources.rc.in.
// FindResource(... MAKEINTRESOURCE(IDR_*) ..., "RCDATA") yields the
// bytes of the corresponding PNG.

#define IDR_MIC_PNG          1001  // toolbar mic (pre-tinted white, runtime tint)
#define IDR_MIC_SPEAKING_PNG 1002  // green solid — currently emitting
#define IDR_MIC_MUTED_PNG    1003  // green outline — silent (not speaking)
#define IDR_MIC_BLOCKED_PNG  1004  // red with X — locally muted by listener

// L2UI_CH3 button textures (extracted from the L2 Essence client at
// UGX_System/utx_extracted/L2UI_CH3/). Cropped to the visible bbox.
#define IDR_L2UI_BTN_BIG           1010
#define IDR_L2UI_BTN_BIG_OVER      1011
#define IDR_L2UI_BTN_BIG_DOWN      1012
#define IDR_L2UI_BTN_SMALL         1013
#define IDR_L2UI_BTN_SMALL_OVER    1014
#define IDR_L2UI_BTN_SMALL_DOWN    1015
// Title-bar frame controls (minimize / close).
#define IDR_L2UI_FRAME_MINI        1016
#define IDR_L2UI_FRAME_MINI_OVER   1017
#define IDR_L2UI_FRAME_MINI_DOWN   1018
#define IDR_L2UI_FRAME_CLOSE       1019
#define IDR_L2UI_FRAME_CLOSE_OVER  1020
#define IDR_L2UI_FRAME_CLOSE_DOWN  1021

// Tab textures from L2UI_NewTex.HennaWnd_TabBtn — clean three-state
// sprites designed for full-width tab buttons.
#define IDR_L2UI_TAB_SELECTED         1030
#define IDR_L2UI_TAB_UNSELECTED       1031
#define IDR_L2UI_TAB_UNSELECTED_OVER  1032

// L2 native window backdrop — `NpcWnd.npc1_back` is the classic
// charcoal panel with the thin gold border used by NPC dialogs.
#define IDR_L2UI_WND_BG               1040
