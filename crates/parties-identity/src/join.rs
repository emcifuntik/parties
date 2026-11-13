//! Join codes and join links
//! (`docs/rust-rewrite/13-profiles-access-and-hardware-keys.md` section 5,
//! `docs/rust-rewrite/04-connection-security.md` section 4).
//!
//! # Join codes
//!
//! A join code is ten symbols of Crockford's Base32 alphabet,
//! `0123456789ABCDEFGHJKMNPQRSTVWXYZ`: 50 bits. It is shown as
//! `XXXXX-XXXXX`. Reading is forgiving in the ways that alphabet defines:
//! case does not matter, `O` is read as `0`, and `I` and `L` are read as
//! `1`. The letter `U` is not part of the alphabet. The hyphen is optional.
//!
//! A join code admits whoever presents it, so it is treated like a secret:
//! [`JoinCode`] is wiped on drop and compared in constant time, neither it
//! nor a link that carries one can be printed by accident, and the text
//! forms that `expose` hands out are wiped when dropped. It can be cloned,
//! because a client keeps the code while it retries a connection.
//!
//! # Join links
//!
//! ```text
//! parties://<host>:<port>/j/<join code>#k=<server key>
//! parties://<host>:<port>#k=<server key>
//! ```
//!
//! | Part | Rule |
//! |---|---|
//! | Scheme | `parties`, any case |
//! | Host | A DNS name (letters, digits and hyphens in dot-separated labels; lowercased), an IPv4 address, or an IPv6 address in square brackets. An address that is not the address of one host (unspecified, multicast, broadcast) is refused |
//! | Port | Decimal, 1 to 65535, always present |
//! | Path | Empty, `/`, or `/j/` followed by a join code |
//! | Fragment | `&`-separated `name=value` pairs. `k` is the server's identity key in its text form (43 characters of unpadded base64url, see [`PublicKey`]) and must appear exactly once. Other names are ignored, so later versions can add some |
//!
//! There is no user-info part and no query. A link is at most
//! [`MAX_LINK_LEN`] bytes of printable ASCII. The second form, without a
//! join code, only says where a server is and which key it has.
//!
//! A client that opens a link pins the key before it sends anything
//! ([`crate::trust`]), and always asks the user first.

use core::fmt;
use core::net::{IpAddr, Ipv4Addr, Ipv6Addr};
use core::str::FromStr;

use subtle::ConstantTimeEq;
use zeroize::{Zeroize, ZeroizeOnDrop, Zeroizing};

use crate::keys::{KeyError, PublicKey};

/// Number of symbols in a join code.
pub const JOIN_CODE_LEN: usize = 10;

/// The symbols of a join code, in value order: Crockford's Base32.
pub const JOIN_CODE_ALPHABET: &[u8; 32] = b"0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/// The scheme of a join link.
pub const LINK_SCHEME: &str = "parties";

/// Longest link that is read, in bytes.
pub const MAX_LINK_LEN: usize = 512;

/// Where the hyphen sits in the displayed form `XXXXX-XXXXX`.
const GROUP_LEN: usize = JOIN_CODE_LEN / 2;

/// Room for the longest canonical link: scheme and `://`, a 253-character
/// name, `:65535`, `/j/XXXXX-XXXXX` and `#k=` with a 43-character key.
const CANONICAL_LINK_CAPACITY: usize = LINK_SCHEME.len() + 3 + MAX_NAME_LEN + 6 + 14 + 46;

/// Why a text was not accepted as a join code.
///
/// No variant carries any part of the code.
#[derive(Clone, Copy, Debug, PartialEq, Eq, thiserror::Error)]
pub enum JoinCodeError {
    /// Not ten symbols, or eleven characters with a hyphen in the middle.
    #[error("a join code has ten characters, written XXXXX-XXXXX")]
    Format,
    /// A character that is not a symbol of the alphabet.
    #[error("character {position} of the join code is not allowed")]
    Character {
        /// Position among the ten symbols, counted from 1.
        position: usize,
    },
}

/// The value of one typed symbol, or `None` if it is not in the alphabet.
fn symbol_value(character: u8) -> Option<u8> {
    let canonical = match character.to_ascii_uppercase() {
        b'O' => b'0',
        b'I' | b'L' => b'1',
        other => other,
    };
    JOIN_CODE_ALPHABET
        .iter()
        .position(|&symbol| symbol == canonical)
        .and_then(|value| u8::try_from(value).ok())
}

