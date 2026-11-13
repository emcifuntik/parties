//! Protocol version and feature negotiation
//! (`docs/rust-rewrite/03-protocol-v2.md` sections 1 and 11).
//!
//! The major version is the ALPN token: an incompatible peer fails in the
//! TLS handshake. Inside one major version, `protocol_minor` and the list of
//! features in `Hello` and `HelloAck` gate additive behaviour: a peer never
//! sends a message the other side did not advertise support for.

/// The ALPN protocol identifier of protocol 2.x. It changes only for
/// incompatible changes.
pub const ALPN: &[u8] = b"parties/2";

/// The major version of the protocol, the one the ALPN token names.
pub const PROTOCOL_MAJOR: u32 = 2;

/// The minor version this crate implements: what a client puts in
/// `Hello.protocol_minor`.
pub const PROTOCOL_MINOR: u32 = 0;

/// The UDP port a server listens on unless configured otherwise.
pub const DEFAULT_PORT: u16 = 7800;

/// The optional features this crate knows, as the names sent in
/// `Hello.features` and `HelloAck.features`. Protocol 2.0 defines none.
pub const FEATURES: &[&str] = &[];

/// The minor version a connection uses: the lower of the two sides'.
#[must_use]
pub const fn negotiate_minor(client: u32, server: u32) -> u32 {
    if client < server { client } else { server }
}

/// Whether `name` has the form of a feature name: 1 to
/// [`crate::limits::MAX_FEATURE_NAME_LEN`] lower-case ASCII letters, digits
/// and hyphens.
#[must_use]
pub fn is_valid_feature_name(name: &str) -> bool {
    (1..=crate::limits::MAX_FEATURE_NAME_LEN).contains(&name.len())
        && name
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-')
}

/// The features a connection uses: those of `local` that the peer
/// advertised too, in the order of `local`. Names in `remote` that are
/// malformed or unknown to this side are ignored.
pub fn negotiate_features<'a, R>(local: &[&'a str], remote: &[R]) -> Vec<&'a str>
where
    R: AsRef<str>,
{
    local
        .iter()
        .copied()
        .filter(|feature| remote.iter().any(|other| other.as_ref() == *feature))
        .collect()
}
