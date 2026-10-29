#include "subbrute_worker_private.h"
#include <furi.h>
#include <string.h>
#include <toolbox/stream/stream.h>
#include <flipper_format.h>
#include <flipper_format_i.h>
#include <lib/subghz/subghz_protocol_registry.h>
// use to clear custom_btn
#include <lib/subghz/blocks/custom_btn.h>

#define TAG                               "SubBruteWorker"
#define SUBBRUTE_TX_TIMEOUT               6
#define SUBBRUTE_MANUAL_TRANSMIT_INTERVAL 250

static void subbrute_worker_wait_thread_stopped(SubBruteWorker* instance, uint32_t timeout_ms);

SubBruteWorker* subbrute_worker_alloc(const SubGhzDevice* radio_device) {
    SubBruteWorker* instance = malloc(sizeof(SubBruteWorker));

    instance->state = SubBruteWorkerStateIDLE;
    instance->step = 0;
    instance->worker_running = false;
    instance->initiated = false;
    instance->last_time_tx_data = 0;
    instance->load_index = 0;
    instance->opencode = 0;

    instance->thread = furi_thread_alloc();
    furi_thread_set_name(instance->thread, "SubBruteAttackWorker");
    furi_thread_set_stack_size(instance->thread, 2048);
    furi_thread_set_context(instance->thread, instance);
    furi_thread_set_callback(instance->thread, subbrute_worker_thread);

    instance->context = NULL;
    instance->callback = NULL;

    instance->tx_timeout_ms = SUBBRUTE_TX_TIMEOUT;
    instance->decoder_result = NULL;
    instance->transmitter = NULL;
    instance->environment = subghz_environment_alloc();
    subghz_environment_set_protocol_registry(
        instance->environment, (void*)&subghz_protocol_registry);

    instance->transmit_mode = false;

    instance->radio_device = radio_device;

    return instance;
}

void subbrute_worker_free(SubBruteWorker* instance) {
    furi_assert(instance);

    // I don't know how to free this
    instance->decoder_result = NULL;

    if(instance->transmitter != NULL) {
        subghz_transmitter_free(instance->transmitter);
        instance->transmitter = NULL;
    }

    subghz_environment_free(instance->environment);
    instance->environment = NULL;

    // furi_thread_free() requires the thread to be stopped.
    subbrute_worker_wait_thread_stopped(instance, 8000);
    if(furi_thread_get_state(instance->thread) == FuriThreadStateStopped) {
        furi_thread_free(instance->thread);
    } else {
        FURI_LOG_E(TAG, "Worker thread did not stop, leaking it to avoid a crash");
    }

    subghz_devices_sleep(instance->radio_device);
    subbrute_radio_device_loader_end(instance->radio_device);

    free(instance);
}

uint64_t subbrute_worker_get_step(SubBruteWorker* instance) {
    return instance->step;
}

bool subbrute_worker_set_step(SubBruteWorker* instance, uint64_t step) {
    furi_assert(instance);
    if(!subbrute_worker_can_manual_transmit(instance)) {
        FURI_LOG_W(TAG, "Cannot set step during running mode");

        return false;
    }

    instance->step = step;

    return true;
}

void subbrute_worker_set_opencode(SubBruteWorker* instance, uint8_t opencode) {
    // furi_assert(instance);
    // if(!subbrute_worker_can_manual_transmit(instance)) {
    //     FURI_LOG_W(TAG, "Cannot set opencode during running mode");
    //
    //     return false;
    // }
    instance->opencode = opencode;

    // return true;
}

uint8_t subbrute_worker_get_opencode(SubBruteWorker* instance) {
    return instance->opencode;
}

bool subbrute_worker_get_is_pt2262(SubBruteWorker* instance) {
    if(instance->attack == SubBruteAttackPT226224bit315 ||
       instance->attack == SubBruteAttackPT226224bit418 ||
       instance->attack == SubBruteAttackPT226224bit430 ||
       instance->attack == SubBruteAttackPT226224bit4305 ||
       instance->attack == SubBruteAttackPT226224bit433) {
        return true;
    } else {
        return false;
    }
}

