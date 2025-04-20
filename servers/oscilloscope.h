/*
 
    oscilloscope.h
 
    This file is part of Esp32_web_ftp_telnet_server_template project: https://github.com/BojanJurca/Esp32_oscilloscope
                
    Issues:
            - when WiFi is in WIFI_AP or WIFI_STA_AP mode is oscillospe causes WDT problem when working at higher frequenceses

    May 22, 2024, Bojan Jurca
             
*/


    // ----- includes, definitions and supporting functions -----

    #include <WiFi.h>

    // digitalRead
    #include "driver/gpio.h"
    #include "hal/gpio_hal.h"

//    #include <soc/gpio_sig_map.h> // to digitalRead PWM and other GPIOs ...
    #include <driver/adc.h>       // to use adc1_get_raw instead of analogRead
    #include <driver/i2s.h>

    // fixed size strings    
    #include "std/Cstring.hpp"


#ifndef __OSCILLOSCOPE__
  #define __OSCILLOSCOPE__

    // #define __OSCILLOSCOPE_H_DEBUG__        // uncomment this line for debugging puroposes
    #ifdef __OSCILLOSCOPE_H_DEBUG__
        #define __oscilloscope_h_debug__(X) { Serial.print("__oscilloscope_h_debug__: ");Serial.println(X); }
    #else
        #define __oscilloscope_h_debug__(X) ;
    #endif


    // ----- TUNNING PARAMETERS -----

    #define HTTP_CONNECTION_STACK_SIZE  12 * 1024                     // increase default HTTP_CONNECTION_STACK_SIZE
    #include "httpServer.hpp"

    #define OSCILLOSCOPE_I2S_BUFFER_SIZE 746                          // max number of samples per screen, 746 samples * 2 bytes per sample = 1492 bytes, which must be <= HTTP_WS_FRAME_MAX_SIZE - 8
    #define OSCILLOSCOPE_1SIGNAL_BUFFER_SIZE 373                      // max number of samples per screen, 373 samples * 4 bytes per sample = 1492 bytes, which must be <= HTTP_WS_FRAME_MAX_SIZE - 8
    #define OSCILLOSCOPE_2SIGNALS_BUFFER_SIZE 248                     // max number of samples per screen, 248 samples * 6 bytes per sample = 1488 bytes, which must be <= HTTP_WS_FRAME_MAX_SIZE - 8


    // some ESP32 boards read analog values inverted, uncomment the following line to invert read values back again 
    #define INVERT_ADC1_GET_RAW

    // some ESP32 boards have wI2S interface which improoves analog sampling (for a single signal), uncomment the following line if your bord has an I2S interface
    // #define USE_I2S_INTERFACE

    // some ESP32 boards with I2S interface read analog values inverted, uncomment the following line to invert read values back again 
    // #define INVERT_I2S_READ

    // define a correction factor for sampling frequency if it needs to be corrected
    #define I2S_FREQ_CORRECTION (1.2)


    #ifdef USE_I2S_INTERFACE
        #pragma message "Oscilloscope will use I2S interface (for monitoring a single analog signal) and adc1_get_raw (for monitoring double analog signals)."
    #else
        #pragma message "I2S interface not used or present, Oscilloscope will use adc1_get_raw (for monitoring single and double analog signals)."
    #endif

    // #define OSCILLOSCOPE_READER_CORE 1 // 1 or 0                   // #define OSCILLOSCOPE_READER_CORE if you want oscilloscope reader to run on specific core    
    #ifndef OSCILLOSCOPE_READER_PRIORITY
        #define OSCILLOSCOPE_READER_PRIORITY 1                        // normal priority if not define differently
    #endif


    // ----- CODE -----

    #include "httpServer.hpp"                 // oscilloscope uses websockets defined in webServer.hpp  

    // for digital reading
    static gpio_hal_context_t __gpio_hal__ = {
        .dev = GPIO_HAL_GET_HW (GPIO_PORT_0)
    };        

    // oscilloscope samples
    struct oscI2sSample {                     // one sample
        int16_t signal1;                       // signal value of 1st GPIO read by analogRead or digialRead   
    }; // = 2 bytes per sample

    struct osc1SignalSample {                        // one sample
        int16_t signal1;                       // signal value of 1st GPIO read by analogRead or digialRead   
        int16_t deltaTime;                     // sample time - offset from previous sample in ms or us  
    }; // = 4 bytes per sample

    struct osc2SignalsSample {                        // one sample
        int16_t signal1;                       // signal value of 1st GPIO read by analogRead or digialRead   
        int16_t signal2;                       // signal value of 2nd GPIO if requested   
        int16_t deltaTime;                     // sample time - offset from previous sample in ms or us  
    }; // = 6 bytes per sample

    // Structure for a multi-channel sample (can hold analog or digital)
    struct oscMultiChannelSample {
        int16_t delta_time;                         // Time delta from previous sample in us (or special value like -1 for dummy message)
        uint8_t num_channels;                       // Actual number of channels included in this sample
        int16_t values[MAX_OSC_CHANNELS];           // Array to hold values for up to MAX_OSC_CHANNELS
                                                    // Size: 2 (delta_time) + 1 (num_channels) + 1 (padding?) + (2 * MAX_OSC_CHANNELS) bytes
                                                    // E.g., MAX_OSC_CHANNELS=4 -> 2+1+1 + 8 = 12 bytes? Check alignment.
                                                    // Let's use 16 bytes for safety/alignment with 4 channels.
                                                    // Let's recalculate: 2 (delta) + 1 (num) + 1 (padding) + 2*4 (values) = 12 bytes. Likely aligns to 4 bytes, so 12 bytes is probably correct.
    } __attribute__((packed)); // Use packed attribute to avoid padding issues if necessary, though maybe not needed here.

    // Define buffer size based on multi-channel samples
    // Example calculation: Target ~1.5kB buffer. If oscMultiChannelSample is ~12 bytes:
    // 1500 / 12 = 125 samples. Let's use 128 for power of 2.
    #define MAX_OSC_SAMPLES_PER_PACKET 128

    struct oscSamples {                               // buffer with samples
        union {
            // Keep old structures for reference/compatibility if needed, but comment out/remove if unused
            // oscI2sSample        samplesI2sSignal  [OSCILLOSCOPE_I2S_BUFFER_SIZE];
            // osc1SignalSample    samples1Signal    [OSCILLOSCOPE_1SIGNAL_BUFFER_SIZE];
            // osc2SignalsSample   samples2Signals   [OSCILLOSCOPE_2SIGNALS_BUFFER_SIZE];
            oscMultiChannelSample samplesMultiChannel [MAX_OSC_SAMPLES_PER_PACKET]; // New buffer for multi-channel data
        };
        unsigned int sampleCount;                      // number of samples (oscMultiChannelSample structs) in the buffer
        bool samplesAreReady;                          // is the buffer ready for sending
    };

    enum readerState { INITIAL = 0, START = 1, STARTED = 2, STOP = 3, STOPPED = 4 };
    /* transitions:
          START   - set by osc main thread
          STARTED - set by oscReader
          STOP    - set by osc main thread
          STOPPED - set by oscReader
    */

    #define MAX_OSC_CHANNELS 4 // Define maximum number of channels

    // Structure to hold configuration for a single channel
    struct OscChannelConfig {
        gpio_num_t gpio;
        adc1_channel_t adc_channel;
        bool is_analog;
        bool is_active;
        // Add sensitivity, position, trigger settings per channel later
        int sensitivity; // Example: Placeholder for vertical sensitivity (0-5 like original)
        int position;    // Example: Placeholder for vertical position
        bool posTriggerEnabled; // Example: Placeholder
        int posTriggerTreshold; // Example: Placeholder
        bool negTriggerEnabled; // Example: Placeholder
        int negTriggerTreshold; // Example: Placeholder
    };

    struct oscSharedMemory {         // data structure to be shared among oscilloscope tasks
      // basic data for web oscilloscope
      WebSocket *webSocket;                   // open webSocket for communication with javascript client
      bool clientIsBigEndian;                 // true if javascript client is big endian machine
      // basic data for PulseView
      int noOfSamples;
      // sampling sharedMemory
      OscChannelConfig channelConfig[MAX_OSC_CHANNELS]; // Array to hold configs for multiple channels
      uint8_t numActiveChannels;              // Number of currently active channels
      // char readType [8];                      // analog or digital  -> Moved into OscChannelConfig
      // bool analog;                            // true if readType is analog, false if digital (digitalRead) -> Moved into OscChannelConfig
      // gpio_num_t gpio1;                       // gpio where ESP32 is taking samples from (digitalRead) -> Replaced by channelConfig array
      // gpio_num_t gpio2;                       // 2nd gpio if requested -> Replaced by channelConfig array
      // adc1_channel_t adcchannel1;             // channel mapped from gpio ESP32 is taking samples from (adc1_get_raw instead of analogRead) -> Replaced by channelConfig array
      // adc1_channel_t adcchannel2;             // channel mapped from gpio ESP32 is taking samples from (adc1_get_raw instead of analogRead) -> Replaced by channelConfig array
      int samplingTime;                       // time between samples in ms or us - Applies to all channels for now?
      char samplingTimeUnit [3];              // ms or us
      unsigned long screenWidthTime;          // oscilloscope screen width in ms or us
      char screenWidthTimeUnit [3];           // ms or us 
      // bool positiveTrigger;                   // true if posotive slope trigger is set -> Moved into OscChannelConfig?
      // int positiveTriggerTreshold;            // positive slope trigger treshold value -> Moved into OscChannelConfig?
      // bool negativeTrigger;                   // true if negative slope trigger is set   -> Moved into OscChannelConfig?
      // int negativeTriggerTreshold;            // negative slope trigger treshold value -> Moved into OscChannelConfig?
      // buffers holding samples 
      oscSamples readBuffer;                  // we'll read samples into this buffer - Structure might need changing later
      oscSamples sendBuffer;                  // we'll copy red buffer into this buffer before sending samples to the client - Structure might need changing later
      // reader state
      readerState oscReaderState;             // helps to execute a proper stopping sequence
    };

    // oscilloscope reader read samples to read-buffer of shared memory - it will be copied to send buffer when it is ready to be sent


    // oscReaders oscReaders oscReaders oscReaders oscReaders oscReaders oscReaders oscReaders oscReaders oscReaders oscReaders oscReaders oscReaders 


    // The most general purpose and slow oscReader
    //  - it can read 1 or 2 digital signals
    //  - it can read 1 or 2 analog signals
    //  - it can work in 'sample at a time' more or 'screen at a time' mode
    // The only drawback is that samplingTime and screenWidthTime must be specified in milliseconds
    void oscReader_millis (void *sharedMemory) {
        bool doAnalogRead =                 !strcmp (((oscSharedMemory *) sharedMemory)->readType, "analog");
        // *not needed* bool unitIsMicroSeconds =           !strcmp (((oscSharedMemory *) sharedMemory)->samplingTimeUnit, "us");
        int samplingTime =                  ((oscSharedMemory *) sharedMemory)->samplingTime;
        bool positiveTrigger =              ((oscSharedMemory *) sharedMemory)->positiveTrigger;
        bool negativeTrigger =              ((oscSharedMemory *) sharedMemory)->negativeTrigger;
        gpio_num_t gpio1 =                  (gpio_num_t) ((oscSharedMemory *) sharedMemory)->gpio1; // easier to check validity with unsigned char then with integer 
        gpio_num_t gpio2 =                  (gpio_num_t) ((oscSharedMemory *) sharedMemory)->gpio2; // easier to check validity with unsigned char then with integer
        unsigned char noOfSignals = 1; if (gpio2 <= 39) noOfSignals = 2;  // monitor 1 or 2 signals
        adc1_channel_t adcchannel1 =        ((oscSharedMemory *) sharedMemory)->adcchannel1;
        adc1_channel_t adcchannel2 =        ((oscSharedMemory *) sharedMemory)->adcchannel2;
        int positiveTriggerTreshold =       ((oscSharedMemory *) sharedMemory)->positiveTriggerTreshold;
        int negativeTriggerTreshold =       ((oscSharedMemory *) sharedMemory)->negativeTriggerTreshold;
        unsigned long screenWidthTime =     ((oscSharedMemory *) sharedMemory)->screenWidthTime; 
        oscSamples *readBuffer =            &((oscSharedMemory *) sharedMemory)->readBuffer;
        oscSamples *sendBuffer =            &((oscSharedMemory *) sharedMemory)->sendBuffer;

        // Is samplingTime large enough to fill the whole screen? If not, make a correction.
        if (noOfSignals == 1) {
            if ((unsigned long) samplingTime * (OSCILLOSCOPE_1SIGNAL_BUFFER_SIZE - 2) < screenWidthTime) {
                samplingTime = max ((int) (screenWidthTime / (OSCILLOSCOPE_1SIGNAL_BUFFER_SIZE - 2)) + 1, 1); // + 1 just to be on the safe side due to integer calculation rounding
                __oscilloscope_h_debug__ ("oscReader_millis: 1 signal samplingTime was too short (regarding to buffer size) and is corrected to " + String (samplingTime));
            }
        } else {
            if ((unsigned long) samplingTime * (OSCILLOSCOPE_2SIGNALS_BUFFER_SIZE - 2) < screenWidthTime) {
                samplingTime = max ((int) (screenWidthTime / (OSCILLOSCOPE_2SIGNALS_BUFFER_SIZE - 2)) + 1, 1); // + 1 just to be on the safe side due to integer calculation rounding
                __oscilloscope_h_debug__ ("oscReader_millis: 2 signals samplingTime was too short (regarding to buffer size) and is corrected to " + String (samplingTime));
            }
        }
        // Is samplingTime is too long for 15 bits, make a correction. Max sample time can be 32767 (15 bits) but since in some case actual sample time can be much larger than required le's keep it below 5000.
        if (samplingTime > 5000) {
                samplingTime = 5000;
                __oscilloscope_h_debug__ ("oscReader_millis: samplingTime was too long (to fit in 15 bits in (almost?) all cases) and is corrected to " + String (samplingTime));
        }        

        /**/
        cout << "----- oscReader_millis () -----\r\n";
        cout << "samplingTime " << samplingTime << " ms" << endl;
        cout << "positiveTrigger " << positiveTrigger << endl;
        cout << "negativeTrigger " << negativeTrigger << endl;
        cout << "gpio1 " << gpio1 << endl;
        cout << "gpio2 " << gpio2 << endl;
        cout << "noOfSignals " << noOfSignals << endl;
        cout << "positiveTriggerTreshold " << positiveTriggerTreshold << endl;
        cout << "negativeTriggerTreshold " << negativeTriggerTreshold << endl;
        cout << "screenWidthTime " << screenWidthTime << endl;
        delay (100);
        /**/

        // Calculate screen refresh period. It sholud be arround 50 ms (sustainable screen refresh rate is arround 20 Hz) but it is better if it is a multiple value of screenWidthTime.
        unsigned long screenRefreshMilliseconds; // screen refresh period
        int noOfSamplesPerScreen = screenWidthTime / samplingTime; if (noOfSamplesPerScreen * samplingTime < screenWidthTime) noOfSamplesPerScreen ++;
        unsigned long correctedScreenWidthTime = noOfSamplesPerScreen * samplingTime;                         
        screenRefreshMilliseconds = correctedScreenWidthTime >= 50000 ? correctedScreenWidthTime / 1000 : ((50500 / correctedScreenWidthTime) * correctedScreenWidthTime) / 1000;
        __oscilloscope_h_debug__ ("oscReader_millis: samplingTime = " + String (samplingTime) + ", screenWidthTime = " + String (screenWidthTime));

        // determine mode of operation sample at a time or screen at a time - this only makes sense when screenWidthTime is measured in ms
        bool oneSampleAtATime = screenWidthTime > 1000;

        readBuffer->samplesAreReady = true; // this information will be always copied to sendBuffer together with the samples

        // enable GPIO reading even if it is not configured so
        if (!doAnalogRead) {
            if (gpio1 <= 39) gpio_hal_input_enable (&__gpio_hal__, gpio1);
            if (gpio2 <= 39) gpio_hal_input_enable (&__gpio_hal__, gpio2);
        }

        // wait for the START signal
        while (((oscSharedMemory *) sharedMemory)->oscReaderState != START) delay (1);
        ((oscSharedMemory *) sharedMemory)->oscReaderState = STARTED; 

        // --- do the sampling, samplingTime and screenWidthTime are in us ---

        // triggered or untriggered mode of operation
        bool triggeredMode = positiveTrigger || negativeTrigger;

        TickType_t lastScreenRefreshTicks = xTaskGetTickCount ();               // for timing screen refresh intervals            

        while (((oscSharedMemory *) sharedMemory)->oscReaderState == STARTED) { // sampling from the left of the screen - while not getting STOP signal

            int screenTime = 0;                                                 // in ms - how far we have already got from the left of the screen (we'll compare this value with screenWidthTime)
            unsigned long deltaTime = 0;                                        // in ms - delta from previous sample
            TickType_t lastSampleTicks = xTaskGetTickCount ();                  // for sample timing                
            TickType_t newSampleTicks = lastSampleTicks;
            // Insert first dummy sample to read-buffer this tells javascript client to start drawing from the left of the screen. Please note that it also tells javascript client how many signals are in each sample
            if (noOfSignals == 1) readBuffer->samples1Signal [0] = {-2, -2}; // no real data sample can look like this
            else                  readBuffer->samples2Signals [0] = {-3, -3, -3}; // no real data sample can look like this
            readBuffer->sampleCount = 1;

            if (triggeredMode) { // if no trigger is set then skip this (waiting) part and start sampling immediatelly

                // take the first sample
                union {
                    osc1SignalSample last1SignalSample;
                    osc2SignalsSample last2SignalsSample;
                };

                #ifdef INVERT_ADC1_GET_RAW
                    if (noOfSignals == 1) { if (doAnalogRead) last1SignalSample = {(int16_t) (~adc1_get_raw (adcchannel1) & 0xFFF), (int16_t) 0}; else last2SignalsSample = {(int16_t) gpio_hal_get_level (&__gpio_hal__, gpio1), (int16_t) 0}; } // gpio1 should always be valid PIN
                    else                  { if (doAnalogRead) last2SignalsSample = {(int16_t) (~adc1_get_raw (adcchannel1) & 0xFFF), (int16_t) (~adc1_get_raw (adcchannel2) & 0xFFF), (int16_t) 0}; else last2SignalsSample = {(int16_t) gpio_hal_get_level (&__gpio_hal__, gpio1), (int16_t) gpio_hal_get_level (&__gpio_hal__, gpio2), (int16_t) 0}; } // gpio1 should always be valid PIN
                #else
                    if (noOfSignals == 1) { if (doAnalogRead) last1SignalSample = {(int16_t) (adc1_get_raw (adcchannel1) & 0xFFF), (int16_t) 0}; else last2SignalsSample = {(int16_t) gpio_hal_get_level (&__gpio_hal__, gpio1), (int16_t) 0}; } // gpio1 should always be valid PIN
                    else                  { if (doAnalogRead) last2SignalsSample = {(int16_t) (adc1_get_raw (adcchannel1) & 0xFFF), (int16_t) (adc1_get_raw (adcchannel2) & 0xFFF), (int16_t) 0}; else last2SignalsSample = {(int16_t) gpio_hal_get_level (&__gpio_hal__, gpio1), (int16_t) gpio_hal_get_level (&__gpio_hal__, gpio2), (int16_t) 0}; } // gpio1 should always be valid PIN
                #endif

                // wait for trigger condition
                while (((oscSharedMemory *) sharedMemory)->oscReaderState == STARTED) { 
                    // wait befor continuing to next sample and calculate delta offset for it
                    vTaskDelayUntil (&newSampleTicks, pdMS_TO_TICKS (samplingTime));
                    deltaTime = pdTICKS_TO_MS (newSampleTicks - lastSampleTicks); // in ms - this value will be used for the next sample offset
                    lastSampleTicks = newSampleTicks;

                    // take the second sample
                    union {
                        osc1SignalSample new1SignalSample;
                        osc2SignalsSample new2SignalsSample;
                    };

                    #ifdef INVERT_ADC1_GET_RAW
                        if (noOfSignals == 1) { if (doAnalogRead) new1SignalSample = {(int16_t) (~adc1_get_raw (adcchannel1) & 0xFFF), (int16_t) 0}; else last2SignalsSample = {(int16_t) gpio_hal_get_level (&__gpio_hal__, gpio1), (int16_t) 0}; } // gpio1 should always be valid PIN
                        else                  { if (doAnalogRead) new2SignalsSample = {(int16_t) (~adc1_get_raw (adcchannel1) & 0xFFF), (int16_t) ~(adc1_get_raw (adcchannel2) & 0xFFF), (int16_t) 0}; else last2SignalsSample = {(int16_t) gpio_hal_get_level (&__gpio_hal__, gpio1), (int16_t) gpio_hal_get_level (&__gpio_hal__, gpio2), (int16_t) 0}; } // gpio1 should always be valid PIN
                    #else
                        if (noOfSignals == 1) { if (doAnalogRead) new1SignalSample = {(int16_t) (adc1_get_raw (adcchannel1) & 0xFFF), (int16_t) 0}; else last2SignalsSample = {(int16_t) gpio_hal_get_level (&__gpio_hal__, gpio1), (int16_t) 0}; } // gpio1 should always be valid PIN
                        else                  { if (doAnalogRead) new2SignalsSample = {(int16_t) (adc1_get_raw (adcchannel1) & 0xFFF), (int16_t) (adc1_get_raw (adcchannel2) & 0xFFF), (int16_t) 0}; else last2SignalsSample = {(int16_t) gpio_hal_get_level (&__gpio_hal__, gpio1), (int16_t) gpio_hal_get_level (&__gpio_hal__, gpio2), (int16_t) 0}; } // gpio1 should always be valid PIN
                    #endif

                    // Compare both samples to check if the trigger condition has occured, only gpio1 is used to trigger the sampling. Please note that it doesn't matter wether we compare last1SignalSample or last2SignalsSample since they share the same space and signal1 is always on the same place.
                    if ((positiveTrigger && last2SignalsSample.signal1 < positiveTriggerTreshold && new2SignalsSample.signal1 >= positiveTriggerTreshold) || (negativeTrigger && last2SignalsSample.signal1 > negativeTriggerTreshold && new2SignalsSample.signal1 <= negativeTriggerTreshold)) { 
                        // trigger condition has occured, insert both samples into read buffer
                        if (noOfSignals == 1) { readBuffer->samples1Signal [1] = last1SignalSample; readBuffer->samples1Signal [2] = new1SignalSample; } // timeOffset (from left of the screen) = 0, this is the first sample after triggered
                        else                  { readBuffer->samples2Signals [1] = last2SignalsSample; readBuffer->samples2Signals [2] = new2SignalsSample; } // timeOffset (from left of the screen) = 0, this is the first sample after triggered
                        screenTime = deltaTime;     // start measuring screen time from new sample on
                        readBuffer->sampleCount = 3;
                
                        // correct screenTime
                        screenTime = deltaTime;

                        // wait befor continuing to next sample and calculate delta offset for it
                        vTaskDelayUntil (&newSampleTicks, pdMS_TO_TICKS (samplingTime));
                        deltaTime = pdTICKS_TO_MS (newSampleTicks - lastSampleTicks); // in ms - this value will be used for the next sample offset
                        lastSampleTicks = newSampleTicks;
                            
                        break; // trigger event occured, stop waiting and proceed to sampling
                    } else {
                        // Just forget the first sample and continue waiting for trigger condition - copy just signal values and let the timing start from 0. Please note that it doesn't matter wether we move new1SignalSample or new2SignalsSample since they share the same space.
                        last2SignalsSample.signal1 = new2SignalsSample.signal1;
                        last2SignalsSample.signal2 = new2SignalsSample.signal2;
                    }
                } // while not triggered
            } // if in trigger mode

            // take (the rest of the) samples that fit on one screen
            while (((oscSharedMemory *) sharedMemory)->oscReaderState == STARTED) { // while screenTime < screenWidthTime

                // if we already passed screenWidthMilliseconds then copy read buffer to send buffer so it can be sent to the javascript client
                if (screenTime >= screenWidthTime || (noOfSignals == 1 && readBuffer->sampleCount >= OSCILLOSCOPE_1SIGNAL_BUFFER_SIZE) || (noOfSignals == 2 && readBuffer->sampleCount >= OSCILLOSCOPE_2SIGNALS_BUFFER_SIZE)) { 
                    // copy read buffer to send buffer so that oscilloscope sender can send it to javascript client 

                    while (oneSampleAtATime && sendBuffer->samplesAreReady) vTaskDelay (pdMS_TO_TICKS (1)); // in oneSampleAtATime mode wait until previous frame is sent
                    if (!sendBuffer->samplesAreReady) 
                        *sendBuffer = *readBuffer; // this also copies 'ready' flag from read buffer which is 'true' - tell oscSender to send the packet, this would refresh client screen
                    // else send buffer with previous frame is still waiting to be sent, do nothing now, skip this frame

                    // break out of the loop and than start taking new samples
                    break; // get out of while loop to start sampling from the left of the screen again
                }

                // one sample at a time mode requires sending (copying) the readBuffer to the sendBuffer so it can be sent to the javascript client even before it gets full (of samples that fit to one screen)
                if (oneSampleAtATime && readBuffer->sampleCount) {
                    // copy read buffer to send buffer so that oscilloscope sender can send it to javascript client 
                    if (!sendBuffer->samplesAreReady) {
                        *sendBuffer = *readBuffer; // this also copies 'ready' flag from read buffer which is 'true' - tell oscSender to send the packet, this would refresh client screen
                        readBuffer->sampleCount = 0; // empty read buffer so we don't send the same data again later
                    }
                    // else send buffer with previous frame is still waiting to be sent, but the buffer is not full yet, so just continue sampling into the same frame
                }
    
                // take the next sample
                union {
                    osc1SignalSample new1SignalSample;
                    osc2SignalsSample new2SignalsSample;
                };

                #ifdef INVERT_ADC1_GET_RAW
                    if (noOfSignals == 1) { if (doAnalogRead) new1SignalSample = {(int16_t) (~adc1_get_raw (adcchannel1) & 0xFFF), (int16_t) deltaTime}; else new1SignalSample = {(int16_t) gpio_hal_get_level (&__gpio_hal__, gpio1), (int16_t) deltaTime}; 
                                            readBuffer->samples1Signal [readBuffer->sampleCount ++] = new1SignalSample;
                    } else                { if (doAnalogRead) new2SignalsSample = {(int16_t) (~adc1_get_raw (adcchannel1) & 0xFFF), (int16_t) (~adc1_get_raw (adcchannel2) & 0xFFF), (int16_t) deltaTime}; else new2SignalsSample = {(int16_t) gpio_hal_get_level (&__gpio_hal__, gpio1), (int16_t) gpio_hal_get_level (&__gpio_hal__, gpio2), (int16_t) deltaTime}; 
                                            readBuffer->samples2Signals [readBuffer->sampleCount ++] = new2SignalsSample;
                    }
                #else
                    if (noOfSignals == 1) { if (doAnalogRead) new1SignalSample = {(int16_t) (adc1_get_raw (adcchannel1) & 0xFFF), (int16_t) deltaTime}; else new1SignalSample = {(int16_t) gpio_hal_get_level (&__gpio_hal__, gpio1), (int16_t) deltaTime}; 
                                            readBuffer->samples1Signal [readBuffer->sampleCount ++] = new1SignalSample;
                    } else                { if (doAnalogRead) new2SignalsSample = {(int16_t) (adc1_get_raw (adcchannel1) & 0xFFF), (int16_t) (adc1_get_raw (adcchannel2) & 0xFFF), (int16_t) deltaTime}; else new2SignalsSample = {(int16_t) gpio_hal_get_level (&__gpio_hal__, gpio1), (int16_t) gpio_hal_get_level (&__gpio_hal__, gpio2), (int16_t) deltaTime}; 
                                            readBuffer->samples2Signals [readBuffer->sampleCount ++] = new2SignalsSample;
                    }
                #endif

                screenTime += deltaTime;

                // wait befor continuing to next sample and calculate delta offset for it
                vTaskDelayUntil (&newSampleTicks, pdMS_TO_TICKS (samplingTime));
                deltaTime = pdTICKS_TO_MS (newSampleTicks - lastSampleTicks); // in ms - this value will be used for the next sample offset

                lastSampleTicks = newSampleTicks;
            } // while screenTime < screenWidthTime

            // wait before next screen refresh
            vTaskDelayUntil (&lastScreenRefreshTicks, pdMS_TO_TICKS (screenRefreshMilliseconds));

        } // while sampling

        // wait for the STOP signal
        while (((oscSharedMemory *) sharedMemory)->oscReaderState != STOP) delay (1);
        ((oscSharedMemory *) sharedMemory)->oscReaderState = STOPPED; 

        vTaskDelete (NULL);
    }


    // oscReader that takes digital samples (digitalRead) on time interval specified in microseconds
    //  - it can only read 1 or 2 digital signals
    //  - it can only work in 'screen at a time' mode (which is not a drawback for the sampling is measured in microseconds)
    void oscReader_digital (void *sharedMemory) {
        // *not needed* bool doAnalogRead =                 !strcmp (((oscSharedMemory *) sharedMemory)->readType, "analog");
        // *not needed* bool unitIsMicroSeconds =           !strcmp (((oscSharedMemory *) sharedMemory)->samplingTimeUnit, "us");
        int samplingTime =                  ((oscSharedMemory *) sharedMemory)->samplingTime;
        bool positiveTrigger =              ((oscSharedMemory *) sharedMemory)->positiveTrigger;
        bool negativeTrigger =              ((oscSharedMemory *) sharedMemory)->negativeTrigger;
        gpio_num_t gpio1 =                  (gpio_num_t) ((oscSharedMemory *) sharedMemory)->gpio1; // easier to check validity with unsigned char then with integer 
        gpio_num_t gpio2 =                  (gpio_num_t) ((oscSharedMemory *) sharedMemory)->gpio2; // easier to check validity with unsigned char then with integer
        unsigned char noOfSignals = 1; if (gpio2 <= 39) noOfSignals = 2;  // monitor 1 or 2 signals
        // *not needed* adc1_channel_t adcchannel1 =        ((oscSharedMemory *) sharedMemory)->adcchannel1;
        // *not needed* adc1_channel_t adcchannel2 =        ((oscSharedMemory *) sharedMemory)->adcchannel2;
        int positiveTriggerTreshold =       ((oscSharedMemory *) sharedMemory)->positiveTriggerTreshold;
        int negativeTriggerTreshold =       ((oscSharedMemory *) sharedMemory)->negativeTriggerTreshold;
        unsigned long screenWidthTime =     ((oscSharedMemory *) sharedMemory)->screenWidthTime; 
        oscSamples *readBuffer =            &((oscSharedMemory *) sharedMemory)->readBuffer;
        oscSamples *sendBuffer =            &((oscSharedMemory *) sharedMemory)->sendBuffer;

        // Is samplingTime large enough to fill the whole screen? If not, make a correction.
        if (noOfSignals == 1) {
            if ((unsigned long) samplingTime * (OSCILLOSCOPE_1SIGNAL_BUFFER_SIZE - 2) < screenWidthTime) {
                samplingTime = max ((int) (screenWidthTime / (OSCILLOSCOPE_1SIGNAL_BUFFER_SIZE - 2)) + 1, 1); // + 1 just to be on the safe side due to integer calculation rounding
                __oscilloscope_h_debug__ ("oscReader_digital: 1 signal samplingTime was too short (regarding to buffer size) and is corrected to " + String (samplingTime));
            }
        } else {
            if ((unsigned long) samplingTime * (OSCILLOSCOPE_2SIGNALS_BUFFER_SIZE - 2) < screenWidthTime) {
                samplingTime = max ((int) (screenWidthTime / (OSCILLOSCOPE_2SIGNALS_BUFFER_SIZE - 2)) + 1, 1); // + 1 just to be on the safe side due to integer calculation rounding
                __oscilloscope_h_debug__ ("oscReader_digital: 2 signals samplingTime was too short (regarding to buffer size) and is corrected to " + String (samplingTime));
            }
        }
        // Is samplingTime is too long for 15 bits, make a correction. Max sample time can be 32767 (15 bits) but since in some case actual sample time can be much larger than required le's keep it below 5000.
        if (samplingTime > 5000) {
                samplingTime = 5000;
                __oscilloscope_h_debug__ ("oscReader_digital: samplingTime was too long (to fit in 15 bits in (almost?) all cases) and is corrected to " + String (samplingTime));
        }  

        /**/
        cout << "----- oscReader_digital () -----\r\n";
        cout << "samplingTime " << samplingTime << " us" << endl;
        cout << "positiveTrigger " << positiveTrigger << endl;
        cout << "negativeTrigger " << negativeTrigger << endl;
        cout << "gpio1 " << gpio1 << endl;
        cout << "gpio2 " << gpio2 << endl;
        cout << "noOfSignals " << noOfSignals << endl;
        cout << "positiveTriggerTreshold " << positiveTriggerTreshold << endl;
        cout << "negativeTriggerTreshold " << negativeTriggerTreshold << endl;
        cout << "screenWidthTime " << screenWidthTime << endl;
        if (noOfSignals == 1) {
            cout << "max number of sampels " << OSCILLOSCOPE_1SIGNAL_BUFFER_SIZE << endl;
            cout << "max possible screenWidthTime covered by sampels " << samplingTime * OSCILLOSCOPE_1SIGNAL_BUFFER_SIZE << endl;
        } else {
            cout << "max number of sampels " << OSCILLOSCOPE_2SIGNALS_BUFFER_SIZE << endl;
            cout << "max possible screenWidthTime covered by sampels " << samplingTime * OSCILLOSCOPE_2SIGNALS_BUFFER_SIZE << endl;
        }
        delay (100);
        /**/

        // Calculate screen refresh period. It sholud be arround 50 ms (sustainable screen refresh rate is arround 20 Hz) but it is better if it is a multiple value of screenWidthTime.
        unsigned long screenRefreshMilliseconds; // screen refresh period
        int noOfSamplesPerScreen = screenWidthTime / samplingTime; if (noOfSamplesPerScreen * samplingTime < screenWidthTime) noOfSamplesPerScreen ++;
        unsigned long correctedScreenWidthTime = noOfSamplesPerScreen * samplingTime;                         
        screenRefreshMilliseconds = correctedScreenWidthTime >= 50000 ? correctedScreenWidthTime / 1000 : ((50500 / correctedScreenWidthTime) * correctedScreenWidthTime) / 1000;
        __oscilloscope_h_debug__ ("oscReader_digital: samplingTime = " + String (samplingTime) + ", screenWidthTime = " + String (screenWidthTime));

        readBuffer->samplesAreReady = true; // this information will be always copied to sendBuffer together with the samples

        // enable GPIO reading even if it is not configured so
        if (gpio1 <= 39) gpio_hal_input_enable (&__gpio_hal__, gpio1);
        if (gpio2 <= 39) gpio_hal_input_enable (&__gpio_hal__, gpio2);

        // wait for the START signal
        while (((oscSharedMemory *) sharedMemory)->oscReaderState != START) delay (1);
        ((oscSharedMemory *) sharedMemory)->oscReaderState = STARTED; 

        // --- do the sampling, samplingTime and screenWidthTime are in us ---

        // triggered or untriggered mode of operation
        bool triggeredMode = positiveTrigger || negativeTrigger;

        TickType_t lastScreenRefreshTicks = xTaskGetTickCount ();               // for timing screen refresh intervals            

        while (((oscSharedMemory *) sharedMemory)->oscReaderState == STARTED) { // sampling from the left of the screen - while not getting STOP signal

            unsigned long screenTime = 0;                                       // in us - how far we have already got from the left of the screen (we'll compare this value with screenWidthTime)
            unsigned long deltaTime = 0;                                        // in us - delta from previous sample
            unsigned long lastSampleMicroseconds = micros ();                   // for sample timing                
            unsigned long newSampleMicroseconds = lastSampleMicroseconds;

            // Insert first dummy sample to read-buffer this tells javascript client to start drawing from the left of the screen. Please note that it also tells javascript client how many signals are in each sample
            if (noOfSignals == 1) readBuffer->samples1Signal [0] = {-2, -2}; // no real data sample can look like this
            else                  readBuffer->samples2Signals [0] = {-3, -3, -3}; // no real data sample can look like this
            readBuffer->sampleCount = 1;

            if (triggeredMode) { // if no trigger is set then skip this (waiting) part and start sampling immediatelly

                // take the first sample
                union {
                    osc1SignalSample last1SignalSample;
                    osc2SignalsSample last2SignalsSample;
                };

                if (noOfSignals == 1) { last2SignalsSample = {(int16_t) gpio_hal_get_level (&__gpio_hal__, gpio1), (int16_t) 0}; } // gpio1 should always be valid PIN
                else                  { last2SignalsSample = {(int16_t) gpio_hal_get_level (&__gpio_hal__, gpio1), (int16_t) gpio_hal_get_level (&__gpio_hal__, gpio2), (int16_t) 0}; } // gpio1 should always be valid PIN

                // wait for trigger condition
                while (((oscSharedMemory *) sharedMemory)->oscReaderState == STARTED) { 
                    // wait befor continuing to next sample and calculate delta offset for it
                    while ((deltaTime = (newSampleMicroseconds = micros ()) - lastSampleMicroseconds) < samplingTime) delayMicroseconds (1);
                    lastSampleMicroseconds = newSampleMicroseconds;

                    // take the second sample
                    union {
                        osc1SignalSample new1SignalSample;
                        osc2SignalsSample new2SignalsSample;
                    };
                    if (noOfSignals == 1) { new2SignalsSample = {(int16_t) gpio_hal_get_level (&__gpio_hal__, gpio1), (int16_t) 0}; } // gpio1 should always be valid PIN
                    else                  { new2SignalsSample = {(int16_t) gpio_hal_get_level (&__gpio_hal__, gpio1), (int16_t) gpio_hal_get_level (&__gpio_hal__, gpio2), (int16_t) 0}; } // gpio1 should always be valid PIN

                    // Compare both samples to check if the trigger condition has occured, only gpio1 is used to trigger the sampling. Please note that it doesn't matter wether we compare last1SignalSample or last2SignalsSample since they share the same space and signal1 is always on the same place.
                    if ((positiveTrigger && last2SignalsSample.signal1 < positiveTriggerTreshold && new2SignalsSample.signal1 >= positiveTriggerTreshold) || (negativeTrigger && last2SignalsSample.signal1 > negativeTriggerTreshold && new2SignalsSample.signal1 <= negativeTriggerTreshold)) { 
                        // trigger condition has occured, insert both samples into read buffer
                        if (noOfSignals == 1) { readBuffer->samples1Signal [1] = last1SignalSample; readBuffer->samples1Signal [2] = new1SignalSample; } // timeOffset (from left of the screen) = 0, this is the first sample after triggered
                        else                  { readBuffer->samples2Signals [1] = last2SignalsSample; readBuffer->samples2Signals [2] = new2SignalsSample; } // timeOffset (from left of the screen) = 0, this is the first sample after triggered
                        screenTime = deltaTime;     // start measuring screen time from new sample on
                        readBuffer->sampleCount = 3;
                
                        // correct screenTime
                        screenTime = deltaTime;

                        // wait befor continuing to next sample and calculate delta offset for it
                        while ((deltaTime = (newSampleMicroseconds = micros ()) - lastSampleMicroseconds) < samplingTime) delayMicroseconds (1);
                        lastSampleMicroseconds = newSampleMicroseconds;
                            
                        break; // trigger event occured, stop waiting and proceed to sampling
                    } else {
                        // Just forget the first sample and continue waiting for trigger condition - copy just signal values and let the timing start from 0. Please note that it doesn't matter wether we move new1SignalSample or new2SignalsSample since they share the same space.
                        last2SignalsSample.signal1 = new2SignalsSample.signal1;
                        last2SignalsSample.signal2 = new2SignalsSample.signal2;
                    }
                } // while not triggered
            } // if in trigger mode

            // take (the rest of the) samples that fit on one screen
            while (((oscSharedMemory *) sharedMemory)->oscReaderState == STARTED) { // while screenTime < screenWidthTime

                // if we already passed screenWidthMilliseconds then copy read buffer to send buffer so it can be sent to the javascript client
                if (screenTime >= screenWidthTime || (noOfSignals == 1 && readBuffer->sampleCount >= OSCILLOSCOPE_1SIGNAL_BUFFER_SIZE) || (noOfSignals == 2 && readBuffer->sampleCount >= OSCILLOSCOPE_2SIGNALS_BUFFER_SIZE)) { 
                    // copy read buffer to send buffer so that oscilloscope sender can send it to javascript client 

                    if (!sendBuffer->samplesAreReady) 
                        *sendBuffer = *readBuffer; // this also copies 'ready' flag from read buffer which is 'true' - tell oscSender to send the packet, this would refresh client screen
                    // else send buffer with previous frame is still waiting to be sent, do nothing now, skip this frame

                    // break out of the loop and than start taking new samples
                    break; // get out of while loop to start sampling from the left of the screen again
                }
    
                // take the next sample
                union {
                    osc1SignalSample new1SignalSample;
                    osc2SignalsSample new2SignalsSample;
                };

                if (noOfSignals == 1) { new1SignalSample = {(int16_t) gpio_hal_get_level (&__gpio_hal__, gpio1), (int16_t) deltaTime}; 
                                        readBuffer->samples1Signal [readBuffer->sampleCount ++] = new1SignalSample;
                } else                { new2SignalsSample = {(int16_t) gpio_hal_get_level (&__gpio_hal__, gpio1), (int16_t) gpio_hal_get_level (&__gpio_hal__, gpio2), (int16_t) deltaTime}; 
                                        readBuffer->samples2Signals [readBuffer->sampleCount ++] = new2SignalsSample;
                }

                screenTime += deltaTime;

                // wait befor continuing to next sample and calculate delta offset for it
                while ((deltaTime = (newSampleMicroseconds = micros ()) - lastSampleMicroseconds) < samplingTime) delayMicroseconds (1);
                lastSampleMicroseconds = newSampleMicroseconds;

            } // while screenTime < screenWidthTime

            // wait before next screen refresh
            vTaskDelayUntil (&lastScreenRefreshTicks, pdMS_TO_TICKS (screenRefreshMilliseconds));

        } // while sampling

        // wait for the STOP signal
        while (((oscSharedMemory *) sharedMemory)->oscReaderState != STOP) delay (1);
        ((oscSharedMemory *) sharedMemory)->oscReaderState = STOPPED; 

        vTaskDelete (NULL);
    }

    // oscReader that takes analog samples (analogRead) on time interval specified in microseconds
    //  - it can only read 1 or 2 digital signals
    //  - it can only work in 'screen at a time' mode (which is not a drawback for the sampling is measured in microseconds)
    void oscReader_analog (void *sharedMemoryPtr) { // Changed parameter name for clarity
        oscSharedMemory *sharedMemory = (oscSharedMemory *) sharedMemoryPtr; // Get typed pointer

        // Extract configuration from shared memory
        OscChannelConfig *channelConfig = sharedMemory->channelConfig;
        uint8_t numActiveChannels = sharedMemory->numActiveChannels;
        int samplingTime = sharedMemory->samplingTime;
        unsigned long screenWidthTime = sharedMemory->screenWidthTime;
        oscSamples *readBuffer = &sharedMemory->readBuffer;
        oscSamples *sendBuffer = &sharedMemory->sendBuffer;

        // Identify the trigger channel (first active analog channel for now)
        int triggerChannelIndex = -1;
        adc1_channel_t triggerAdcChannel = ADC1_CHANNEL_MAX; // Default invalid
        bool positiveTrigger = false;
        bool negativeTrigger = false;
        int positiveTriggerTreshold = 0;
        int negativeTriggerTreshold = 0;

        uint8_t numActiveAnalog = 0;
        for (int i = 0; i < MAX_OSC_CHANNELS; ++i) {
            if (channelConfig[i].is_active && channelConfig[i].is_analog) {
                numActiveAnalog++;
                if (triggerChannelIndex == -1) { // Use first active analog channel for trigger
                    triggerChannelIndex = i;
                    triggerAdcChannel = channelConfig[i].adc_channel;
                    positiveTrigger = channelConfig[i].posTriggerEnabled;
                    negativeTrigger = channelConfig[i].negTriggerEnabled;
                    positiveTriggerTreshold = channelConfig[i].posTriggerTreshold;
                    negativeTriggerTreshold = channelConfig[i].negTriggerTreshold;
                }
            }
        }

        // Check if any analog channels are active
        if (numActiveAnalog == 0) {
             __oscilloscope_h_debug__("oscReader_analog: No active analog channels. Stopping.");
             sharedMemory->oscReaderState = STOPPED; // Ensure it stops cleanly
             vTaskDelete(NULL);
             return; // Should not happen if started correctly, but safety check
        }


        // --- REMOVE OLD VARIABLE EXTRACTION ---
        // bool positiveTrigger =              ((oscSharedMemory *) sharedMemory)->positiveTrigger;
        // bool negativeTrigger =              ((oscSharedMemory *) sharedMemory)->negativeTrigger;
        // unsigned char gpio1 =               (unsigned char) ((oscSharedMemory *) sharedMemory)->gpio1; // easier to check validity with unsigned char then with integer
        // unsigned char gpio2 =               (unsigned char) ((oscSharedMemory *) sharedMemory)->gpio2; // easier to check validity with unsigned char then with integer
        // unsigned char noOfSignals = 1; if (gpio2 <= 39) noOfSignals = 2;  // monitor 1 or 2 signals
        // adc1_channel_t adcchannel1 =        ((oscSharedMemory *) sharedMemory)->adcchannel1;
        // adc1_channel_t adcchannel2 =        ((oscSharedMemory *) sharedMemory)->adcchannel2;
        // int positiveTriggerTreshold =       ((oscSharedMemory *) sharedMemory)->positiveTriggerTreshold;
        // int negativeTriggerTreshold =       ((oscSharedMemory *) sharedMemory)->negativeTriggerTreshold;
        // unsigned long screenWidthTime =     ((oscSharedMemory *) sharedMemory)->screenWidthTime;
        // oscSamples *readBuffer =            &((oscSharedMemory *) sharedMemory)->readBuffer;
        // oscSamples *sendBuffer =            &((oscSharedMemory *) sharedMemory)->sendBuffer;
        // --- END REMOVE OLD VARIABLE EXTRACTION ---


        // Adjust sampling time calculation based on MAX_OSC_SAMPLES_PER_PACKET
        // MAX_OSC_SAMPLES_PER_PACKET includes the initial dummy message slot
        if ((unsigned long) samplingTime * (MAX_OSC_SAMPLES_PER_PACKET - 1) < screenWidthTime) {
             samplingTime = max ((int) (screenWidthTime / (MAX_OSC_SAMPLES_PER_PACKET - 1)) + 1, 1);
             char dbg_msg_samp[150];
             snprintf(dbg_msg_samp, sizeof(dbg_msg_samp), "oscReader_analog: %d analog channels. SamplingTime adjusted to %d us due to buffer size and screenWidthTime.", numActiveAnalog, samplingTime);
             __oscilloscope_h_debug__(dbg_msg_samp);
        }

        // --- REMOVE OLD SAMPLING TIME ADJUSTMENT ---
        // if (noOfSignals == 1) {
        //     if ((unsigned long) samplingTime * (OSCILLOSCOPE_1SIGNAL_BUFFER_SIZE - 2) < screenWidthTime) {
        //         samplingTime = max ((int) (screenWidthTime / (OSCILLOSCOPE_1SIGNAL_BUFFER_SIZE - 2)) + 1, 1); // + 1 just to be on the safe side due to integer calculation rounding
        //         __oscilloscope_h_debug__ ("oscReader_analog: 1 signal samplingTime was too short (regarding to buffer size) and is corrected to " + String (samplingTime));
        //     }
        // } else {
        //     if ((unsigned long) samplingTime * (OSCILLOSCOPE_2SIGNALS_BUFFER_SIZE - 2) < screenWidthTime) {
        //         samplingTime = max ((int) (screenWidthTime / (OSCILLOSCOPE_2SIGNALS_BUFFER_SIZE - 2)) + 1, 1); // + 1 just to be on the safe side due to integer calculation rounding
        //         __oscilloscope_h_debug__ ("oscReader_analog: 2 signals samplingTime was too short (regarding to buffer size) and is corrected to " + String (samplingTime));
        //     }
        // }
        // --- END REMOVE OLD SAMPLING TIME ADJUSTMENT ---

        // Is samplingTime is too long for 15 bits? Make a correction. Max sample time can be 32767 (15 bits) but since in some case actual sample time can be much larger than required let's keep it below 5000.
        if (samplingTime > 5000) {
                samplingTime = 5000;
                // __oscilloscope_h_debug__ ("oscReader_analog: samplingTime was too long (to fit in 15 bits in (almost?) all cases) and is corrected to " + String (samplingTime));
                cout << "oscReader_analog: samplingTime was too long (to fit in 15 bits in (almost?) all cases) and is corrected to " << samplingTime << endl; // Keep cout for now
        }

        /**/ // Keep debug output for now, update later if needed
        cout << "----- oscReader_analog () -----\\r\\n";
        cout << "samplingTime " << samplingTime << " us" << endl;
        cout << "Trigger Ch Idx: " << triggerChannelIndex << " (ADC: " << triggerAdcChannel << ")" << endl;
        cout << "positiveTrigger " << positiveTrigger << endl;
        cout << "negativeTrigger " << negativeTrigger << endl;
        cout << "positiveTriggerTreshold " << positiveTriggerTreshold << endl;
        cout << "negativeTriggerTreshold " << negativeTriggerTreshold << endl;
        cout << "screenWidthTime " << screenWidthTime << endl;
        cout << "numActiveAnalog " << numActiveAnalog << endl;
        cout << "max number of samples (incl. dummy) " << MAX_OSC_SAMPLES_PER_PACKET << endl;
        cout << "max possible screenWidthTime covered by samples " << (long long)samplingTime * (MAX_OSC_SAMPLES_PER_PACKET -1) << endl; // Use long long for calculation
        delay (100);
        /**/

        // Calculate screen refresh period. It sholud be arround 50 ms (sustainable screen refresh rate is arround 20 Hz) but it is better if it is a multiple value of screenWidthTime.
        unsigned long screenRefreshMilliseconds; // screen refresh period
        int noOfSampleSetsPerScreen = screenWidthTime / samplingTime; if ((unsigned long)noOfSampleSetsPerScreen * samplingTime < screenWidthTime) noOfSampleSetsPerScreen ++; // Number of *sets* of samples
        unsigned long correctedScreenWidthTime = (unsigned long)noOfSampleSetsPerScreen * samplingTime;
        screenRefreshMilliseconds = correctedScreenWidthTime >= 50000 ? correctedScreenWidthTime / 1000 : ((50500 / correctedScreenWidthTime) * correctedScreenWidthTime) / 1000;

        char dbg_msg_timing[150];
        snprintf(dbg_msg_timing, sizeof(dbg_msg_timing), "oscReader_analog: samplingTime = %d, screenWidthTime = %lu, corrected = %lu, refresh = %lu ms",
                 samplingTime, screenWidthTime, correctedScreenWidthTime, screenRefreshMilliseconds);
        __oscilloscope_h_debug__(dbg_msg_timing);


        readBuffer->samplesAreReady = true; // this information will be always copied to sendBuffer together with the samples

        // ADC setup (Assuming ADC handles are already configured and stored in channelConfig during runOscilloscope)
        // No GPIO setup needed here as ADC config handles it.


        // wait for the START signal
        while (sharedMemory->oscReaderState != START) delay (1);
        sharedMemory->oscReaderState = STARTED;

        // --- do the sampling, samplingTime and screenWidthTime are in us ---

        // triggered or untriggered mode of operation
        bool triggeredMode = (triggerChannelIndex != -1) && (positiveTrigger || negativeTrigger);

        TickType_t lastScreenRefreshTicks = xTaskGetTickCount ();               // for timing screen refresh intervals

        // Temporary storage for samples read in one go
        int adc_raw[MAX_OSC_CHANNELS]; // Max possible channels

        while (sharedMemory->oscReaderState == STARTED) { // sampling from the left of the screen - while not getting STOP signal

            unsigned long screenTime = 0;                                       // in us - how far we have already got from the left of the screen (we'll compare this value with screenWidthTime)
            unsigned long deltaTime = 0;                                        // in us - delta from previous sample
            unsigned long lastSampleMicroseconds = micros ();                   // for sample timing
            unsigned long newSampleMicroseconds = lastSampleMicroseconds;

            // TODO: Define and insert the correct multi-channel dummy message.
            // This message needs to convey the number and type (A/D) of active channels.
            // For now, using a placeholder. The client JS needs updating to parse this.
            readBuffer->samplesMultiChannel[0].num_channels = numActiveAnalog; // Example placeholder
            readBuffer->samplesMultiChannel[0].delta_time = -1; // Indicate dummy message
            for(int k=0; k<numActiveAnalog; ++k) readBuffer->samplesMultiChannel[0].values[k] = -1; // Placeholder values
            readBuffer->sampleCount = 1;


            if (triggeredMode) { // if no trigger is set then skip this (waiting) part and start sampling immediatelly

                // take the first sample for the trigger channel
                int lastTriggerSampleValue = -1;
                // Assume adc_oneshot_read is thread-safe or handle potential conflicts if needed
                esp_err_t read_ret = adc_oneshot_read(channelConfig[triggerChannelIndex].adc_handle, channelConfig[triggerChannelIndex].adc_channel, &lastTriggerSampleValue);
                if (read_ret != ESP_OK) {
                    char dbg_msg_adc[100];
                    snprintf(dbg_msg_adc, sizeof(dbg_msg_adc), "ADC Read Error (Trigger - Last): %s", esp_err_to_name(read_ret));
                    __oscilloscope_h_debug__(dbg_msg_adc);
                    // Handle error? Maybe retry or stop? For now, log and continue with -1
                    lastTriggerSampleValue = -1; // Indicate error?
                }
                #ifdef INVERT_ADC1_GET_RAW
                    if (lastTriggerSampleValue != -1) lastTriggerSampleValue = 4095 - lastTriggerSampleValue;
                #endif


                // wait for trigger condition
                while (sharedMemory->oscReaderState == STARTED) {
                    // wait before continuing to next sample and calculate delta offset for it
                    while ((deltaTime = (newSampleMicroseconds = micros ()) - lastSampleMicroseconds) < samplingTime) delayMicroseconds (1);
                    lastSampleMicroseconds = newSampleMicroseconds;

                    // take the second sample for the trigger channel
                    int newTriggerSampleValue = -1;
                    read_ret = adc_oneshot_read(channelConfig[triggerChannelIndex].adc_handle, channelConfig[triggerChannelIndex].adc_channel, &newTriggerSampleValue);
                    if (read_ret != ESP_OK) {
                       char dbg_msg_adc[100];
                       snprintf(dbg_msg_adc, sizeof(dbg_msg_adc), "ADC Read Error (Trigger - New): %s", esp_err_to_name(read_ret));
                       __oscilloscope_h_debug__(dbg_msg_adc);
                       newTriggerSampleValue = -1;
                    }
                   #ifdef INVERT_ADC1_GET_RAW
                       if (newTriggerSampleValue != -1) newTriggerSampleValue = 4095 - newTriggerSampleValue;
                   #endif

                    // Compare both samples to check if the trigger condition has occured
                    if (lastTriggerSampleValue != -1 && newTriggerSampleValue != -1 && // Check for valid reads
                        ((positiveTrigger && lastTriggerSampleValue < positiveTriggerTreshold && newTriggerSampleValue >= positiveTriggerTreshold) ||
                         (negativeTrigger && lastTriggerSampleValue > negativeTriggerTreshold && newTriggerSampleValue <= negativeTriggerTreshold)))
                    {
                        // trigger condition has occured, insert both trigger samples conceptually
                        // We need to read *all* active analog channels for these two time points

                        // --- Conceptual insertion point 1 (corresponds to lastTriggerSampleValue time) ---
                        // Read all *other* active analog channels at this time point (approximately)
                        // Store this set of samples. Delta time = 0 for the first set.

                        // --- Conceptual insertion point 2 (corresponds to newTriggerSampleValue time) ---
                        // Read all active analog channels *again* (approximately deltaTime later)
                        // Store this second set of samples. Delta time = deltaTime.

                        // TODO: Implement reading *all* active analog channels for the two trigger points
                        // and store them correctly in readBuffer->samplesMultiChannel[1] and [2]
                        // For now, we skip this detailed insertion and just break to start normal sampling.
                        // This means the first few samples after the trigger might be missing or have incorrect timing.

                        __oscilloscope_h_debug__("Trigger condition met.");

                        screenTime = deltaTime; // Start measuring screen time from the 'new' sample time
                        // readBuffer->sampleCount = 3; // If we were inserting the two trigger points

                        // Need to read the *current* sample set for all channels to start the main loop
                        int activeAnalogIdx = 0;
                        for (int i = 0; i < MAX_OSC_CHANNELS; ++i) {
                            if (channelConfig[i].is_active && channelConfig[i].is_analog) {
                                int val = -1;
                                read_ret = adc_oneshot_read(channelConfig[i].adc_handle, channelConfig[i].adc_channel, &val);
                                if (read_ret != ESP_OK) val = -1; // Handle error
                                #ifdef INVERT_ADC1_GET_RAW
                                    if (val != -1) val = 4095 - val;
                                #endif
                                adc_raw[activeAnalogIdx++] = val; // Store in temp array
                            }
                        }
                        // Store the *current* full sample set (this corresponds approx to newTriggerSampleValue time)
                        if (readBuffer->sampleCount < MAX_OSC_SAMPLES_PER_PACKET) {
                            readBuffer->samplesMultiChannel[readBuffer->sampleCount].delta_time = (int16_t)deltaTime; // Or should this be 0 for the first post-trigger? Needs thought.
                            readBuffer->samplesMultiChannel[readBuffer->sampleCount].num_channels = numActiveAnalog;
                             for(int k=0; k<numActiveAnalog; ++k) {
                                readBuffer->samplesMultiChannel[readBuffer->sampleCount].values[k] = (int16_t)adc_raw[k];
                             }
                             readBuffer->sampleCount++;
                        } else {
                             __oscilloscope_h_debug__("Trigger: Buffer full immediately!");
                        }


                        // Update lastSampleMicroseconds *after* reading the current sample set
                        lastSampleMicroseconds = micros(); // Reset timing base for next sample
                        deltaTime = 0; // Reset delta for the loop logic


                        break; // trigger event occured, stop waiting and proceed to sampling
                    } else {
                        // Trigger not met, update last sample value and continue waiting
                        lastTriggerSampleValue = newTriggerSampleValue;
                    }
                } // while not triggered
            } // if in trigger mode


            // take (the rest of the) samples that fit on one screen
            while (sharedMemory->oscReaderState == STARTED) {

                 // Check buffer full condition first
                 if (readBuffer->sampleCount >= MAX_OSC_SAMPLES_PER_PACKET) {
                      __oscilloscope_h_debug__("Analog Read: Buffer full.");
                      // Buffer is full, copy to send buffer if possible
                      if (!sendBuffer->samplesAreReady) {
                          memcpy(sendBuffer, readBuffer, sizeof(oscSamples)); // Use memcpy for efficiency
                          sendBuffer->samplesAreReady = true; // Ensure flag is set (memcpy copies it, but good practice)
                          __oscilloscope_h_debug__("Analog Read: Copied full buffer to sendBuffer.");
                      } else {
                          __oscilloscope_h_debug__("Analog Read: Send buffer busy, dropping frame.");
                          // send buffer with previous frame is still waiting, drop this frame
                      }
                      break; // Break inner loop to start new frame cycle
                 }

                 // Check if we already passed screenWidthTime
                 if (screenTime >= screenWidthTime) {
                     __oscilloscope_h_debug__("Analog Read: Screen time reached.");
                     // Screen time reached, copy potentially partial buffer to send buffer
                     if (!sendBuffer->samplesAreReady) {
                         memcpy(sendBuffer, readBuffer, sizeof(oscSamples)); // Copy current buffer state
                         sendBuffer->samplesAreReady = true;
                          __oscilloscope_h_debug__("Analog Read: Copied partial buffer to sendBuffer.");
                     } else {
                         __oscilloscope_h_debug__("Analog Read: Send buffer busy, dropping partial frame.");
                         // send buffer busy, drop frame
                     }
                     break; // Break inner loop to start new frame cycle
                 }


                // --- Read all active analog channels ---
                int activeAnalogIdx = 0;
                for (int i = 0; i < MAX_OSC_CHANNELS; ++i) {
                    if (channelConfig[i].is_active && channelConfig[i].is_analog) {
                        int val = -1;
                        esp_err_t read_ret = adc_oneshot_read(channelConfig[i].adc_handle, channelConfig[i].adc_channel, &val);

                        if (read_ret != ESP_OK) {
                            char dbg_msg_adc[100];
                            snprintf(dbg_msg_adc, sizeof(dbg_msg_adc), "ADC Read Error (Chan %d): %s", i, esp_err_to_name(read_ret));
                            __oscilloscope_h_debug__(dbg_msg_adc);
                            val = -1; // Indicate error in data? Or use previous value? For now -1.
                        }

                        #ifdef INVERT_ADC1_GET_RAW
                            if (val != -1) val = 4095 - val;
                        #endif
                        adc_raw[activeAnalogIdx++] = val; // Store in temp array
                    }
                }

                // Clamp delta time to fit in int16_t if necessary, although samplingTime limit should prevent overflow
                deltaTime = min((unsigned long)32767, deltaTime);

                // Store the collected samples in the next slot
                readBuffer->samplesMultiChannel[readBuffer->sampleCount].delta_time = (int16_t)deltaTime;
                readBuffer->samplesMultiChannel[readBuffer->sampleCount].num_channels = numActiveAnalog; // Store number of *active analog* channels in this sample set
                for(int k=0; k<numActiveAnalog; ++k) {
                    readBuffer->samplesMultiChannel[readBuffer->sampleCount].values[k] = (int16_t)adc_raw[k];
                }
                readBuffer->sampleCount++;

                screenTime += deltaTime;


                // --- REMOVE OLD SAMPLING LOGIC ---
                // if (noOfSignals == 1) { new1SignalSample = {(int16_t) adc1_get_raw (adcchannel1), (int16_t) deltaTime};
                //                        #ifdef INVERT_ADC1_GET_RAW
                //                           new1SignalSample.signal1 = 4095 - new1SignalSample.signal1;
                //                        #endif
                //                        readBuffer->samples1Signal [readBuffer->sampleCount ++] = new1SignalSample;
                // } else                { new2SignalsSample = {(int16_t) adc1_get_raw (adcchannel1), (int16_t) adc1_get_raw (adcchannel2), (int16_t) deltaTime};
                //                        #ifdef INVERT_ADC1_GET_RAW
                //                           new2SignalsSample.signal1 = 4095 - new2SignalsSample.signal1;
                //                           new2SignalsSample.signal2 = 4095 - new2SignalsSample.signal2;
                //                        #endif
                //                        readBuffer->samples2Signals [readBuffer->sampleCount ++] = new2SignalsSample;
                // }
                // --- END REMOVE OLD SAMPLING LOGIC ---


                // wait before continuing to next sample and calculate delta offset for it
                // Make sure deltaTime calculation happens *after* storing the sample related to the *previous* deltaTime
                while ((deltaTime = (newSampleMicroseconds = micros()) - lastSampleMicroseconds) < samplingTime) {
                    // Yield if the wait is long enough, otherwise busy wait
                    if (samplingTime - deltaTime > 100) { // Heuristic threshold: 100 us
                       vTaskDelay(pdMS_TO_TICKS(1)); // Delay 1 tick (~1ms or ~10ms)
                    } else {
                       delayMicroseconds(1); // Busy wait for short durations
                    }
                 }
                lastSampleMicroseconds = newSampleMicroseconds;


            } // while screenTime < screenWidthTime

            // wait before next screen refresh
             __oscilloscope_h_debug__("Analog Read: End of frame cycle. Waiting for refresh delay.");
            vTaskDelayUntil (&lastScreenRefreshTicks, pdMS_TO_TICKS (screenRefreshMilliseconds));
             __oscilloscope_h_debug__("Analog Read: Refresh delay passed. Starting new frame.");


        } // while sampling

        // wait for the STOP signal
        while (sharedMemory->oscReaderState != STOP) delay (1);
        sharedMemory->oscReaderState = STOPPED;
        __oscilloscope_h_debug__("oscReader_analog: Stopped.");

        vTaskDelete (NULL);
    }

    // --- oscReader_analog_1_signal_i2s ---
    #ifdef USE_I2S_INTERFACE
        void oscReader_analog_1_signal_i2s (void *sharedMemory) {
            // *not needed* bool doAnalogRead =                 !strcmp (((oscSharedMemory *) sharedMemory)->readType, "analog");
            // *not needed* bool unitIsMicroSeconds =           !strcmp (((oscSharedMemory *) sharedMemory)->samplingTimeUnit, "us");
            int samplingTime =                  ((oscSharedMemory *) sharedMemory)->samplingTime;
            bool positiveTrigger =              ((oscSharedMemory *) sharedMemory)->positiveTrigger;
            bool negativeTrigger =              ((oscSharedMemory *) sharedMemory)->negativeTrigger;
            unsigned char gpio1 =               (unsigned char) ((oscSharedMemory *) sharedMemory)->gpio1; // easier to check validity with unsigned char then with integer 
            // * not needed * unsigned char gpio2 =               (unsigned char) ((oscSharedMemory *) sharedMemory)->gpio2; // easier to check validity with unsigned char then with integer
            // * not needed * unsigned char noOfSignals = 1; if (gpio2 <= 39) noOfSignals = 2;  // monitor 1 or 2 signals
            adc1_channel_t adcchannel1 =        ((oscSharedMemory *) sharedMemory)->adcchannel1;
            // * not needed * adc1_channel_t adcchannel2 =        ((oscSharedMemory *) sharedMemory)->adcchannel2;
            int positiveTriggerTreshold =       ((oscSharedMemory *) sharedMemory)->positiveTriggerTreshold;
            int negativeTriggerTreshold =       ((oscSharedMemory *) sharedMemory)->negativeTriggerTreshold;
            unsigned long screenWidthTime =     ((oscSharedMemory *) sharedMemory)->screenWidthTime; 
            oscSamples *readBuffer =            &((oscSharedMemory *) sharedMemory)->readBuffer;
            oscSamples *sendBuffer =            &((oscSharedMemory *) sharedMemory)->sendBuffer;

            // How many samples do we need to take? The following should be considered:
            // (A) - at leastsampleRate * screenWidthTime / 1000000 + 1; (1 sample more than distance between them)
            // (B) - it must be an even number, otherwise the last sample taken would be 0
            // (C) - it must be at most OSCILLOSCOPE_I2S_BUFFER_SIZE - 1 (the 0-th sample in the buffer is reserved for "dummy" value)
            // (D) - it must be at least 8 due to i2s_read limitations, if working in triggered mode we have to consider that at least 8 samples must be read (at second reading) for the rest of teh signal
            // (E) - the first is2_read after the initialisation often contains false readings (all 16 bits are 0) at the beginning (normally at the first 6 samples read), let's always delete the first 8 samples read just to be on the safe side which wil also solve the problem (D)

            cout << "----- oscReader_I2S () before correction -----\r\n";
            cout << "samplingTime " << samplingTime << " us" << endl;

            // calculate correct sampling time so that it will prefectly aligh with sampleRate (regarding integer calculation rounding) and that the sample buffer is large enough 
            unsigned long sampleRate = 1000000 / samplingTime; // samplingTime is in us
            int noOfSamplesToTakeFirstTime = sampleRate * screenWidthTime / 1000000 + 1 + 8; // screenWidhtTime is in us, 1 sample more than the distancesbetween them (A) (E)
            while (samplingTime != 1000000 / sampleRate // integer clculation rounding missmatch
              || (unsigned long) samplingTime * (OSCILLOSCOPE_I2S_BUFFER_SIZE - 1 - 1) < screenWidthTime // samples do not fill the screen (additional - 1 due to possible (B))
              || noOfSamplesToTakeFirstTime > (OSCILLOSCOPE_I2S_BUFFER_SIZE - 1 - 1) // samples do not fit in the buffer (the first sample is dummy sample, additional - 1 due to (B))
              || samplingTime < 4 // dummy values -2 and -3 are already taken for different types of buffer
              || samplingTime < 7) // max ESP32 sampling rate = 150 kHz (sampling time >= 6.6 us)
            {
                samplingTime ++;
                sampleRate = 1000000 / samplingTime;
                noOfSamplesToTakeFirstTime = sampleRate * screenWidthTime / 1000000 + 1 + 8; // (A) (E)
            }
            if (noOfSamplesToTakeFirstTime % 2 != 0) noOfSamplesToTakeFirstTime ++; // (B)
            if (noOfSamplesToTakeFirstTime <= 8) noOfSamplesToTakeFirstTime = 10; // (D), add at least 2 usefull samples

            // Is samplingTime is too long for 15 bits, make a correction. Max sample time can be 32767 (15 bits) but since in some case actual sample time can be much larger than required le's keep it below 5000.
            if (samplingTime > 32767) {
                samplingTime = 32767;
                __oscilloscope_h_debug__ ("oscReader_analog_1_signal_i2s: oscReader_oscReader_analog_1_signal_i2s] sampling with these parameters will not be precise");
                #ifdef __DMESG__
                    dmesgQueue << "[oscilloscope][oscReader_oscReader_analog_1_signal_i2s] sampling with these parameters will not be precise";
                #endif
            }

            /**/
            cout << "----- oscReader_I2S () after correction -----\r\n";
            cout << "samplingTime " << samplingTime << " us" << endl;
            cout << "sample rate " << 1000000 / samplingTime << endl;
            cout << "positiveTrigger " << positiveTrigger << endl;
            cout << "negativeTrigger " << negativeTrigger << endl;
            cout << "gpio1 " << gpio1 << endl;
            // cout << "gpio2 " << gpio2 << endl;
            // cout << "noOfSignals " << noOfSignals << endl;
            cout << "positiveTriggerTreshold " << positiveTriggerTreshold << endl;
            cout << "negativeTriggerTreshold " << negativeTriggerTreshold << endl;
            cout << "screenWidthTime " << screenWidthTime << endl;
            cout << "max number of sampels " << OSCILLOSCOPE_I2S_BUFFER_SIZE << endl;
            cout << "max possible screenWidthTime covered by sampels " << samplingTime * OSCILLOSCOPE_I2S_BUFFER_SIZE << endl;
            delay (100);
            /**/

            // Calculate screen refresh period. It sholud be arround 50 ms (sustainable screen refresh rate is arround 20 Hz) but it is better if it is a multiple value of screenWidthTime.
            unsigned long screenRefreshMilliseconds; // screen refresh period
            int noOfSamplesPerScreen = screenWidthTime / samplingTime; if (noOfSamplesPerScreen * samplingTime < screenWidthTime) noOfSamplesPerScreen ++;
            unsigned long correctedScreenWidthTime = noOfSamplesPerScreen * samplingTime;                         
            screenRefreshMilliseconds = correctedScreenWidthTime >= 50000 ? correctedScreenWidthTime / 1000 : ((50500 / correctedScreenWidthTime) * correctedScreenWidthTime) / 1000;
            __oscilloscope_h_debug__ ("oscReader_analog_1_signal_i2s: samplingTime = " + String (samplingTime) + ", screenWidthTime = " + String (screenWidthTime));
            __oscilloscope_h_debug__ ("oscReader_analog_1_signal_i2s: sampleRate = " + String (sampleRate) + ", noOfSamplesToTake = " + String (noOfSamplesToTakeFirstTime));
            __oscilloscope_h_debug__ ("oscReader_analog_1_signal_i2s: screenRefreshMilliseconds = " + String (screenRefreshMilliseconds) + " ms (should be close to 50 ms), screen refresh frequency = " + String (1000.0 / screenRefreshMilliseconds) + " Hz (should be close to 20 Hz)");

            readBuffer->samplesAreReady = true; // this information will be always copied to sendBuffer together with the samples

            // wait for the START signal
            while (((oscSharedMemory *) sharedMemory)->oscReaderState != START) delay (1);
            ((oscSharedMemory *) sharedMemory)->oscReaderState = STARTED; 

            // --- do the sampling, samplingTime and screenWidthTime are in us ---

            // triggered or untriggered mode of operation
            bool triggeredMode = positiveTrigger || negativeTrigger;
            int noOfSamplesTaken;

            TickType_t lastScreenRefreshTicks = xTaskGetTickCount ();               // for timing screen refresh intervals            

            while (((oscSharedMemory *) sharedMemory)->oscReaderState == STARTED) { // sampling from the left of the screen - while not getting STOP signal

                // Insert first dummy sample to read-buffer this tells javascript client to start drawing from the left of the screen. Please note that it also tells javascript client how many signals are in each sample
                readBuffer->samplesI2sSignal [0].signal1 = -samplingTime; // no real data sample can look like this
                // readBuffer->sampleCount = 1;
                // take (the rest of the) samples that fit on one screen: https://www.instructables.com/The-Best-Way-for-Sampling-Audio-With-ESP32

                // setupI2S
                esp_err_t err;
                i2s_config_t i2s_config = { 
                    .mode = (i2s_mode_t) (I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN),
                    .sample_rate = (uint32_t) ((1000000 / samplingTime) * I2S_FREQ_CORRECTION), // = samplingFrequency (samplingTime is in us),
                    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, // could only get it to work with 32bits
                    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // <- mono signal - stereo signal -> I2S_CHANNEL_FMT_RIGHT_LEFT, // although the SEL config should be left, it seems to transmit on right
                    .communication_format = i2s_comm_format_t (I2S_COMM_FORMAT_STAND_I2S), //// I2S_COMM_FORMAT_STAND_I2S, // I2S_COMM_FORMAT_I2S_MSB, - deprecated
                    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, // Interrupt level 1
                    .dma_buf_count = 4, // number of buffers
                    .dma_buf_len = noOfSamplesToTakeFirstTime, // samples per buffer
                    .use_apll = true // false//,
                    // .tx_desc_auto_clear = false,
                    // .fixed_mclk = 1
                };
              
                err = i2s_driver_install (I2S_NUM_0, &i2s_config,  0, NULL);  //step 2

                if (err != ESP_OK) {
                    Serial.printf ("Failed installing driver: %d\n", err);
                    #ifdef __DMESG__
                        dmesgQueue << "[oscilloscope][oscReader_oscReader_analog_1_signal_i2s] failed to install the driver: " << err;
                    #endif

                    ((oscSharedMemory *) sharedMemory)->webSocket->sendString ("[oscilloscope] failed to install the i2s driver."); // send error to javascript client
                    ((oscSharedMemory *) sharedMemory)->webSocket->closeWebSocket ();
                    // wait for the STOP signal
                    while (((oscSharedMemory *) sharedMemory)->oscReaderState != STOP) delay (1);
                    ((oscSharedMemory *) sharedMemory)->oscReaderState = STOPPED;
                    vTaskDelete (NULL);
                }

                err = i2s_set_adc_mode (ADC_UNIT_1, adcchannel1);
                if (err != ESP_OK) {
                    Serial.printf ("Failed setting up adc mode: %d\n", err);
                    #ifdef __DMESG__
                        dmesgQueue << "[oscilloscope][oscReader_oscReader_analog_1_signal_i2s] failed setting up adc mode: " << err;
                    #endif
                    i2s_driver_uninstall (I2S_NUM_0);

                    ((oscSharedMemory *) sharedMemory)->webSocket->sendString ("[oscilloscope] failed setting up i2s adc mode."); // send error to javascript client
                    ((oscSharedMemory *) sharedMemory)->webSocket->closeWebSocket ();
                    // wait for the STOP signal
                    while (((oscSharedMemory *) sharedMemory)->oscReaderState != STOP) delay (1);
                    ((oscSharedMemory *) sharedMemory)->oscReaderState = STOPPED;                
                    vTaskDelete (NULL);
                }

                while (((oscSharedMemory *) sharedMemory)->oscReaderState == STARTED) {

                    // read the whole buffer
                    size_t bytesRead = 0;
                    err = i2s_read (I2S_NUM_0, 
                                    (void *) &readBuffer->samplesI2sSignal [1], // skip the first (dummy) sample
                                    noOfSamplesToTakeFirstTime << 1, // in bytes
                                    &bytesRead,
                                    pdMS_TO_TICKS (1000)); // portMAX_DELAY); // no timeout
                    noOfSamplesTaken = bytesRead >> 1; // samples are 16 bit integers 
                    if (err != ESP_OK || noOfSamplesTaken < 10) { // we'll leave first 8 samples out so if we don't have even 2 usable samples there id probably something wrong
                        Serial.printf ("Failed reading the samples: %d\n", err);
                        #ifdef __DMESG__
                            dmesgQueue << "[oscilloscope][oscReader_oscReader_analog_1_signal_i2s] failed reading  the samples: " << err;
                        #endif
                        i2s_driver_uninstall (I2S_NUM_0);

                        ((oscSharedMemory *) sharedMemory)->webSocket->sendString ("[oscilloscope] failed reading the samples."); // send error to javascript client
                        ((oscSharedMemory *) sharedMemory)->webSocket->closeWebSocket ();
                        // wait for the STOP signal
                        while (((oscSharedMemory *) sharedMemory)->oscReaderState != STOP) delay (1);
                        ((oscSharedMemory *) sharedMemory)->oscReaderState = STOPPED;                    
                        vTaskDelete (NULL);
                    }

                    // For some strange reason the sample come swapped two-by two. Unswap them and filter out only 12 bits that actually hold the value while also skipping the first 8 samples
                    noOfSamplesTaken -= 8;
                    #ifdef INVERT_I2S_READ
                        for (int i = 1; i <= noOfSamplesTaken; i += 2) {
                            readBuffer->samplesI2sSignal [i].signal1 = ~readBuffer->samplesI2sSignal [i + 9].signal1 & 0xFFF;
                            readBuffer->samplesI2sSignal [i + 1].signal1 = ~readBuffer->samplesI2sSignal [i + 8].signal1 & 0xFFF;
                        }
                    #else
                        for (int i = 1; i <= noOfSamplesTaken; i += 2) {
                            readBuffer->samplesI2sSignal [i].signal1 = readBuffer->samplesI2sSignal [i + 9].signal1 & 0xFFF;
                            readBuffer->samplesI2sSignal [i + 1].signal1 = readBuffer->samplesI2sSignal [i + 8].signal1 & 0xFFF;
                        }
                    #endif

                    if (!triggeredMode) break; // if not in triggered modt then we already have what we need

                    // if in triggered mode try to find trigger condition
                    for (int i = 1; i < noOfSamplesTaken; i++) {
                        if ( (positiveTrigger && readBuffer->samplesI2sSignal [i].signal1 < positiveTriggerTreshold && readBuffer->samplesI2sSignal [i + 1].signal1 >= positiveTriggerTreshold) || (negativeTrigger && readBuffer->samplesI2sSignal [i].signal1 > negativeTriggerTreshold && readBuffer->samplesI2sSignal [i + 1].signal1 <= negativeTriggerTreshold) ) { 
                            // trigger condition found at i, copy the rest of the buffer to its beginning and do another i2s_read for the samples that are missing
                            if (i > 1) {
                                int noOfSamplesToTakeSecondTime = (i - 1);
                                // there is a bug in i2s_read: it odd number of samples are to be read the last one is always 0, so make sure we have even number of samples
                                if (noOfSamplesToTakeSecondTime % 2 != 0) noOfSamplesToTakeSecondTime ++;
                                if (noOfSamplesToTakeSecondTime < 8) noOfSamplesToTakeSecondTime = 8;
                                int fromInd = noOfSamplesTaken - (i - 1) + 1;
                                __oscilloscope_h_debug__ ("oscReader_analog_1_signal_i2s: trigger condition found at " + String (i) + " need to read additional " + String (need to read additional) + " samples at buffer position " + String (fromInd));
                                memcpy ((void*) &readBuffer->samplesI2sSignal [1], (void*) &readBuffer->samplesI2sSignal [i], (noOfSamplesTaken - (i - 1)) << 1);
                                err = i2s_read (I2S_NUM_0, 
                                                (void*) &readBuffer->samplesI2sSignal [fromInd],
                                                noOfSamplesToTakeSecondTime << 1, // in bytes
                                                &bytesRead,
                                                pdMS_TO_TICKS (1000)); // portMAX_DELAY); // no timeout
                                if (err != ESP_OK || noOfSamplesTaken == 0) {
                                    Serial.printf ("Failed reading the samples: %d\n", err);
                                    #ifdef __DMESG__
                                        dmesgQueue << "[oscilloscope][oscReader_oscReader_analog_1_signal_i2s] failed reading the samples: " << err;
                                    #endif
                                    i2s_driver_uninstall (I2S_NUM_0);

                                    ((oscSharedMemory *) sharedMemory)->webSocket->sendString ("[oscilloscope] failed reading the samples."); // send error to javascript client
                                    ((oscSharedMemory *) sharedMemory)->webSocket->closeWebSocket ();
                                    // wait for the STOP signal
                                    while (((oscSharedMemory *) sharedMemory)->oscReaderState != STOP) delay (1);
                                    ((oscSharedMemory *) sharedMemory)->oscReaderState = STOPPED;                                
                                    vTaskDelete (NULL);
                                }
                                noOfSamplesTaken = noOfSamplesTaken - (i - 1) + (bytesRead >> 1); // - deleted samples + newly read samples (normally we would end up with the same number)
                                // for (int j = fromInd; j <= noOfSamplesTaken; j++) readBuffer->samplesI2sSignal [j].signal1 &= 0x0FFF;
                                // For some strange reason the sample come swapped two-by two. Unswap them and filter out only 12 bits that actually hold the value
                                #ifdef INVERT_I2S_READ
                                    for (int j = fromInd; j < noOfSamplesTaken; j += 2) {
                                        int16_t tmp = readBuffer->samplesI2sSignal [j].signal1;
                                        readBuffer->samplesI2sSignal [j].signal1 = ~readBuffer->samplesI2sSignal [j + 1].signal1 & 0xFFF;
                                        readBuffer->samplesI2sSignal [j + 1].signal1 = ~tmp & 0x0FFF;
                                    }
                                #else
                                    for (int j = fromInd; j < noOfSamplesTaken; j += 2) {
                                        int16_t tmp = readBuffer->samplesI2sSignal [j].signal1;
                                        readBuffer->samplesI2sSignal [j].signal1 = readBuffer->samplesI2sSignal [j + 1].signal1 & 0xFFF;
                                        readBuffer->samplesI2sSignal [j + 1].signal1 = tmp & 0x0FFF;
                                    }
                                #endif
                            }

                            goto passSamplesToOscSender;
                        }
                    }
                    // trigger condition not found, continue reading
                } // while (true)

            passSamplesToOscSender:

                // pass readBuffer to oscSender
                readBuffer->sampleCount = noOfSamplesTaken + 1; // + 1 dummy sample
                if (!sendBuffer->samplesAreReady) 
                    *sendBuffer = *readBuffer;

                // uninstall the driver
                i2s_driver_uninstall (I2S_NUM_0);

                // wait before next screen refresh
                vTaskDelayUntil (&lastScreenRefreshTicks, pdMS_TO_TICKS (screenRefreshMilliseconds));
            
            } // while sampling

            // wait for the STOP signal
            while (((oscSharedMemory *) sharedMemory)->oscReaderState != STOP) delay (1);
            ((oscSharedMemory *) sharedMemory)->oscReaderState = STOPPED; 

            vTaskDelete (NULL);
        }
    #endif


    // oscSender oscSender oscSender oscSender oscSender oscSender oscSender oscSender oscSender oscSender oscSender oscSender oscSender oscSender 
    
    void oscSender (void *sharedMemoryPtr) {
        oscSharedMemory *sharedMemory = (oscSharedMemory *) sharedMemoryPtr;
        oscSamples *sendBuffer = &sharedMemory->sendBuffer;
        bool clientIsBigEndian = sharedMemory->clientIsBigEndian;
        WebSocket *webSocket = sharedMemory->webSocket;

        // Remove old logic based on gpio1/gpio2
        // unsigned char gpio1 =                   (unsigned char) ((oscSharedMemory *) sharedMemory)->gpio1; // easier to check validity with unsigned char then with integer
        // unsigned char gpio2 =                   (unsigned char) ((oscSharedMemory *) sharedMemory)->gpio2; // easier to check validity with unsigned char then with integer
        // unsigned char noOfSignals = 1; if (gpio2 <= 39) noOfSignals = 2;  // monitor 1 or 2 signals

        sendBuffer->samplesAreReady = false;

        __oscilloscope_h_debug__("oscSender started.");

        unsigned long lastMillis = millis ();
        while (true) {
            // Check WebSocket status before attempting to send
            if (!webSocket || webSocket->getReadyState() != WS_READY_STATE_OPEN) {
                __oscilloscope_h_debug__("oscSender: WebSocket not open. Stopping sender.");
                // Optionally signal reader to stop? Or just exit.
                sharedMemory->oscReaderState = STOP; // Signal reader to stop
                break; // Exit sender loop
            }

            delay (1); // Yield

            // Send samples to javascript client if they are ready
            if (sendBuffer->samplesAreReady && sendBuffer->sampleCount > 0) {
                __oscilloscope_h_debug__("oscSender: sendBuffer is ready.");

                // Copy buffer locally. Using memcpy might be slightly safer if reader modifies concurrently, though flags should prevent it.
                oscSamples sendSamples;
                memcpy(&sendSamples, sendBuffer, sizeof(oscSamples));
                sendBuffer->samplesAreReady = false; // Mark buffer as available for reader immediately after copy

                // Calculate the total size to send: number of samples * size of each multi-channel sample struct.
                size_t sendBytes = sendSamples.sampleCount * sizeof(oscMultiChannelSample);
                __oscilloscope_h_debug__(("oscSender: Sending " + String(sendSamples.sampleCount) + " samples (" + String(sendBytes) + " bytes).").c_str());


                // Perform byte swapping IF necessary
                if (clientIsBigEndian) {
                     __oscilloscope_h_debug__("oscSender: Swapping bytes for big-endian client.");
                    for (unsigned int i = 0; i < sendSamples.sampleCount; i++) {
                        oscMultiChannelSample *sample = &sendSamples.samplesMultiChannel[i];
                        // Swap delta_time (int16_t)
                        sample->delta_time = __builtin_bswap16(sample->delta_time);
                        // num_channels (uint8_t) does not need swapping
                        // Swap values (int16_t array)
                        for (uint8_t j = 0; j < sample->num_channels; j++) { // Only swap the actual number of channels used
                            sample->values[j] = __builtin_bswap16(sample->values[j]);
                        }
                    }
                }

                // Send the binary data
                // Use the pointer to the actual data within the copied structure
                if (!webSocket->sendBinary((uint8_t *)sendSamples.samplesMultiChannel, sendBytes)) {
                    __oscilloscope_h_debug__("oscSender: WebSocket sendBinary failed.");
                    // Handle send failure - maybe close socket or break loop?
                    sharedMemory->oscReaderState = STOP; // Signal reader to stop
                    break; // Exit sender loop
                }

                 __oscilloscope_h_debug__("oscSender: Sent data successfully.");
                 lastMillis = millis (); // Reset timeout watchdog

            } else {
                // Simple watchdog: if no samples sent for a while, maybe something is wrong?
                if (millis () - lastMillis > 5000) { // 5 second watchdog
                    __oscilloscope_h_debug__("oscSender: Watchdog timeout - no samples sent for 5s. Stopping.");
                    sharedMemory->oscReaderState = STOP; // Signal reader to stop
                    if (webSocket && webSocket->getReadyState() == WS_READY_STATE_OPEN) {
                       webSocket->sendString("[Oscilloscope Error] No samples received from reader task.");
                       // Consider closing socket? webSocket->closeWebSocket();
                    }
                    break; // Exit sender loop
                }
            }

            // Check if the reader task has requested a stop
            if (sharedMemory->oscReaderState == STOP || sharedMemory->oscReaderState == STOPPED) {
                __oscilloscope_h_debug__("oscSender: Reader task stopped/stopping. Exiting sender.");
                break;
            }

            // Removed old logic for checking buffer type and size based on dummy values
            // // find out the type of buffer used
            // if (noOfSignals == 1)
            //     if (sendSamples.samplesI2sSignal [0].signal1 < -3)
            //         sendBytes = sendSamples.sampleCount * sizeof (oscI2sSample);  // 1 I2S signal (I2S signal does not starts with -1, -2 or -3 dummy value)
            //     else
            //         sendBytes = sendSamples.sampleCount * sizeof (osc1SignalSample); // 1 signal
            // else sendBytes = sendSamples.sampleCount * sizeof (osc2SignalsSample); // 2 signals
            //
            // // swap bytes if javascript client is big endian
            // if (clientIsBigEndian) {
            //     if (noOfSignals == 1)
            //         if (sendSamples.samplesI2sSignal [0].signal1 < -3) {
            //             for (int i = 0; i < sendSamples.sampleCount; i++) {
            //                 sendSamples.samplesI2sSignal [i].signal1 = __builtin_bswap16 (sendSamples.samplesI2sSignal [i].signal1);
            //             }
            //         } else {
            //             for (int i = 0; i < sendSamples.sampleCount; i++) {
            //                 sendSamples.samples1Signal [i].signal1 = __builtin_bswap16 (sendSamples.samples1Signal [i].signal1);
            //                 sendSamples.samples1Signal [i].deltaTime = __builtin_bswap16 (sendSamples.samples1Signal [i].deltaTime);
            //             }
            //         }
            //     else
            //         for (int i = 0; i < sendSamples.sampleCount; i++) {
            //             sendSamples.samples2Signals [i].signal1 = __builtin_bswap16 (sendSamples.samples2Signals [i].signal1);
            //             sendSamples.samples2Signals [i].signal2 = __builtin_bswap16 (sendSamples.samples2Signals [i].signal2);
            //             sendSamples.samples2Signals [i].deltaTime = __builtin_bswap16 (sendSamples.samples2Signals [i].deltaTime);
            //         }
            // }
            // webSocket->sendBinary ((uint8_t *) &sendSamples, sendBytes);
            //
            // lastMillis = millis ();
            // __oscilloscope_h_debug__ ("oscSender: " + String (sendBytes) + " B sent");
        }

        __oscilloscope_h_debug__("oscSender stopped.");
        // Do not delete task here, let the caller handle it if needed.
        // Ensure reader is stopped if we exit due to error
        if (sharedMemory->oscReaderState != STOPPED) {
             sharedMemory->oscReaderState = STOP;
        }
    }

    // main oscilloscope function - it reads request from javascript client then starts two threads: oscilloscope reader (that reads samples ans packs them into buffer) and oscilloscope sender (that sends buffer to javascript client)

    void runOscilloscope (WebSocket *webSocket) {
    
      // set up oscilloscope shared memory that will be shared among all 3 oscilloscope threads
      oscSharedMemory sharedMemory = {};                           // get some memory that will be shared among all oscilloscope threads and initialize it with zerros
      sharedMemory.webSocket = webSocket;                          // put webSocket rference into shared memory
      sharedMemory.readBuffer.samplesAreReady = true;              // this value will be copied into sendBuffer later where this flag will be checked
    
      // oscilloscope protocol starts with binary endian identification from the client
      uint16_t endianIdentification = 0;
      if (webSocket->readBinary ((byte *) &endianIdentification, sizeof (endianIdentification)) == sizeof (endianIdentification))
        sharedMemory.clientIsBigEndian = (endianIdentification == 0xBBAA); // cient has sent 0xAABB
      if (!(endianIdentification == 0xAABB || endianIdentification == 0xBBAA)) {
        #ifdef __DMESG__
            dmesgQueue << "[oscilloscope] communication does not follow oscilloscope protocol. Expected endian identification.";
        #endif
        webSocket->sendString ("[oscilloscope] communication does not follow oscilloscope protocol. Expected endian identification."); // send error also to javascript client
        return;
      }
    
      // oscilloscope protocol continues with (text) start command in the following forms:
      // start digital sampling on GPIO 36 every 250 ms screen width = 10000 ms
      // start analog sampling on GPIO 22, 23 every 100 ms screen width = 400 ms set positive slope trigger to 512 set negative slope trigger to 0
      cstring s = webSocket->readString (); 
      __oscilloscope_h_debug__ ("runOscilloscope: command =  " + String ((char *) s));

      if (s == "") {
        #ifdef __DMESG__
          dmesgQueue << "[oscilloscope] communication does not follow oscilloscope protocol. Expected start oscilloscope parameters.";
        #endif
        webSocket->sendString ("[oscilloscope] communication does not follow oscilloscope protocol. Expected start oscilloscope parameters."); // send error also to javascript client
        return;
      }
      
      // try to parse what we have got from client
      char posNeg1 [9] = "";
      char posNeg2 [9] = "";
      int treshold1;
      int treshold2;
      char *cmdPart1 = (char *) s;
      char *cmdPart2 = strstr (cmdPart1, " every"); 
      char *cmdPart3 = NULL;
      if (cmdPart2) {
        *(cmdPart2++) = 0;
        cmdPart3 = strstr (cmdPart2, " set"); 
        if (cmdPart3) 
          *(cmdPart3++) = 0;
      }
      // parse 1st part
      sharedMemory.gpio1 = sharedMemory.gpio2 = (gpio_num_t) 255; // invalid GPIO
      if (sscanf (cmdPart1, "start %7s sampling on GPIO %2i, %2i", sharedMemory.readType, &sharedMemory.gpio1, &sharedMemory.gpio2) < 2) {
        #ifdef __DMESG__
            dmesgQueue << "[oscilloscope] oscilloscope protocol syntax error.";
        #endif
        webSocket->sendString ("[oscilloscope] oscilloscope protocol syntax error."); // send error also to javascript client
        return;
      }
      // use adc1_get_raw instead of analogRead
      if (!strcmp (sharedMemory.readType, "analog")) {

          #if CONFIG_IDF_TARGET_ESP32
              __oscilloscope_h_debug__ ("MCU ESP32 pinout");

              // ESP32 board: https://docs.espressif.com/projects/esp-idf/en/v4.2/esp32/api-reference/peripherals/adc.html
              switch (sharedMemory.gpio1) {
                  // ADC1
                  case 36: sharedMemory.adcchannel1 = ADC1_CHANNEL_0; break;
                  case 37: sharedMemory.adcchannel1 = ADC1_CHANNEL_1; break;
                  case 38: sharedMemory.adcchannel1 = ADC1_CHANNEL_2; break;
                  case 39: sharedMemory.adcchannel1 = ADC1_CHANNEL_3; break;
                  case 32: sharedMemory.adcchannel1 = ADC1_CHANNEL_4; break;
                  case 33: sharedMemory.adcchannel1 = ADC1_CHANNEL_5; break;
                  case 34: sharedMemory.adcchannel1 = ADC1_CHANNEL_6; break;
                  case 35: sharedMemory.adcchannel1 = ADC1_CHANNEL_7; break;
                  // ADC2 (GPIOs 4, 0, 2, 15, 13, 12, 14, 27, 25, 26), the reading blocks when used together with WiFi?
                  // other GPIOs do not have ADC
                  default:  webSocket->sendString (cstring ("[oscilloscope] can't analogRead GPIO ") + cstring (sharedMemory.gpio1) + "."); // send error also to javascript client
                            return;  
              }
              switch (sharedMemory.gpio2) {
                  // ADC1
                  case 36: sharedMemory.adcchannel2 = ADC1_CHANNEL_0; break;
                  case 37: sharedMemory.adcchannel2 = ADC1_CHANNEL_1; break;
                  case 38: sharedMemory.adcchannel2 = ADC1_CHANNEL_2; break;
                  case 39: sharedMemory.adcchannel2 = ADC1_CHANNEL_3; break;
                  case 32: sharedMemory.adcchannel2 = ADC1_CHANNEL_4; break;
                  case 33: sharedMemory.adcchannel2 = ADC1_CHANNEL_5; break;
                  case 34: sharedMemory.adcchannel2 = ADC1_CHANNEL_6; break;
                  case 35: sharedMemory.adcchannel2 = ADC1_CHANNEL_7; break;
                  // not used
                  case 255: break;
                  // ADC2 (GPIOs 4, 0, 2, 15, 13, 12, 14, 27, 25, 26), the reading blocks when used together with WiFi?
                  // other GPIOs do not have ADC
                  default:  webSocket->sendString (cstring ("[oscilloscope] can't analogRead GPIO ") + cstring (sharedMemory.gpio2) + "."); // send error also to javascript client
                            return;  
              }


          // GPIO to CHANNEL mapping depending on the board type: https://github.com/espressif/arduino-esp32/blob/master/boards.txt
          #elif CONFIG_IDF_TARGET_ESP32S2
              __oscilloscope_h_debug__ ("MCU ESP32S2 pinout");
              // ESP32 S2 board: https://docs.espressif.com/projects/esp-idf/en/v4.4.1/esp32s2/api-reference/peripherals/adc.html
              switch (sharedMemory.gpio1) {
                  // ADC1
                  case  1: sharedMemory.adcchannel1 = ADC1_CHANNEL_0; break;
                  case  2: sharedMemory.adcchannel1 = ADC1_CHANNEL_1; break;
                  case  3: sharedMemory.adcchannel1 = ADC1_CHANNEL_2; break;
                  case  4: sharedMemory.adcchannel1 = ADC1_CHANNEL_3; break;
                  case  5: sharedMemory.adcchannel1 = ADC1_CHANNEL_4; break;
                  case  6: sharedMemory.adcchannel1 = ADC1_CHANNEL_5; break;
                  case  7: sharedMemory.adcchannel1 = ADC1_CHANNEL_6; break;
                  case  8: sharedMemory.adcchannel1 = ADC1_CHANNEL_7; break;
                  case  9: sharedMemory.adcchannel1 = ADC1_CHANNEL_8; break;
                  case 10: sharedMemory.adcchannel1 = ADC1_CHANNEL_9; break;
                  // ADC2 (GPIOs 11, 12, 13, 14, 15, 16, 17, 18, 19, 20), the reading blocks when used together with WiFi?
                  // other GPIOs do not have ADC
                  default:  webSocket->sendString (cstring ("[oscilloscope] can't analogRead GPIO ") + cstring (sharedMemory.gpio1) + "."); // send error also to javascript client
                            return;  
              }
              switch (sharedMemory.gpio2) {
                  // ADC1
                  case  1: sharedMemory.adcchannel2 = ADC1_CHANNEL_0; break;
                  case  2: sharedMemory.adcchannel2 = ADC1_CHANNEL_1; break;
                  case  3: sharedMemory.adcchannel2 = ADC1_CHANNEL_2; break;
                  case  4: sharedMemory.adcchannel2 = ADC1_CHANNEL_3; break;
                  case  5: sharedMemory.adcchannel2 = ADC1_CHANNEL_4; break;
                  case  6: sharedMemory.adcchannel2 = ADC1_CHANNEL_5; break;
                  case  7: sharedMemory.adcchannel2 = ADC1_CHANNEL_6; break;
                  case  8: sharedMemory.adcchannel2 = ADC1_CHANNEL_7; break;
                  case  9: sharedMemory.adcchannel2 = ADC1_CHANNEL_8; break;
                  case 10: sharedMemory.adcchannel2 = ADC1_CHANNEL_9; break;
                  // not used
                  case 255: break;
                  // ADC2 (GPIOs 11, 12, 13, 14, 15, 16, 17, 18, 19, 20), the reading blocks when used together with WiFi?
                  // other GPIOs do not have ADC
                  default:  webSocket->sendString (cstring ("[oscilloscope] can't analogRead GPIO ") + cstring (sharedMemory.gpio2) + "."); // send error also to javascript client
                            return;  
              }

          // #elif CONFIG_IDF_TARGET_ESP32C2

          #elif CONFIG_IDF_TARGET_ESP32S3
              __oscilloscope_h_debug__ ("MCU ESP32S3 pinout");

              // ESP32 S3 board: https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32s3/api-reference/peripherals/adc.html
              switch (sharedMemory.gpio1) {
                  // ADC1
                  case  1: sharedMemory.adcchannel1 = ADC1_CHANNEL_0; break;
                  case  2: sharedMemory.adcchannel1 = ADC1_CHANNEL_1; break;
                  case  3: sharedMemory.adcchannel1 = ADC1_CHANNEL_2; break;
                  case  4: sharedMemory.adcchannel1 = ADC1_CHANNEL_3; break;
                  case  5: sharedMemory.adcchannel1 = ADC1_CHANNEL_4; break;
                  case  6: sharedMemory.adcchannel1 = ADC1_CHANNEL_5; break;
                  case  7: sharedMemory.adcchannel1 = ADC1_CHANNEL_6; break;
                  case  8: sharedMemory.adcchannel1 = ADC1_CHANNEL_7; break;
                  case  9: sharedMemory.adcchannel1 = ADC1_CHANNEL_8; break;
                  case 10: sharedMemory.adcchannel1 = ADC1_CHANNEL_9; break;
                  // ADC2 (GPIOs 11, 12, 13, 14, 15, 16, 17, 18, 19, 20), the reading blocks when used together with WiFi?
                  // other GPIOs do not have ADC
                  default:  webSocket->sendString (cstring ("[oscilloscope] can't analogRead GPIO ") + cstring (sharedMemory.gpio1) + "."); // send error also to javascript client
                            return;  
              }
              switch (sharedMemory.gpio2) {
                  // ADC1
                  case  1: sharedMemory.adcchannel2 = ADC1_CHANNEL_0; break;
                  case  2: sharedMemory.adcchannel2 = ADC1_CHANNEL_1; break;
                  case  3: sharedMemory.adcchannel2 = ADC1_CHANNEL_2; break;
                  case  4: sharedMemory.adcchannel2 = ADC1_CHANNEL_3; break;
                  case  5: sharedMemory.adcchannel2 = ADC1_CHANNEL_4; break;
                  case  6: sharedMemory.adcchannel2 = ADC1_CHANNEL_5; break;
                  case  7: sharedMemory.adcchannel2 = ADC1_CHANNEL_6; break;
                  case  8: sharedMemory.adcchannel2 = ADC1_CHANNEL_7; break;
                  case  9: sharedMemory.adcchannel2 = ADC1_CHANNEL_8; break;
                  case 10: sharedMemory.adcchannel2 = ADC1_CHANNEL_9; break;
                  // not used
                  case 255: break;
                  // ADC2 (GPIOs 11, 12, 13, 14, 15, 16, 17, 18, 19, 20), the reading blocks when used together with WiFi?
                  // other GPIOs do not have ADC
                  default:  webSocket->sendString (cstring ("[oscilloscope] can't analogRead GPIO ") + cstring (sharedMemory.gpio2) + "."); // send error also to javascript client
                            return;  
              }

          #elif CONFIG_IDF_TARGET_ESP32C3
              __oscilloscope_h_debug__ ("MCU ESP32C3 pinout");

              // ESP32 C3 board: https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/hw-reference/esp32c3/user-guide-devkitm-1.html
              switch (sharedMemory.gpio1) {
                  // ADC1
                  case  0: sharedMemory.adcchannel1 = ADC1_CHANNEL_0; break;
                  case  1: sharedMemory.adcchannel1 = ADC1_CHANNEL_1; break;
                  case  2: sharedMemory.adcchannel1 = ADC1_CHANNEL_2; break;
                  case  3: sharedMemory.adcchannel1 = ADC1_CHANNEL_3; break;
                  case  4: sharedMemory.adcchannel1 = ADC1_CHANNEL_4; break;
                  // ADC2 (GPIO 5), the reading blocks when used together with WiFi?
                  // other GPIOs do not have ADC
                  default:  webSocket->sendString (cstring ("[oscilloscope] can't analogRead GPIO ") + cstring (sharedMemory.gpio1) + "."); // send error also to javascript client
                            return;  
              }
              switch (sharedMemory.gpio2) {
                  // ADC1
                  case  0: sharedMemory.adcchannel2 = ADC1_CHANNEL_0; break;
                  case  1: sharedMemory.adcchannel2 = ADC1_CHANNEL_1; break;
                  case  2: sharedMemory.adcchannel2 = ADC1_CHANNEL_2; break;
                  case  3: sharedMemory.adcchannel2 = ADC1_CHANNEL_3; break;
                  case  4: sharedMemory.adcchannel2 = ADC1_CHANNEL_4; break;
                  // not used
                  case 255: break;
                  // ADC2 (GPIO 5), the reading blocks when used together with WiFi?
                  // other GPIOs do not have ADC
                  default:  webSocket->sendString (cstring ("[oscilloscope] can't analogRead GPIO ") + cstring (sharedMemory.gpio2) + "."); // send error also to javascript client
                            return;  
              }

          // #elif CONFIG_IDF_TARGET_ESP32C6

          // #elif CONFIG_IDF_TARGET_ESP32H2
            
          #else
              #error "Your board (CONFIG_IDF_TARGET) is not supported by oscilloscope.h"
          #endif

      }
      
      // parse 2nd part
      if (!cmdPart2) {
        #ifdef __DMESG__
            dmesgQueue << "[oscilloscope] oscilloscope protocol syntax error.";
        #endif
        webSocket->sendString ("[oscilloscope] oscilloscope protocol syntax error."); // send error also to javascript client
        return;        
      }
      if (sscanf (cmdPart2, "every %i %2s screen width = %lu %2s", &sharedMemory.samplingTime, sharedMemory.samplingTimeUnit, &sharedMemory.screenWidthTime, sharedMemory.screenWidthTimeUnit) != 4) {
        #ifdef __DMESG__
            dmesgQueue << "[oscilloscope] oscilloscope protocol syntax error.";
        #endif
        webSocket->sendString ("[oscilloscope] oscilloscope protocol syntax error."); // send error also to javascript client
        return;    
      }
          
      // parse 3rd part
      if (cmdPart3) { 
        switch (sscanf (cmdPart3, "set %8s slope trigger to %i set %8s slope trigger to %i", posNeg1, &treshold1, posNeg2, &treshold2)) {
          case 0: // no trigger
                  break;
          case 4: // two triggers
                  if (!strcmp (posNeg2, "positive")) {
                    sharedMemory.positiveTrigger = true;
                    sharedMemory.positiveTriggerTreshold = treshold2;
                  }
                  if (!strcmp (posNeg2, "negative")) {
                    sharedMemory.negativeTrigger = true;
                    sharedMemory.negativeTriggerTreshold = treshold2;
                  }    
                  // don't break, continue to the next case
          case 2: // one trigger
                  if (!strcmp (posNeg1, "positive")) {
                    sharedMemory.positiveTrigger = true;
                    sharedMemory.positiveTriggerTreshold = treshold1;
                  }
                  if (!strcmp (posNeg1, "negative")) {
                    sharedMemory.negativeTrigger = true;
                    sharedMemory.negativeTriggerTreshold = treshold1;
                  }
                  break;
          default:
                  #ifdef __DMESG__
                      dmesgQueue << "[oscilloscope] oscilloscope protocol syntax error.";
                  #endif
                  webSocket->sendString ("[oscilloscope] oscilloscope protocol syntax error."); // send error also to javascript client
                  return;    
        }
      }

      // check the values and calculate derived values
      if (!(!strcmp (sharedMemory.readType, "analog") || !strcmp (sharedMemory.readType, "digital"))) {
        #ifdef __DMESG__
            dmesgQueue << "[oscilloscope] wrong readType. Read type can only be analog or digital.";
        #endif
        webSocket->sendString ("[oscilloscope] wrong readType. Read type can only be analog or digital."); // send error also to javascript client
        return;    
      }
      if (sharedMemory.gpio1 < 0 || sharedMemory.gpio2 < 0) {
        #ifdef __DMESG__
            dmesgQueue << "[oscilloscope] invalid GPIO.";
        #endif
        webSocket->sendString ("[oscilloscope] invalid GPIO."); // send error also to javascript client
        return;      
      }
      if (!(sharedMemory.samplingTime >= 1 && sharedMemory.samplingTime <= 25000)) {
        #ifdef __DMESG__
            dmesgQueue << "[oscilloscope] invalid sampling time. Sampling time must be between 1 and 25000.";
        #endif
        webSocket->sendString ("[oscilloscope] invalid sampling time. Sampling time must be between 1 and 25000."); // send error also to javascript client
        return;      
      }
      if (strcmp (sharedMemory.samplingTimeUnit, "ms") && strcmp (sharedMemory.samplingTimeUnit, "us")) {
        #ifdef __DMESG__
            dmesgQueue << "[oscilloscope] wrong samplingTimeUnit. Sampling time unit can only be ms or us.";
        #endif
        webSocket->sendString ("[oscilloscope] wrong samplingTimeUnit. Sampling time unit can only be ms or us."); // send error also to javascript client
        return;    
      }

      if (strcmp (sharedMemory.screenWidthTimeUnit, sharedMemory.samplingTimeUnit)) {
        #ifdef __DMESG__
            dmesgQueue << "[oscilloscope] screenWidthTimeUnit must be the same as samplingTimeUnit.";
        #endif        
        webSocket->sendString ("[oscilloscope] screenWidthTimeUnit must be the same as samplingTimeUnit."); // send error also to javascript client
        return;    
      }

      if (sharedMemory.positiveTrigger) {
        if (sharedMemory.positiveTriggerTreshold > 0 && sharedMemory.positiveTriggerTreshold <= (strcmp (sharedMemory.readType, "analog") ? 1 : 4095)) {
          ;// Serial.printf ("[oscilloscope] positive slope trigger treshold = %i\n", sharedMemory.positiveTriggerTreshold);
        } else {
          #ifdef __DMESG__
              dmesgQueue << "[oscilloscope] invalid positive slope trigger treshold (according to other settings).";
          #endif
          webSocket->sendString ("[oscilloscope] invalid positive slope trigger treshold (according to other settings)."); // send error also to javascript client
          return;      
        }
      }
      if (sharedMemory.negativeTrigger) {
        if (sharedMemory.negativeTriggerTreshold >= 0 && sharedMemory.negativeTriggerTreshold < (strcmp (sharedMemory.readType, "analog") ? 1 : 4095)) {
          ;//Serial.printf ("[oscilloscope] negative slope trigger treshold = %i\n", sharedMemory.negativeTriggerTreshold);
        } else {
          #ifdef __DMESG__
              dmesgQueue << "[oscilloscope] invalid negative slope trigger treshold (according to other settings).";
          #endif
          webSocket->sendString ("[oscilloscope] invalid negative slope trigger treshold (according to other settings)."); // send error also to javascript client
          return;      
        }
      }

      // choose the corect oscReader
      void (*oscReader) (void *sharedMemory);
      if (strcmp (sharedMemory.readType, "analog")) {
          oscReader = oscReader_digital; // us sampling interval, 1-2 signals, digital reader
      } else {
          oscReader = oscReader_analog; // us sampling interval, 1-2 signals, analog reader
          #ifdef USE_I2S_INTERFACE
            if (sharedMemory.gpio2 > 39) // 1 signal only
                oscReader = oscReader_analog_1_signal_i2s; // us sampling interval, 1 signal, (fast, DMA) I2S analog reader
          #endif
      }
      if (!strcmp (sharedMemory.samplingTimeUnit, "ms")) {
          oscReader = oscReader_millis; // ms sampling intervl, 1-2 signals, digital or analog reader with 'sample at a time' or 'screen at a time' options
      }

      sharedMemory.oscReaderState = INITIAL;

      #ifdef OSCILLOSCOPE_READER_CORE
          BaseType_t taskCreated = xTaskCreatePinnedToCore (oscReader, "oscReader", 4 * 1024, (void *) &sharedMemory, OSCILLOSCOPE_READER_PRIORITY, NULL, OSCILLOSCOPE_READER_CORE);
      #else
          BaseType_t taskCreated = xTaskCreate (oscReader, "oscReader", 4 * 1024, (void *) &sharedMemory, OSCILLOSCOPE_READER_PRIORITY, NULL);
      #endif
      if (pdPASS != taskCreated) {
            #ifdef __DMESG__
                dmesgQueue << "[oscilloscope] could not start oscReader";
            #endif
            webSocket->sendString ("[oscilloscope] could not start oscReader."); // send error also to javascript client
      } else {

                // send oscReader START signal and wait until STARTED
                sharedMemory.oscReaderState = START; 
                while (sharedMemory.oscReaderState == START) delay (1); 

        // start oscilloscope sender in this thread

        oscSender ((void *) &sharedMemory); 
        // stop reader - we can not simply vTaskDelete (oscReaderHandle) since this could happen in the middle of analogRead which would leave its internal semaphore locked

                // send oscReader STOP signal
                sharedMemory.oscReaderState = STOP; 

                // wait until oscReader STOPPED or error
                while (sharedMemory.oscReaderState != STOPPED) delay (1); 
      }
      
      return;
    }
#endif

