//! Identity level: the hash, the check, and the bounded, resumable search.

mod common;

use common::{key, list, number, number32, public_key, vectors};
use parties_identity::level::{IDENTITY_LEVEL_LABEL, MAX_IDENTITY_LEVEL};
use parties_identity::{
    LevelSearch, LevelTooLow, identity_level, search_identity_level, verify_identity_level,
};

#[test]
fn label_and_range_are_pinned() {
    assert_eq!(IDENTITY_LEVEL_LABEL, b"parties-identity-level-v1");
    assert_eq!(MAX_IDENTITY_LEVEL, 256);
}

/// `SHA-256(label || 0x00 || key || counter as u64 little-endian)`, computed by
/// `tests/vectors/generate.py`. The counters include the ends of the range
/// and values that tell the byte order apart.
#[test]
fn levels_match_the_vectors() {
    let file = vectors("statements.json");
    let cases = list(&file, "level");
    assert!(cases.len() >= 20);
    for case in cases {
        let key = public_key(case, "public_key");
        let counter = number(case, "counter");
        let expected = number32(case, "level");

        assert_eq!(identity_level(&key, counter), expected, "counter {counter}");
        // The digest in the vector has exactly that many leading zero bits.
        let digest = common::bytes(case, "digest");
        let zeros: u32 = digest
            .iter()
            .position(|&byte| byte != 0)
            .map_or(256, |at| at as u32 * 8 + digest[at].leading_zeros());
        assert_eq!(zeros, expected);
    }
}

#[test]
fn search_finds_the_first_counter_of_the_vectors() {
    let file = vectors("statements.json");
    for case in list(&file, "level_search") {
        let key = public_key(case, "public_key");
        let target = number32(case, "target");
        let counter = number(case, "first_counter");
        let level = number32(case, "level");

        assert_eq!(
            search_identity_level(&key, target, 0, counter + 1),
            LevelSearch::Found { counter, level },
            "target {target}"
        );
        assert!(level >= target);
        assert_eq!(identity_level(&key, counter), level);
        assert_eq!(verify_identity_level(&key, counter, target), Ok(level));
    }
}

#[test]
fn search_stops_at_its_limit_and_resumes() {
    let key = key(7).public_key();
    // From the vectors: the first counter that reaches level 8 is 93.
    let whole = search_identity_level(&key, 8, 0, 1_000);
    assert_eq!(
        whole,
        LevelSearch::Found {
            counter: 93,
            level: 12
        }
    );

    // One attempt short: the best counter below 93 is 14, with level 6.
    let short = search_identity_level(&key, 8, 0, 93);
    assert_eq!(
        short,
        LevelSearch::Exhausted {
            best: Some((14, 6)),
            resume_at: Some(93)
        }
    );

    // In slices of ten, continuing from `resume_at`, the result is the same.
    let mut start = 0;
    let mut slices = 0;
    let found = loop {
        match search_identity_level(&key, 8, start, 10) {
            LevelSearch::Found { counter, level } => break (counter, level),
            LevelSearch::Exhausted {
                best: Some((counter, level)),
                resume_at: Some(next),
            } => {
                assert!(level < 8);
                assert!((start..next).contains(&counter));
                assert_eq!(next, start + 10);
                start = next;
                slices += 1;
            }
            other => panic!("unexpected result {other:?}"),
        }
    };
    assert_eq!(found, (93, 12));
    assert_eq!(slices, 9);
}

#[test]
fn search_reports_the_best_counter_of_the_range() {
    let key = key(7).public_key();
    // Levels of counters 0 to 9: 0 0 5 0 5 1 0 1 0 0. The first of the two
    // best counters is kept.
    assert_eq!(
        search_identity_level(&key, 200, 0, 10),
        LevelSearch::Exhausted {
            best: Some((2, 5)),
            resume_at: Some(10)
        }
    );
    assert_eq!(
        search_identity_level(&key, 200, 3, 1),
        LevelSearch::Exhausted {
            best: Some((3, 0)),
            resume_at: Some(4)
        }
    );
}

#[test]
fn search_with_no_attempts_does_nothing() {
    let key = key(7).public_key();
    assert_eq!(
        search_identity_level(&key, 1, 42, 0),
        LevelSearch::Exhausted {
            best: None,
            resume_at: Some(42)
        }
    );
    // Not even a target of zero is "found" without looking.
    assert_eq!(
        search_identity_level(&key, 0, 42, 0),
        LevelSearch::Exhausted {
            best: None,
            resume_at: Some(42)
        }
    );
}

#[test]
fn level_zero_is_reached_by_every_counter() {
    let key = key(7).public_key();
    assert_eq!(
        search_identity_level(&key, 0, 5, 1),
        LevelSearch::Found {
            counter: 5,
            level: identity_level(&key, 5)
        }
    );
    assert_eq!(verify_identity_level(&key, 0, 0), Ok(0));
}

#[test]
fn search_ends_at_the_last_counter() {
    let key = key(7).public_key();
    let last = u64::MAX;
    let level = identity_level(&key, last);
    assert_eq!(
        search_identity_level(&key, 200, last, 10),
        LevelSearch::Exhausted {
            best: Some((last, level)),
            resume_at: None
        }
    );
    // Two counters from the end.
    let result = search_identity_level(&key, 200, last - 1, u64::MAX);
    assert!(matches!(
        result,
        LevelSearch::Exhausted {
            best: Some(_),
            resume_at: None
        }
    ));
}

#[test]
fn targets_above_the_maximum_are_unreachable() {
    let key = key(7).public_key();
    assert_eq!(
        search_identity_level(&key, 257, 0, u64::MAX),
        LevelSearch::Unreachable
    );
    assert_eq!(
        search_identity_level(&key, u32::MAX, 0, 1),
        LevelSearch::Unreachable
    );
    // 256 itself is a legitimate, if hopeless, target.
    assert!(matches!(
        search_identity_level(&key, 256, 0, 5),
        LevelSearch::Exhausted {
            resume_at: Some(5),
            ..
        }
    ));
}

#[test]
fn verification_reports_both_levels() {
    let key = key(7).public_key();
    assert_eq!(identity_level(&key, 2), 5);
    assert_eq!(verify_identity_level(&key, 2, 5), Ok(5));
    assert_eq!(verify_identity_level(&key, 2, 3), Ok(5));
    assert_eq!(
        verify_identity_level(&key, 2, 6),
        Err(LevelTooLow {
            found: 5,
            required: 6
        })
    );
    assert_eq!(
        verify_identity_level(&key, 2, 1_000),
        Err(LevelTooLow {
            found: 5,
            required: 1_000
        })
    );
    // The counter of one key is worth nothing for another key.
    let other = common::key(8).public_key();
    assert!(
        identity_level(&other, 93) < 8,
        "unless by a 1 in 256 chance"
    );
    assert_eq!(identity_level(&key, 93), 12);
}
