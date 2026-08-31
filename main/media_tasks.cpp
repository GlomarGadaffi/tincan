#include "media_tasks.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_random.h"

#include <lwip/sockets.h>

#include "poc_config.h"
#include "audio_io.h"
#include "g711.h"
#include "rtp.h"
#include "playout_buffer.hpp"

static const char *TAG = "media";

// PlayoutBuffer (tincan-core) takes its sizing at construction; derive it
// from this board's config here rather than inside the shared component.
#define PLAYOUT_TARGET_SAMPLES ((POC_JITTER_TARGET_MS) * (POC_SAMPLE_RATE_HZ) / 1000)
#define PLAYOUT_MAX_SAMPLES    ((POC_JITTER_MAX_MS) * (POC_SAMPLE_RATE_HZ) / 1000)

#define MEDIA_BIT_SHUTDOWN       BIT0
#define MEDIA_BIT_TX_EXITED      BIT1
#define MEDIA_BIT_RX_EXITED      BIT2
#define MEDIA_BIT_PLAYOUT_EXITED BIT3
#define MEDIA_BITS_ALL_EXITED (MEDIA_BIT_TX_EXITED | MEDIA_BIT_RX_EXITED | MEDIA_BIT_PLAYOUT_EXITED)

namespace {

// Read-only-after-construction handles shared by the three media tasks.
struct MediaCtx {
    int rtp_sock;
    sockaddr_in dst;
    PlayoutBuffer *jbuf;
    EventGroupHandle_t evt;
};

inline bool shuttingDown(MediaCtx *ctx)
{
    return (xEventGroupGetBits(ctx->evt) & MEDIA_BIT_SHUTDOWN) != 0;
}

// mic -> G.711 -> RTP send. Paced by i2s_channel_read()'s natural ~20 ms DMA
// fill — never touches the network's receive side.
void mediaTxTask(void *param)
{
    auto *ctx = static_cast<MediaCtx *>(param);
    int16_t pcm[POC_FRAME_SAMPLES];
    uint8_t pkt[RTP_HEADER_LEN + POC_FRAME_SAMPLES];
    rtp_tx_t tx = { (uint16_t)esp_random(), esp_random(), esp_random() };
    bool first = true;   // RTP marker bit: set once, on the very first frame of the call

    ESP_LOGI(TAG, "tx task up (core %d)", xPortGetCoreID());

    while (!shuttingDown(ctx)) {
        size_t got = audio_read_mic(pcm, POC_FRAME_SAMPLES);
        if (got > 0) {
            g711_ulaw_encode_buf(pcm, pkt + RTP_HEADER_LEN, got);
            rtp_write_header(pkt, POC_RTP_PAYLOAD_PCMU, first ? 1 : 0,
                              tx.seq, tx.timestamp, tx.ssrc);
            sendto(ctx->rtp_sock, pkt, RTP_HEADER_LEN + got, 0,
                   (const sockaddr *)&ctx->dst, sizeof(ctx->dst));
            tx.seq++;
            tx.timestamp += (uint32_t)got;
            first = false;
        }
    }

    ESP_LOGI(TAG, "tx task exiting");
    xEventGroupSetBits(ctx->evt, MEDIA_BIT_TX_EXITED);
    vTaskDelete(NULL);
}

// RTP recv -> validate -> G.711 decode -> jitter buffer write. Never touches
// the speaker directly — only Playout (below) calls audio_write_spk(), so a
// stalled/jittery network can never starve it.
void mediaRxTask(void *param)
{
    auto *ctx = static_cast<MediaCtx *>(param);
    uint8_t rx[RTP_HEADER_LEN + POC_FRAME_SAMPLES * 2];
    int16_t pcm[POC_FRAME_SAMPLES];
    rtp_rx_track_t track = {};
    uint32_t rejected = 0;   // packets from an unexpected source endpoint

    ESP_LOGI(TAG, "rx task up (core %d)", xPortGetCoreID());

    while (!shuttingDown(ctx)) {
        sockaddr_in src{};
        socklen_t slen = sizeof(src);
        int n = recvfrom(ctx->rtp_sock, rx, sizeof(rx), 0, (sockaddr *)&src, &slen);
        if (n <= 0) continue;   // SO_RCVTIMEO timeout or error -> loop back, recheck shutdown

        // Only accept RTP from the peer endpoint learned from the call's SDP
        // (today's spike accepted any UDP datagram to the RTP port). Some
        // phones/PBXes send RTP from a different port than advertised in SDP
        // (symmetric-RTP/NAT quirks) -- if that ever happens, log it: every
        // packet getting silently dropped here looks identical on the wire
        // to "connected but silent," with nothing else to explain why.
        if (src.sin_addr.s_addr != ctx->dst.sin_addr.s_addr ||
            src.sin_port != ctx->dst.sin_port) {
            if (++rejected == 1 || (rejected % 250) == 0) {
                const uint8_t *got = (const uint8_t *)&src.sin_addr.s_addr;
                const uint8_t *want = (const uint8_t *)&ctx->dst.sin_addr.s_addr;
                ESP_LOGW(TAG, "rx: dropping RTP from %u.%u.%u.%u:%u, expected %u.%u.%u.%u:%u (x%u)",
                         got[0], got[1], got[2], got[3], (unsigned)ntohs(src.sin_port),
                         want[0], want[1], want[2], want[3], (unsigned)ntohs(ctx->dst.sin_port),
                         (unsigned)rejected);
            }
            continue;
        }

        rtp_rx_hdr_t hdr;
        if (!rtp_parse_header(rx, (size_t)n, &hdr)) continue;
        if (hdr.pt != POC_RTP_PAYLOAD_PCMU) continue;
        rtp_rx_track_update(&track, hdr.seq);

        size_t plen = (size_t)n - hdr.payload_offset;
        if (plen > POC_FRAME_SAMPLES) plen = POC_FRAME_SAMPLES;
        g711_ulaw_decode_buf(rx + hdr.payload_offset, pcm, plen);
        ctx->jbuf->write(pcm, plen);
    }

    ESP_LOGI(TAG, "rx task exiting — recv=%u lost=%u oo=%u rejected=%u",
             (unsigned)track.received, (unsigned)track.lost, (unsigned)track.out_of_order,
             (unsigned)rejected);
    xEventGroupSetBits(ctx->evt, MEDIA_BIT_RX_EXITED);
    vTaskDelete(NULL);
}

// Jitter buffer -> speaker. Paced only by I2S DMA backpressure (audio_write_spk's
// blocking i2s_channel_write) — never blocks on the network, so RTP arrival
// jitter/loss can never starve the speaker. This is the structural fix for the
// old design's "speaker never fed during the talk branch" starvation gap:
// Playout feeds it unconditionally every iteration regardless of local mic activity.
void mediaPlayoutTask(void *param)
{
    auto *ctx = static_cast<MediaCtx *>(param);
    int16_t pcm[POC_FRAME_SAMPLES];

    ESP_LOGI(TAG, "playout task up (core %d)", xPortGetCoreID());

    while (!shuttingDown(ctx)) {
        ctx->jbuf->read(pcm, POC_FRAME_SAMPLES);   // always fills: real audio or comfort noise
        size_t put = audio_write_spk(pcm, POC_FRAME_SAMPLES);
        if (put == 0) {
            // i2s_channel_write failed instead of blocking ~20ms as normal --
            // without this, a fast-failing write would spin this loop far
            // faster than audio-rate, draining the jitter buffer at memcpy
            // speed instead of pacing on real DMA backpressure.
            ESP_LOGW(TAG, "playout: speaker write failed");
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    ESP_LOGI(TAG, "playout task exiting");
    xEventGroupSetBits(ctx->evt, MEDIA_BIT_PLAYOUT_EXITED);
    vTaskDelete(NULL);
}

}  // namespace

void media_run_full_duplex(TincanUac &uac, bool (*localHangup)(void))
{
    // The media endpoint is published by the UAC once the SDP answer is in.
    // On a delayed-offer INVITE (drawbridge's inbound ring-all) that is the
    // ACK, which can trail answer() by a round trip -- so poll briefly for it
    // rather than failing the call at t=0.
    uint32_t ipBe = 0;
    uint16_t portBe = 0;
    for (int i = 0; i < 100 && uac.inCall() && !uac.mediaTarget(ipBe, portBe); i++) {
        uac.poll();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!uac.inCall()) return;
    if (!uac.mediaTarget(ipBe, portBe)) {
        ESP_LOGE(TAG, "no media endpoint after 2 s -- hanging up");
        uac.hangup();
        return;
    }
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = ipBe;
    dst.sin_port = portBe;
    uac.flushRtpBacklog();   // don't inherit pre-answer backlog as latency

    PlayoutBuffer jbuf(PLAYOUT_MAX_SAMPLES, PLAYOUT_TARGET_SAMPLES);
    EventGroupHandle_t evt = xEventGroupCreate();
    if (evt == NULL) {
        ESP_LOGE(TAG, "xEventGroupCreate failed (out of heap?) — aborting full-duplex media");
        return;
    }
    MediaCtx ctx{ uac.rtpSocket(), dst, &jbuf, evt };

    ESP_LOGI(TAG, "full-duplex media up (jitter target=%dms max=%dms)",
             POC_JITTER_TARGET_MS, POC_JITTER_MAX_MS);

    // TX + Playout on CPU1 (idle today, and the pair Phase B's AEC reference
    // signal will couple). RX stays on CPU0, naturally network-I/O-bound.
    // If a task fails to spawn (heap pressure), mark its EXITED bit
    // ourselves right now — it will never run to set it, and the teardown
    // wait below must not block forever on a bit that can never arrive.
    if (xTaskCreatePinnedToCore(mediaTxTask, "media_tx", POC_TASK_STACK_TX,
                                 &ctx, POC_TASK_PRIO_AUDIO, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn media_tx task");
        xEventGroupSetBits(ctx.evt, MEDIA_BIT_TX_EXITED);
    }
    if (xTaskCreatePinnedToCore(mediaRxTask, "media_rx", POC_TASK_STACK_RX,
                                 &ctx, POC_TASK_PRIO_AUDIO, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn media_rx task");
        xEventGroupSetBits(ctx.evt, MEDIA_BIT_RX_EXITED);
    }
    if (xTaskCreatePinnedToCore(mediaPlayoutTask, "media_playout", POC_TASK_STACK_PLAYOUT,
                                 &ctx, POC_TASK_PRIO_AUDIO, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn media_playout task");
        xEventGroupSetBits(ctx.evt, MEDIA_BIT_PLAYOUT_EXITED);
    }

    // Supervisor: drive the UAC's signalling (BYE from the peer ends the
    // call; OPTIONS keepalives get answered so the registrar keeps us) at a
    // coarse 50 ms cadence, plus a periodic jitter-buffer stats log for
    // hardware validation.
    TickType_t lastStats = xTaskGetTickCount();
    for (;;) {
        uac.poll();
        if (!uac.inCall()) {
            ESP_LOGI(TAG, "call ended by peer");
            break;
        }
        if (localHangup && localHangup()) {
            ESP_LOGI(TAG, "local hangup");
            uac.hangup();
            break;
        }
        if (xTaskGetTickCount() - lastStats >= pdMS_TO_TICKS(5000)) {
            ESP_LOGI(TAG, "jbuf: len=%u underruns=%llu overruns=%llu",
                     (unsigned)jbuf.getLength(),
                     (unsigned long long)jbuf.getUnderruns(),
                     (unsigned long long)jbuf.getOverruns());
            lastStats = xTaskGetTickCount();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Signal shutdown and wait for all three tasks to confirm exit before
    // returning — caller (app_main) gates audio_amp_enable(false) and
    // close(rtp_sock) on this, so nothing tears down hardware/the socket out
    // from under a still-running task.
    xEventGroupSetBits(ctx.evt, MEDIA_BIT_SHUTDOWN);
    EventBits_t done = xEventGroupWaitBits(ctx.evt, MEDIA_BITS_ALL_EXITED,
                                            pdFALSE, pdTRUE, pdMS_TO_TICKS(1000));
    if ((done & MEDIA_BITS_ALL_EXITED) != MEDIA_BITS_ALL_EXITED) {
        // Should be unreachable on a quiet LAN -- every task's blocking call
        // (200ms I2S timeouts, 20ms SO_RCVTIMEO) is bounded well under 1s.
        // But ctx/jbuf are stack-locals below: never proceed to delete the
        // event group and return while a task might still touch them, or a
        // slow task turns into a use-after-free instead of just a log line.
        ESP_LOGE(TAG, "media task teardown slow (bits=0x%x) — waiting it out", (unsigned)done);
        xEventGroupWaitBits(ctx.evt, MEDIA_BITS_ALL_EXITED, pdFALSE, pdTRUE, portMAX_DELAY);
    }

    vEventGroupDelete(ctx.evt);
}
