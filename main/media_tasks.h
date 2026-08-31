#pragma once
// Full-duplex media engine: three FreeRTOS tasks (mic/TX, RTP-RX, playout)
// replacing the original single-task half-duplex media_loop(). TX and
// Playout run on CPU1 (otherwise idle); RX stays on CPU0 near Wi-Fi/lwIP.
// A jitter buffer (PlayoutBuffer, tincan-core) decouples playout pacing from
// RTP arrival jitter, so the speaker never blocks on the network.
#include "tincan_uac.hpp"

// Runs full-duplex media against the UAC's RTP socket and the media endpoint
// it published for the current call, until the call ends -- peer BYE (seen
// via uac.poll()) or localHangup() returning true (e.g. a button), in which
// case we send the BYE. Blocks the calling task; spawns and tears down its
// own three media tasks internally. localHangup may be null.
//
// The caller owns amp/mic power around this call and must have set
// SO_RCVTIMEO on uac.rtpSocket() (bounds the RX task's shutdown-poll interval).
void media_run_full_duplex(TincanUac &uac, bool (*localHangup)(void));
