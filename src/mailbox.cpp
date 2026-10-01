#include "mailbox.h"
#include <tlhelp32.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace piu {
namespace {
constexpr std::array<double, 4> Signature{204081632653, 918273645546, 672345891234, 135792468013};
constexpr size_t ChunkBytes = 1024 * 1024;
constexpr size_t ScanBudget = 16 * ChunkBytes;

bool integer(const MailboxSlot& slot, double maximum) {
    return slot.type == 3 && std::isfinite(slot.value) && slot.value >= 0 && slot.value <= maximum &&
           std::floor(slot.value) == slot.value;
}

bool read_memory(HANDLE process, uintptr_t address, void* target, size_t size) {
    SIZE_T read = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address), target, size, &read) &&
           read == size;
}
} // namespace

static_assert(sizeof(MailboxSlot) == 16);

bool mailbox_signature(std::span<const MailboxSlot> slots) {
    if (slots.size() < 9) {
        return false;
    }

    for (size_t i = 0; i < Signature.size(); ++i) {
        if (slots[i].type != 3 || slots[i].value != Signature[i]) {
            return false;
        }
    }

    return slots[4].type == 3 && slots[4].value == 1 && slots[5].type == 3 && slots[5].value == MailboxSize;
}

std::optional<ExportFrame> decode_mailbox(const MailboxBlock& first, const MailboxBlock& second) {
    if (!mailbox_signature(first) || !mailbox_signature(second) || !integer(first[6], 9007199254740990.0) ||
        first[6].value == 0 || std::fmod(first[6].value, 2) != 0 || !integer(first[7], 6090) ||
        first[7].value == 0 || first[8].type != 3 || !std::isfinite(first[8].value) || first[8].value < 0) {
        return {};
    }

    // Compare every published value, not allocator padding. This rejects partial writes
    // and an array being reclaimed or replaced while ReadProcessMemory is in progress.
    auto length = static_cast<size_t>(first[7].value);
    auto used = 9 + (length + 5) / 6;
    for (size_t i = 0; i < used; ++i) {
        if (first[i].type != second[i].type || first[i].value != second[i].value) {
            return {};
        }
    }

    ExportFrame frame;
    frame.clock = first[8].value;
    frame.sequence = static_cast<uint64_t>(first[6].value);
    frame.observed = now();
    frame.payload.reserve(length);
    for (size_t i = 9; i < used; ++i) {
        if (!integer(first[i], 281474976710655.0)) {
            return {};
        }

        auto value = static_cast<uint64_t>(first[i].value);
        for (size_t byte = 0; byte < 6 && frame.payload.size() < length; ++byte) {
            frame.payload += static_cast<char>((value >> (byte * 8)) & 255);
        }
    }

    return frame;
}

void MailboxReader::disconnect() {
    process_.reset();
    address_ = scan_ = 0;
    sequence_ = changed_ = 0;
    clock_ = -1;
    confirmed_ = false;
}

bool MailboxReader::connect(const fs::path& root) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) {
        return false;
    }

    do {
        if (_wcsicmp(entry.szExeFile, L"XSanity.exe") != 0) {
            continue;
        }

        auto handle = std::make_unique<Handle>(OpenProcess(
            PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, entry.th32ProcessID));
        if (!handle->get()) {
            continue;
        }

        wchar_t image[32768];
        DWORD length = static_cast<DWORD>(std::size(image));
        if (!QueryFullProcessImageNameW(handle->get(), 0, image, &length)) {
            continue;
        }

        std::error_code error;
        auto path = fs::path(std::wstring(image, length));
        bool matches = fs::equivalent(path, root / L"Program64" / L"XSanity.exe", error);
        if (!matches) {
            error.clear();
            matches = fs::equivalent(path, root / L"Program32" / L"XSanity.exe", error);
        }

        if (!matches) {
            continue;
        }

        process_ = std::move(handle);
        return true;
    } while (Process32NextW(snapshot.get(), &entry));
    return false;
}

