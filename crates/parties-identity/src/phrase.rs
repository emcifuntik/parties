//! Recovery phrases and the derivation of the identity key from them
//! (`docs/rust-rewrite/04-connection-security.md` section 5.2).
//!
//! | Version | Type | Derivation | Use |
//! |---|---|---|---|
//! | v1 | [`PhraseV1`] | `SHA-256(phrase bytes)` is the Ed25519 seed | Import of identities made by the C++ client. The same phrase keeps the same public key |
//! | v2 | [`PhraseV2`] | BIP-39 mnemonic with checksum, then PBKDF2-HMAC-SHA512, then HKDF-SHA256 | Every new identity |
//!
//! # Version 1, exactly as the C++ client does it
//!
//! `derive_keypair` in `common/src/crypto.cpp` hashes the phrase bytes as
//! they are: no trimming, no case folding, no Unicode normalisation.
//! `validate_seed_phrase` accepts twelve words of the list in lowercase,
//! separated by any run of the six ASCII white-space characters, with no
//! checksum. So `"a b"` and `"a  b"` are two valid phrases with two different
//! keys (finding F12). [`PhraseV1`] keeps the text byte for byte for that
//! reason, and [`import_phrase`] offers both the text as typed and its tidy
//! form when they differ.
//!
//! # Version 2
//!
//! ```text
//! entropy   16 bytes from the random-number generator (12 words)
//! mnemonic  BIP-39, English list, checksum = first bits of SHA-256(entropy)
//! seed      PBKDF2-HMAC-SHA512(password = mnemonic, salt = "mnemonic",
//!                              2048 rounds, 64 bytes)      -- BIP-39, no passphrase
//! key seed  HKDF-SHA256(salt = none, ikm = seed,
//!                       info = "parties/phrase/identity/v2", 32 bytes)
//! ```
//!
//! The mnemonic is the words in lowercase joined by single spaces. New
//! phrases always have twelve words ([`RECOVERY_ENTROPY_LEN`]); the other
//! BIP-39 lengths (15, 18, 21 and 24 words) are accepted when a phrase is
//! read, so the published BIP-39 test vectors apply unchanged.
//!
//! Parties does not use a BIP-39 passphrase: the salt is the plain string
//! `mnemonic`. [`PhraseV2::bip39_seed`] still takes one, because the
//! published vectors are made with the passphrase `TREZOR`.
//!
//! # Secrets
//!
//! Phrases, entropy and the intermediate seeds are wiped when dropped, and
//! the phrase types can be neither printed nor cloned. Errors name the
//! position of a wrong word, never the word.

use core::fmt;

use sha2::{Digest, Sha256, Sha512};
use zeroize::{Zeroize, ZeroizeOnDrop, Zeroizing};

use crate::fingerprint::Fingerprint;
use crate::kdf::hkdf_sha256;
use crate::keys::{PublicKey, SEED_LEN, SigningKey};
use crate::wordlist;

/// Entropy of a new recovery phrase, in bytes: 128 bits, twelve words.
///
/// It is also the length the passkey derivation produces
/// ([`crate::passkey`]).
pub const RECOVERY_ENTROPY_LEN: usize = 16;

/// Number of words of a version 1 phrase.
pub const V1_WORD_COUNT: usize = 12;

/// HKDF `info` of the version 2 derivation.
pub const PHRASE_V2_INFO: &[u8] = b"parties/phrase/identity/v2";

/// PBKDF2 rounds of the BIP-39 seed.
pub const BIP39_ROUNDS: u32 = 2048;

/// Length of the BIP-39 seed in bytes.
pub const BIP39_SEED_LEN: usize = 64;

/// Largest BIP-39 entropy, in bytes (24 words).
const MAX_ENTROPY_LEN: usize = 32;

/// Largest BIP-39 word count.
const MAX_WORD_COUNT: usize = 24;

