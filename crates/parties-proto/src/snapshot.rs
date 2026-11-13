//! Putting the `Ready` snapshot back together
//! (`docs/rust-rewrite/03-protocol-v2.md` section 5.3).
//!
//! A snapshot that does not fit one control frame is sent as several
//! `Ready` frames in a row. Every frame but the last has `more` set. The
//! receiver appends the repeated fields in the order received; every other
//! field is sent in one frame only.
//!
//! A server is not trusted either. One snapshot has at most
//! [`MAX_READY_FRAMES`] frames, and its repeated fields together hold at
//! most [`MAX_READY_ITEMS`] entries, so a server cannot make a client buffer
//! without bound. The second limit is needed because an entry can be two
//! bytes on the wire and a hundred times that in memory.

use thiserror::Error;

use crate::limits::{MAX_READY_FRAMES, MAX_READY_ITEMS};
use crate::v2::Ready;

/// A snapshot is larger than a client accepts. This is a protocol
/// violation: the client closes the connection.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Error)]
pub enum SnapshotError {
    /// The snapshot has more frames than [`MAX_READY_FRAMES`].
    #[error("the state snapshot has more than {MAX_READY_FRAMES} frames")]
    TooManyFrames,
    /// The repeated fields of the snapshot hold more entries than
    /// [`MAX_READY_ITEMS`].
    #[error("the state snapshot has more than {MAX_READY_ITEMS} entries")]
    TooManyItems,
}

/// Collects the `Ready` frames of one snapshot.
///
/// ```
/// use parties_proto::snapshot::ReadyAssembler;
/// use parties_proto::v2::{Ready, Role};
///
/// let role = |id| Role { id, ..Role::default() };
/// let first = Ready { more: true, seq: 40, roles: vec![role(1)], ..Ready::default() };
/// let last = Ready { roles: vec![role(2)], ..Ready::default() };
///
/// let mut assembler = ReadyAssembler::new();
/// assert_eq!(assembler.push(first)?, None);
/// let ready = assembler.push(last)?.expect("the last frame completes the snapshot");
/// assert_eq!(ready.seq, 40);
/// assert_eq!(ready.roles, [role(1), role(2)]);
/// assert!(!ready.more);
/// # Ok::<(), parties_proto::snapshot::SnapshotError>(())
/// ```
#[derive(Clone, Debug, Default)]
pub struct ReadyAssembler {
    snapshot: Ready,
    frames: usize,
}

impl ReadyAssembler {
    /// An assembler that has seen no frame yet.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Adds the next `Ready` frame. Returns the whole snapshot, with `more`
    /// cleared, once the last frame has arrived; the assembler is then empty
    /// again.
    ///
    /// After an error the assembler is empty too, and the connection is
    /// closed.
    pub fn push(&mut self, part: Ready) -> Result<Option<Ready>, SnapshotError> {
        self.frames += 1;
        if self.frames > MAX_READY_FRAMES {
            *self = Self::default();
            return Err(SnapshotError::TooManyFrames);
        }
        if items(&self.snapshot).saturating_add(items(&part)) > MAX_READY_ITEMS {
            *self = Self::default();
            return Err(SnapshotError::TooManyItems);
        }
        let more = part.more;
        merge(&mut self.snapshot, part);
        if more {
            return Ok(None);
        }
        Ok(Some(core::mem::take(self).snapshot))
    }
}

/// How many entries the repeated fields of `ready` hold.
fn items(ready: &Ready) -> usize {
    ready.roles.len()
        + ready.channels.len()
        + ready.channel_permissions.len()
        + ready.voice_states.len()
        + ready.tracks.len()
        + ready.members.len()
}

/// Merges `part` into `snapshot`. The pattern names every field, so a field
/// added to `Ready` does not compile until it is merged here and, when it
/// is repeated, counted in [`items`].
fn merge(snapshot: &mut Ready, part: Ready) {
    let Ready {
        more: _,
        seq,
        settings,
        owner_id,
        everyone_role_id,
        me,
        roles,
        channels,
        server_permissions,
        channel_permissions,
        limits,
        voice_states,
        tracks,
        members,
        members_partial,
        member_count,
    } = part;

    // Fields sent in one frame only: a frame that sets one supplies it.
    if seq != 0 {
        snapshot.seq = seq;
    }
    if owner_id != 0 {
        snapshot.owner_id = owner_id;
    }
    if everyone_role_id != 0 {
        snapshot.everyone_role_id = everyone_role_id;
    }
    if server_permissions != 0 {
        snapshot.server_permissions = server_permissions;
    }
    if member_count != 0 {
        snapshot.member_count = member_count;
    }
    if members_partial {
        snapshot.members_partial = true;
    }
    if settings.is_some() {
        snapshot.settings = settings;
    }
    if me.is_some() {
        snapshot.me = me;
    }
    if limits.is_some() {
        snapshot.limits = limits;
    }

    // Repeated fields: appended in the order received.
    snapshot.roles.extend(roles);
    snapshot.channels.extend(channels);
    snapshot.channel_permissions.extend(channel_permissions);
    snapshot.voice_states.extend(voice_states);
    snapshot.tracks.extend(tracks);
    snapshot.members.extend(members);
    snapshot.more = false;
}
