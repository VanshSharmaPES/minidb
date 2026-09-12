#include "sstable.h"

#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace minidb {

namespace {

constexpr uint64_t kMagic = 0x6d696e6964625353ULL;  // "minidbSS" as little-endian bytes
constexpr size_t kFooterSize = 24;  // index_offset(8) + index_count(4) + data_crc32(4) + magic(8)

void AppendU32(std::string* buf, uint32_t v) {
    char bytes[4];
    std::memcpy(bytes, &v, 4);
    buf->append(bytes, 4);
}

void AppendU64(std::string* buf, uint64_t v) {
    char bytes[8];
    std::memcpy(bytes, &v, 8);
    buf->append(bytes, 8);
}

uint32_t ReadU32(const char* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

uint64_t ReadU64(const char* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

bool WriteFully(int fd, const char* data, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = ::write(fd, data + written, len - written);
        if (n <= 0) return false;
        written += static_cast<size_t>(n);
    }
    return true;
}

// One index entry per this many data records. Small enough to keep Scan's
// "walk from the nearest earlier entry" cheap, large enough that the index
// stays a small fraction of the file.
constexpr size_t kIndexInterval = 16;

}  // namespace

bool SSTable::Write(const std::string& path, const std::vector<Entry>& entries) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;

    std::string data_block;
    std::string index_block;
    uint32_t index_count = 0;

    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];

        if (i % kIndexInterval == 0) {
            AppendU32(&index_block, static_cast<uint32_t>(e.key.size()));
            index_block.append(e.key);
            AppendU64(&index_block, static_cast<uint64_t>(data_block.size()));
            ++index_count;
        }

        data_block.push_back(static_cast<char>(e.is_delete ? 1 : 0));
        AppendU32(&data_block, static_cast<uint32_t>(e.key.size()));
        data_block.append(e.key);
        AppendU32(&data_block, static_cast<uint32_t>(e.value.size()));
        data_block.append(e.value);
    }

    uint32_t data_crc = static_cast<uint32_t>(
        ::crc32(0L, reinterpret_cast<const Bytef*>(data_block.data()),
                static_cast<uInt>(data_block.size())));

    uint64_t index_offset = data_block.size();

    std::string footer;
    footer.reserve(kFooterSize);
    AppendU64(&footer, index_offset);
    AppendU32(&footer, index_count);
    AppendU32(&footer, data_crc);
    AppendU64(&footer, kMagic);

    bool ok = WriteFully(fd, data_block.data(), data_block.size()) &&
              WriteFully(fd, index_block.data(), index_block.size()) &&
              WriteFully(fd, footer.data(), footer.size());

    // fsync before close: a file that Open can later see must actually be on
    // disk, the same durability discipline as the WAL.
    if (ok) ok = (::fsync(fd) == 0);
    ::close(fd);
    return ok;
}

std::unique_ptr<SSTable> SSTable::Open(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return nullptr;

    off_t file_size = ::lseek(fd, 0, SEEK_END);
    if (file_size < 0 || static_cast<uint64_t>(file_size) < kFooterSize) {
        ::close(fd);
        return nullptr;
    }

    char footer[kFooterSize];
    if (::pread(fd, footer, kFooterSize, file_size - static_cast<off_t>(kFooterSize)) !=
        static_cast<ssize_t>(kFooterSize)) {
        ::close(fd);
        return nullptr;
    }

    uint64_t index_offset = ReadU64(footer);
    uint32_t index_count = ReadU32(footer + 8);
    uint32_t data_crc = ReadU32(footer + 12);
    uint64_t magic = ReadU64(footer + 16);

    if (magic != kMagic) {
        ::close(fd);
        return nullptr;
    }

    uint64_t index_end = static_cast<uint64_t>(file_size) - kFooterSize;
    if (index_offset > index_end) {
        ::close(fd);
        return nullptr;
    }

    // Verify the data block wasn't corrupted. An SSTable is fsynced before
    // ever becoming visible to Open, so unlike the WAL there is no tail to
    // forgive: any mismatch here is real corruption.
    std::string data_block(index_offset, '\0');
    if (index_offset > 0 &&
        ::pread(fd, &data_block[0], index_offset, 0) != static_cast<ssize_t>(index_offset)) {
        ::close(fd);
        return nullptr;
    }
    uint32_t actual_crc = static_cast<uint32_t>(
        ::crc32(0L, reinterpret_cast<const Bytef*>(data_block.data()),
                static_cast<uInt>(data_block.size())));
    if (actual_crc != data_crc) {
        ::close(fd);
        return nullptr;
    }

    std::string index_block(index_end - index_offset, '\0');
    if (!index_block.empty() &&
        ::pread(fd, &index_block[0], index_block.size(), static_cast<off_t>(index_offset)) !=
            static_cast<ssize_t>(index_block.size())) {
        ::close(fd);
        return nullptr;
    }

    std::vector<IndexEntry> index;
    index.reserve(index_count);
    size_t p = 0;
    for (uint32_t i = 0; i < index_count; ++i) {
        if (p + 4 > index_block.size()) {
            ::close(fd);
            return nullptr;
        }
        uint32_t key_len = ReadU32(index_block.data() + p);
        p += 4;
        if (p + key_len + 8 > index_block.size()) {
            ::close(fd);
            return nullptr;
        }
        std::string key(index_block.data() + p, key_len);
        p += key_len;
        uint64_t offset = ReadU64(index_block.data() + p);
        p += 8;
        index.push_back({std::move(key), offset});
    }

    return std::unique_ptr<SSTable>(new SSTable(fd, std::move(index), index_offset));
}

