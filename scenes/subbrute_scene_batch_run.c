#include "../subbrute_i.h"
#include "subbrute_scene.h"

#include <toolbox/name_generator.h>
#include <gui/modules/validators.h>

#define TAG "SubBruteSceneBatchRun"

static void subbrute_scene_batch_run_callback(SubBruteCustomEvent event, void* context) {
    furi_assert(context);

    SubBruteState* instance = (SubBruteState*)context;
    view_dispatcher_send_custom_event(instance->view_dispatcher, event);
}

static void
    subbrute_scene_batch_run_device_state_changed(void* context, SubBruteWorkerState state) {
    furi_assert(context);

    SubBruteState* instance = (SubBruteState*)context;

    if(state == SubBruteWorkerStateIDLE) {
        // Can't be IDLE on this step!
        view_dispatcher_send_custom_event(instance->view_dispatcher, SubBruteCustomEventTypeError);
    } else if(state == SubBruteWorkerStateFinished) {
        view_dispatcher_send_custom_event(
            instance->view_dispatcher, SubBruteCustomEventTypeTransmitFinished);
    }
}

static void subbrute_scene_batch_run_finish(SubBruteState* instance) {
    FURI_LOG_E(TAG, "Batch finished");
    notification_message(instance->notifications, &sequence_double_vibro);
    scene_manager_search_and_switch_to_previous_scene(
        instance->scene_manager, SubBruteSceneStart);
}

static void subbrute_scene_batch_run_update_view(SubBruteState* instance, bool attacking) {
    SubBruteAttackView* view = instance->view_attack;
    char label[32] = {0};
    subbrute_batch_current_label(instance, label, sizeof(label));

    subbrute_attack_view_init_values(
        view,
        instance->device->attack,
        instance->device->max_value,
        instance->device->current_step,
        attacking,
        subbrute_worker_get_repeats(instance->worker));
    subbrute_attack_view_set_batch_info(
        view, true, instance->batch_index, instance->batch_count, label);
}

static void subbrute_scene_batch_run_advance(SubBruteState* instance) {
    while(instance->batch_index < instance->batch_count) {
        instance->batch_paused = false;
        instance->device->current_step = 0;
        instance->batch_last_step = 0;
        instance->batch_stall_ticks = 0;
        SubBruteFileResult result = subbrute_batch_load_current(instance);
        if(result != SubBruteFileResultOk) {
            FURI_LOG_E(
                TAG, "Skipping file %d (error %d)", (int)instance->batch_index, result);
            instance->batch_index++;
            continue;
        }

        subbrute_scene_batch_run_update_view(instance, true);

        if(subbrute_worker_start(instance->worker)) {
            notification_message(instance->notifications, &sequence_single_vibro);
            return;
        }

        FURI_LOG_E(TAG, "Failed to start file %d", (int)instance->batch_index);
        instance->batch_index++;
    }

    subbrute_scene_batch_run_finish(instance);
}

static void subbrute_scene_batch_run_pause(SubBruteState* instance) {
    subbrute_worker_stop(instance->worker);
    instance->device->current_step = subbrute_worker_get_step(instance->worker);
    instance->batch_paused = true;
    notification_message(instance->notifications, &sequence_single_vibro);
    subbrute_scene_batch_run_update_view(instance, false);
}

static void subbrute_scene_batch_run_resume(SubBruteState* instance) {
    instance->batch_paused = false;
    if(!subbrute_worker_set_step(instance->worker, instance->device->current_step)) {
        FURI_LOG_W(TAG, "Cannot set step before resume");
    }

    if(subbrute_worker_start(instance->worker)) {
        notification_message(instance->notifications, &sequence_single_vibro);
        notification_message(instance->notifications, &sequence_blink_start_yellow);
        subbrute_scene_batch_run_update_view(instance, true);
    } else {
        notification_message(instance->notifications, &sequence_error);
    }
}

static void subbrute_scene_batch_run_open_text_input(SubBruteState* instance) {
    subbrute_worker_stop(instance->worker);

    if(instance->device->file_protocol_info != NULL) {
        name_generator_make_auto(
            instance->text_store,
            sizeof(instance->text_store),
            subbrute_protocol_file(instance->device->file_protocol_info->file));
    } else {
        instance->text_store[0] = '\0';
    }

    text_input_set_header_text(instance->text_input, "Name of file");
    text_input_set_result_callback(
        instance->text_input,
        subbrute_text_input_callback,
        instance,
        instance->text_store,
        SUBBRUTE_MAX_LEN_NAME,
        true);

    furi_string_reset(instance->file_path);
    furi_string_set_str(instance->file_path, SUBBRUTE_PATH);

    ValidatorIsFile* validator_is_file = validator_is_file_alloc_init(
        furi_string_get_cstr(instance->file_path), SUBBRUTE_FILE_EXT, "");
    text_input_set_validator(instance->text_input, validator_is_file_callback, validator_is_file);

    instance->current_view = SubBruteViewTextInput;
    view_dispatcher_switch_to_view(instance->view_dispatcher, instance->current_view);
}

