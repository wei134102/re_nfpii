#include <forward_list>
#include <wups.h>
#include <wups/config/WUPSConfigItemMultipleValues.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemButtonCombo.h>
#include <wups/config/WUPSConfigItemStub.h>
#include <wups/button_combo/api.h>
#include <string>
#include <map>

#include <whb/libmanager.h>
#include <whb/log_cafe.h>
#include <whb/log_module.h>
#include <whb/log_udp.h>

#include <nfpii.h>
#include <notifications/notifications.h>
#include <sys/stat.h>
#include <sys/syslimits.h>

#include "quick_select.h"
#include "debug/logger.h"
#include "config/ConfigItemSelectAmiibo.hpp"
#include "config/ConfigItemLog.hpp"
#include "config/ConfigItemDumpAmiibo.hpp"

#define STR_VALUE(arg) #arg
#define VERSION_STRING(x, y, z) "v" STR_VALUE(x) "." STR_VALUE(y) "." STR_VALUE(z)

WUPS_PLUGIN_NAME("re_nfpii");
WUPS_PLUGIN_DESCRIPTION("nn_nfp 再实现：支持 Amiibo 文件模拟（界面汉化版）");
WUPS_PLUGIN_VERSION(VERSION_STRING(VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH));
WUPS_PLUGIN_AUTHOR("GaryOderNichts");
WUPS_PLUGIN_LICENSE("GPLv2");

WUPS_USE_STORAGE("re_nfpii");
WUPS_USE_WUT_DEVOPTAB();

// TODO make this dynamic again
// #define MAX_REMOVE_AFTER_SECONDS 20

#define TAG_EMULATION_PATH std::string("/vol/external01/wiiu/re_nfpii/")

uint32_t currentRemoveAfterOption = 0;

WUPSButtonCombo_Buttons currentQuickSelectCombination = QUICK_SELECT_BUTTON_COMBO_DEFAULT;
WUPSButtonCombo_Buttons currentToggleEmulationCombination = TOGGLE_EMULATION_BUTTON_COMBO_DEFAULT;

bool favoritesPerTitle = false;

static void nfpiiLogHandler(NfpiiLogVerbosity verb, const char* message)
{
    ConfigItemLog_PrintType((LogType) verb, message);
}

WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle rootHandle);

void ConfigMenuClosedCallback();

INITIALIZE_PLUGIN()
{
    if (!WHBLogModuleInit()) {
        WHBLogCafeInit();
        WHBLogUdpInit();
    }

    if (NotificationModule_InitLibrary() != NOTIFICATION_MODULE_RESULT_SUCCESS) {
        DEBUG_FUNCTION_LINE("Failed to init notifications");
    }

    WUPSConfigAPIOptionsV1 configOptions = {.name = "re_nfpii（Amiibo 模拟）"};
    if (WUPSConfigAPI_Init(configOptions, ConfigMenuOpenedCallback, ConfigMenuClosedCallback) != WUPSCONFIG_API_RESULT_SUCCESS) {
        DEBUG_FUNCTION_LINE("Failed to init config api");
    }

    ConfigItemLog_Init();
    NfpiiSetLogHandler(nfpiiLogHandler);

    migrateStorage();
    // Read values from config
    {
        auto emulationState = static_cast<int32_t>(NfpiiGetEmulationState());
        WUPSStorageError err;
        if ((err = WUPSStorageAPI::Get("emulationState", emulationState)) == WUPS_STORAGE_ERROR_NOT_FOUND) {
            WUPSStorageAPI::Store("emulationState", emulationState);
        } else if (err == WUPS_STORAGE_ERROR_SUCCESS) {
            NfpiiSetEmulationState((NfpiiEmulationState)emulationState);
        }

        if ((err = WUPSStorageAPI::Get("removeAfter", currentRemoveAfterOption)) ==
            WUPS_STORAGE_ERROR_NOT_FOUND) {
            WUPSStorageAPI::Store("removeAfter", currentRemoveAfterOption);
        } else if (err == WUPS_STORAGE_ERROR_SUCCESS) {
            NfpiiSetRemoveAfterSeconds(currentRemoveAfterOption / 2.0f);
        }

        std::string path = NfpiiGetTagEmulationPath();
        if ((err = WUPSStorageAPI::Get<std::string>("currentPath", path, WUPSStorageAPI::RESIZE_EXISTING_BUFFER)) == WUPS_STORAGE_ERROR_NOT_FOUND) {
            WUPSStorageAPI::Store("currentPath", path);
        } else if (err == WUPS_STORAGE_ERROR_SUCCESS) {
            // check that the stored path actually exists
            struct stat sb{};
            if (stat(path.c_str(), &sb) == 0 && (sb.st_mode & S_IFMT) == S_IFREG) {
                NfpiiSetTagEmulationPath(path.c_str());
            }
        }

        if ((err = WUPSStorageAPI::Get("favoritesPerTitle", favoritesPerTitle)) == WUPS_STORAGE_ERROR_NOT_FOUND) {
            WUPSStorageAPI::Store("favoritesPerTitle", favoritesPerTitle);
        }
        ConfigItemSelectAmiibo_Init(TAG_EMULATION_PATH, favoritesPerTitle);

        // Todo check default value?
        WUPSStorageAPI::GetOrStoreDefault(BUTTON_COMBO_QUICK_SELECT_CONFIG_ID, currentQuickSelectCombination, QUICK_SELECT_BUTTON_COMBO_DEFAULT);
        WUPSStorageAPI::GetOrStoreDefault(BUTTON_COMBO_TOGGLE_EMULATION_CONFIG_ID, currentToggleEmulationCombination, TOGGLE_EMULATION_BUTTON_COMBO_DEFAULT);

        if (currentQuickSelectCombination == 0) {
            currentQuickSelectCombination = QUICK_SELECT_BUTTON_COMBO_DEFAULT;
            WUPSStorageAPI::Store(BUTTON_COMBO_QUICK_SELECT_CONFIG_ID, currentQuickSelectCombination);
        }

        // Make sure the button combo is not empty.
        if (currentToggleEmulationCombination == 0) {
            currentToggleEmulationCombination = TOGGLE_EMULATION_BUTTON_COMBO_DEFAULT;
            WUPSStorageAPI::Store(BUTTON_COMBO_TOGGLE_EMULATION_CONFIG_ID, currentToggleEmulationCombination);
        }

        if (WUPSStorageAPI::SaveStorage() != WUPS_STORAGE_ERROR_SUCCESS) {
            DEBUG_FUNCTION_LINE("Failed to save storage");
        }
    }

    // Make sure to always show notifications
    NotificationModule_SetDefaultValue(NOTIFICATION_MODULE_NOTIFICATION_TYPE_INFO, NOTIFICATION_MODULE_DEFAULT_OPTION_KEEP_UNTIL_SHOWN, true);
    NotificationModule_SetDefaultValue(NOTIFICATION_MODULE_NOTIFICATION_TYPE_ERROR, NOTIFICATION_MODULE_DEFAULT_OPTION_KEEP_UNTIL_SHOWN, true);
    RegisterButtonCombos();
}