SSTable::~SSTable() {
    if (fd_ >= 0) ::close(fd_);
}

namespace {

// Reads one record at `offset` in the data block. Returns the offset just
// past the record, or 0 on a malformed read (never a valid past-header
// offset, so callers can treat 0 as failure).
uint64_t ReadRecordAt(int fd, uint64_t offset, uint64_t data_end, SSTable::Entry* out) {
    char type_and_len[5];
    if (offset + 5 > data_end) return 0;
    if (::pread(fd, type_and_len, 5, static_cast<off_t>(offset)) != 5) return 0;

    bool is_delete = type_and_len[0] != 0;
    uint32_t key_len = ReadU32(type_and_len + 1);
    uint64_t pos = offset + 5;
    if (pos + key_len + 4 > data_end) return 0;

    std::string key(key_len, '\0');
    if (key_len > 0 && ::pread(fd, &key[0], key_len, static_cast<off_t>(pos)) !=
                           static_cast<ssize_t>(key_len)) {
        return 0;
    }
    pos += key_len;

    char value_len_buf[4];
    if (::pread(fd, value_len_buf, 4, static_cast<off_t>(pos)) != 4) return 0;
    uint32_t value_len = ReadU32(value_len_buf);
    pos += 4;
    if (pos + value_len > data_end) return 0;

    std::string value(value_len, '\0');
    if (value_len > 0 && ::pread(fd, &value[0], value_len, static_cast<off_t>(pos)) !=
                              static_cast<ssize_t>(value_len)) {
        return 0;
    }
    pos += value_len;

    out->key = std::move(key);
    out->value = std::move(value);
    out->is_delete = is_delete;
    return pos;
}

}  // namespace

std::optional<SSTable::Entry> SSTable::Get(const std::string& key) const {
    // Binary search the sparse index for the last entry whose key <= target,
    // then linear-scan the data block from there: at most kIndexInterval
    // records, which is the whole point of the interval choice.
    uint64_t start_offset = 0;
    auto it = std::upper_bound(
        index_.begin(), index_.end(), key,
        [](const std::string& k, const IndexEntry& e) { return k < e.key; });
    if (it != index_.begin()) start_offset = std::prev(it)->offset;

    uint64_t pos = start_offset;
    while (pos < data_end_) {
        Entry e;
        uint64_t next = ReadRecordAt(fd_, pos, data_end_, &e);
        if (next == 0) return std::nullopt;  // malformed: treat as not found
        if (e.key == key) return e;
        if (e.key > key) return std::nullopt;
        pos = next;
    }
    return std::nullopt;
}

std::vector<SSTable::Entry> SSTable::Scan(const std::string& start,
                                           const std::string& end) const {
    std::vector<Entry> result;

    uint64_t start_offset = 0;
    auto it = std::upper_bound(
        index_.begin(), index_.end(), start,
        [](const std::string& k, const IndexEntry& e) { return k < e.key; });
    if (it != index_.begin()) start_offset = std::prev(it)->offset;

    uint64_t pos = start_offset;
    while (pos < data_end_) {
        Entry e;
        uint64_t next = ReadRecordAt(fd_, pos, data_end_, &e);
        if (next == 0) break;  // malformed tail of a scan window: stop, don't fail the whole call
        if (e.key >= start) {
            if (!end.empty() && e.key >= end) break;
            result.push_back(std::move(e));
        }
        pos = next;
    }
    return result;
}

}  // namespace minidb