/// A join code: ten symbols that admit one new identity to a server.
///
/// ```
/// use parties_identity::JoinCode;
///
/// // Typed by a person: lowercase, and the letters O and l for 0 and 1.
/// let code: JoinCode = "7kq2m-9xOlt".parse()?;
/// assert_eq!(code.expose().as_str(), "7KQ2M-9X01T");
/// assert_eq!(code.expose_compact().as_str(), "7KQ2M9X01T");
/// assert_eq!(format!("{code:?}"), "JoinCode(..)");
/// # Ok::<(), parties_identity::JoinCodeError>(())
/// ```
#[derive(Clone)]
pub struct JoinCode {
    /// Symbol values, each below 32.
    symbols: [u8; JOIN_CODE_LEN],
}

impl JoinCode {
    /// Reads a code as a person types it or as a link carries it.
    ///
    /// # Errors
    ///
    /// [`JoinCodeError::Format`] unless the text is ten symbols, with or
    /// without a hyphen after the fifth; [`JoinCodeError::Character`] for
    /// the first symbol outside the alphabet.
    pub fn parse(text: &str) -> Result<Self, JoinCodeError> {
        let characters = text.as_bytes();
        let (first, second) = match characters.len() {
            JOIN_CODE_LEN => characters.split_at(GROUP_LEN),
            len if len == JOIN_CODE_LEN + 1 && characters[GROUP_LEN] == b'-' => {
                (&characters[..GROUP_LEN], &characters[GROUP_LEN + 1..])
            }
            _ => return Err(JoinCodeError::Format),
        };
        let mut code = Self {
            symbols: [0; JOIN_CODE_LEN],
        };
        for (position, &character) in first.iter().chain(second).enumerate() {
            code.symbols[position] = symbol_value(character).ok_or(JoinCodeError::Character {
                position: position + 1,
            })?;
        }
        Ok(code)
    }

    /// Makes a code from ten random bytes: the low five bits of each byte
    /// choose one symbol, so uniform bytes give a uniform code.
    #[must_use]
    pub fn from_random(random: &[u8; JOIN_CODE_LEN]) -> Self {
        Self {
            symbols: random.map(|byte| byte & 0x1F),
        }
    }

    /// Creates a new code from the operating system's random-number
    /// generator.
    ///
    /// # Panics
    ///
    /// If the operating system cannot provide random bytes
    /// (`docs/rust-rewrite/04-connection-security.md`, finding F21).
    #[cfg(feature = "os-rng")]
    #[must_use]
    pub fn generate() -> Self {
        let mut random = Zeroizing::new([0u8; JOIN_CODE_LEN]);
        crate::fill_random(random.as_mut_slice());
        Self::from_random(&random)
    }

    /// The symbol at a position, as its uppercase character.
    fn character(&self, position: usize) -> char {
        char::from(JOIN_CODE_ALPHABET[usize::from(self.symbols[position] & 0x1F)])
    }

    /// The code as people see it: `XXXXX-XXXXX`, uppercase. This reveals the
    /// code; the copy is wiped when dropped.
    #[must_use]
    pub fn expose(&self) -> Zeroizing<String> {
        let mut text = Zeroizing::new(String::with_capacity(JOIN_CODE_LEN + 1));
        self.write(&mut text, true);
        text
    }

    /// The ten symbols without the hyphen, uppercase: the one form to send
    /// and to hash. This reveals the code; the copy is wiped when dropped.
    #[must_use]
    pub fn expose_compact(&self) -> Zeroizing<String> {
        let mut text = Zeroizing::new(String::with_capacity(JOIN_CODE_LEN));
        self.write(&mut text, false);
        text
    }

    /// Appends the symbols, with or without the hyphen between the groups.
    fn write(&self, text: &mut String, hyphen: bool) {
        for position in 0..JOIN_CODE_LEN {
            if hyphen && position == GROUP_LEN {
                text.push('-');
            }
            text.push(self.character(position));
        }
    }
}

impl PartialEq for JoinCode {
    /// Compares in constant time: how long it takes does not tell where two
    /// codes differ.
    fn eq(&self, other: &Self) -> bool {
        self.symbols.ct_eq(&other.symbols).into()
    }
}

