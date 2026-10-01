#pragma once
#include "platform.h"
#include <array>
#include <span>

namespace piu {
// Lua 5.1 numeric TValue: double + numeric type tag; padding is never interpreted.
struct MailboxSlot {
    double value;
    uint32_t type;
    uint32_t padding;
};

inline constexpr size_t MailboxSize = 1024;
using MailboxBlock = std::array<MailboxSlot, MailboxSize>;

struct ExportFrame {
    std::string payload;
    double clock = 0;
    uint64_t sequence = 0;
    uint64_t observed = 0;
};

bool mailbox_signature(std::span<const MailboxSlot> slots);
std::optional<ExportFrame> decode_mailbox(const MailboxBlock& first, const MailboxBlock& second);

class MailboxReader {
    std::unique_ptr<Handle> process_;
    fs::path root_;
    uintptr_t address_ = 0, scan_ = 0;
    uint64_t sequence_ = 0, changed_ = 0, next_search_ = 0;
    double clock_ = -1;
    bool confirmed_ = false;
    std::vector<MailboxSlot> buffer_;
    void disconnect();
    bool connect(const fs::path& root);
    void discover();
    std::optional<ExportFrame> snapshot();

public:
    std::optional<ExportFrame> poll(const fs::path& root);
};

using ExportFeed = std::function<std::optional<ExportFrame>(const fs::path&)>;
} // namespace piu
