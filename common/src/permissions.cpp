#include <parties/permissions.h>

namespace parties {

uint32_t default_permissions(Role role) {
    switch (role) {
        case Role::Owner:     return DEFAULT_OWNER_PERMS;
        case Role::Admin:     return DEFAULT_ADMIN_PERMS;
        case Role::Moderator: return DEFAULT_MODERATOR_PERMS;
        case Role::User:      return DEFAULT_USER_PERMS;
    }
    return 0;
}

bool has_permission(Role role, Permission perm,
                    const std::optional<uint32_t>& channel_override) {
    if (role == Role::Owner) return true;

    uint32_t perms = channel_override.value_or(default_permissions(role));
    return (perms & static_cast<uint32_t>(perm)) != 0;
}

bool can_moderate(Role actor, Role target) {
    // Can only moderate users with strictly lower rank (higher numeric value)
    return static_cast<uint8_t>(actor) < static_cast<uint8_t>(target);
}

} // namespace parties
