#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

#include "check.h"
#include "zb/guest_io.h"
#include "zb/runtime_report.h"

namespace {

std::uint8_t pattern_byte(std::size_t index) {
    return static_cast<std::uint8_t>((index * 131u + (index >> 7) * 17u + 29u) & 0xffu);
}

std::string make_temp_apk_path(const char* tag) {
    const char* tmpdir = std::getenv("TMPDIR");
    const std::string base = (tmpdir != nullptr && tmpdir[0] != '\0') ? tmpdir : "/data/local/tmp";
    return base + "/zb_" + tag + "_" + std::to_string(static_cast<long>(::getpid())) + ".apk";
}

}  // namespace

int main() {
    const std::size_t total_size = 200u * 1024u + 137u;
    std::vector<std::uint8_t> expected(total_size);
    for (std::size_t i = 0; i < total_size; ++i) expected[i] = pattern_byte(i);

    const std::string path = make_temp_apk_path("apk_cache_test");
    const int wfd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    CHECK(wfd >= 0);
    std::size_t written = 0;
    while (written < total_size) {
        const ssize_t n = ::write(wfd, expected.data() + written, total_size - written);
        CHECK(n > 0);
        written += static_cast<std::size_t>(n);
    }
    CHECK(::close(wfd) == 0);

    // Non-cached fd returns nullopt.
    const int rfd = ::open(path.c_str(), O_RDONLY);
    CHECK(rfd >= 0);
    std::uint8_t buf[8192];
    CHECK(!zb::guest_io_apk_read(rfd, buf, 16).has_value());

    // Register as read-only cached APK fd.
    zb::guest_io_note_apk_fd(rfd, true);

    // 1. Sequential small reads crossing 64 KiB boundary (pos 0 -> 70000).
    std::size_t offset = 0;
    while (offset < 70000u) {
        const std::size_t chunk = std::min<std::size_t>(4099u, 70000u - offset);
        const auto got = zb::guest_io_apk_read(rfd, buf, chunk);
        CHECK(got.has_value());
        CHECK(*got == static_cast<std::int32_t>(chunk));
        CHECK(std::memcmp(buf, expected.data() + offset, chunk) == 0);
        offset += chunk;
    }

    // 2. lseek SEEK_CUR / SEEK_SET / SEEK_END + 32-bit overflow + negative EINVAL checks.
    const auto cur = zb::guest_io_apk_lseek(rfd, 0, SEEK_CUR, true);
    CHECK(cur.has_value() && *cur == 70000);
    const auto neg = zb::guest_io_apk_lseek(rfd, -1, SEEK_SET, true);
    CHECK(neg.has_value() && *neg == -EINVAL);
    const auto over32 = zb::guest_io_apk_lseek(rfd, 0x1'0000'0000LL, SEEK_SET, true);
    CHECK(over32.has_value() && *over32 == -EOVERFLOW);
    // Overflow must leave cursor unchanged.
    const auto cur_after_err = zb::guest_io_apk_lseek(rfd, 0, SEEK_CUR, true);
    CHECK(cur_after_err.has_value() && *cur_after_err == 70000);

    const auto tail = zb::guest_io_apk_lseek(rfd, -50, SEEK_END, false);
    CHECK(tail.has_value() && *tail == static_cast<std::int64_t>(total_size - 50));
    const auto eof_read = zb::guest_io_apk_read(rfd, buf, 100);
    CHECK(eof_read.has_value() && *eof_read == 50);
    CHECK(std::memcmp(buf, expected.data() + total_size - 50, 50) == 0);
    const auto past_eof = zb::guest_io_apk_read(rfd, buf, 16);
    CHECK(past_eof.has_value() && *past_eof == 0);

    // 3. pread across chunk boundary does not alter sequential cursor.
    std::uint8_t pbuf[1024];
    const auto pread_res = zb::guest_io_apk_pread(rfd, pbuf, sizeof(pbuf), 65000);
    CHECK(pread_res.has_value() && *pread_res == static_cast<std::int32_t>(sizeof(pbuf)));
    CHECK(std::memcmp(pbuf, expected.data() + 65000, sizeof(pbuf)) == 0);
    const auto still_at_end = zb::guest_io_apk_lseek(rfd, 0, SEEK_CUR, false);
    CHECK(still_at_end.has_value() && *still_at_end == static_cast<std::int64_t>(total_size));

    // 4. readv across chunk boundary (from 65000).
    CHECK(zb::guest_io_apk_lseek(rfd, 65000, SEEK_SET, false).value_or(-1) == 65000);
    std::uint8_t v0[600];
    std::uint8_t v1[1200];
    iovec iov[2]{{v0, sizeof(v0)}, {v1, sizeof(v1)}};
    const auto readv_res = zb::guest_io_apk_readv(rfd, iov, 2);
    CHECK(readv_res.has_value() && *readv_res == 1800);
    CHECK(std::memcmp(v0, expected.data() + 65000, sizeof(v0)) == 0);
    CHECK(std::memcmp(v1, expected.data() + 65600, sizeof(v1)) == 0);

    // 5. dup flushes kernel fd offset back and falls back cleanly for shared cursor reads, while
    //    pread continues hitting cache.
    const int dup_fd = ::dup(rfd);
    CHECK(dup_fd >= 0);
    zb::guest_io_note_dup(rfd, dup_fd);
    CHECK(!zb::guest_io_apk_read(rfd, buf, 16).has_value());
    CHECK(!zb::guest_io_apk_lseek(rfd, 0, SEEK_CUR, true).has_value());
    const off_t kernel_pos = ::lseek(dup_fd, 0, SEEK_CUR);
    CHECK(kernel_pos == 66800);
    CHECK(::read(dup_fd, buf, 64) == 64);
    CHECK(std::memcmp(buf, expected.data() + 66800, 64) == 0);
    // pread on original fd still hits cache.
    CHECK(zb::guest_io_apk_pread(rfd, pbuf, 256, 131000).value_or(-1) == 256);
    CHECK(std::memcmp(pbuf, expected.data() + 131000, 256) == 0);

    zb::guest_log_note_close(dup_fd);
    CHECK(::close(dup_fd) == 0);
    zb::guest_log_note_close(rfd);
    CHECK(::close(rfd) == 0);
    CHECK(!zb::guest_io_apk_pread(rfd, pbuf, 16, 0).has_value());

    std::remove(path.c_str());

    // 6. OBB & pread64 diagnostics + cached read/seek acceleration for read-only OBB fds.
    zb::runtime_report().clear();
    const std::string obb_path =
        make_temp_apk_path("obb_diag_test") + ".obb";
    const int owfd = ::open(obb_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    CHECK(owfd >= 0);
    written = 0;
    while (written < total_size) {
        const ssize_t n = ::write(owfd, expected.data() + written, total_size - written);
        CHECK(n > 0);
        written += static_cast<std::size_t>(n);
    }
    CHECK(::close(owfd) == 0);

    zb::guest_io_note_obb_open("/missing/main.1.obb", -ENOENT, O_RDONLY);
    const int orfd = ::open(obb_path.c_str(), O_RDONLY);
    CHECK(orfd >= 0);
    zb::guest_io_note_obb_open(obb_path.c_str(), orfd, O_RDONLY);
    CHECK(zb::guest_io_is_obb_fd(orfd));
    {
        const std::string open_rep = zb::runtime_report().text();
        CHECK(open_rep.find("io-obb-open: fd=" + std::to_string(orfd)) != std::string::npos);
        CHECK(open_rep.find("cached=1") != std::string::npos);
    }

    // OBB fd uses the read-only chunk cache for lseek/read/pread, without counting into apk-reads.
    CHECK(zb::guest_io_apk_lseek(orfd, 65000, SEEK_SET, false).value_or(-1) == 65000);
    std::uint8_t obb_buf[2048];
    CHECK(zb::guest_io_apk_read(orfd, obb_buf, sizeof(obb_buf)).value_or(-1) ==
          static_cast<std::int32_t>(sizeof(obb_buf)));
    CHECK(std::memcmp(obb_buf, expected.data() + 65000, sizeof(obb_buf)) == 0);
    CHECK(!zb::guest_io_note_read(orfd, sizeof(obb_buf), false));
    CHECK(zb::guest_io_apk_pread(orfd, obb_buf, 512, 130000).value_or(-1) == 512);
    CHECK(std::memcmp(obb_buf, expected.data() + 130000, 512) == 0);

    // O_RDWR OBB open must NOT enable read-only caching.
    const int orwfd = ::open(obb_path.c_str(), O_RDWR);
    CHECK(orwfd >= 0);
    zb::guest_io_note_obb_open(obb_path.c_str(), orwfd, O_RDWR);
    CHECK(!zb::guest_io_apk_read(orwfd, obb_buf, 16).has_value());
    zb::guest_log_note_close(orwfd);
    CHECK(::close(orwfd) == 0);

    const int odup = ::dup(orfd);
    CHECK(odup >= 0);
    zb::guest_io_note_dup(orfd, odup);
    CHECK(zb::guest_io_is_obb_fd(odup));

    CHECK(zb::guest_io_note_obb_io(odup, zb::ObbIoOp::kPread64, 512, 128, 128));
    CHECK(!zb::guest_io_note_obb_io(odup, zb::ObbIoOp::kLseek, 0, SEEK_END, total_size));
    CHECK(!zb::guest_io_note_obb_io(odup, zb::ObbIoOp::kPread64, 0x90000000ULL, 64, -EINVAL));
    zb::guest_io_note_pread64(odup, 0x90000000LL, 64, -EINVAL, false);

    const std::string rep = zb::runtime_report().text();
    CHECK(rep.find("io-obb-opens: ok=2 fail=1") != std::string::npos);
    CHECK(rep.find("io-obb-open: fd=" + std::to_string(orwfd)) != std::string::npos);
    CHECK(rep.find("cached=0") != std::string::npos);
    CHECK(rep.find("io-obb-open-fail: errno=2") != std::string::npos);
    CHECK(rep.find("io-obb-first-pread: fd=" + std::to_string(odup)) != std::string::npos);
    CHECK(rep.find("io-obb-first-err: op=pread64") != std::string::npos);
    CHECK(rep.find("io-obb-reads:") != std::string::npos);
    CHECK(rep.find("io-pread-all:") != std::string::npos);
    CHECK(rep.find("io-pread-err-1:") != std::string::npos);
    CHECK(rep.find("io-pread-gt2g-1:") != std::string::npos);
    CHECK(rep.find("io-cache:") != std::string::npos);

    zb::guest_log_note_close(odup);
    CHECK(::close(odup) == 0);
    CHECK(!zb::guest_io_is_obb_fd(odup));
    zb::guest_log_note_close(orfd);
    CHECK(!zb::guest_io_is_cached_fd(orfd));
    CHECK(::close(orfd) == 0);
    CHECK(!zb::guest_io_is_obb_fd(orfd));
    std::remove(obb_path.c_str());

    // 7. Stress sharded LRU eviction, open-addressing backward-shift deletion, and async read-ahead.
    zb::guest_io_set_mmap_enabled(false);
    const std::string big_path = make_temp_apk_path("zb_big_cache_test") + ".obb";
    constexpr std::size_t kBigChunks = 640;  // Exceeds 512 total slots across 16 shards.
    constexpr std::size_t kChunk = 64u * 1024u;
    {
        const int wfd = ::open(big_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        CHECK(wfd >= 0);
        std::vector<std::uint8_t> blk(kChunk);
        for (std::size_t b = 0; b < kBigChunks; ++b) {
            for (std::size_t j = 0; j < kChunk; ++j) {
                blk[j] = static_cast<std::uint8_t>((b * 37u + j * 13u) & 0xFFu);
            }
            std::size_t off = 0;
            while (off < kChunk) {
                const ssize_t n = ::write(wfd, blk.data() + off, kChunk - off);
                CHECK(n > 0);
                off += static_cast<std::size_t>(n);
            }
        }
        CHECK(::close(wfd) == 0);
    }
    const int bfd = ::open(big_path.c_str(), O_RDONLY);
    CHECK(bfd >= 0);
    zb::guest_io_note_obb_open(big_path.c_str(), bfd, O_RDONLY);
    CHECK(zb::guest_io_is_cached_fd(bfd));
    std::vector<std::uint8_t> rbuf(90000);
    for (std::size_t i = 0; i < kBigChunks; i += 1) {
        const off_t pos = static_cast<off_t>(i * kChunk + 123);
        const std::size_t rlen = (i % 7 == 0 && i + 1 < kBigChunks) ? 80000u : 1024u;
        CHECK(zb::guest_io_apk_lseek(bfd, pos, SEEK_SET, false).value_or(-1) == pos);
        CHECK(zb::guest_io_apk_read(bfd, rbuf.data(), rlen).value_or(-1) ==
              static_cast<std::int32_t>(rlen));
        for (std::size_t k = 0; k < rlen; ++k) {
            const std::size_t abs = static_cast<std::size_t>(pos) + k;
            const std::size_t b = abs / kChunk;
            const std::size_t j = abs % kChunk;
            const std::uint8_t exp = static_cast<std::uint8_t>((b * 37u + j * 13u) & 0xFFu);
            CHECK(rbuf[k] == exp);
        }
    }
    // Re-read early chunks after eviction to ensure post-eviction hash lookups work cleanly.
    for (std::size_t b : {std::size_t{0}, std::size_t{1}, std::size_t{17}, std::size_t{255}}) {
        CHECK(zb::guest_io_apk_pread(bfd, rbuf.data(), 4096, static_cast<off_t>(b * kChunk + 500))
                  .value_or(-1) == 4096);
        for (std::size_t k = 0; k < 4096; ++k) {
            const std::uint8_t exp = static_cast<std::uint8_t>((b * 37u + (500u + k) * 13u) & 0xFFu);
            CHECK(rbuf[k] == exp);
        }
    }
    zb::guest_log_note_close(bfd);
    CHECK(!zb::guest_io_is_cached_fd(bfd));
    CHECK(::close(bfd) == 0);
    std::remove(big_path.c_str());

    // 8. Verify 64-bit host mmap zero-copy archive path (>= 1 MiB) + shared reuse across opens.
    zb::guest_io_set_mmap_enabled(true);
    const std::string mmap_path = make_temp_apk_path("zb_mmap_cache_test") + ".obb";
    constexpr std::size_t kMmapBytes = 2u * 1024u * 1024u + 513u;
    {
        const int wfd = ::open(mmap_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        CHECK(wfd >= 0);
        std::vector<std::uint8_t> data(kMmapBytes);
        for (std::size_t i = 0; i < kMmapBytes; ++i) {
            data[i] = static_cast<std::uint8_t>((i * 73u + (i >> 9)) & 0xFFu);
        }
        std::size_t off = 0;
        while (off < kMmapBytes) {
            const ssize_t n = ::write(wfd, data.data() + off, kMmapBytes - off);
            CHECK(n > 0);
            off += static_cast<std::size_t>(n);
        }
        CHECK(::close(wfd) == 0);
    }
    const int mfd1 = ::open(mmap_path.c_str(), O_RDONLY);
    CHECK(mfd1 >= 0);
    zb::guest_io_note_obb_open(mmap_path.c_str(), mfd1, O_RDONLY);
    CHECK(zb::guest_io_is_cached_fd(mfd1));
    CHECK(zb::guest_io_apk_lseek(mfd1, 1048500, SEEK_SET, false).value_or(-1) == 1048500);
    std::vector<std::uint8_t> mbuf(8192);
    CHECK(zb::guest_io_apk_read(mfd1, mbuf.data(), mbuf.size()).value_or(-1) ==
          static_cast<std::int32_t>(mbuf.size()));
    for (std::size_t i = 0; i < mbuf.size(); ++i) {
        const std::size_t abs = 1048500u + i;
        CHECK(mbuf[i] == static_cast<std::uint8_t>((abs * 73u + (abs >> 9)) & 0xFFu));
    }
    CHECK(zb::guest_io_apk_pread(mfd1, mbuf.data(), 4096, 1500000).value_or(-1) == 4096);
    for (std::size_t i = 0; i < 4096; ++i) {
        const std::size_t abs = 1500000u + i;
        CHECK(mbuf[i] == static_cast<std::uint8_t>((abs * 73u + (abs >> 9)) & 0xFFu));
    }
    const std::string mrep = zb::runtime_report().text();
    CHECK(mrep.find("mmap_hits=") != std::string::npos);
    zb::guest_log_note_close(mfd1);
    CHECK(::close(mfd1) == 0);
    std::remove(mmap_path.c_str());
    return 0;
}
