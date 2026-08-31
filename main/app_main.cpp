// ─────────────────────────────────────────────────────────────────────────
//  tincan  —  app_main
//
//  Flow: Wi-Fi -> bring up mic+speaker -> REGISTER (TincanUac, tincan-core)
//  -> idle loop. BOOT button places a call to POC_SIP_EXT_CALLEE through the
//  server; an inbound INVITE (another extension, drawbridge's ring-all, or
//  its register-beep) is auto-answered -- this is an intercom. Either way
//  the call runs the FULL-DUPLEX G.711 media engine (media_tasks.cpp):
//  mic->RTP and RTP->speaker concurrently on separate tasks with a jitter
//  buffer. Holding BOOT during a call hangs up. Then back to idle for the
//  next call -- no reset needed.
//
//  Full duplex without acoustic echo cancellation WILL produce audible echo
//  (the far end hears themselves) — AEC is tracked separately. This stage is
//  the concurrency/jitter-buffer foundation AEC plugs into.
// ─────────────────────────────────────────────────────────────────────────
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include <lwip/sockets.h>

#include "poc_config.h"
#include "board_mvsr.h"
#include "net_wifi.h"
#include "audio_io.h"
#include "tincan_uac.hpp"
#include "media_tasks.h"

static const char *TAG = "app";

static void gpio_init(void)
{
    gpio_set_direction(MVSR_MOTOR, GPIO_MODE_OUTPUT);
    gpio_set_level(MVSR_MOTOR, 0);
    gpio_set_direction(MVSR_PTT_BUTTON, GPIO_MODE_INPUT);
    gpio_set_pull_mode(MVSR_PTT_BUTTON, GPIO_PULLUP_ONLY);
}

static void motor_buzz(int ms)
{
    gpio_set_level(MVSR_MOTOR, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(MVSR_MOTOR, 0);
}

static bool button_pressed(void)
{
    return gpio_get_level(MVSR_PTT_BUTTON) == 0;   // active LOW
}

// Run one call's media to completion, with haptics and amp power around it.
static void run_call(TincanUac &uac)
{
    // If the press that dialled is still held, don't let it read as hangup.
    while (button_pressed()) vTaskDelay(pdMS_TO_TICKS(20));

    motor_buzz(60);                                // connected — short confirm buzz
    audio_amp_enable(true);                        // power the speaker amp only for the live call
    media_run_full_duplex(uac, button_pressed);    // returns when the call ends
    audio_amp_enable(false);                       // amp off -> silent on idle (no underrun click)
    motor_buzz(120);                               // call-ended haptic
    (void)uac.callEnded();                         // consume the one-shot
    ESP_LOGI(TAG, "call done — idle");
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    gpio_init();

    // Bring up the PDM mic + speaker FIRST, so hardware init is validated and
    // logged on boot — before the (blocking) Wi-Fi join.
    ESP_ERROR_CHECK(audio_init());

    ESP_ERROR_CHECK(wifi_sta_connect(POC_WIFI_SSID, POC_WIFI_PASS));

    ESP_LOGI(TAG, "local IP %s — registering ext %s with server %s:%d",
             wifi_local_ip(), POC_SIP_EXT_SELF, POC_SIP_SERVER_IP, POC_SIP_SERVER_PORT);

    static TincanUac uac;
    if (!uac.init(wifi_local_ip(), POC_SIP_LOCAL_PORT, POC_RTP_LOCAL_PORT,
                  POC_SIP_SERVER_IP, POC_SIP_SERVER_PORT, POC_SIP_EXT_SELF,
                  POC_SIP_REG_EXPIRES)) {
        ESP_LOGE(TAG, "UAC init failed (socket bind?)");
        return;
    }
    // Bounds the RX media task's shutdown-poll interval (it blocks in recvfrom).
    struct timeval rtv = { 0, POC_RTP_RX_TIMEOUT_MS * 1000 };
    setsockopt(uac.rtpSocket(), SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

    if (!uac.registerExt()) {
        // Not fatal: maintainRegistration() is not armed until the first
        // success, so retry here until the server shows up.
        ESP_LOGE(TAG, "registration failed — check server IP and network; retrying");
        while (!uac.registerExt()) vTaskDelay(pdMS_TO_TICKS(10000));
    }
    motor_buzz(150);   // registered

    bool prevPressed = false;
    for (;;) {
        uac.poll();
        if (uac.maintainRegistration()) {
            ESP_LOGW(TAG, "registration refresh failed");
        }

        if (uac.hasIncomingCall()) {
            ESP_LOGI(TAG, "incoming call from %s — auto-answering", uac.incomingCallerId().c_str());
            motor_buzz(150);
            if (uac.answer()) run_call(uac);
            prevPressed = false;
            continue;
        }

        bool pressed = button_pressed();
        if (pressed && !prevPressed) {
            ESP_LOGI(TAG, "calling ext %s through the server ...", POC_SIP_EXT_CALLEE);
            motor_buzz(150);
            if (uac.placeCall(POC_SIP_EXT_CALLEE)) {
                run_call(uac);
            } else {
                ESP_LOGE(TAG, "call to %s failed (is it registered/online?)", POC_SIP_EXT_CALLEE);
                (void)uac.callEnded();
            }
            prevPressed = false;
            continue;
        }
        prevPressed = pressed;

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