/// Longest phrase text: 24 words of at most 8 letters, 23 spaces.
const MAX_PHRASE_TEXT_LEN: usize = MAX_WORD_COUNT * 8 + (MAX_WORD_COUNT - 1);

/// Which derivation turns a recovery phrase into the identity key.
///
/// The client stores this next to an identity.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PhraseVersion {
    /// `SHA-256(phrase bytes)`: identities made by the C++ client.
    V1,
    /// BIP-39, PBKDF2 and HKDF: every new identity.
    V2,
}

impl PhraseVersion {
    /// The version as a number, 1 or 2, for storage.
    #[must_use]
    pub const fn number(self) -> u8 {
        match self {
            Self::V1 => 1,
            Self::V2 => 2,
        }
    }

    /// The version with the given number.
    #[must_use]
    pub const fn from_number(number: u8) -> Option<Self> {
        match number {
            1 => Some(Self::V1),
            2 => Some(Self::V2),
            _ => None,
        }
    }
}

/// Why a text or entropy was not accepted as a recovery phrase.
///
/// No variant carries any part of the phrase.
#[derive(Clone, Copy, Debug, PartialEq, Eq, thiserror::Error)]
pub enum PhraseError {
    /// Not twelve words (version 1), or not 12, 15, 18, 21 or 24.
    #[error("a recovery phrase cannot have {found} words")]
    WordCount {
        /// How many words were given.
        found: usize,
    },
    /// A word is not in the list (for version 1: not in lowercase).
    #[error("word {position} of the recovery phrase is not in the word list")]
    UnknownWord {
        /// Position of the word, counted from 1.
        position: usize,
    },
    /// The words do not carry a valid BIP-39 checksum. From
    /// [`import_phrase`]: and they cannot be a version 1 phrase either.
    #[error("the recovery phrase has a wrong checksum")]
    Checksum,
    /// Entropy must be 16, 20, 24, 28 or 32 bytes.
    #[error("recovery entropy cannot have {found} bytes")]
    EntropyLength {
        /// How many bytes were given.
        found: usize,
    },
}

/// The white space of `std::istringstream >> word` in the "C" locale, which
/// is how the C++ client splits a phrase. Note the vertical tab, which
/// `char::is_ascii_whitespace` leaves out.
const fn is_c_space(character: char) -> bool {
    matches!(character, ' ' | '\t' | '\n' | '\x0B' | '\x0C' | '\r')
}

/// The words of a version 1 phrase, split as the C++ client splits them.
fn c_words(text: &str) -> impl Iterator<Item = &str> {
    text.split(is_c_space).filter(|word| !word.is_empty())
}

/// Appends the words with the given indices, joined by single spaces.
fn push_words(text: &mut String, indices: &[u16]) {
    for (position, &index) in indices.iter().enumerate() {
        if position > 0 {
            text.push(' ');
        }
        // Every index comes from the list or from an 11-bit mask.
        text.push_str(wordlist::word(index).unwrap_or_default());
    }
}

/// An empty string that never reallocates while a phrase is written to it,
/// so no stale copy of the phrase stays behind on the heap.
fn phrase_buffer() -> Zeroizing<String> {
    Zeroizing::new(String::with_capacity(MAX_PHRASE_TEXT_LEN))
}

/// A version 1 recovery phrase: the exact text the C++ client hashed.
///
/// ```
/// use parties_identity::PhraseV1;
///
/// let phrase = PhraseV1::parse(
///     "abandon abandon abandon abandon abandon abandon \
///      abandon abandon abandon abandon abandon about",
/// )?;
/// assert_eq!(
///     phrase.signing_key().fingerprint().to_string(),
///     "6f:d1:45:bc:c8:f8:07:1a:72:39:61:9b:6a:1a:68:cb:\
///      8c:8b:2d:ea:2b:48:85:fb:c8:dd:c7:ff:f1:63:f2:c9",
/// );
/// # Ok::<(), parties_identity::PhraseError>(())
/// ```
pub struct PhraseV1 {
    text: Zeroizing<String>,
}

