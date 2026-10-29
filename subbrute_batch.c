#include "subbrute_i.h"

#include <storage/storage.h>
#include <storage/filesystem_api_defines.h>
#include <string.h>

#define TAG "SubBruteBatch"

#define SUBBRUTE_BATCH_NAME_MAX 32

static void subbrute_batch_add_path(SubBruteState* instance, const FuriString* path) {
    if(instance->batch_count >= SUBBRUTE_BATCH_MAX_FILES) {
        return;
    }

    FuriString** new_paths = (FuriString**)realloc(
        instance->batch_paths, (instance->batch_count + 1) * sizeof(FuriString*));
    furi_check(new_paths != NULL);

    instance->batch_paths = new_paths;
    instance->batch_paths[instance->batch_count] = furi_string_alloc_set(path);
    instance->batch_count++;
}

static void subbrute_batch_collect_dir(SubBruteState* instance, FuriString* path, uint8_t depth) {
    if(depth > SUBBRUTE_BATCH_MAX_DEPTH) {
        return;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(storage);

    if(storage_dir_open(dir, furi_string_get_cstr(path))) {
        FileInfo info;
        FuriString* child_path = furi_string_alloc();
        char name[96];

        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            furi_string_printf(child_path, "%s/%s", furi_string_get_cstr(path), name);

            if(file_info_is_dir(&info)) {
                subbrute_batch_collect_dir(instance, child_path, depth + 1);
            } else if(
                furi_string_end_with_str(child_path, SUBBRUTE_FILE_EXT) &&
                instance->batch_count < SUBBRUTE_BATCH_MAX_FILES) {
                // Skip the currently selected single file - it will be re-added
                FURI_LOG_D(TAG, "Batch collect: %s", furi_string_get_cstr(child_path));
                subbrute_batch_add_path(instance, child_path);
            }
        }

        furi_string_free(child_path);
        storage_dir_close(dir);
    }

    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);
}

void subbrute_batch_collect(SubBruteState* instance) {
    furi_assert(instance);

    subbrute_batch_clear(instance);

    if(furi_string_empty(instance->batch_folder)) {
        FURI_LOG_W(TAG, "Batch folder is empty");
        return;
    }

    subbrute_batch_collect_dir(instance, instance->batch_folder, 0);

    FURI_LOG_I(TAG, "Collected %d files from %s", (int)instance->batch_count, furi_string_get_cstr(instance->batch_folder));
}

bool subbrute_batch_has_files(SubBruteState* instance) {
    furi_assert(instance);
    return instance->batch_count > 0;
}

void subbrute_batch_clear(SubBruteState* instance) {
    furi_assert(instance);

    for(size_t i = 0; i < instance->batch_count; i++) {
        furi_string_free(instance->batch_paths[i]);
    }
    if(instance->batch_paths != NULL) {
        free(instance->batch_paths);
        instance->batch_paths = NULL;
    }
    instance->batch_count = 0;
    instance->batch_index = 0;
}

SubBruteFileResult subbrute_batch_load_current(SubBruteState* instance) {
    furi_assert(instance);

    if(instance->batch_index >= instance->batch_count) {
        return SubBruteFileResultErrorOpenFile;
    }

    const char* path = furi_string_get_cstr(instance->batch_paths[instance->batch_index]);
    FURI_LOG_I(TAG, "Batch load [%d/%d]: %s", (int)instance->batch_index, (int)instance->batch_count, path);

    SubBruteFileResult result = subbrute_device_load_from_file(instance->device, path);
    if(result != SubBruteFileResultOk) {
        return result;
    }

    uint8_t extra_repeats = subbrute_settings_get_current_repeats(instance->settings);
    result = subbrute_device_attack_set(instance->device, SubBruteAttackLoadFile, extra_repeats);
    if(result != SubBruteFileResultOk) {
        return result;
    }

    // Brute-force the key bytes from bit_index-1/bit_index for every file; the
    // max_value is capped to the file's real code space (e.g. 4095 for 12-bit).
    instance->device->bit_index = 7;
    instance->device->two_bytes = true;
    instance->device->current_step = 0;
    instance->device->max_value = subbrute_protocol_calc_max_value(
        SubBruteAttackLoadFile, instance->device->file_protocol_info->bits, true);

    if(!subbrute_worker_init_file_attack(
           instance->worker,
           instance->device->current_step,
           instance->device->bit_index,
           instance->device->key_from_file,
           instance->device->file_protocol_info,
           extra_repeats,
           instance->device->two_bytes)) {
        return SubBruteFileResultProtocolNotFound;
    }

    return SubBruteFileResultOk;
}

const char* subbrute_batch_file_display_name(SubBruteState* instance, char* out, size_t out_size) {
    furi_assert(instance);
    furi_assert(out);

    const char* name = "batch";
    if(instance->batch_index < instance->batch_count) {
        const char* full = furi_string_get_cstr(instance->batch_paths[instance->batch_index]);
        const char* last_slash = strrchr(full, '/');
        name = last_slash != NULL ? last_slash + 1 : full;
    }

    if(out_size <= 0) {
        return out;
    }

    const size_t max_len = out_size - 1;
    size_t len = strlen(name);
    if(len > max_len) {
        len = max_len - 2; // leave room for ".."
        if(len > SUBBRUTE_BATCH_NAME_MAX) {
            len = SUBBRUTE_BATCH_NAME_MAX;
        }
        memcpy(out, name, len);
        strcpy(&out[len], "..");
    } else {
        memcpy(out, name, len + 1);
    }

    return out;
}

const char* subbrute_batch_current_label(SubBruteState* instance, char* out, size_t out_size) {
    furi_assert(instance);
    furi_assert(out);

    if(instance->batch_index >= instance->batch_count) {
        strlcpy(out, "Batch", out_size);
        return out;
    }

    const SubBruteProtocol* info = instance->device->file_protocol_info;
    if(info == NULL) {
        strlcpy(out, "Batch", out_size);
        return out;
    }

    snprintf(
        out,
        out_size,
        "%s %dbit %uMHz",
        subbrute_protocol_file(info->file),
        info->bits,
        (unsigned int)(info->frequency / 1000000));

    return out;
}