#include "../subbrute_i.h"
#include "subbrute_scene.h"

#define TAG "SubBruteSceneRunAttack"

static void subbrute_scene_run_attack_callback(SubBruteCustomEvent event, void* context) {
    furi_assert(context);

    SubBruteState* instance = (SubBruteState*)context;
    view_dispatcher_send_custom_event(instance->view_dispatcher, event);
}

static void
    subbrute_scene_run_attack_device_state_changed(void* context, SubBruteWorkerState state) {
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
void subbrute_scene_run_attack_on_exit(void* context) {
    furi_assert(context);
    SubBruteState* instance = (SubBruteState*)context;
    // FURI_LOG_W(TAG, "subbrute_scene_run_attack_on_exit");
    notification_message(instance->notifications, &sequence_blink_stop);
    subbrute_worker_stop(instance->worker);
}

// A single-preset run finished (event received or self-healed). For preset
// attacks, keep going automatically with the next attack of the same family by
// handing over to the sequence scene (just like the long-press entry). For
// file-based runs the classic flow is kept: back to the setup screen.
static void subbrute_scene_run_attack_on_finished(SubBruteState* instance) {
    instance->run_finished_ticks = 0;
    notification_message(instance->notifications, &sequence_display_backlight_on);
    notification_message(instance->notifications, &sequence_double_vibro);

    if(instance->device->attack == SubBruteAttackLoadFile) {
        scene_manager_next_scene(instance->scene_manager, SubBruteSceneSetupAttack);
        return;
    }

    SubBruteAttacks first = subbrute_attack_family_at(instance->device->attack, 0);
    SubBruteAttacks next =
        subbrute_attack_family_at(first, subbrute_attack_family_position(instance->device->attack) + 1);
    if(next >= SubBruteAttackTotalCount) {
        // Last attack of the family: nothing left to try.
        scene_manager_next_scene(instance->scene_manager, SubBruteSceneSetupAttack);
        return;
    }

    instance->device->attack = next;
    FURI_LOG_E(TAG, "RunAttack finished, continuing with %s", subbrute_protocol_name(next));
    scene_manager_next_scene(instance->scene_manager, SubBruteSceneSequenceRun);
}

void subbrute_scene_run_attack_on_enter(void* context) {
    furi_assert(context);
    SubBruteState* instance = (SubBruteState*)context;
    SubBruteAttackView* view = instance->view_attack;

    instance->current_view = SubBruteViewAttack;
    instance->run_finished_ticks = 0;
    FURI_LOG_E(
        TAG,
        "RunAttack on_enter: dev_attack=%s(%d) worker_running=%d step=%llu",
        subbrute_protocol_name(instance->device->attack),
        (int)instance->device->attack,
        subbrute_worker_is_running(instance->worker),
        (unsigned long long)instance->device->current_step);
    subbrute_attack_view_set_callback(view, subbrute_scene_run_attack_callback, instance);
    view_dispatcher_switch_to_view(instance->view_dispatcher, instance->current_view);

    subbrute_worker_set_callback(
        instance->worker, subbrute_scene_run_attack_device_state_changed, instance);

    // The short-press flow must NEVER inherit stale state from an interrupted
    // sequence (e.g. the auto-advancing CAME variant). Re-derive the run target
    // from the last menu selection when it drifted, then (re)initialize the
    // worker so it transmits exactly the selected preset.
    if(!subbrute_worker_is_running(instance->worker)) {
        SubBruteAttacks target = (SubBruteAttacks)instance->settings->last_index;
        if(instance->device->attack != target && target != SubBruteAttackLoadFile &&
           target < SubBruteAttackTotalCount) {
            FURI_LOG_E(
                TAG,
                "RunAttack: stale device attack %d, re-deriving to %d",
                (int)instance->device->attack,
                (int)target);
            subbrute_device_attack_set(
                instance->device, target, subbrute_settings_get_current_repeats(instance->settings));
            instance->device->current_step = 0;
        }

        if(instance->device->attack == SubBruteAttackLoadFile) {
            // File-based attack: the worker is already configured by the
            // load-file flow; just start it from the requested step.
            subbrute_worker_set_step(instance->worker, instance->device->current_step);
        } else {
            subbrute_worker_init_default_attack(
                instance->worker,
                instance->device->attack,
                instance->device->current_step,
                instance->device->protocol_info,
                instance->device->extra_repeats);
        }

        if(!subbrute_worker_start(instance->worker)) {
            view_dispatcher_send_custom_event(
                instance->view_dispatcher, SubBruteCustomEventTypeError);
        } else {
            notification_message(instance->notifications, &sequence_single_vibro);
            notification_message(instance->notifications, &sequence_blink_start_yellow);
        }
    }
}

bool subbrute_scene_run_attack_on_event(void* context, SceneManagerEvent event) {
    SubBruteState* instance = (SubBruteState*)context;
    SubBruteAttackView* view = instance->view_attack;

    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        uint64_t step = subbrute_worker_get_step(instance->worker);
        instance->device->current_step = step;
        subbrute_attack_view_set_current_step(view, step);
        FURI_LOG_E(TAG, "RunAttack custom event: %lu", (unsigned long)event.event);

        if(event.event == SubBruteCustomEventTypeTransmitFinished) {
            subbrute_scene_run_attack_on_finished(instance);
        } else if(
            event.event == SubBruteCustomEventTypeTransmitNotStarted ||
            event.event == SubBruteCustomEventTypeBackPressed) {
            if(subbrute_worker_is_running(instance->worker)) {
                // Notify
                notification_message(instance->notifications, &sequence_single_vibro);
            }
            // Stop transmit
            scene_manager_search_and_switch_to_previous_scene(
                instance->scene_manager, SubBruteSceneSetupAttack);
        } else if(event.event == SubBruteCustomEventTypeError) {
            notification_message(instance->notifications, &sequence_error);

            // Stop transmit
            scene_manager_search_and_switch_to_previous_scene(
                instance->scene_manager, SubBruteSceneSetupAttack);
        } else if(event.event == SubBruteCustomEventTypeUpdateView) {
            //subbrute_attack_view_set_current_step(view, instance->device->current_step);
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeTick) {
        uint64_t step = subbrute_worker_get_step(instance->worker);
        instance->device->current_step = step;
        subbrute_attack_view_set_current_step(view, step);

        if(subbrute_worker_is_running(instance->worker)) {
            instance->run_finished_ticks = 0;
        } else if(instance->device->current_step >= instance->device->max_value) {
            // The worker thread is done but we have not processed (or received)
            // its TransmitFinished event yet. Self-heal after a short grace
            // period so the single-file run never leaves the user stuck on the
            // final value with the GUI alive.
            instance->run_finished_ticks++;
            if(instance->run_finished_ticks > 25) { // ~2.5 s at 100 ms tick
                FURI_LOG_E(TAG, "TransmitFinished lost, self-healing the run");
                subbrute_scene_run_attack_on_finished(instance);
            }
        } else {
            instance->run_finished_ticks = 0;
        }

        consumed = true;
    }

    return consumed;
}