DEINITIALIZE_PLUGIN()
{
    NfpiiSetLogHandler(nullptr);
}

ON_APPLICATION_START()
{
    if (!WHBLogModuleInit()) {
        WHBLogCafeInit();
        WHBLogUdpInit();
    }

    // Make sure favorites are refreshed for the new title
    ConfigItemSelectAmiibo_Init(TAG_EMULATION_PATH, favoritesPerTitle);

    NfpiiSetPluginLoaded();
}

static void stateChangedCallback(ConfigItemMultipleValues* values, uint32_t index)
{
    WUPSStorageAPI::Store("emulationState", index);
    NfpiiSetEmulationState((NfpiiEmulationState) index);
}

static void removeAfterChangedCallback(ConfigItemMultipleValues* values, uint32_t index)
{
    currentRemoveAfterOption = index;
    WUPSStorageAPI::Store("removeAfter", (int32_t) currentRemoveAfterOption);
    NfpiiSetRemoveAfterSeconds(index / 2.0f);
}

static void uuidRandomizationChangedCallback(ConfigItemMultipleValues* values, uint32_t index)
{
    NfpiiSetUUIDRandomizationState((NfpiiUUIDRandomizationState) index);
}

static void amiiboSelectedCallback(ConfigItemSelectAmiibo* amiibos, const char* filePath)
{
    std::string filePathStr = filePath;
    WUPSStorageAPI::Store("currentPath", filePathStr);
    NfpiiSetTagEmulationPath(filePath);
}

static void favoritesPerTitleCallback(ConfigItemBoolean* item, bool enable)
{
    favoritesPerTitle = enable;
    WUPSStorageAPI::Store("favoritesPerTitle", favoritesPerTitle);

    // refresh favorites
    ConfigItemSelectAmiibo_Init(TAG_EMULATION_PATH, favoritesPerTitle);
}

static void quickSelectComboCallback(ConfigItemButtonCombo* item, uint32_t newValue)
{
    currentQuickSelectCombination = static_cast<WUPSButtonCombo_Buttons>(newValue);
    WUPSStorageAPI::Store(BUTTON_COMBO_QUICK_SELECT_CONFIG_ID, currentQuickSelectCombination);
}

static void toggleEmulationComboCallback(ConfigItemButtonCombo* item, uint32_t newValue)
{
    currentToggleEmulationCombination = static_cast<WUPSButtonCombo_Buttons>(newValue);
    WUPSStorageAPI::Store(BUTTON_COMBO_TOGGLE_EMULATION_CONFIG_ID, currentToggleEmulationCombination);
}


WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle rootHandle)
{
    WUPSConfigCategory root = WUPSConfigCategory(rootHandle);
    try {
        auto settingsCat = WUPSConfigCategory::Create("设置");

        constexpr WUPSConfigItemMultipleValues::ValuePair possibleValues[] = {
            {NFPII_EMULATION_OFF, "模拟已关闭"},
            {NFPII_EMULATION_ON, "模拟已开启"},
        };

        // TODO: Double check if `NFPII_EMULATION_OFF`is the correct default value.
        settingsCat.add(WUPSConfigItemMultipleValues::CreateFromValue("state", "模拟状态",
                                                                      NFPII_EMULATION_OFF, NfpiiGetEmulationState(),
                                                                      possibleValues,
                                                                      stateChangedCallback));


        constexpr WUPSConfigItemMultipleValues::ValuePair removeAfterValues[] = {
            {0, "从不"},
            {1, "0.5 秒"},
            {2, "1.0 秒"},
            {3, "1.5 秒"},
            {4, "2.0 秒"},
            {5, "2.5 秒"},
            {6, "3.0 秒"},
            {7, "3.5 秒"},
            {8, "4.0 秒"},
            {9, "4.5 秒"},
            {10, "5.0 秒"},
            {11, "5.5 秒"},
            {12, "6.0 秒"},
            {13, "6.5 秒"},
            {14, "7.0 秒"},
            {15, "7.5 秒"},
            {16, "8.0 秒"},
            {17, "8.5 秒"},
            {18, "9.0 秒"},
            {19, "9.5 秒"},
            {20, "10.0 秒"}
        };

        settingsCat.add(WUPSConfigItemMultipleValues::CreateFromValue("remove_after", "读卡后移除（计时）",
                                                                      0, NfpiiGetEmulationState(),
                                                                      removeAfterValues,
                                                                      removeAfterChangedCallback));

#if 0 //TODO
        values[0].value = RANDOMIZATION_OFF;
        values[0].valueName = (char*)"Off";
        values[1].value = RANDOMIZATION_ONCE;
        values[1].valueName = (char*)"Once";
        values[2].value = RANDOMIZATION_EVERY_READ;
        values[2].valueName = (char*)"After reading";
        WUPSConfigItemMultipleValues_AddToCategoryHandled(config, cat, "random_uuid", "Randomize UUID",
                                                          NfpiiGetUUIDRandomizationState(), values, 3,
                                                          uuidRandomizationChangedCallback);
#endif


        std::string currentAmiiboPath = NfpiiGetTagEmulationPath();
        settingsCat.add(ConfigItemSelectAmiiboCPP::Create("select_amiibo", "选择 Amiibo",
                                                          TAG_EMULATION_PATH.c_str(), currentAmiiboPath.c_str(),
                                                          amiiboSelectedCallback));

        settingsCat.add(WUPSConfigItemBoolean::Create("favorites_per_title", "按游戏分别收藏", false, favoritesPerTitle, favoritesPerTitleCallback));

        bool buttonCombosSupported = false;
        if (sQuickSelectButtonComboHandle != nullptr && sToggleEmulationButtonComboHandle != nullptr) {
            buttonCombosSupported = true;
            settingsCat.add(WUPSConfigItemButtonCombo::Create("quick_select_combination", "快速切换收藏（按键组合）",
                                                              currentQuickSelectCombination,
                                                              sQuickSelectButtonComboHandle,
                                                              quickSelectComboCallback));

            settingsCat.add(WUPSConfigItemButtonCombo::Create("quick_remove_combination", "开关模拟（按键组合）",
                                                              currentToggleEmulationCombination,
                                                              sToggleEmulationButtonComboHandle,
                                                              toggleEmulationComboCallback));

            settingsCat.add(WUPSConfigItemStub::Create(
                    "说明：打开本插件菜单为 L+十字键下+SELECT；上两项为游戏内快捷键，请分别进入并绑定按键。"));
        }

        settingsCat.add(ConfigItemDumpAmiiboCPP::Create("dump_amiibo", "导出 Amiibo（实物）",
                                                        (TAG_EMULATION_PATH + "dumps").c_str()));

        settingsCat.add(ConfigItemLogCPP::Create("log", "日志"));
        if (!buttonCombosSupported) {
            settingsCat.add(WUPSConfigItemStub::Create(
                    "无法注册按键组合：请更新 Aroma / 环境（需含 WUPS Button Combo 支持）。"));
            settingsCat.add(WUPSConfigItemStub::Create(
                    "成功后会出现两项：①在收藏（选卡界面按 X）的 Amiibo 间轮换 ②开关模拟；无出厂默认键。"));
            settingsCat.add(WUPSConfigItemStub::Create(
                    "打开本配置菜单：L + 十字键下 + SELECT（与上两项游戏内快捷键不是同一套）。"));
        }
        root.add(std::move(settingsCat));
    } catch (std::exception& e) {
        DEBUG_FUNCTION_LINE("Creating config menu failed: %s", e.what());
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

void ConfigMenuClosedCallback()
{
    WUPSStorageAPI::SaveStorage();
}