impl PhraseV1 {
    /// Accepts a phrase by the rules of `validate_seed_phrase` and keeps it
    /// byte for byte.
    ///
    /// The rules: split at runs of space, tab, line feed, vertical tab, form
    /// feed and carriage return; every piece is a word of the list, in
    /// lowercase; there are exactly twelve. There is no checksum.
    ///
    /// # Errors
    ///
    /// [`PhraseError::UnknownWord`] for the first piece that is not a
    /// lowercase word of the list, [`PhraseError::WordCount`] if all pieces
    /// are words but there are not twelve.
    pub fn parse(text: &str) -> Result<Self, PhraseError> {
        let mut found = 0usize;
        for word in c_words(text) {
            found += 1;
            let exact = word.bytes().all(|byte| byte.is_ascii_lowercase())
                && wordlist::index_of(word).is_some();
            if !exact {
                return Err(PhraseError::UnknownWord { position: found });
            }
        }
        if found != V1_WORD_COUNT {
            return Err(PhraseError::WordCount { found });
        }
        Ok(Self {
            text: Zeroizing::new(text.to_owned()),
        })
    }

    /// The tidy phrase of twelve word indices: lowercase, single spaces.
    /// This is the form `generate_seed_phrase` produced.
    fn from_indices(indices: &[u16; V1_WORD_COUNT]) -> Self {
        let mut text = phrase_buffer();
        push_words(&mut text, indices);
        Self { text }
    }

    /// The phrase, exactly as it is hashed. This reveals the secret.
    #[must_use]
    pub fn expose(&self) -> &str {
        &self.text
    }

    /// Whether the text is in the form the C++ client generated: the twelve
    /// words separated by single spaces and nothing else.
    #[must_use]
    pub fn is_tidy(&self) -> bool {
        // Cut at every single space: an empty piece means a leading,
        // trailing or doubled space, and no piece may hold other white space.
        self.text
            .split(' ')
            .all(|piece| !piece.is_empty() && !piece.contains(is_c_space))
    }

    /// Derives the identity key: the Ed25519 seed is `SHA-256(phrase bytes)`.
    ///
    /// Bit for bit what `derive_keypair` of the C++ client computes.
    #[must_use]
    pub fn signing_key(&self) -> SigningKey {
        let mut seed = Zeroizing::new([0u8; SEED_LEN]);
        let mut hasher = Sha256::new();
        hasher.update(self.text.as_bytes());
        hasher.finalize_into((&mut *seed).into());
        SigningKey::from_seed(&seed)
    }
}

/// The text is wiped when the phrase is dropped.
impl ZeroizeOnDrop for PhraseV1 {}

/// A version 2 recovery phrase: a BIP-39 mnemonic of the English list.
///
/// The value holds the entropy; the words are computed from it on demand.
///
/// ```
/// use parties_identity::PhraseV2;
///
/// let phrase = PhraseV2::from_entropy(&[0x7f; 16])?;
/// assert_eq!(
///     phrase.expose().as_str(),
///     "legal winner thank year wave sausage worth useful legal winner thank yellow",
/// );
///
/// // Reading is forgiving about case and spacing, strict about the checksum.
/// let typed = "Legal winner  thank year\twave sausage worth useful legal winner thank yellow\n";
/// let again = PhraseV2::parse(typed)?;
/// assert_eq!(again.entropy(), phrase.entropy());
/// assert_eq!(
///     again.signing_key().public_key(),
///     phrase.signing_key().public_key(),
/// );
/// # Ok::<(), parties_identity::PhraseError>(())
/// ```
pub struct PhraseV2 {
    /// The first `entropy_len` bytes are the entropy; the rest is zero.
    entropy: Zeroizing<[u8; MAX_ENTROPY_LEN]>,
    entropy_len: usize,
}

