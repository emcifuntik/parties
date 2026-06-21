#pragma once

#include <cstdint>
#include <optional>

namespace parties {

enum class Role : uint8_t {
    Owner     = 0,
    Admin     = 1,
    Moderator = 2,
    User      = 3,
};

enum class Permission : uint32_t {
    None              = 0,
    JoinChannel       = 1 << 0,
    Speak             = 1 << 1,
    MuteOthers        = 1 << 2,
    DeafenOthers      = 1 << 3,
    KickFromChannel   = 1 << 4,
    KickFromServer    = 1 << 5,
    CreateChannel     = 1 << 6,
    DeleteChannel     = 1 << 7,
    ManagePermissions = 1 << 8,
    ManageRoles       = 1 << 9,
    ManageServer      = 1 << 10,
    SendText          = 1 << 11,
    UploadFiles       = 1 << 12,
    ShareScreen       = 1 << 13,
    ShareWebcam       = 1 << 14,
};

constexpr uint32_t DEFAULT_OWNER_PERMS     = 0xFFFFFFFF;
constexpr uint32_t DEFAULT_ADMIN_PERMS     = 0x000007FF;
constexpr uint32_t DEFAULT_MODERATOR_PERMS = 0x0000001F;
constexpr uint32_t DEFAULT_USER_PERMS      = 0x00000003;

uint32_t default_permissions(Role role);
bool has_permission(Role role, Permission perm,
                    const std::optional<uint32_t>& channel_override = std::nullopt);
bool can_moderate(Role actor, Role target);

} // namespace parties