impl Eq for JoinCode {}

impl Drop for JoinCode {
    fn drop(&mut self) {
        self.symbols.zeroize();
    }
}

impl ZeroizeOnDrop for JoinCode {}

impl fmt::Debug for JoinCode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("JoinCode(..)")
    }
}

impl FromStr for JoinCode {
    type Err = JoinCodeError;

    fn from_str(text: &str) -> Result<Self, JoinCodeError> {
        Self::parse(text)
    }
}

/// Why a text was not accepted as a join link.
#[derive(Clone, Copy, Debug, PartialEq, Eq, thiserror::Error)]
pub enum LinkError {
    /// Longer than [`MAX_LINK_LEN`].
    #[error("the link is too long")]
    TooLong,
    /// A space, a control character or a character outside ASCII.
    #[error("the link contains a character that is not allowed")]
    Character,
    /// Does not start with `parties://`.
    #[error("the link does not start with parties://")]
    Scheme,
    /// Has a `user@` part.
    #[error("the link must not contain user information")]
    UserInfo,
    /// Has a `?query` part.
    #[error("the link must not contain a query")]
    Query,
    /// The host is not a DNS name, an IPv4 address or a bracketed IPv6
    /// address, or it is an address that is not the address of one host.
    #[error("the link's host is not valid")]
    Host,
    /// The port is missing, not a decimal number, or not in 1 to 65535.
    #[error("the link's port is missing or not valid")]
    Port,
    /// The path is not empty, `/` or `/j/<join code>`.
    #[error("the link's path is not recognised")]
    Path,
    /// The join code in the path is not valid.
    #[error("link: {0}")]
    JoinCode(#[from] JoinCodeError),
    /// No `k=` in the fragment. A link without the server's key cannot pin
    /// the server and is refused.
    #[error("the link does not carry the server's key")]
    MissingKey,
    /// More than one `k=` in the fragment.
    #[error("the link carries more than one server key")]
    DuplicateKey,
    /// The server key is not valid.
    #[error("link: {0}")]
    Key(#[from] KeyError),
}

/// The host of a join link.
///
/// `Display` writes the form a link carries: a lowercase DNS name, a dotted
/// IPv4 address, or an IPv6 address in square brackets.
#[derive(Clone, PartialEq, Eq, Hash)]
pub struct Host(HostKind);

#[derive(Clone, PartialEq, Eq, Hash)]
enum HostKind {
    /// A DNS name, lowercase.
    Name(String),
    Ip(IpAddr),
}

/// Longest DNS name, in characters.
const MAX_NAME_LEN: usize = 253;

/// Longest label of a DNS name, in characters.
const MAX_LABEL_LEN: usize = 63;

/// Whether a label would be read as a number by software that accepts
/// shorthand IPv4 forms (`127.1`, `0x7f000001`). Such a name is refused
/// rather than risk two programs disagreeing about where it points.
fn looks_numeric(label: &str) -> bool {
    let hex = label
        .strip_prefix("0x")
        .or_else(|| label.strip_prefix("0X"));
    match hex {
        Some(digits) => digits.bytes().all(|byte| byte.is_ascii_hexdigit()),
        None => label.bytes().all(|byte| byte.is_ascii_digit()),
    }
}

/// Whether an address can be the address of one server. The unspecified
/// address (which many systems treat as "this machine"), multicast and
/// broadcast addresses cannot, also when written as IPv4-mapped IPv6.
fn names_one_host(address: &IpAddr) -> bool {
    let v4_names_one_host = |address: &Ipv4Addr| {
        !address.is_unspecified() && !address.is_multicast() && !address.is_broadcast()
    };
    match address {
        IpAddr::V4(address) => v4_names_one_host(address),
        IpAddr::V6(address) => match address.to_ipv4_mapped() {
            Some(mapped) => v4_names_one_host(&mapped),
            None => !address.is_unspecified() && !address.is_multicast(),
        },
    }
}

fn is_label(label: &str) -> bool {
    (1..=MAX_LABEL_LEN).contains(&label.len())
        && label
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-')
        && !label.starts_with('-')
        && !label.ends_with('-')
}

impl Host {
    /// Reads a host as a link carries it.
    ///
    /// # Errors
    ///
    /// [`LinkError::Host`] unless the text is a DNS name of letters, digits
    /// and hyphens, an IPv4 address in dotted decimal, or an IPv6 address in
    /// square brackets; also for an address [`Host::from_ip`] refuses.
    pub fn parse(text: &str) -> Result<Self, LinkError> {
        if let Some(bracketed) = text.strip_prefix('[') {
            let address = bracketed.strip_suffix(']').ok_or(LinkError::Host)?;
            let address = Ipv6Addr::from_str(address).map_err(|_| LinkError::Host)?;
            return Self::from_ip(IpAddr::V6(address));
        }
        if let Ok(address) = Ipv4Addr::from_str(text) {
            return Self::from_ip(IpAddr::V4(address));
        }
        let well_formed = text.len() <= MAX_NAME_LEN
            && text.split('.').all(is_label)
            && !text.rsplit('.').next().is_some_and(looks_numeric);
        if well_formed {
            Ok(Self(HostKind::Name(text.to_ascii_lowercase())))
        } else {
            Err(LinkError::Host)
        }
    }

    /// Makes a host of an address.
    ///
    /// # Errors
    ///
    /// [`LinkError::Host`] for the unspecified address (`0.0.0.0`, `::`), a
    /// multicast address and the IPv4 broadcast address: a link must point
    /// at one server.
    pub fn from_ip(address: IpAddr) -> Result<Self, LinkError> {
        if names_one_host(&address) {
            Ok(Self(HostKind::Ip(address)))
        } else {
            Err(LinkError::Host)
        }
    }

    /// The DNS name, if the host is one.
    #[must_use]
    pub fn name(&self) -> Option<&str> {
        match &self.0 {
            HostKind::Name(name) => Some(name),
            HostKind::Ip(_) => None,
        }
    }

    /// The address, if the host is one.
    #[must_use]
    pub const fn ip(&self) -> Option<IpAddr> {
        match &self.0 {
            HostKind::Name(_) => None,
            HostKind::Ip(address) => Some(*address),
        }
    }
}

impl fmt::Display for Host {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match &self.0 {
            HostKind::Name(name) => f.write_str(name),
            HostKind::Ip(IpAddr::V4(address)) => write!(f, "{address}"),
            HostKind::Ip(IpAddr::V6(address)) => write!(f, "[{address}]"),
        }
    }
}

impl fmt::Debug for Host {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Host({self})")
    }
}