bool subbrute_worker_init_default_attack(
    SubBruteWorker* instance,
    SubBruteAttacks attack_type,
    uint64_t step,
    const SubBruteProtocol* protocol,
    uint8_t repeats) {
    furi_assert(instance);

    if(instance->worker_running) {
        FURI_LOG_W(TAG, "Init Worker when it's running");
        subbrute_worker_stop(instance);
    }
    // Always wait for a possibly dying worker thread before overwriting the
    // shared fields below (a non-blocking stop may still have the thread
    // finishing its last radio call).
    subbrute_worker_wait_thread_stopped(instance, 8000);

    instance->attack = attack_type;
    instance->frequency = protocol->frequency;
    instance->preset = protocol->preset;
    instance->file = protocol->file;
    instance->step = step;
    instance->bits = protocol->bits;
    instance->te = protocol->te;
    instance->opencode = protocol->opencode;
    instance->repeat = repeats;
    instance->load_index = 0;
    instance->file_key = 0;
    instance->two_bytes = false;

    instance->max_value =
        subbrute_protocol_calc_max_value(instance->attack, instance->bits, instance->two_bytes);

    instance->initiated = true;
    instance->state = SubBruteWorkerStateReady;
    subbrute_worker_send_callback(instance);
#ifdef FURI_DEBUG
    FURI_LOG_I(
        TAG,
        "subbrute_worker_init_default_attack: %s, bits: %d, preset: %s, file: %s, te: %ld, repeat: %d, max_value: %lld",
        subbrute_protocol_name(instance->attack),
        instance->bits,
        subbrute_protocol_preset(instance->preset),
        subbrute_protocol_file(instance->file),
        instance->te,
        instance->repeat,
        instance->max_value);
#endif

    return true;
}

bool subbrute_worker_init_file_attack(
    SubBruteWorker* instance,
    uint64_t step,
    uint8_t load_index,
    uint64_t file_key,
    SubBruteProtocol* protocol,
    uint8_t repeats,
    bool two_bytes) {
    furi_assert(instance);

    if(instance->worker_running) {
        FURI_LOG_W(TAG, "Init Worker when it's running");
        subbrute_worker_stop(instance);
    }
    // Always wait for a possibly dying worker thread before overwriting the
    // shared fields below (a non-blocking stop may still have the thread
    // finishing its last radio call).
    subbrute_worker_wait_thread_stopped(instance, 8000);

    instance->attack = SubBruteAttackLoadFile;
    instance->frequency = protocol->frequency;
    instance->preset = protocol->preset;
    instance->file = protocol->file;
    instance->step = step;
    instance->bits = protocol->bits;
    instance->te = protocol->te;
    instance->load_index = load_index;
    instance->repeat = repeats;
    instance->file_key = file_key;
    instance->two_bytes = two_bytes;

    instance->max_value =
        subbrute_protocol_calc_max_value(instance->attack, instance->bits, instance->two_bytes);

    instance->initiated = true;
    instance->state = SubBruteWorkerStateReady;
    subbrute_worker_send_callback(instance);
#ifdef FURI_DEBUG
    FURI_LOG_I(
        TAG,
        "subbrute_worker_init_file_attack: %s, bits: %d, preset: %s, file: %s, te: %ld, repeat: %d, max_value: %lld, key: %llX",
        subbrute_protocol_name(instance->attack),
        instance->bits,
        subbrute_protocol_preset(instance->preset),
        subbrute_protocol_file(instance->file),
        instance->te,
        instance->repeat,
        instance->max_value,
        instance->file_key);
#endif

    return true;
}

static void subbrute_worker_wait_thread_stopped(SubBruteWorker* instance, uint32_t timeout_ms) {
    const uint32_t started_at = furi_get_tick();
    while(furi_thread_get_state(instance->thread) != FuriThreadStateStopped) {
        if((furi_get_tick() - started_at) >= timeout_ms) {
            FURI_LOG_W(
                TAG, "Worker thread did not stop within %lu ms", (unsigned long)timeout_ms);
            break;
        }
        furi_delay_ms(10);
    }
}

