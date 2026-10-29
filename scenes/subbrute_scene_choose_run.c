#include "../subbrute_i.h"
#include "subbrute_scene.h"

#define TAG "SubBruteSceneChooseRun"

enum SubBruteRunModeIndex {
    SubBruteRunModeIndexFile,
    SubBruteRunModeIndexFolder,
    SubBruteRunModeIndexCount,
};

static const char* const subbrute_run_mode_short_names[SubBruteRunModeIndexCount] = {
    "One file",
    "Folder (all)",
};

static void subbrute_scene_choose_run_enter_callback(void* context, uint32_t index) {
    furi_assert(context);
    SubBruteState* instance = (SubBruteState*)context;

    if(index == 0) {
        VariableItem* item = variable_item_list_get(instance->var_list, 0);
        if(item != NULL) {
            uint8_t mode = variable_item_get_current_value_index(item);
            if(mode == SubBruteRunModeIndexFile) {
                scene_manager_next_scene(instance->scene_manager, SubBruteSceneSetupAttack);
            } else {
                scene_manager_next_scene(instance->scene_manager, SubBruteSceneBatchRun);
            }
        }
    }
}

static void subbrute_scene_choose_run_item_callback(VariableItem* item) {
    furi_assert(item);
    SubBruteState* instance = variable_item_get_context(item);
    furi_assert(instance);

    uint8_t mode = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, subbrute_run_mode_short_names[mode]);
}

void subbrute_scene_choose_run_on_enter(void* context) {
    furi_assert(context);
    SubBruteState* instance = (SubBruteState*)context;

    VariableItemList* var_list = instance->var_list;
    variable_item_list_reset(var_list);

    VariableItem* item = variable_item_list_add(
        var_list, "Run mode", SubBruteRunModeIndexCount, subbrute_scene_choose_run_item_callback, instance);
    variable_item_set_current_value_index(item, instance->batch_mode ? 1 : 0);
    variable_item_set_current_value_text(
        item, subbrute_run_mode_short_names[instance->batch_mode ? 1 : 0]);

    if(!furi_string_empty(instance->batch_folder)) {
        VariableItem* folder_item =
            variable_item_list_add(var_list, "Folder", 1, NULL, instance);
        variable_item_set_current_value_index(folder_item, 0);
        variable_item_set_current_value_text(folder_item, furi_string_get_cstr(instance->batch_folder));
    }

    variable_item_list_set_selected_item(var_list, 0);
    variable_item_list_set_enter_callback(
        var_list, subbrute_scene_choose_run_enter_callback, instance);

    view_dispatcher_switch_to_view(instance->view_dispatcher, SubBruteViewVarList);
}

void subbrute_scene_choose_run_on_exit(void* context) {
    furi_assert(context);
    SubBruteState* instance = (SubBruteState*)context;
    variable_item_list_reset(instance->var_list);
}

bool subbrute_scene_choose_run_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}