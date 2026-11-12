//! The roles and settings of a new server
//! (`docs/rust-rewrite/05-permissions-and-roles.md` section 9), so that the
//! server, the first-run wizard and the tests share one definition.

use crate::ids::RoleId;
use crate::limits::RoleLimits;
use crate::model::{NewMemberPolicy, Role};
use crate::permissions::Permissions;

/// A role to create on a new server. The server assigns the identifier.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RoleTemplate {
    /// The role's display name.
    pub name: &'static str,
    /// The role's position; `@everyone` is 0.
    pub position: u32,
    /// The role's own permission mask. Roles add to `@everyone`, so this
    /// holds only what the role adds.
    pub permissions: Permissions,
    /// The role's numeric limits.
    pub limits: RoleLimits,
}

impl RoleTemplate {
    /// The role as the permission engine sees it, under the given
    /// identifier.
    #[must_use]
    pub const fn to_role(&self, id: RoleId) -> Role {
        Role {
            id,
            position: self.position,
            permissions: self.permissions,
            limits: self.limits,
        }
    }
}

/// The presets the first-run wizard offers.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Preset {
    /// Open defaults, for a group of friends.
    Friends,
    /// For a larger community: new members are restricted for ten minutes
    /// and `@everyone` can neither create join codes nor mention everyone.
    Community,
}

/// The display name of the built-in role every member has.
pub const EVERYONE_NAME: &str = "@everyone";

/// What `@everyone` may do on a new server.
pub const EVERYONE_PERMISSIONS: Permissions = Permissions::VIEW_CHANNEL
    .union(Permissions::CREATE_JOIN_CODES)
    .union(Permissions::CHANGE_NICKNAME)
    .union(Permissions::SEND_MESSAGES)
    .union(Permissions::READ_MESSAGE_HISTORY)
    .union(Permissions::ATTACH_FILES)
    .union(Permissions::EMBED_LINKS)
    .union(Permissions::ADD_REACTIONS)
    .union(Permissions::CONNECT)
    .union(Permissions::SPEAK)
    .union(Permissions::USE_VOICE_ACTIVITY)
    .union(Permissions::STREAM_SCREEN)
    .union(Permissions::STREAM_CAMERA)
    .union(Permissions::USE_AUX_AUDIO)
    .union(Permissions::REQUEST_TO_SPEAK);

/// What the default Moderator role adds to `@everyone`.
pub const MODERATOR_PERMISSIONS: Permissions = Permissions::KICK_MEMBERS
    .union(Permissions::TIMEOUT_MEMBERS)
    .union(Permissions::MANAGE_NICKNAMES)
    .union(Permissions::MANAGE_MESSAGES)
    .union(Permissions::PIN_MESSAGES)
    .union(Permissions::MUTE_MEMBERS)
    .union(Permissions::DEAFEN_MEMBERS)
    .union(Permissions::MOVE_MEMBERS)
    .union(Permissions::DISCONNECT_MEMBERS)
    .union(Permissions::VIEW_AUDIT_LOG)
    .union(Permissions::APPROVE_MEMBERS);

/// What the default Admin role holds: `Administrator`.
pub const ADMIN_PERMISSIONS: Permissions = Permissions::ADMINISTRATOR;

/// The bits a restricted new member loses unless the server chooses
/// otherwise: speaking, sending messages and publishing any stream.
pub const NEW_MEMBER_RESTRICTED: Permissions = Permissions::SPEAK
    .union(Permissions::SEND_MESSAGES)
    .union(Permissions::STREAM_SCREEN)
    .union(Permissions::STREAM_CAMERA)
    .union(Permissions::USE_AUX_AUDIO);

/// The new-member period of the Community preset: ten minutes.
pub const COMMUNITY_NEW_MEMBER_PERIOD_MS: u64 = 10 * 60 * 1000;

/// Everything the permission model of a new server starts with.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ServerDefaults {
    /// The built-in role, position 0.
    pub everyone: RoleTemplate,
    /// The Moderator role, position 1.
    pub moderator: RoleTemplate,
    /// The Admin role, position 2.
    pub admin: RoleTemplate,
    /// The restrictions on members who are not yet trusted.
    pub new_members: NewMemberPolicy,
}

impl ServerDefaults {
    /// The defaults of a new server under `preset`.
    #[must_use]
    pub const fn new(preset: Preset) -> Self {
        let (everyone_permissions, period_ms) = match preset {
            Preset::Friends => (EVERYONE_PERMISSIONS, 0),
            Preset::Community => (
                EVERYONE_PERMISSIONS
                    .difference(Permissions::MENTION_EVERYONE)
                    .difference(Permissions::CREATE_JOIN_CODES),
                COMMUNITY_NEW_MEMBER_PERIOD_MS,
            ),
        };
        Self {
            everyone: RoleTemplate {
                name: EVERYONE_NAME,
                position: 0,
                permissions: everyone_permissions,
                limits: RoleLimits::EVERYONE_DEFAULT,
            },
            moderator: RoleTemplate {
                name: "Moderator",
                position: 1,
                permissions: MODERATOR_PERMISSIONS,
                limits: RoleLimits::NONE,
            },
            admin: RoleTemplate {
                name: "Admin",
                position: 2,
                permissions: ADMIN_PERMISSIONS,
                limits: RoleLimits::NONE,
            },
            new_members: NewMemberPolicy {
                period_ms,
                require_role: false,
                restricted: NEW_MEMBER_RESTRICTED,
            },
        }
    }

    /// The three roles, lowest position first.
    #[must_use]
    pub const fn roles(&self) -> [&RoleTemplate; 3] {
        [&self.everyone, &self.moderator, &self.admin]
    }
}