bool subbrute_worker_start(SubBruteWorker* instance) {
    furi_assert(instance);
    FURI_LOG_E(
        TAG,
        "Worker start requested for %s (attack=%d, running=%d, state=%d)",
        subbrute_protocol_name(instance->attack),
        (int)instance->attack,
        instance->worker_running,
        (int)instance->state);

    if(!instance->initiated) {
        FURI_LOG_W(TAG, "Worker not init!");
        return false;
    }

    if(instance->worker_running) {
        FURI_LOG_W(TAG, "Worker is already running!");
        return false;
    }
    if(instance->state != SubBruteWorkerStateReady &&
       instance->state != SubBruteWorkerStateFinished) {
        FURI_LOG_W(TAG, "Worker cannot start, invalid device state: %d", instance->state);
        return false;
    }

    // furi_thread_start() requires the underlying thread to be fully stopped.
    // Wait for it (bounded) so we never crash or race against a dying thread.
    subbrute_worker_wait_thread_stopped(instance, 5000);
    if(furi_thread_get_state(instance->thread) != FuriThreadStateStopped) {
        FURI_LOG_E(TAG, "Worker thread failed to stop, cannot restart it");
        return false;
    }

    instance->worker_running = true;
    furi_thread_start(instance->thread);

    return true;
}

void subbrute_worker_stop(SubBruteWorker* instance) {
    furi_assert(instance);

    if(!instance->worker_running) {
        return;
    }

    // Request the worker to stop WITHOUT blocking the caller: joining a thread
    // that is stuck inside a radio call would freeze the GUI thread forever.
    // The worker loop observes worker_running == false, exits, and the thread
    // reaches FuriThreadStateStopped. subbrute_worker_wait_thread_stopped() is
    // used by the init/restart/teardown paths to wait for that state before the
    // worker fields are touched again.
    instance->worker_running = false;
}

bool subbrute_worker_transmit_current_key(SubBruteWorker* instance, uint64_t step) {
    furi_assert(instance);

    if(!instance->initiated) {
        FURI_LOG_W(TAG, "Worker not init!");
        return false;
    }
    if(instance->worker_running) {
        FURI_LOG_W(TAG, "Worker in running state!");
        return false;
    }
    if(instance->state != SubBruteWorkerStateReady &&
       instance->state != SubBruteWorkerStateFinished) {
        FURI_LOG_W(TAG, "Invalid state for running worker! State: %d", instance->state);
        return false;
    }

    uint32_t ticks = furi_get_tick();
    if((ticks - instance->last_time_tx_data) < SUBBRUTE_MANUAL_TRANSMIT_INTERVAL) {
#ifdef FURI_DEBUG
        FURI_LOG_D(TAG, "Need to wait, current: %ld", ticks - instance->last_time_tx_data);
#endif
        return false;
    }

    instance->last_time_tx_data = ticks;
    instance->step = step;

    bool result;
    instance->protocol_name = subbrute_protocol_file(instance->file);
    FlipperFormat* flipper_format = flipper_format_string_alloc();
    Stream* stream = flipper_format_get_raw_stream(flipper_format);

    stream_clean(stream);

    if(instance->attack == SubBruteAttackLoadFile) {
        subbrute_protocol_file_payload(
            stream,
            step,
            instance->bits,
            instance->te,
            instance->repeat,
            instance->load_index,
            instance->file_key,
            instance->two_bytes);
    } else {
        subbrute_protocol_default_payload(
            stream,
            instance->file,
            step,
            instance->bits,
            instance->te,
            instance->repeat,
            instance->opencode);
    }

    subbrute_worker_subghz_transmit(instance, flipper_format);

    result = true;
#ifdef FURI_DEBUG
    FURI_LOG_W(TAG, "Manual transmit done");
#endif
    flipper_format_free(flipper_format);

    return result;
}

bool subbrute_worker_is_running(SubBruteWorker* instance) {
    return instance->worker_running;
}

bool subbrute_worker_can_manual_transmit(SubBruteWorker* instance) {
    furi_assert(instance);

    if(!instance->initiated) {
        FURI_LOG_W(TAG, "Worker not init!");
        return false;
    }

    return !instance->worker_running && instance->state != SubBruteWorkerStateIDLE &&
           instance->state != SubBruteWorkerStateTx &&
           ((furi_get_tick() - instance->last_time_tx_data) > SUBBRUTE_MANUAL_TRANSMIT_INTERVAL);
}