static void subbrute_scene_batch_run_close_text_input(SubBruteState* instance) {
    void* validator_context = text_input_get_validator_callback_context(instance->text_input);
    text_input_set_validator(instance->text_input, NULL, NULL);
    validator_is_file_free(validator_context);

    text_input_reset(instance->text_input);
    furi_string_reset(instance->file_path);

    instance->current_view = SubBruteViewAttack;
    view_dispatcher_switch_to_view(instance->view_dispatcher, instance->current_view);
    subbrute_scene_batch_run_update_view(instance, false);
}

void subbrute_scene_batch_run_on_enter(void* context) {
    furi_assert(context);
    SubBruteState* instance = (SubBruteState*)context;

    instance->batch_mode = true;
    instance->batch_pending_next = false;
    instance->batch_paused = false;
    instance->batch_last_step = 0;
    instance->batch_stall_ticks = 0;
    instance->batch_finish_ticks = 0;

    subbrute_batch_collect(instance);
    if(!subbrute_batch_has_files(instance)) {
        FURI_LOG_E(TAG, "No .sub files found in %s", furi_string_get_cstr(instance->batch_folder));
        dialog_message_show_storage_error(instance->dialogs, "No .sub files in this folder!");
        subbrute_scene_batch_run_finish(instance);
        return;
    }
    FURI_LOG_E(
        TAG,
        "Batch start, files: %d, folder: %s",
        (int)instance->batch_count,
        furi_string_get_cstr(instance->batch_folder));

    instance->current_view = SubBruteViewAttack;
    subbrute_attack_view_set_callback(
        instance->view_attack, subbrute_scene_batch_run_callback, instance);
    view_dispatcher_switch_to_view(instance->view_dispatcher, instance->current_view);

    subbrute_worker_set_callback(
        instance->worker, subbrute_scene_batch_run_device_state_changed, instance);

    subbrute_scene_batch_run_advance(instance);
}

void subbrute_scene_batch_run_on_exit(void* context) {
    furi_assert(context);
    SubBruteState* instance = (SubBruteState*)context;

    notification_message(instance->notifications, &sequence_blink_stop);
    subbrute_worker_stop(instance->worker);
    instance->batch_pending_next = false;
    subbrute_batch_clear(instance);
    instance->batch_mode = false;
}

