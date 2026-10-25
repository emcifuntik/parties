#include <parties/permissions.h>

namespace parties {

static_assert(enum_names_v<Role>.name(Role::Moderator) == "moderator");
static_assert(enum_names_v<Role>.parse("admin") == Role::Admin);
static_assert(!enum_names_v<Role>.parse("root"));
static_assert(enum_names_v<Role>.from_persisted_int(3, Role::Owner) == Role::User);
static_assert(enum_names_v<Role>.from_persisted_int(-1, Role::User) == Role::User);
static_assert(enum_names_v<Role>.from_persisted_int(4, Role::User) == Role::User);

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
    return static_cast<uint8_t>(actor) < static_cast<uint8_t>(target);
}

}
