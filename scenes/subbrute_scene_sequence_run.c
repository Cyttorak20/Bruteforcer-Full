#include "../subbrute_i.h"
#include "subbrute_scene.h"

#define TAG "SubBruteSceneSequenceRun"

static void subbrute_scene_sequence_run_callback(SubBruteCustomEvent event, void* context) {
    furi_assert(context);

    SubBruteState* instance = (SubBruteState*)context;
    view_dispatcher_send_custom_event(instance->view_dispatcher, event);
}

static void
    subbrute_scene_sequence_run_device_state_changed(void* context, SubBruteWorkerState state) {
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

static void subbrute_scene_sequence_run_finish(SubBruteState* instance);
static void subbrute_scene_sequence_run_advance(SubBruteState* instance);

static void subbrute_scene_sequence_run_finish(SubBruteState* instance) {
    FURI_LOG_E(TAG, "Sequence finished");
    notification_message(instance->notifications, &sequence_double_vibro);
    // Always land on a fresh Start scene. scene_manager_next_scene() is the
    // same exit used by the single-run flow (proven to work); the fresh Start
    // on_enter() resets device->attack to the saved last_index, so no mid-run
    // value (e.g. a CAME variant set while advancing) can survive here.
    scene_manager_next_scene(instance->scene_manager, SubBruteSceneStart);
}

static void subbrute_scene_sequence_run_update_view(SubBruteState* instance, bool attacking) {
    SubBruteAttackView* view = instance->view_attack;

    subbrute_attack_view_init_values(
        view,
        instance->device->attack,
        instance->device->max_value,
        instance->device->current_step,
        attacking,
        subbrute_worker_get_repeats(instance->worker));
    subbrute_attack_view_set_batch_info(
        view, true, instance->seq_pos, instance->seq_count, subbrute_protocol_name(instance->device->attack));
    subbrute_attack_view_set_seq_countdown(view, attacking ? 0 : (uint8_t)instance->seq_countdown);
}

// Load and start the preset at seq_pos. Called on enter and when the countdown
// reaches zero, or when the user presses OK during the countdown.
static void subbrute_scene_sequence_run_start_current(SubBruteState* instance) {
    SubBruteAttacks current = subbrute_attack_family_at(instance->seq_first, instance->seq_pos);
    if(current >= SubBruteAttackTotalCount) {
        subbrute_scene_sequence_run_finish(instance);
        return;
    }
    instance->seq_paused = false;

    uint8_t total_repeats = subbrute_settings_get_current_repeats(instance->settings);
    instance->device->two_bytes = false;
    instance->device->key_from_file = 0;
    instance->device->current_step = 0;

    if((subbrute_device_attack_set(instance->device, current, total_repeats) !=
        SubBruteFileResultOk) ||
       (!subbrute_worker_init_default_attack(
           instance->worker,
           current,
           0,
           instance->device->protocol_info,
           instance->device->extra_repeats))) {
        FURI_LOG_E(TAG, "Cannot init preset %d, skipping", (int)current);
        instance->device->current_step = 0;
        subbrute_scene_sequence_run_advance(instance);
        return;
    }

    FURI_LOG_E(TAG, "Sequence start current [%d/%d]: %s", (int)instance->seq_pos + 1, (int)instance->seq_count, subbrute_protocol_name(current));

    subbrute_scene_sequence_run_update_view(instance, true);

    if(subbrute_worker_start(instance->worker)) {
        notification_message(instance->notifications, &sequence_single_vibro);
        notification_message(instance->notifications, &sequence_blink_start_yellow);
        return;
    }

    FURI_LOG_E(TAG, "Failed to start preset %d", (int)current);
    subbrute_scene_sequence_run_advance(instance);
}

// Move to the next preset of the family after a short on-screen countdown.
// Also used for forced advances (watchdog, errors). Stops the worker first so a
// late TransmitFinished from the previous preset cannot collide with the new run.
static void subbrute_scene_sequence_run_advance(SubBruteState* instance) {
    subbrute_worker_stop(instance->worker);

    instance->seq_pos++;
    if(instance->seq_pos >= instance->seq_count) {
        subbrute_scene_sequence_run_finish(instance);
        return;
    }

    SubBruteAttacks next = subbrute_attack_family_at(instance->seq_first, instance->seq_pos);
    if(next >= SubBruteAttackTotalCount) {
        subbrute_scene_sequence_run_finish(instance);
        return;
    }

    instance->device->attack = next; // drive the view title
    instance->seq_pending_next = true;
    instance->seq_countdown = SUBBRUTE_SEQ_COUNTDOWN_TICKS;
    instance->seq_stall_ticks = 0;
    instance->seq_finish_ticks = 0;
    instance->seq_last_step = 0;

    FURI_LOG_E(TAG, "Sequence advance to [%d/%d]: %s", (int)instance->seq_pos + 1, (int)instance->seq_count, subbrute_protocol_name(next));

    notification_message(instance->notifications, &sequence_double_vibro);
    subbrute_scene_sequence_run_update_view(instance, false);
}

static void subbrute_scene_sequence_run_pause(SubBruteState* instance) {
    subbrute_worker_stop(instance->worker);
    instance->device->current_step = subbrute_worker_get_step(instance->worker);
    instance->seq_paused = true;
    instance->seq_pending_next = false;
    instance->seq_countdown = 0;
    instance->seq_stall_ticks = 0;
    instance->seq_finish_ticks = 0;
    notification_message(instance->notifications, &sequence_single_vibro);
    subbrute_scene_sequence_run_update_view(instance, false);
}

static void subbrute_scene_sequence_run_resume(SubBruteState* instance) {
    instance->seq_paused = false;
    if(!subbrute_worker_set_step(instance->worker, instance->device->current_step)) {
        FURI_LOG_W(TAG, "Cannot set step before resume");
    }

    if(subbrute_worker_start(instance->worker)) {
        notification_message(instance->notifications, &sequence_single_vibro);
        notification_message(instance->notifications, &sequence_blink_start_yellow);
        subbrute_scene_sequence_run_update_view(instance, true);
    } else {
        notification_message(instance->notifications, &sequence_error);
    }
}

void subbrute_scene_sequence_run_on_enter(void* context) {
    furi_assert(context);
    SubBruteState* instance = (SubBruteState*)context;
    SubBruteAttackView* view = instance->view_attack;

    instance->seq_paused = false;
    instance->seq_pending_next = false;
    instance->seq_countdown = 0;
    instance->seq_stall_ticks = 0;
    instance->seq_finish_ticks = 0;
    instance->seq_last_step = 0;

    SubBruteAttacks start = instance->device->attack;
    FURI_LOG_E(
        TAG,
        "SEQ on_enter: start=%d (%s)",
        (int)start,
        subbrute_protocol_name(start));
    uint8_t count = subbrute_attack_family_count(start);
    if(count == 0) {
        FURI_LOG_E(TAG, "No family for attack %d, aborting sequence", (int)start);
        subbrute_scene_sequence_run_finish(instance);
        return;
    }
    instance->seq_first = subbrute_attack_family_at(start, 0);
    instance->seq_count = count;
    instance->seq_pos = subbrute_attack_family_position(start);

    FURI_LOG_E(
        TAG,
        "Sequence start, family: %s (+%d variants)",
        subbrute_protocol_name(start),
        (int)(count - instance->seq_pos));

    instance->current_view = SubBruteViewAttack;
    subbrute_attack_view_set_callback(view, subbrute_scene_sequence_run_callback, instance);
    view_dispatcher_switch_to_view(instance->view_dispatcher, instance->current_view);

    subbrute_worker_set_callback(
        instance->worker, subbrute_scene_sequence_run_device_state_changed, instance);

    subbrute_scene_sequence_run_start_current(instance);
}

void subbrute_scene_sequence_run_on_exit(void* context) {
    furi_assert(context);
    SubBruteState* instance = (SubBruteState*)context;

    notification_message(instance->notifications, &sequence_blink_stop);
    subbrute_worker_stop(instance->worker);
    instance->seq_pending_next = false;
    instance->seq_countdown = 0;
    instance->seq_paused = false;
}

bool subbrute_scene_sequence_run_on_event(void* context, SceneManagerEvent event) {
    SubBruteState* instance = (SubBruteState*)context;
    SubBruteAttackView* view = instance->view_attack;

    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        uint64_t step = subbrute_worker_get_step(instance->worker);
        instance->device->current_step = step;
        subbrute_attack_view_set_current_step(view, step);
        FURI_LOG_E(TAG, "Sequence custom event: %lu", (unsigned long)event.event);

        if(event.event == SubBruteCustomEventTypeTransmitFinished) {
            // Ignore finishing events that do not belong to the preset that is
            // currently running (they can survive a forced advance or a restart).
            if(instance->seq_pending_next || subbrute_worker_is_running(instance->worker)) {
                FURI_LOG_E(TAG, "Ignoring stale TransmitFinished");
            } else if(instance->device->current_step < instance->device->max_value) {
                FURI_LOG_E(
                    TAG,
                    "Ignoring stale TransmitFinished (step %llu)",
                    (unsigned long long)instance->device->current_step);
            } else {
                notification_message(instance->notifications, &sequence_display_backlight_on);
                FURI_LOG_E(
                    TAG, "Preset %d finished, advancing", (int)instance->device->attack);
                subbrute_scene_sequence_run_advance(instance);
            }
        } else if(event.event == SubBruteCustomEventTypeTransmitNotStarted) {
            // OK/Back pressed while attacking: pause the current preset.
            subbrute_scene_sequence_run_pause(instance);
        } else if(event.event == SubBruteCustomEventTypeTransmitStarted) {
            // OK pressed: resume after a pause, or skip the countdown.
            if(instance->seq_paused) {
                subbrute_scene_sequence_run_resume(instance);
            } else if(instance->seq_pending_next) {
                instance->seq_pending_next = false;
                instance->seq_countdown = 0;
                subbrute_scene_sequence_run_start_current(instance);
            }
        } else if(event.event == SubBruteCustomEventTypeBackPressed) {
            if(subbrute_worker_is_running(instance->worker)) {
                subbrute_scene_sequence_run_pause(instance);
            } else {
                // During the countdown or while paused: abort the sequence.
                subbrute_scene_sequence_run_finish(instance);
            }
        } else if(event.event == SubBruteCustomEventTypeError) {
            notification_message(instance->notifications, &sequence_error);
            subbrute_scene_sequence_run_advance(instance);
        } else if(event.event == SubBruteCustomEventTypeSaveFile) {
            // Ignored in automatic sequence mode.
        } else if(event.event == SubBruteCustomEventTypeTransmitCustom) {
            if(subbrute_worker_can_manual_transmit(instance->worker)) {
                notification_message(instance->notifications, &sequence_blink_green_100);
                subbrute_worker_transmit_current_key(
                    instance->worker, instance->device->current_step);
                notification_message(instance->notifications, &sequence_blink_stop);
            }
        } else if(
            event.event == SubBruteCustomEventTypeChangeStepUp ||
            event.event == SubBruteCustomEventTypeChangeStepDown ||
            event.event == SubBruteCustomEventTypeChangeStepUpMore ||
            event.event == SubBruteCustomEventTypeChangeStepDownMore) {
            if(instance->seq_paused) {
                int8_t delta = 0;
                if(event.event == SubBruteCustomEventTypeChangeStepUp) {
                    delta = 1;
                } else if(event.event == SubBruteCustomEventTypeChangeStepDown) {
                    delta = -1;
                } else if(event.event == SubBruteCustomEventTypeChangeStepUpMore) {
                    delta = 50;
                } else {
                    delta = -50;
                }
                uint64_t new_step = subbrute_device_add_step(instance->device, delta);
                subbrute_worker_set_step(instance->worker, new_step);
                subbrute_attack_view_set_current_step(view, new_step);
            }
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeTick) {
        uint64_t step = subbrute_worker_get_step(instance->worker);
        instance->device->current_step = step;
        subbrute_attack_view_set_current_step(view, step);
        consumed = true;

        if(instance->seq_pending_next) {
            // Countdown before starting the next preset.
            if(instance->seq_countdown > 0) {
                instance->seq_countdown--;
                subbrute_attack_view_set_seq_countdown(view, (uint8_t)instance->seq_countdown);
            }
            if(instance->seq_countdown == 0) {
                instance->seq_pending_next = false;
                subbrute_worker_stop(instance->worker);
                subbrute_scene_sequence_run_start_current(instance);
            }
        } else if(subbrute_worker_is_running(instance->worker)) {
            // No-progress watchdog: if the step has not advanced for ~3.5 s at
            // ANY value, force the advance to the next preset. This covers radio
            // hangs below the real code space and lost events.
            if(instance->device->current_step == instance->seq_last_step) {
                instance->seq_stall_ticks++;
                if(instance->seq_stall_ticks > 35) { // ~3.5 s at 100 ms tick
                    FURI_LOG_E(
                        TAG,
                        "No progress at step %llu, forcing advance",
                        (unsigned long long)instance->device->current_step);
                    instance->seq_last_step = 0;
                    instance->seq_stall_ticks = 0;
                    subbrute_worker_stop(instance->worker);
                    subbrute_scene_sequence_run_advance(instance);
                }
            } else {
                instance->seq_last_step = instance->device->current_step;
                instance->seq_stall_ticks = 0;
            }
            instance->seq_finish_ticks = 0;
        } else if(!instance->seq_paused) {
            // Worker is not running and the user did not pause it: the preset just
            // finished. If the TransmitFinished event was not delivered, self-heal
            // after a short grace period and move on so the sequence never stalls.
            instance->seq_last_step = instance->device->current_step;
            instance->seq_stall_ticks = 0;
            if(instance->device->current_step >= instance->device->max_value) {
                instance->seq_finish_ticks++;
                if(instance->seq_finish_ticks > 20) { // ~2 s at 100 ms tick
                    FURI_LOG_E(
                        TAG,
                        "Preset end not processed, self-healing advance (step %llu)",
                        (unsigned long long)instance->device->current_step);
                    instance->seq_finish_ticks = 0;
                    subbrute_worker_stop(instance->worker);
                    subbrute_scene_sequence_run_advance(instance);
                }
            } else {
                instance->seq_finish_ticks = 0;
            }
        } else {
            // Paused by the user: keep counters reset, nothing to self-heal.
            instance->seq_last_step = instance->device->current_step;
            instance->seq_stall_ticks = 0;
            instance->seq_finish_ticks = 0;
        }
    }

    return consumed;
}