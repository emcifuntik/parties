//! How a person is drawn: a picture, or initials.

use std::sync::Arc;

/// How a person is drawn: their picture, or their initials when there is none.
#[derive(Clone, Debug, PartialEq)]
pub struct Avatar {
    pub initials: String,
    pub picture: Option<Picture>,
}

impl Avatar {
    /// An avatar without a picture, with the first letter of `name`.
    pub fn monogram(name: &str) -> Self {
        Self {
            initials: name
                .chars()
                .next()
                .map(|first| first.to_uppercase().collect())
                .unwrap_or_default(),
            picture: None,
        }
    }

    pub fn with_picture(mut self, picture: Option<Picture>) -> Self {
        self.picture = picture;
        self
    }
}

/// An encoded image (PNG, JPEG, WebP). Two pictures with the same `key` are
/// the same image; the views use the key to decode each image once.
#[derive(Clone)]
pub struct Picture {
    pub key: u64,
    pub bytes: Arc<[u8]>,
}

impl Picture {
    /// A picture keyed by a hash of its bytes.
    pub fn from_bytes(bytes: impl Into<Arc<[u8]>>) -> Self {
        let bytes: Arc<[u8]> = bytes.into();
        // FNV-1a: stable across runs, unlike `DefaultHasher`.
        let key = bytes.iter().fold(0xcbf2_9ce4_8422_2325_u64, |hash, byte| {
            (hash ^ u64::from(*byte)).wrapping_mul(0x0000_0100_0000_01b3)
        });
        Self { key, bytes }
    }
}

impl PartialEq for Picture {
    fn eq(&self, other: &Self) -> bool {
        self.key == other.key
    }
}

impl std::fmt::Debug for Picture {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Picture")
            .field("key", &format_args!("{:016x}", self.key))
            .field("bytes", &self.bytes.len())
            .finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn monogram_takes_the_first_letter_in_upper_case() {
        assert_eq!(Avatar::monogram("liam").initials, "L");
        assert_eq!(Avatar::monogram("").initials, "");
    }

    #[test]
    fn pictures_compare_by_content() {
        let a = Picture::from_bytes(vec![1, 2, 3]);
        let b = Picture::from_bytes(vec![1, 2, 3]);
        let c = Picture::from_bytes(vec![1, 2, 4]);
        assert_eq!(a, b);
        assert_ne!(a, c);
    }
}