impl PhraseV2 {
    /// Makes the phrase of the given entropy.
    ///
    /// New identities use [`RECOVERY_ENTROPY_LEN`] bytes from a
    /// cryptographic random-number generator. The caller owns the entropy
    /// and should wipe its copy.
    ///
    /// # Errors
    ///
    /// [`PhraseError::EntropyLength`] unless the entropy has 16, 20, 24, 28
    /// or 32 bytes.
    pub fn from_entropy(entropy: &[u8]) -> Result<Self, PhraseError> {
        if !matches!(entropy.len(), 16 | 20 | 24 | 28 | 32) {
            return Err(PhraseError::EntropyLength {
                found: entropy.len(),
            });
        }
        let mut stored = Zeroizing::new([0u8; MAX_ENTROPY_LEN]);
        stored[..entropy.len()].copy_from_slice(entropy);
        Ok(Self {
            entropy: stored,
            entropy_len: entropy.len(),
        })
    }

    /// Generates a new twelve-word phrase from the operating system's
    /// random-number generator.
    ///
    /// # Panics
    ///
    /// If the operating system cannot provide random bytes
    /// (`docs/rust-rewrite/04-connection-security.md`, finding F21).
    #[cfg(feature = "os-rng")]
    #[must_use]
    pub fn generate() -> Self {
        let mut entropy = Zeroizing::new([0u8; RECOVERY_ENTROPY_LEN]);
        crate::fill_random(entropy.as_mut_slice());
        Self::from_recovery_entropy(&entropy)
    }

    /// The twelve-word phrase of entropy of the standard length.
    pub(crate) fn from_recovery_entropy(entropy: &[u8; RECOVERY_ENTROPY_LEN]) -> Self {
        let mut stored = Zeroizing::new([0u8; MAX_ENTROPY_LEN]);
        stored[..RECOVERY_ENTROPY_LEN].copy_from_slice(entropy);
        Self {
            entropy: stored,
            entropy_len: RECOVERY_ENTROPY_LEN,
        }
    }

    /// Reads a phrase a person typed.
    ///
    /// Words may be separated by any white space and written in any case;
    /// the BIP-39 checksum must be right.
    ///
    /// # Errors
    ///
    /// [`PhraseError::UnknownWord`], [`PhraseError::WordCount`] or
    /// [`PhraseError::Checksum`].
    pub fn parse(text: &str) -> Result<Self, PhraseError> {
        let words = Words::read(text)?;
        Self::from_words(&words).ok_or(PhraseError::Checksum)
    }

    /// The phrase of these words, if their checksum is right.
    fn from_words(words: &Words) -> Option<Self> {
        // 11 bits per word, most significant first, packed into bytes. A
        // last partial byte is padded with zero bits, so the checksum bits
        // always start at the top of the byte after the entropy.
        let mut packed = Zeroizing::new([0u8; MAX_ENTROPY_LEN + 1]);
        let mut written = 0usize;
        let mut window = 0u32;
        let mut bits = 0u32;
        for &index in words.indices() {
            window = (window << 11) | u32::from(index);
            bits += 11;
            while bits >= 8 {
                bits -= 8;
                packed[written] = (window >> bits) as u8;
                written += 1;
                window &= (1 << bits) - 1;
            }
        }
        if bits > 0 {
            packed[written] = (window << (8 - bits)) as u8;
        }
        window.zeroize();

        let entropy_len = words.len() / 3 * 4;
        let checksum_bits = entropy_len / 4;
        let digest = Sha256::digest(&packed[..entropy_len]);
        let shift = 8 - checksum_bits;
        if packed[entropy_len] >> shift != digest[0] >> shift {
            return None;
        }
        let mut entropy = Zeroizing::new([0u8; MAX_ENTROPY_LEN]);
        entropy[..entropy_len].copy_from_slice(&packed[..entropy_len]);
        Some(Self {
            entropy,
            entropy_len,
        })
    }