impl FromStr for Host {
    type Err = LinkError;

    fn from_str(text: &str) -> Result<Self, LinkError> {
        Self::parse(text)
    }
}

/// A link to a server: its address, its identity key and, usually, a join
/// code.
///
/// The link is a credential when it carries a join code, so it has no
/// `Display`; [`JoinLink::to_uri`] writes it out, and `Debug` leaves the code
/// out.
///
/// ```
/// use parties_identity::{JoinLink, SigningKey};
///
/// let server = SigningKey::from_seed(&[7; 32]).public_key();
/// let text = format!("parties://voice.example.org:7800/j/7KQ2M-9X01T#k={server}");
///
/// let link = JoinLink::parse(&text)?;
/// assert_eq!(link.host().name(), Some("voice.example.org"));
/// assert_eq!(link.port(), 7800);
/// assert_eq!(link.server_key(), &server);
/// let code = link.join_code().expect("the link carries a code");
/// assert_eq!(code.expose().as_str(), "7KQ2M-9X01T");
/// assert_eq!(link.to_uri().as_str(), text);
/// # Ok::<(), parties_identity::LinkError>(())
/// ```
#[derive(Clone, PartialEq, Eq)]
pub struct JoinLink {
    host: Host,
    port: u16,
    join_code: Option<JoinCode>,
    server_key: PublicKey,
}

impl JoinLink {
    /// Makes a link from its parts.
    ///
    /// # Errors
    ///
    /// [`LinkError::Port`] if the port is 0.
    pub fn new(
        host: Host,
        port: u16,
        join_code: Option<JoinCode>,
        server_key: PublicKey,
    ) -> Result<Self, LinkError> {
        if port == 0 {
            return Err(LinkError::Port);
        }
        Ok(Self {
            host,
            port,
            join_code,
            server_key,
        })
    }

