# TASK: Refactor Oscilloscope Readers and Fix Build Errors

**ID:** TASK_RefactorReadersAndFixBuild
**Status:** Open
**Assignee:** Gemini
**Reporter:** User
**Date Created:** 2024-05-27

## Goal

Complete the refactoring of `oscReader_analog` and `oscSender` functions in `servers/oscilloscope.h` for multi-channel support using the `oscMultiChannelSample` struct. Fix all remaining build errors reported by PlatformIO (`pio run`).

## Current Status

*   **`servers/oscilloscope.h`:**
    *   `oscReader_digital`: Partially refactored. Variable initialization, sampling time correction, debug output, and GPIO setup blocks updated. Inner loop and cleanup block edits failed via tool/manual attempts (reported by user). Current state may be inconsistent.
    *   `oscReader_analog`: Refactoring started (variable init updated). Attempts to replace inner loop logic failed. Function likely contains old 1/2 channel logic mixed with partial multi-channel refactors.
    *   `oscSender`: Not yet refactored for `oscMultiChannelSample`. Still uses old logic (`noOfSignals`, `samplesI2sSignal`, etc.).
    *   `runOscilloscope`: JSON parsing implemented, ADC/GPIO setup for multi-channel added. Missing includes (`<esp_adc/adc_oneshot.h>`, `<ArduinoJson.h>`) and helper function placement need correction. Indices (`activeAnalogChannelIndices`, `activeDigitalChannelIndices`) need population during parsing. `adc_handle` assignment needs verification.
    *   `oscReader_millis`, `oscReader_analog_1_signal_i2s`: Commented out.
*   **Build Errors:** Previous build (`pio run`) failed with errors including:
    *   Missing includes (`WiFiServer.h`, ArduinoJson, ESP-ADC).
    *   Undeclared identifiers (`JsonDocument`, `adc_oneshot_unit_handle_t`).
    *   Errors in `oscReader_analog` and `oscSender` due to using old variables/struct members.
    *   Potential errors in `oscReader_digital`'s inner loop/cleanup due to failed edits.
*   **`platformio.ini`:** ArduinoJson dependency added. Missing `upload_tool` warning present.

## Remaining Implementation Steps

1.  **Verify/Complete `oscReader_analog` Refactoring:**
    *   **Manually** confirm/replace the inner sampling loop logic (lines ~527-621) using `adc_oneshot_read`, iterating `activeAnalogChannelIndices`, populating `samplesMultiChannel`, and correct timing. (Code provided in previous conversation turns).
    *   **Manually** add the `next_screen_analog:` label before `vTaskDelayUntil`.
    *   Replace the final cleanup block (lines ~625-629) with the multi-channel version (using `sm`, correct state checks, `vTaskDelete`).
2.  **Refactor `oscSender`:**
    *   Modify the main loop to check `sendBuffer->samplesAreReady`.
    *   Inside the `if`, copy the `sendBuffer` (using `oscMultiChannelSample` union member).
    *   Calculate `sendBytes` based on `sendSamples.sampleCount` and `sizeof(oscMultiChannelSample)`.
    *   Remove logic based on `noOfSignals`, `samplesI2sSignal`, `samples1Signal`, `samples2Signals`.
    *   Adapt dummy message detection if needed (e.g., check `sendSamples.samplesMultiChannel[0].delta_time == -1` - TBC if sender needs this).
    *   Update endianness swapping logic for the `samplesMultiChannel` data if `clientIsBigEndian`.
3.  **Fix `runOscilloscope` Issues:**
    *   Add `#include <ArduinoJson.h>` at the top.
    *   Add `#include <esp_adc/adc_oneshot.h>` at the top.
    *   Add `#include <WiFiServer.h>` (verify this is the correct header for the WebSocket dependency).
    *   Move the `static esp_err_t gpio_to_adc1_channel(...)` helper function definition *outside* and *before* `runOscilloscope`.
    *   In the JSON channel parsing loop:
        *   Populate `sm->config.activeAnalogChannelIndices` and `sm->config.activeDigitalChannelIndices` arrays and update their respective counts (`sm->config.activeAnalogChannelCount`, `sm->config.activeDigitalChannelCount`).
        *   Ensure `sm->config.adc_handle = adc1_handle;` is assigned correctly *within the loop* for each active analog channel *after* `adc_oneshot_config_channel` succeeds (or perhaps store it once globally in `sm->config.adc_handle` after `adc_oneshot_new_unit` if only one handle is used). **Clarification:** The current code assigns the same `adc1_handle` to *all* active channels' `adc_handle` field, which is likely incorrect. The reader needs the *unit* handle. Store the `adc1_handle` in `sharedMemory.config.adc_handle` once after `adc_oneshot_new_unit`. Remove the `adc_handle` field from `OscChannelConfig`.
    *   Verify the ADC cleanup logic `adc_oneshot_del_unit(adc1_handle);` is called correctly at the end.
4.  **Fix Remaining Build Errors:**
    *   Run `pio run`.
    *   Address any remaining undeclared identifiers or struct member access errors, likely stemming from the mix of old/new code in the unverified/unrefactored functions.
    *   Address the `upload_tool` warning in `platformio.ini` if necessary (or confirm it's benign).
5.  **Build and Test:**
    *   Perform a clean build: `pio run -t clean`, then `pio run`.
    *   Upload firmware: `pio run -t upload`.
    *   Test basic multi-channel analog and digital capture via the web interface.

## Constraints

*   Follow Git Flow and TDD principles (as per `CONSTRAINTS.md`). Testing may be challenging until basic build/functionality is restored.
*   Use semantic commit messages.
*   Address build errors methodically.

## Open Questions

*   How to implement trigger logic (`triggeredMode`) for multi-channel setup? (Deferred)
*   Performance implications of `adc_oneshot_read` in a loop vs. older methods?
*   Is `WiFiServer.h` the correct include for the WebSocket dependency?

## Definition of Done

*   `oscReader_analog` and `oscSender` are fully refactored for `oscMultiChannelSample`.
*   `runOscilloscope` includes and helper function placement are corrected.
*   `pio run` completes without errors.
*   Basic multi-channel analog and digital capture functions correctly via the web UI.

---
*Generated by Gemini based on conversation history.* 