    /// The entropy the phrase encodes: 16 bytes for a phrase made by this
    /// crate. This reveals the secret; it is what the OS keystore holds.
    #[must_use]
    pub fn entropy(&self) -> &[u8] {
        &self.entropy[..self.entropy_len]
    }

    /// Number of words: 12 for a phrase made by this crate.
    #[must_use]
    pub const fn word_count(&self) -> usize {
        self.entropy_len / 4 * 3
    }

    /// The word indices: entropy followed by the checksum, 11 bits a word.
    fn words(&self) -> Words {
        let entropy = self.entropy();
        let checksum = Sha256::digest(entropy)[0];
        let mut words = Words::empty();
        let mut window = 0u32;
        let mut bits = 0u32;
        for &byte in entropy.iter().chain(core::iter::once(&checksum)) {
            window = (window << 8) | u32::from(byte);
            bits += 8;
            while bits >= 11 {
                bits -= 11;
                // An 11-bit value; entropy plus one checksum byte never
                // yields more words than the phrase has.
                words.push(((window >> bits) & 0x7FF) as u16);
                window &= (1 << bits) - 1;
            }
        }
        window.zeroize();
        words
    }

    /// The phrase: the words in lowercase, joined by single spaces. This
    /// reveals the secret; the copy is wiped when dropped.
    #[must_use]
    pub fn expose(&self) -> Zeroizing<String> {
        let mut text = phrase_buffer();
        push_words(&mut text, self.words().indices());
        text
    }

    /// The BIP-39 seed: `PBKDF2-HMAC-SHA512(phrase, "mnemonic" || passphrase,
    /// 2048 rounds, 64 bytes)`.
    ///
    /// Parties derives identities with the empty passphrase
    /// ([`PhraseV2::signing_key`]). The parameter exists so that the
    /// published BIP-39 vectors, which use `TREZOR`, can be checked. The
    /// passphrase is used as given: BIP-39 asks for NFKD normalisation, which
    /// is the caller's job for anything that is not ASCII.
    #[must_use]
    pub fn bip39_seed(&self, passphrase: &str) -> Zeroizing<[u8; BIP39_SEED_LEN]> {
        const SALT_PREFIX: &[u8] = b"mnemonic";

        let phrase = self.expose();
        let mut salt = Zeroizing::new(Vec::with_capacity(SALT_PREFIX.len() + passphrase.len()));
        salt.extend_from_slice(SALT_PREFIX);
        salt.extend_from_slice(passphrase.as_bytes());
        let mut seed = Zeroizing::new([0u8; BIP39_SEED_LEN]);
        pbkdf2::pbkdf2_hmac::<Sha512>(phrase.as_bytes(), &salt, BIP39_ROUNDS, seed.as_mut_slice());
        seed
    }

    /// Derives the identity key: `HKDF-SHA256` over the BIP-39 seed (empty
    /// passphrase) with the label [`PHRASE_V2_INFO`] gives the Ed25519 seed.
    #[must_use]
    pub fn signing_key(&self) -> SigningKey {
        let seed = self.bip39_seed("");
        let key_seed = hkdf_sha256::<SEED_LEN>(seed.as_slice(), PHRASE_V2_INFO);
        SigningKey::from_seed(&key_seed)
    }
}

/// The entropy is wiped when the phrase is dropped.
impl ZeroizeOnDrop for PhraseV2 {}

/// Up to 24 word indices of a phrase being read or written. Wiped on drop.
struct Words {
    indices: Zeroizing<[u16; MAX_WORD_COUNT]>,
    len: usize,
}

impl Words {
    fn empty() -> Self {
        Self {
            indices: Zeroizing::new([0u16; MAX_WORD_COUNT]),
            len: 0,
        }
    }