bool subbrute_scene_batch_run_on_event(void* context, SceneManagerEvent event) {
    SubBruteState* instance = (SubBruteState*)context;
    SubBruteAttackView* view = instance->view_attack;

    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        uint64_t step = subbrute_worker_get_step(instance->worker);
        instance->device->current_step = step;
        subbrute_attack_view_set_current_step(view, step);
        FURI_LOG_E(TAG, "Batch custom event: %lu", (unsigned long)event.event);

        if(event.event == SubBruteCustomEventTypeTransmitFinished) {
            // Ignore finishing events that do not belong to the file that is
            // currently running (they can survive a forced advance).
            if(instance->device->current_step < instance->device->max_value) {
                FURI_LOG_E(TAG, "Ignoring stale TransmitFinished (step %llu)", (unsigned long long)
                      instance->device->current_step);
            } else {
                instance->batch_paused = false;
                notification_message(instance->notifications, &sequence_display_backlight_on);

                subbrute_worker_stop(instance->worker);
                instance->batch_index++;
                FURI_LOG_E(
                    TAG,
                    "TransmitFinished done, next index: %d/%d",
                    (int)instance->batch_index,
                    (int)instance->batch_count);
                if(instance->batch_index >= instance->batch_count) {
                    subbrute_scene_batch_run_finish(instance);
                } else {
                    // Defer the actual file switch to the next tick: by then the worker
                    // thread is fully stopped and can be safely restarted.
                    instance->batch_pending_next = true;
                }
            }
        } else if(event.event == SubBruteCustomEventTypeTransmitNotStarted) {
            subbrute_scene_batch_run_pause(instance);
        } else if(event.event == SubBruteCustomEventTypeTransmitStarted) {
            subbrute_scene_batch_run_resume(instance);
        } else if(event.event == SubBruteCustomEventTypeBackPressed) {
            if(subbrute_worker_is_running(instance->worker)) {
                subbrute_scene_batch_run_pause(instance);
            } else {
                scene_manager_search_and_switch_to_previous_scene(
                    instance->scene_manager, SubBruteSceneStart);
            }
        } else if(event.event == SubBruteCustomEventTypeSaveFile) {
            subbrute_scene_batch_run_open_text_input(instance);
        } else if(event.event == SubBruteCustomEventTypeTextEditDone) {
            if(strcmp(instance->text_store, "") != 0) {
                furi_string_reset(instance->file_path);
                furi_string_cat_printf(
                    instance->file_path,
                    "%s/%s%s",
                    SUBBRUTE_PATH,
                    instance->text_store,
                    SUBBRUTE_FILE_EXT);

                if(subbrute_device_save_file(
                       instance->device, furi_string_get_cstr(instance->file_path))) {
                    notification_message(instance->notifications, &sequence_single_vibro);
                } else {
                    dialog_message_show_storage_error(instance->dialogs, "Error during saving!");
                }
            }
            subbrute_scene_batch_run_close_text_input(instance);
        } else if(event.event == SubBruteCustomEventTypeTransmitCustom) {
            // We can transmit only in not working states
            if(subbrute_worker_can_manual_transmit(instance->worker)) {
                notification_message(instance->notifications, &sequence_blink_green_100);
                subbrute_worker_transmit_current_key(
                    instance->worker, instance->device->current_step);
                notification_message(instance->notifications, &sequence_blink_stop);
            }
        } else if(event.event == SubBruteCustomEventTypeChangeStepUp) {
            uint64_t new_step = subbrute_device_add_step(instance->device, 1);
            subbrute_worker_set_step(instance->worker, new_step);
            subbrute_attack_view_set_current_step(view, new_step);
        } else if(event.event == SubBruteCustomEventTypeChangeStepUpMore) {
            uint64_t new_step = subbrute_device_add_step(instance->device, 50);
            subbrute_worker_set_step(instance->worker, new_step);
            subbrute_attack_view_set_current_step(view, new_step);
        } else if(event.event == SubBruteCustomEventTypeChangeStepDown) {
            uint64_t new_step = subbrute_device_add_step(instance->device, -1);
            subbrute_worker_set_step(instance->worker, new_step);
            subbrute_attack_view_set_current_step(view, new_step);
        } else if(event.event == SubBruteCustomEventTypeChangeStepDownMore) {
            uint64_t new_step = subbrute_device_add_step(instance->device, -50);
            subbrute_worker_set_step(instance->worker, new_step);
            subbrute_attack_view_set_current_step(view, new_step);
        } else if(event.event == SubBruteCustomEventTypeError) {
            notification_message(instance->notifications, &sequence_error);

            instance->batch_index++;
            if(instance->batch_index >= instance->batch_count) {
                subbrute_scene_batch_run_finish(instance);
            } else {
                subbrute_scene_batch_run_advance(instance);
            }
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeTick) {
        uint64_t step = subbrute_worker_get_step(instance->worker);
        instance->device->current_step = step;
        subbrute_attack_view_set_current_step(view, step);
        consumed = true;

        if(instance->batch_pending_next) {
            instance->batch_pending_next = false;
            subbrute_worker_stop(instance->worker);
            subbrute_scene_batch_run_advance(instance);
        } else if(subbrute_worker_is_running(instance->worker)) {
            // No-progress watchdog: if the step has not advanced for ~3.5 s at
            // ANY value (not only at max_value), treat the file as stuck and
            // force the advance to the next one. This covers radio hangs below
            // the file's real code space (e.g. step 4095 of a 12-bit CAME).
            if(instance->device->current_step == instance->batch_last_step) {
                instance->batch_stall_ticks++;
                if(instance->batch_stall_ticks > 35) { // ~3.5 s at 100 ms tick
                    FURI_LOG_E(
                        TAG,
                        "No progress at step %llu, forcing advance",
                        (unsigned long long)instance->device->current_step);
                    instance->batch_last_step = 0;
                    instance->batch_stall_ticks = 0;
                    subbrute_worker_stop(instance->worker);
                    instance->batch_index++;
                    if(instance->batch_index >= instance->batch_count) {
                        subbrute_scene_batch_run_finish(instance);
                    } else {
                        subbrute_scene_batch_run_advance(instance);
                    }
                }
            } else {
                instance->batch_last_step = instance->device->current_step;
                instance->batch_stall_ticks = 0;
            }
            instance->batch_finish_ticks = 0;
        } else if(!instance->batch_paused) {
            // Worker is not running and the user did not pause it: the file just
            // finished. If the TransmitFinished event was not delivered (or was
            // dropped), self-heal after a short grace period and move on to the
            // next file so the batch never stalls on its last value.
            instance->batch_last_step = instance->device->current_step;
            instance->batch_stall_ticks = 0;
            if(instance->device->current_step >= instance->device->max_value) {
                instance->batch_finish_ticks++;
                if(instance->batch_finish_ticks > 20) { // ~2 s at 100 ms tick
                    FURI_LOG_E(
                        TAG,
                        "File end not processed, self-healing advance (step %llu)",
                        (unsigned long long)instance->device->current_step);
                    instance->batch_finish_ticks = 0;
                    instance->batch_pending_next = true;
                }
            } else {
                instance->batch_finish_ticks = 0;
            }
        } else {
            // Paused by the user: keep counters reset, nothing to self-heal.
            instance->batch_last_step = instance->device->current_step;
            instance->batch_stall_ticks = 0;
            instance->batch_finish_ticks = 0;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        if(instance->current_view == SubBruteViewTextInput) {
            subbrute_scene_batch_run_close_text_input(instance);
            consumed = true;
        }
    }

    return consumed;
}