    /// Reads a link. The grammar is in the module documentation.
    ///
    /// The text comes from outside (a chat message, a QR code, another
    /// program), so every part is checked and nothing is guessed.
    ///
    /// # Errors
    ///
    /// A [`LinkError`] naming the first part that is wrong.
    pub fn parse(text: &str) -> Result<Self, LinkError> {
        if text.len() > MAX_LINK_LEN {
            return Err(LinkError::TooLong);
        }
        if !text.bytes().all(|byte| byte.is_ascii_graphic()) {
            return Err(LinkError::Character);
        }
        let (scheme, rest) = text.split_once("://").ok_or(LinkError::Scheme)?;
        if !scheme.eq_ignore_ascii_case(LINK_SCHEME) {
            return Err(LinkError::Scheme);
        }
        let (location, fragment) = match rest.split_once('#') {
            Some((location, fragment)) => (location, Some(fragment)),
            None => (rest, None),
        };
        if location.contains('?') {
            return Err(LinkError::Query);
        }
        let (authority, path) = match location.find('/') {
            Some(at) => location.split_at(at),
            None => (location, ""),
        };
        if authority.contains('@') {
            return Err(LinkError::UserInfo);
        }

        // The port follows the last colon. An IPv6 host has colons of its
        // own inside its brackets, so without a port the text after the
        // last colon ends in a bracket and is refused as a port.
        let (host, port) = authority.rsplit_once(':').ok_or(LinkError::Port)?;
        let port = parse_port(port)?;
        let host = Host::parse(host)?;

        let join_code = match path {
            "" | "/" => None,
            _ => {
                let code = path.strip_prefix("/j/").ok_or(LinkError::Path)?;
                Some(JoinCode::parse(code)?)
            }
        };

        let mut server_key = None;
        for parameter in fragment.ok_or(LinkError::MissingKey)?.split('&') {
            if let Some(value) = parameter.strip_prefix("k=") {
                if server_key.is_some() {
                    return Err(LinkError::DuplicateKey);
                }
                server_key = Some(value.parse::<PublicKey>()?);
            }
        }

        Ok(Self {
            host,
            port,
            join_code,
            server_key: server_key.ok_or(LinkError::MissingKey)?,
        })
    }

    /// Writes the link in its canonical form. This reveals the join code;
    /// the copy is wiped when dropped.
    #[must_use]
    pub fn to_uri(&self) -> Zeroizing<String> {
        use fmt::Write as _;

        let Self {
            host,
            port,
            join_code,
            server_key,
        } = self;
        // Sized up front, so the text is written once and no partial copy
        // is left behind by a reallocation.
        let mut text = Zeroizing::new(String::with_capacity(CANONICAL_LINK_CAPACITY));
        // Writing to a `String` cannot fail.
        let _ = write!(text, "{LINK_SCHEME}://{host}:{port}");
        if let Some(code) = join_code {
            text.push_str("/j/");
            code.write(&mut text, true);
        }
        let _ = write!(text, "#k={server_key}");
        text
    }

    /// The server's host.
    #[must_use]
    pub const fn host(&self) -> &Host {
        &self.host
    }

    /// The server's UDP port, never 0.
    #[must_use]
    pub const fn port(&self) -> u16 {
        self.port
    }

    /// The join code, if the link carries one.
    #[must_use]
    pub const fn join_code(&self) -> Option<&JoinCode> {
        self.join_code.as_ref()
    }

    /// The server's identity key: the pin this link establishes.
    #[must_use]
    pub const fn server_key(&self) -> &PublicKey {
        &self.server_key
    }
}

/// Reads a port: decimal digits without sign or leading zero, 1 to 65535.
fn parse_port(text: &str) -> Result<u16, LinkError> {
    let plain = !text.is_empty()
        && !text.starts_with('0')
        && text.bytes().all(|byte| byte.is_ascii_digit());
    if !plain {
        return Err(LinkError::Port);
    }
    text.parse().map_err(|_| LinkError::Port)
}

impl fmt::Debug for JoinLink {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("JoinLink")
            .field("host", &self.host)
            .field("port", &self.port)
            .field("join_code", &self.join_code)
            .field("server_key", &self.server_key)
            .finish()
    }
}

impl FromStr for JoinLink {
    type Err = LinkError;

    fn from_str(text: &str) -> Result<Self, LinkError> {
        Self::parse(text)
    }
}