    /// Adds a word. A 25th word is dropped; no caller produces one.
    fn push(&mut self, index: u16) {
        if let Some(slot) = self.indices.get_mut(self.len) {
            *slot = index;
            self.len += 1;
        }
    }

    const fn len(&self) -> usize {
        self.len
    }

    fn indices(&self) -> &[u16] {
        &self.indices[..self.len]
    }

    /// Reads the words of a typed phrase: split at any white space, compared
    /// without regard to ASCII case. The count must be one BIP-39 allows.
    fn read(text: &str) -> Result<Self, PhraseError> {
        let mut words = Self::empty();
        let mut found = 0usize;
        for word in text.split_whitespace() {
            found += 1;
            let index =
                wordlist::index_of(word).ok_or(PhraseError::UnknownWord { position: found })?;
            words.push(index);
        }
        if matches!(found, 12 | 15 | 18 | 21 | 24) {
            Ok(words)
        } else {
            Err(PhraseError::WordCount { found })
        }
    }
}

/// A recovery phrase of either version.
pub enum RecoveryPhrase {
    /// A phrase of the C++ client.
    V1(PhraseV1),
    /// A BIP-39 phrase.
    V2(PhraseV2),
}

impl RecoveryPhrase {
    /// Which derivation the phrase uses.
    #[must_use]
    pub const fn version(&self) -> PhraseVersion {
        match self {
            Self::V1(_) => PhraseVersion::V1,
            Self::V2(_) => PhraseVersion::V2,
        }
    }

    /// The phrase as text. This reveals the secret; the copy is wiped when
    /// dropped.
    #[must_use]
    pub fn expose(&self) -> Zeroizing<String> {
        match self {
            Self::V1(phrase) => Zeroizing::new(phrase.expose().to_owned()),
            Self::V2(phrase) => phrase.expose(),
        }
    }

    /// Derives the identity key with the phrase's own derivation.
    #[must_use]
    pub fn signing_key(&self) -> SigningKey {
        match self {
            Self::V1(phrase) => phrase.signing_key(),
            Self::V2(phrase) => phrase.signing_key(),
        }
    }
}

/// Both kinds of phrase wipe themselves when dropped.
impl ZeroizeOnDrop for RecoveryPhrase {}

impl From<PhraseV1> for RecoveryPhrase {
    fn from(phrase: PhraseV1) -> Self {
        Self::V1(phrase)
    }
}

impl From<PhraseV2> for RecoveryPhrase {
    fn from(phrase: PhraseV2) -> Self {
        Self::V2(phrase)
    }
}

/// How [`import_phrase`] read the text to arrive at a candidate.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ImportReading {
    /// A BIP-39 phrase with a valid checksum, derived with version 2.
    V2,
    /// Version 1 over the tidy text: the words in lowercase with single
    /// spaces. This is the form the C++ client generated, so it is the
    /// right reading for nearly every 1.x identity.
    V1,
    /// Version 1 over the text exactly as typed, which has extra white
    /// space. Only offered when the C++ client would have accepted that
    /// text, because then it may have made an identity from it.
    V1AsTyped,
}

impl ImportReading {
    /// The derivation this reading uses.
    #[must_use]
    pub const fn version(self) -> PhraseVersion {
        match self {
            Self::V2 => PhraseVersion::V2,
            Self::V1 | Self::V1AsTyped => PhraseVersion::V1,
        }
    }
}

/// One identity a typed phrase can stand for.
///
/// `Debug` shows the reading and the fingerprint, never the phrase or the
/// key.
pub struct ImportCandidate {
    /// How the text was read.
    pub reading: ImportReading,
    /// The phrase to store if the user picks this candidate.
    pub phrase: RecoveryPhrase,
    /// The identity key of this candidate.
    pub signing_key: SigningKey,
}

impl fmt::Debug for ImportCandidate {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ImportCandidate")
            .field("reading", &self.reading)
            .field("fingerprint", &self.fingerprint())
            .finish_non_exhaustive()
    }
}

