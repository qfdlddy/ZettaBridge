#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include <sys/types.h>
#include <sys/uio.h>

namespace zb {

// Two black-screen diagnostics that need the syscall layer's view of the guest, nothing more.
//
// 1. Guest engine log capture. Bionic's liblog sends every log record to logd's socket
//    (/dev/socket/logdw). Some ROMs hide third-party logcat output, so the engine's own error
//    text is unreachable by any other means, yet it is usually the fastest way to name a
//    failure. The hooks below copy each record into the runtime report, which the launcher does
//    persist. Nothing here changes a syscall's arguments, result or errno: the record is parsed
//    after the real write has been handed to the kernel.
//
// 2. Plugin APK read meter & read-only chunk cache. Unity parses its own base.apk instead of going
//    through AAsset*, issuing tens of thousands of small sequential reads. For O_RDONLY regular
//    plugin APK fds we meter reads and cache 64 KiB read-only chunks in user space so sequential
//    small reads avoid repeated kernel read/lseek transitions.
void guest_log_note_connect(int fd, const void* address, std::uint32_t length);
void guest_log_note_close(int fd);
void guest_log_note_write(int fd, const void* data, std::size_t size);
void guest_log_note_writev(int fd, const struct iovec* iov, int count);

void guest_io_note_apk_fd(int fd, bool read_only = false);
// Fast user-space read / pread / readv / lseek backed by the shared read-only APK chunk cache.
// Returns std::nullopt when fd is not an active cached APK cursor, leaving the caller to run the
// underlying kernel syscall unchanged.
std::optional<std::int32_t> guest_io_apk_read(int fd, void* buf, std::size_t size);
std::optional<std::int32_t> guest_io_apk_pread(int fd, void* buf, std::size_t size, off_t offset);
std::optional<std::int32_t> guest_io_apk_readv(int fd, const struct iovec* iov, int count);
std::optional<std::int64_t> guest_io_apk_lseek(int fd, std::int64_t offset, int whence,
                                               bool limit_32bit);
// Returns true when fd is a tracked plugin-APK fd, so the caller can name the guest site that
// is reading the archive.
bool guest_io_note_read(int fd, std::size_t size, bool positioned);
// A successful dup/dup2/dup3/fcntl(F_DUPFD): clears any previous state on new_fd (for dup2/dup3),
// flushes tracked kernel position on old_fd and copies the log/APK flags.
void guest_io_note_dup(int old_fd, int new_fd);
// Total bytes read from plugin APK fds so far (for the hang watchdog's heartbeat).
std::uint64_t guest_io_apk_bytes();

// OBB & pread64 diagnostics: meters real kernel I/O on .obb descriptors (and all pread64 calls)
// without altering syscall behavior, so black-screen / asset-loading runs show exact offsets,
// counts, errors, and guest reader call sites even after fonts evict opened-1..16.
enum class ObbIoOp : std::uint8_t {
    kRead,
    kPread64,
    kReadv,
    kLseek,
    kLlseek,
    kMmap2,
    kFstat64,
};

void guest_io_note_obb_open(const char* path, int fd_or_neg_errno, int open_flags);
bool guest_io_is_obb_fd(int fd);
bool guest_io_is_cached_fd(int fd);
void guest_io_set_mmap_enabled(bool enabled);
bool guest_io_note_obb_io(int fd, ObbIoOp op, std::uint64_t offset, std::size_t req_len,
                          std::int64_t result);
void guest_io_note_pread64(int fd, std::int64_t offset, std::size_t len, std::int32_t res,
                           bool fast_cached);

}  // namespace zb
