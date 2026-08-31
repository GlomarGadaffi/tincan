#pragma once
// ─────────────────────────────────────────────────────────────────────────
//  PoC configuration — EDIT THESE before flashing.
//  (Kept as #defines for spike simplicity; promote to menuconfig later.)
//
//  Topology:  board REGISTERs as ext 500 (no auth) to the pocket-dial SIP
//  server, then calls ext 102 THROUGH the server. The server brokers
//  signaling only — RTP audio flows peer-to-peer straight to ext 102's phone.
// ─────────────────────────────────────────────────────────────────────────

// Wi-Fi STA credentials (SIP rides Wi-Fi; the board's LoRa is unused).
#define POC_WIFI_SSID        "CHANGE_ME"
#define POC_WIFI_PASS        "CHANGE_ME"

// pocket-dial SIP server (registrar/proxy) currently running on the LAN.
#define POC_SIP_SERVER_IP    "192.168.12.244"
#define POC_SIP_SERVER_PORT  5060

// Our identity and who we call. pocket-dial is an OPEN registrar (no auth).
#define POC_SIP_EXT_SELF     "500"   // we register as this extension
#define POC_SIP_EXT_CALLEE   "113"   // we call this extension via the server
#define POC_SIP_REG_EXPIRES  3600
// Digest secret for Learn/Secure registrar modes (drawbridge: SECURITY ->
// Devices). Leave empty for an Open registrar; only ever sent in answer to a
// 401/407 challenge.
#define POC_SIP_SECRET       ""

// Local ports.
#define POC_SIP_LOCAL_PORT   5060
#define POC_RTP_LOCAL_PORT   4000             // even port; RTCP would be +1

// Audio: G.711 µ-law (PCMU, payload type 0), 8 kHz mono, 20 ms frames.
#define POC_SAMPLE_RATE_HZ   8000
#define POC_FRAME_SAMPLES    160              // 8000 Hz * 0.020 s
#define POC_RTP_PAYLOAD_PCMU 0

// V1.1 PDM mic: the ESP32-S3 hardware PDM->PCM filter is speced for ~16-48 kHz,
// so we capture at 16 kHz and decimate 2:1 down to the 8 kHz G.711 rate.
#define POC_MIC_CAPTURE_HZ   16000
#define POC_MIC_DECIMATE     (POC_MIC_CAPTURE_HZ / POC_SAMPLE_RATE_HZ)   // = 2

// I2S DMA ring depth — explicit, not the IDF default (6 desc x 240 frames, which
// banks up to 180 ms/90 ms of hidden latency). One descriptor = one 20 ms frame,
// at each channel's own sample rate, x4 deep => ~80 ms worst-case ring on either side.
#define POC_SPK_DMA_DESC_NUM 4
#define POC_SPK_DMA_FRAME_NUM POC_FRAME_SAMPLES                 // 160 @ 8 kHz
#define POC_MIC_DMA_DESC_NUM 4
#define POC_MIC_DMA_FRAME_NUM (POC_FRAME_SAMPLES * POC_MIC_DECIMATE)  // 320 @ 16 kHz

// Jitter buffer (PlayoutBuffer) — target depth is the standing cushion read()
// drains toward; max is the hard overrun ceiling (drop-oldest beyond this).
#define POC_JITTER_TARGET_MS 60
#define POC_JITTER_MAX_MS    200

// Full-duplex media task stacks/priority. Mic/TX + Playout run on CPU1
// (otherwise idle); RTP-RX stays on CPU0 near Wi-Fi/lwIP. Priority 5: above
// app_main's supervisor (1), comfortably below lwIP's tcpip_thread (18) and
// Wi-Fi's internal task (~23).
#define POC_TASK_STACK_TX      4096
#define POC_TASK_STACK_RX      4096
#define POC_TASK_STACK_PLAYOUT 4096
#define POC_TASK_PRIO_AUDIO    5

// RTP-RX socket recv timeout — bounds how often that task re-checks shutdown.
#define POC_RTP_RX_TIMEOUT_MS 20