impl ImportCandidate {
    fn new(reading: ImportReading, phrase: RecoveryPhrase) -> Self {
        let signing_key = phrase.signing_key();
        Self {
            reading,
            phrase,
            signing_key,
        }
    }

    /// The public key of this candidate.
    #[must_use]
    pub fn public_key(&self) -> PublicKey {
        self.signing_key.public_key()
    }

    /// The fingerprint of this candidate: what the client compares with a
    /// fingerprint it knows, or shows to the user to choose from.
    #[must_use]
    pub fn fingerprint(&self) -> Fingerprint {
        self.signing_key.fingerprint()
    }
}

/// Reads a recovery phrase without knowing its version, and returns every
/// identity it can stand for.
///
/// The text is split at white space and compared without regard to case.
/// The result has one to three candidates, most likely first:
///
/// | Candidate | Offered when |
/// |---|---|
/// | [`ImportReading::V2`] | The BIP-39 checksum is right |
/// | [`ImportReading::V1`] | There are twelve words |
/// | [`ImportReading::V1AsTyped`] | There are twelve words, the C++ client would have accepted the text as typed, and it differs from the tidy form |
///
/// One in sixteen version 1 phrases happens to carry a valid checksum, so a
/// twelve-word phrase with a valid checksum is ambiguous. The caller settles
/// it: by the fingerprint it expects, or by showing the fingerprints and
/// letting the user choose.
///
/// The other thing the 1.x client imports, a private key written as 64 hex
/// digits, is not a phrase: see [`SigningKey::from_seed_hex`].
///
/// ```
/// use parties_identity::{ImportReading, import_phrase};
///
/// // No valid checksum: this can only be a phrase of the C++ client.
/// let candidates = import_phrase(
///     "abandon abandon abandon abandon abandon abandon \
///      abandon abandon abandon abandon abandon abandon",
/// )?;
/// assert_eq!(candidates.len(), 1);
/// assert_eq!(candidates[0].reading, ImportReading::V1);
///
/// // A valid checksum: the client has to ask, or to know the fingerprint.
/// let candidates = import_phrase(
///     "abandon abandon abandon abandon abandon abandon \
///      abandon abandon abandon abandon abandon about",
/// )?;
/// let readings: Vec<_> = candidates.iter().map(|candidate| candidate.reading).collect();
/// assert_eq!(readings, [ImportReading::V2, ImportReading::V1]);
/// assert_ne!(candidates[0].fingerprint(), candidates[1].fingerprint());
/// # Ok::<(), parties_identity::PhraseError>(())
/// ```
///
/// # Errors
///
/// [`PhraseError::UnknownWord`] and [`PhraseError::WordCount`] as for
/// [`PhraseV2::parse`]; [`PhraseError::Checksum`] if the checksum is wrong
/// and the phrase does not have twelve words.
pub fn import_phrase(text: &str) -> Result<Vec<ImportCandidate>, PhraseError> {
    let words = Words::read(text)?;
    let mut candidates = Vec::with_capacity(3);

    if let Some(phrase) = PhraseV2::from_words(&words) {
        candidates.push(ImportCandidate::new(ImportReading::V2, phrase.into()));
    }
    if let Ok(indices) = <&[u16; V1_WORD_COUNT]>::try_from(words.indices()) {
        let tidy = PhraseV1::from_indices(indices);
        let as_typed = PhraseV1::parse(text).ok().filter(|typed| !typed.is_tidy());
        candidates.push(ImportCandidate::new(ImportReading::V1, tidy.into()));
        if let Some(typed) = as_typed {
            candidates.push(ImportCandidate::new(ImportReading::V1AsTyped, typed.into()));
        }
    }

    if candidates.is_empty() {
        Err(PhraseError::Checksum)
    } else {
        Ok(candidates)
    }
}
