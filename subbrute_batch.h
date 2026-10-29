#pragma once

#include "subbrute.h"
#include "subbrute_device.h"

#define SUBBRUTE_BATCH_MAX_FILES 256
#define SUBBRUTE_BATCH_MAX_DEPTH 8

/**
 * @brief Collects all .sub files (recursively) from instance->batch_folder
 *        into the instance->batch_paths list, replacing any previous list.
 *
 * @param instance SubBruteState to populate
 */
void subbrute_batch_collect(SubBruteState* instance);

/**
 * @brief Checks whether the batch list contains at least one entry.
 *
 * @param instance SubBruteState
 * @return true if batch_count > 0, false otherwise
 */
bool subbrute_batch_has_files(SubBruteState* instance);

/**
 * @brief Frees the collected batch file list and resets indexes.
 *
 * @param instance SubBruteState
 */
void subbrute_batch_clear(SubBruteState* instance);

/**
 * @brief Loads the file at instance->batch_index into the device and
 *        re-initializes the worker for a full file attack.
 *
 * Defaults bit_index to 7 (low byte) and two_bytes to true so that the low
 * 16 bits of the key are brute-forced for every file.
 *
 * @param instance SubBruteState
 * @return SubBruteFileResultOk on success, error otherwise
 */
SubBruteFileResult subbrute_batch_load_current(SubBruteState* instance);

/**
 * @brief Writes a short display name (basename, truncated) of the current
 *        batch file into out.
 *
 * @param instance SubBruteState
 * @param out output buffer
 * @param out_size size of out
 * @return out
 */
const char* subbrute_batch_file_display_name(SubBruteState* instance, char* out, size_t out_size);

/**
 * @brief Builds a human readable label for the current batch file,
 *        e.g. "CAME 12bit 303MHz", using the loaded file protocol data.
 *
 * @param instance SubBruteState
 * @param out output buffer
 * @param out_size size of out
 * @return out
 */
const char* subbrute_batch_current_label(SubBruteState* instance, char* out, size_t out_size);