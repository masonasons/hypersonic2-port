#include "hs2/bank.h"

#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hs2 {

float rdf(const uint8_t* p) {
    uint32_t u = rd32(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

namespace {
constexpr uint32_t kOffMask = 0xBADEAFFE, kSizeMask = 0xDEADBEEF;
constexpr size_t kDir = 0x58;

// Read-only memory map of a whole file: {owner, data, size}; throws std::runtime_error.
struct Mapped {
    std::shared_ptr<const void> owner;
    const uint8_t* data = nullptr;
    size_t size = 0;
};

Mapped mapFile(const std::filesystem::path& path) {
    Mapped m;
#ifdef _WIN32
    HANDLE f = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open " + path.string());
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart == 0) {
        CloseHandle(f);
        throw std::runtime_error("cannot read " + path.string());
    }
    HANDLE map = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(f);  // the mapping keeps the file open
    if (!map) throw std::runtime_error("cannot map " + path.string());
    const void* view = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(map);  // the view keeps the mapping
    if (!view) throw std::runtime_error("cannot map " + path.string());
    m.owner = std::shared_ptr<const void>(view, [](const void* v) { UnmapViewOfFile(v); });
    m.size = size_t(sz.QuadPart);
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("cannot open " + path.string());
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size == 0) {
        ::close(fd);
        throw std::runtime_error("cannot read " + path.string());
    }
    const size_t size = size_t(st.st_size);
    void* view = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);  // the mapping keeps the file
    if (view == MAP_FAILED) throw std::runtime_error("cannot map " + path.string());
    m.owner = std::shared_ptr<const void>(view, [size](const void* v) { munmap(const_cast<void*>(v), size); });
    m.size = size;
#endif
    m.data = static_cast<const uint8_t*>(m.owner.get());
    return m;
}

std::string pstr(std::span<const uint8_t> d, size_t p) {
    if (p >= d.size()) return {};
    size_t n = d[p];
    if (p + 1 + n > d.size()) return {};
    return std::string(reinterpret_cast<const char*>(&d[p + 1]), n);
}
}  // namespace

std::unique_ptr<MilanBank> MilanBank::open(const std::filesystem::path& path) {
    auto b = std::make_unique<MilanBank>();
    b->path_ = path;
    auto m = mapFile(path);
    b->map_ = std::move(m.owner);
    b->data_ = m.data;
    b->size_ = m.size;
    const std::span<const uint8_t> d = b->bytes();
    if (d.size() < 0x80 || !(d[0] == 'M' || d[0] == 'H') || !(d[1] == 'i' || d[1] == 'y') ||
        !(d[2] == 'l' || d[2] == 'p') || d[0x29] != 'W')
        throw std::runtime_error("not a Milan bank: " + path.string());
    b->letter = char(d[16]);
    b->kind = rd16(&d[0x30]);
    b->version = rd16(&d[0x32]);
    if (b->version >= 0x15) throw std::runtime_error("unsupported bank version");
    const bool masked = b->version == 13;
    const uint32_t count = rd32(&d[kDir]), namesOff = rd32(&d[kDir + 4]);
    const uint32_t secOff = rd32(&d[kDir + 8]), secCount = rd32(&d[kDir + 12]);

    auto slot = [&](size_t o) {
        BankSlot s{rd32(&d[o]), rd32(&d[o + 4]), rd32(&d[o + 8])};
        if (s.size && masked) {
            s.offset ^= kOffMask;
            s.size ^= kSizeMask;
        }
        if (size_t(s.offset) + s.size > d.size()) s = {};
        return s;
    };
    for (uint32_t i = 0; i < count; ++i) {
        size_t e = kDir + 0x10 + size_t(i) * 0x30;
        if (e + 0x30 > d.size()) break;
        BankEntry be;
        be.index = int(i);
        be.name = pstr(d, kDir + namesOff + rd32(&d[e]));
        be.name2 = pstr(d, kDir + namesOff + rd32(&d[e + 4]));
        be.slot1 = slot(e + 0x0c);
        be.slot2 = slot(e + 0x1c);
        b->entries.push_back(std::move(be));
    }
    for (uint32_t i = 0; i < secCount; ++i) {
        size_t r = kDir + secOff + size_t(i) * 0x20;
        if (r + 0x20 > d.size()) break;
        b->records.push_back(slot(r + 4));
    }
    return b;
}

std::span<const uint8_t> MilanBank::read(const BankSlot& s) const {
    if (!s.size) return {};
    return {data_ + s.offset, s.size};
}

}  // namespace hs2