void MailboxReader::discover() {
    if (buffer_.empty()) {
        buffer_.resize((ChunkBytes + sizeof(MailboxBlock)) / sizeof(MailboxSlot));
    }

    size_t budget = ScanBudget;
    auto deadline = GetTickCount64() + 20;
    MEMORY_BASIC_INFORMATION region{};
    while (budget && GetTickCount64() < deadline) {
        if (!VirtualQueryEx(process_->get(), reinterpret_cast<void*>(scan_), &region, sizeof(region))) {
            scan_ = 0;
            next_search_ = GetTickCount64() + 1000;
            return;
        }

        auto begin = reinterpret_cast<uintptr_t>(region.BaseAddress);
        if (region.RegionSize > std::numeric_limits<uintptr_t>::max() - begin) {
            scan_ = 0;
            return;
        }

        auto end = begin + region.RegionSize;
        if (end <= scan_) {
            scan_ = 0;
            return;
        }

        if (region.State != MEM_COMMIT || region.Type != MEM_PRIVATE ||
            (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(region.Protect & PAGE_READWRITE)) {
            scan_ = end;
            continue;
        }

        auto size = std::min(buffer_.size() * sizeof(MailboxSlot), static_cast<size_t>(end - scan_));
        auto advance = std::min(ChunkBytes, static_cast<size_t>(end - scan_));
        if (read_memory(process_->get(), scan_, buffer_.data(), size)) {
            // Arrays may start at either eight-byte alignment within a sixteen-byte slot.
            auto bytes = reinterpret_cast<const char*>(buffer_.data());
            for (size_t offset = 0; offset + 9 * sizeof(MailboxSlot) <= size; offset += 8) {
                double first = 0;
                std::memcpy(&first, bytes + offset, sizeof(first));
                if (first != Signature[0]) {
                    continue;
                }

                std::array<MailboxSlot, 9> header;
                std::memcpy(header.data(), bytes + offset, sizeof(header));
                if (mailbox_signature(header)) {
                    address_ = scan_ + offset;
                    scan_ += offset + 8;
                    changed_ = GetTickCount64();
                    sequence_ = 0;
                    clock_ = -1;
                    confirmed_ = false;
                    return;
                }
            }
        }

        scan_ += advance;
        budget -= std::min(budget, advance);
    }
}

std::optional<ExportFrame> MailboxReader::snapshot() {
    MailboxBlock first, second;
    if (!read_memory(process_->get(), address_, first.data(), sizeof(first)) ||
        !read_memory(process_->get(), address_, second.data(), sizeof(second))) {
        return {};
    }

    return decode_mailbox(first, second);
}

std::optional<ExportFrame> MailboxReader::poll(const fs::path& root) {
    auto clock = GetTickCount64();
    if (root != root_ || (process_ && WaitForSingleObject(process_->get(), 0) != WAIT_TIMEOUT)) {
        disconnect();
        root_ = root;
        next_search_ = 0;
    }

    if (!process_) {
        if (clock < next_search_) {
            return {};
        }

        next_search_ = clock + 1000;
        if (!connect(root)) {
            return {};
        }

        next_search_ = 0;
    }

    if (!address_ && clock >= next_search_) {
        discover();
    }

    if (!address_) {
        return {};
    }

    auto frame = snapshot();
    if (frame && frame->sequence > sequence_ && frame->clock > clock_) {
        bool live = sequence_ != 0;
        sequence_ = frame->sequence;
        clock_ = frame->clock;
        changed_ = clock;
        confirmed_ = confirmed_ || live;
        if (confirmed_) {
            return frame;
        }
    }

    // Require a changing heartbeat before trusting a discovered heap block. Old
    // arrays left by Lua's allocator never become a live connection.
    if (clock - changed_ > 4000) {
        address_ = 0;
        next_search_ = 0;
    }

    return {};
}
} // namespace piu
