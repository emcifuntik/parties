//! Identity level: a proof of work attached to a public key
//! (`docs/rust-rewrite/04-connection-security.md` section 6.3).
//!
//! A new identity costs nothing, so a ban on a key alone only stops honest
//! users. A server can ask for a minimum identity level instead: work that
//! has to be done once per key and can then be shown to every server, the
//! same idea as TeamSpeak's security level.
//!
//! ```text
//! level(key, counter) = number of leading zero bits of
//!     SHA-256("parties-identity-level-v1" || 0x00 || key || counter)
//! ```
//!
//! `key` is the 32-byte public key and `counter` an unsigned 64-bit number
//! in little-endian byte order. The level is between 0 and 256. Reaching
//! level `n` takes about 2^n hashes; checking it takes one.
//!
//! The client searches for a counter ([`search_identity_level`]), stores the
//! best one it found next to the identity and sends it in `Authenticate`
//! (`identity_level_counter`). The server recomputes the level with
//! [`identity_level`] or checks it against its minimum with
//! [`verify_identity_level`].

use sha2::{Digest, Sha256};

use crate::keys::PublicKey;

/// Domain label of the identity-level hash.
pub const IDENTITY_LEVEL_LABEL: &[u8] = b"parties-identity-level-v1";

/// The highest level there is: every bit of the hash is zero.
pub const MAX_IDENTITY_LEVEL: u32 = 256;

/// The hash state after the label, the zero byte and the key: the part that
/// is the same for every counter.
fn prefix(key: &PublicKey) -> Sha256 {
    let mut hasher = Sha256::new();
    hasher.update(IDENTITY_LEVEL_LABEL);
    hasher.update([0]);
    hasher.update(key.as_bytes());
    hasher
}

fn level_with(prefix: &Sha256, counter: u64) -> u32 {
    let mut hasher = prefix.clone();
    hasher.update(counter.to_le_bytes());
    let digest = hasher.finalize();
    let mut level = 0;
    for &byte in &digest {
        level += byte.leading_zeros();
        if byte != 0 {
            break;
        }
    }
    level
}

/// The level a counter gives a key.
///
/// ```
/// use parties_identity::{SigningKey, identity_level};
///
/// let key = SigningKey::from_seed(&[7; 32]).public_key();
/// assert_eq!(identity_level(&key, 0), 0);
/// assert_eq!(identity_level(&key, 2), 5);
/// assert_eq!(identity_level(&key, 93), 12);
/// ```
#[must_use]
pub fn identity_level(key: &PublicKey, counter: u64) -> u32 {
    level_with(&prefix(key), counter)
}

/// The level a key and counter reach is below what the server asks for.
#[derive(Clone, Copy, Debug, PartialEq, Eq, thiserror::Error)]
#[error("identity level {found} is below the required level {required}")]
pub struct LevelTooLow {
    /// The level the key and counter reach.
    pub found: u32,
    /// The level that was asked for.
    pub required: u32,
}

/// Checks that a key and counter reach at least `minimum`, and returns the
/// level they reach.
///
/// # Errors
///
/// [`LevelTooLow`], which a server reports as `IDENTITY_LEVEL_TOO_LOW`.
pub fn verify_identity_level(
    key: &PublicKey,
    counter: u64,
    minimum: u32,
) -> Result<u32, LevelTooLow> {
    let found = identity_level(key, counter);
    if found >= minimum {
        Ok(found)
    } else {
        Err(LevelTooLow {
            found,
            required: minimum,
        })
    }
}

/// The result of one bounded search for a counter.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[must_use]
pub enum LevelSearch {
    /// A counter that reaches the target.
    Found {
        /// The counter to store and to send.
        counter: u64,
        /// The level it reaches, at least the target.
        level: u32,
    },
    /// No counter in the searched range reaches the target.
    Exhausted {
        /// The best counter of this range and its level, if any counter was
        /// tried. Worth keeping: it may satisfy a server that asks for less.
        best: Option<(u64, u32)>,
        /// Where to continue, or `None` once every counter has been tried.
        resume_at: Option<u64>,
    },
    /// The target is above [`MAX_IDENTITY_LEVEL`]; no counter can reach it.
    Unreachable,
}

/// Tries up to `max_attempts` counters, starting at `start`, and stops at
/// the first that gives the key at least the level `target`.
///
/// The search is the caller's to bound and to schedule: each level doubles
/// the expected work (level 20 is about a million hashes), so a client runs
/// it in slices on a background thread, checks for cancellation between
/// slices and continues from `resume_at`. A server's minimum comes from the
/// network; the caller decides how much work it is willing to do for it.
///
/// ```
/// use parties_identity::{LevelSearch, SigningKey, identity_level, search_identity_level};
///
/// let key = SigningKey::from_seed(&[7; 32]).public_key();
///
/// // Ten attempts are not enough for level 8 ...
/// let LevelSearch::Exhausted { best, resume_at } = search_identity_level(&key, 8, 0, 10) else {
///     unreachable!("the first ten counters stay below level 8");
/// };
/// assert_eq!(best, Some((2, 5)));
/// assert_eq!(resume_at, Some(10));
///
/// // ... so the search continues where it stopped.
/// let found = search_identity_level(&key, 8, 10, 1_000);
/// assert_eq!(found, LevelSearch::Found { counter: 93, level: 12 });
/// assert_eq!(identity_level(&key, 93), 12);
/// ```
pub fn search_identity_level(
    key: &PublicKey,
    target: u32,
    start: u64,
    max_attempts: u64,
) -> LevelSearch {
    if target > MAX_IDENTITY_LEVEL {
        return LevelSearch::Unreachable;
    }
    let prefix = prefix(key);
    let mut best: Option<(u64, u32)> = None;
    let mut counter = start;
    for _ in 0..max_attempts {
        let level = level_with(&prefix, counter);
        if level >= target {
            return LevelSearch::Found { counter, level };
        }
        if best.is_none_or(|(_, best_level)| level > best_level) {
            best = Some((counter, level));
        }
        match counter.checked_add(1) {
            Some(next) => counter = next,
            None => {
                return LevelSearch::Exhausted {
                    best,
                    resume_at: None,
                };
            }
        }
    }
    LevelSearch::Exhausted {
        best,
        resume_at: Some(counter),
    }
}
