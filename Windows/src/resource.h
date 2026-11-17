#pragma once

#define IDI_APPICON                 101

// Menu & Accelerator Command IDs
#define IDM_APP_ABOUT               1001
#define IDM_APP_PASSWORD_SETTINGS   1002
#define IDM_APP_LOCK_VAULT          1003
#define IDM_APP_QUIT                1004

#define IDM_REC_TOGGLE_ARM            1101
#define IDM_REC_TOGGLE_AUDIO          1102
#define IDM_REC_CHOOSE_SAVE_FOLDER    1103
#define IDM_REC_EXPORT_MP4            1104
#define IDM_REC_TOGGLE_WAKE_SOUND     1105
#define IDM_REC_TOGGLE_MAX_BRIGHTNESS 1106

#define IDM_PLAY_ALL                1201
#define IDM_PLAY_PAUSE              1202
#define IDM_PLAY_SPEED_1X           1203
#define IDM_PLAY_SPEED_4X           1204
#define IDM_PLAY_SPEED_12X          1205
#define IDM_PLAY_MUTE_TOGGLE        1206
#define IDM_PLAY_PREV_CLIP          1207
#define IDM_PLAY_NEXT_CLIP          1208
#define IDM_PLAY_SAVE_AND_NEXT      1209
#define IDM_PLAY_DELETE_AND_NEXT    1210

#define IDM_WIN_SHOW_MAIN           1301
#define IDM_WIN_SWITCH_MODE         1302

// Dynamic popup command ranges
#define IDM_CAM_DEVICE_BASE         2000
#define IDM_MIC_DEVICE_BASE         2200

// System tray & async capture callback messages
#define WM_TRAYICON_CALLBACK        (0x8000 + 77)
#define WM_APP_MOTION_TRIGGERED     (0x8000 + 78)
#define WM_APP_PLAYER_CLIP_ENDED    (0x8000 + 79)
#define WM_APP_SOUND_TRIGGERED      (0x8000 + 80)
