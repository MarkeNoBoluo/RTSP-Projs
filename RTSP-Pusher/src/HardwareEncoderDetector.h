#pragma once

// Print two-tier encoder availability to stdout.
// Tier 1: "compiled" = avcodec_find_encoder_by_name succeeds.
// Tier 2: "openable" = minimal avcodec_open2/close succeeds.
void printAvailableEncoders();

// Resolve --hw-encoder user string to a concrete encoder name.
// Returns string literal (program lifetime), or nullptr if not found.
// "off" / nullptr -> "libx264"
// "auto"           -> "h264_qsv"  (caller handles probe + fallback)
// "qsv"            -> "h264_qsv" or nullptr
// other            -> pass-through if avcodec_find_encoder_by_name succeeds, else nullptr
const char* resolveEncoderName(const char* hwEncoderSetting);
