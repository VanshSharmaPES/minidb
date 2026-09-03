#include "wal.h"

#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

#include <cstring>
#include <vector>

namespace minidb {

namespace {

constexpr size_t kHeaderSize = 8;   // 4-byte length + 4-byte crc
constexpr size_t kMinPayload = 9;   // 1-byte type + 4-byte key_len + 4-byte value_len

void AppendU32(std::string* buf, uint32_t v) {
    char bytes[4];
    std::memcpy(bytes, &v, 4);
    buf->append(bytes, 4);
}

uint32_t ReadU32(const char* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// Loops because write() is allowed to write fewer bytes than asked for.
bool WriteFully(int fd, const char* data, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = ::write(fd, data + written, len - written);
        if (n <= 0) return false;
        written += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

std::unique_ptr<WAL> WAL::Open(const std::string& path, const Options& opts) {
    // O_APPEND makes every write land at the end of the file.
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return nullptr;
    return std::unique_ptr<WAL>(new WAL(fd, opts));
}

WAL::~WAL() {
    if (fd_ >= 0) {
        ::fsync(fd_);
        ::close(fd_);
    }
}

bool WAL::Append(RecordType type, const std::string& key, const std::string& value) {
    std::string payload;
    payload.reserve(kMinPayload + key.size() + value.size());
    payload.push_back(static_cast<char>(type));
    AppendU32(&payload, static_cast<uint32_t>(key.size()));
    payload.append(key);
    AppendU32(&payload, static_cast<uint32_t>(value.size()));
    payload.append(value);

    uint32_t crc = static_cast<uint32_t>(
        ::crc32(0L, reinterpret_cast<const Bytef*>(payload.data()),
                static_cast<uInt>(payload.size())));

    std::string record;
    record.reserve(kHeaderSize + payload.size());
    AppendU32(&record, static_cast<uint32_t>(payload.size()));
    AppendU32(&record, crc);
    record.append(payload);

    // One write() call for the whole record. Not atomic, but it keeps the
    // torn-write window as small as the kernel allows, and replay is built
    // to survive a torn tail anyway.
    if (!WriteFully(fd_, record.data(), record.size())) return false;

    if (opts_.sync_every_write) {
        if (::fsync(fd_) != 0) return false;
    } else {
        ++writes_since_sync_;
        if (opts_.sync_interval > 0 && writes_since_sync_ >= opts_.sync_interval) {
            if (::fsync(fd_) != 0) return false;
            writes_since_sync_ = 0;
        }
    }
    return true;
}

bool WAL::Replay(const std::string& path, const ApplyFn& apply) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return true;  // no log yet: a fresh database, not an error

    std::string data;
    char chunk[65536];
    ssize_t n;
    while ((n = ::read(fd, chunk, sizeof(chunk))) > 0) {
        data.append(chunk, static_cast<size_t>(n));
    }
    bool read_failed = (n < 0);
    ::close(fd);
    if (read_failed) return false;

    size_t pos = 0;
    while (pos < data.size()) {
        // Not enough bytes left for a header: the crash cut the record short.
        if (data.size() - pos < kHeaderSize) break;

        uint32_t length = ReadU32(data.data() + pos);
        uint32_t stored_crc = ReadU32(data.data() + pos + 4);

        // A length that overruns the file means either a genuinely truncated
        // record or a half-written length field. Both are unacknowledged
        // writes, so both get the same treatment and we never need to tell
        // them apart.
        if (length < kMinPayload) break;
        if (data.size() - pos - kHeaderSize < length) break;

        const char* payload = data.data() + pos + kHeaderSize;
        uint32_t actual_crc = static_cast<uint32_t>(
            ::crc32(0L, reinterpret_cast<const Bytef*>(payload),
                    static_cast<uInt>(length)));

        bool is_last_record = (pos + kHeaderSize + length == data.size());
        if (actual_crc != stored_crc) {
            if (is_last_record) break;   // torn tail: accept the file
            return false;                // corruption: fail loudly
        }

        // The CRC already proved these bytes are what was written, so any
        // inconsistency below is corruption, not a crash artifact.
        size_t p = 0;
        RecordType type = static_cast<RecordType>(payload[p]);
        p += 1;
        if (type != RecordType::kPut && type != RecordType::kDelete) return false;

        uint32_t key_len = ReadU32(payload + p);
        p += 4;
        if (p + key_len + 4 > length) return false;
        std::string key(payload + p, key_len);
        p += key_len;

        uint32_t value_len = ReadU32(payload + p);
        p += 4;
        if (p + value_len != length) return false;
        std::string value(payload + p, value_len);

        apply(type, key, value);
        pos += kHeaderSize + length;
    }

    return true;
}

}  // namespace minidb