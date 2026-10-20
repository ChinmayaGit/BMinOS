// BMinOS - Bare Minimum Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Streaming ISO Download Manager
// ==============================
// High-throughput, low-memory HTTP/HTTPS streaming downloader designed
// specifically for bare-metal hypervisor environments:
//   - Streams ISO data directly from network NIC to disk or pendrive (no high RAM buffer needed)
//   - Supports HTTP Range headers for resuming interrupted multi-gigabyte ISO downloads
//   - Computes on-the-fly SHA-256 hash to verify ISO integrity before boot
//   - Multi-threaded chunk downloads and bandwidth throttling

#pragma once

#include "kernel/common/types.hpp"

namespace bminos::net {

enum class DownloadStatus : u8 {
    Idle,
    Connecting,
    Downloading,
    Paused,
    VerifyingChecksum,
    Complete,
    Error,
};

struct DownloadProgress {
    DownloadStatus status;
    u64            bytes_received;
    u64            total_bytes;
    u32            bytes_per_sec;
    u32            eta_seconds;
    u8             percent;
    FixedString<68> calculated_sha256;
    FixedString<128> error_message;
};

class Downloader {
public:
    static constexpr u32 kChunkBufferSize = 65536; // 64 KB streaming buffer

    /// Initialize network stack & download subsystem
    static void init();

    /// Start a streaming download from url directly to target_path (file or /dev/pendrive)
    static Result<void> start(const char* url, const char* target_path, const char* expected_sha256 = nullptr);

    /// Pause active download
    static void pause();

    /// Resume paused download (uses HTTP Range header)
    static Result<void> resume();

    /// Cancel and clean up partial download
    static void cancel();

    /// Query live download statistics
    static DownloadProgress get_progress();

    /// Verify SHA-256 hash of a file on disk or pendrive
    static bool verify_sha256(const char* file_path, const char* expected_sha256);
};

} // namespace bminos::net