void subbrute_worker_set_callback(
    SubBruteWorker* instance,
    SubBruteWorkerCallback callback,
    void* context) {
    furi_assert(instance);

    instance->callback = callback;
    instance->context = context;
}

static void subbrute_worker_wait_tx_complete(SubBruteWorker* instance, uint8_t timeout) {
    // Bounded wait: if the radio never reports TX completion we still must move
    // on, otherwise the worker would hang ~2 s on every frame and the whole
    // brute/sequence would take hours. A short bound keeps the counts flowing
    // (and the sequence advancing) no matter the radio state; frames of the
    // presets here are a few ms long, so 500 ms is far more than enough.
    const uint16_t max_waits = (uint16_t)(500 / (timeout > 0 ? timeout : 1)) + 1;
    uint16_t waits = 0;
    while(!subghz_devices_is_async_complete_tx(instance->radio_device) && waits < max_waits) {
        furi_delay_ms(timeout);
        waits++;
    }
}

void subbrute_worker_subghz_transmit(SubBruteWorker* instance, FlipperFormat* flipper_format) {
    const uint8_t timeout = instance->tx_timeout_ms;
    while(instance->transmit_mode) {
        furi_delay_ms(timeout);
    }
    instance->transmit_mode = true;
    if(instance->transmitter != NULL) {
        subghz_transmitter_free(instance->transmitter);
        instance->transmitter = NULL;
    }

    // instance->protocol_name = subbrute_protocol_file(instance->file);

    instance->transmitter =
        subghz_transmitter_alloc_init(instance->environment, instance->protocol_name);
    subghz_transmitter_deserialize(instance->transmitter, flipper_format);

    subghz_devices_reset(instance->radio_device);
    subghz_devices_idle(instance->radio_device);
    subghz_devices_load_preset(instance->radio_device, instance->preset, NULL);
    subghz_devices_set_frequency(
        instance->radio_device, instance->frequency); // TODO is freq valid check

    if(subghz_devices_set_tx(instance->radio_device)) {
        subghz_devices_start_async_tx(
            instance->radio_device, subghz_transmitter_yield, instance->transmitter);
        subbrute_worker_wait_tx_complete(instance, timeout);
        subghz_devices_stop_async_tx(instance->radio_device);
    }

    subghz_devices_idle(instance->radio_device);

    subghz_transmitter_stop(instance->transmitter);
    subghz_transmitter_free(instance->transmitter);
    instance->transmitter = NULL;

    instance->transmit_mode = false;

    Stream* stream = flipper_format_get_raw_stream(flipper_format);
    stream_rewind(stream);
    //test_read_full_stream(stream, "Transmit data");

    subghz_custom_btns_reset();
}

void subbrute_worker_send_callback(SubBruteWorker* instance) {
    if(instance->callback != NULL) {
        instance->callback(instance->context, instance->state);
    }
}

/**
 * Entrypoint for worker
 *
 * @param context SubBruteWorker*
 * @return 0 if ok
 */
int32_t subbrute_worker_thread(void* context) {
    furi_assert(context);
    SubBruteWorker* instance = (SubBruteWorker*)context;

    if(!instance->worker_running) {
        FURI_LOG_W(TAG, "Worker is not set to running state!");

        return -1;
    }
    if(instance->state != SubBruteWorkerStateReady &&
       instance->state != SubBruteWorkerStateFinished) {
        FURI_LOG_W(TAG, "Invalid state for running worker! State: %d", instance->state);

        return -2;
    }
#ifdef FURI_DEBUG
    FURI_LOG_I(TAG, "Worker start");
#endif

    SubBruteWorkerState local_state = instance->state = SubBruteWorkerStateTx;
    subbrute_worker_send_callback(instance);

    instance->protocol_name = subbrute_protocol_file(instance->file);

    FlipperFormat* flipper_format = flipper_format_string_alloc();
    Stream* stream = flipper_format_get_raw_stream(flipper_format);

    uint32_t heartbeat = 0;
    while(instance->worker_running) {
        if((heartbeat++ % 256) == 0) {
            FURI_LOG_I(
                TAG,
                "Heartbeat step %llu/%llu",
                (unsigned long long)instance->step,
                (unsigned long long)instance->max_value);
        }
        stream_clean(stream);
        if(instance->attack == SubBruteAttackLoadFile) {
            subbrute_protocol_file_payload(
                stream,
                instance->step,
                instance->bits,
                instance->te,
                instance->repeat,
                instance->load_index,
                instance->file_key,
                instance->two_bytes);
        } else {
            subbrute_protocol_default_payload(
                stream,
                instance->file,
                instance->step,
                instance->bits,
                instance->te,
                instance->repeat,
                instance->opencode);
        }
#ifdef FURI_DEBUG
        //FURI_LOG_I(TAG, "Payload: %s", furi_string_get_cstr(payload));
        //furi_delay_ms(SUBBRUTE_MANUAL_TRANSMIT_INTERVAL / 4);
#endif

        subbrute_worker_subghz_transmit(instance, flipper_format);

        if(instance->step + 1 > instance->max_value) {
#ifdef FURI_DEBUG
            FURI_LOG_I(TAG, "Worker finished to end");
#endif
            local_state = SubBruteWorkerStateFinished;

            break;
        }
        instance->step++;

        furi_delay_ms(instance->tx_timeout_ms);
    }

    flipper_format_free(flipper_format);

    // Keep worker_running = true here: the thread is not fully terminated yet,
    // so a following subbrute_worker_stop() (called on the "Finished" event)
    // must join() it before the worker is started again. Otherwise the thread
    // could be restarted while it is still tearing down. subbrute_worker_stop()
    // clears the flag and joins.
    instance->state = local_state == SubBruteWorkerStateTx ? SubBruteWorkerStateReady :
                                                             local_state;
    FURI_LOG_E(TAG, "Worker thread end, state: %d, step: %lld", instance->state, instance->step);
    subbrute_worker_send_callback(instance);
    FURI_LOG_E(TAG, "Worker callback sent (state %d)", instance->state);

    // Signal that the worker is truly done. worker_running stays false from
    // here on, so the scenes' tick handlers can detect "file finished and not
    // running" (step == max_value) and self-heal when the TransmitFinished
    // custom event was lost, instead of being stuck on the final value.
    instance->worker_running = false;

#ifdef FURI_DEBUG
    FURI_LOG_I(TAG, "Worker stop");
#endif

    return 0;
}

uint8_t subbrute_worker_get_timeout(SubBruteWorker* instance) {
    return instance->tx_timeout_ms;
}

void subbrute_worker_set_timeout(SubBruteWorker* instance, uint8_t timeout) {
    instance->tx_timeout_ms = timeout;
}

uint8_t subbrute_worker_get_repeats(SubBruteWorker* instance) {
    return instance->repeat;
}

void subbrute_worker_set_repeats(SubBruteWorker* instance, uint8_t repeats) {
    instance->repeat = repeats;
}

uint32_t subbrute_worker_get_te(SubBruteWorker* instance) {
    return instance->te;
}

void subbrute_worker_set_te(SubBruteWorker* instance, uint32_t te) {
    instance->te = te;
}

bool subbrute_worker_is_tx_allowed(SubBruteWorker* instance, uint32_t value) {
    furi_assert(instance);
    bool res = false;

    if(!subghz_devices_is_frequency_valid(instance->radio_device, value)) {
        return false;
    } else {
        subghz_devices_set_frequency(instance->radio_device, value);
        res = subghz_devices_set_tx(instance->radio_device);
        subghz_devices_idle(instance->radio_device);
    }

    return res;
}

/*
void test_read_full_stream(Stream* stream, const char* msg) {
    // read data
    // 循环读取stream中的数据每次读取一个字节（uint8_t）
    size_t size_2 = stream_size(stream);
    // read data
    // 循环读取stream中的数据每次读取一个字节（uint8_t）
    char* data_2 = (char*)malloc(size_2 + 1);
    for(size_t i = 0; i < size_2; i++) {
        stream_read(stream, (uint8_t*)&data_2[i], 1);
    }
    data_2[size_2] = '\0';

    FURI_LOG_W(TAG, "%s Transmit data: %s", msg, data_2);
    free(data_2);
}
*/