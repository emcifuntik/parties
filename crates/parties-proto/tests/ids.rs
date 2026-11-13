//! Identifiers (section 5.2): the parts of the time-sortable id, its
//! ordering and its epoch.

use parties_proto::ids::{
    AttachmentId, AuditEntryId, ChannelId, EPOCH_UNIX_MS, IdError, JoinCodeId, MessageId, RoleId,
    SEQUENCE_BITS, SessionId, TIMESTAMP_BITS, TimeSortableId, TrackId, UserId,
};

#[test]
fn the_layout_is_42_bits_of_time_and_22_bits_of_sequence() {
    assert_eq!(TIMESTAMP_BITS, 42);
    assert_eq!(SEQUENCE_BITS, 22);
    assert_eq!(TIMESTAMP_BITS + SEQUENCE_BITS, 64);
    assert_eq!(TimeSortableId::MAX_TIMESTAMP_MS, 0x3FF_FFFF_FFFF);
    assert_eq!(TimeSortableId::MAX_SEQUENCE, 0x3F_FFFF);

    let id = TimeSortableId::new(0x123_4567_89AB, 0x2A_BCDE).expect("in range");
    assert_eq!(id.get(), (0x123_4567_89AB << 22) | 0x2A_BCDE);
    assert_eq!(id.timestamp_ms(), 0x123_4567_89AB);
    assert_eq!(id.sequence(), 0x2A_BCDE);
    assert_eq!(u64::from(id), id.get());
    assert_eq!(id.to_string(), id.get().to_string());
}

#[test]
fn the_extremes_fit() {
    let last = TimeSortableId::new(
        TimeSortableId::MAX_TIMESTAMP_MS,
        TimeSortableId::MAX_SEQUENCE,
    )
    .expect("the largest id");
    assert_eq!(last.get(), u64::MAX);
    assert_eq!(last.timestamp_ms(), TimeSortableId::MAX_TIMESTAMP_MS);
    assert_eq!(last.sequence(), TimeSortableId::MAX_SEQUENCE);

    let first = TimeSortableId::new(0, 1).expect("the smallest id");
    assert_eq!(first.get(), 1);
    assert_eq!((first.timestamp_ms(), first.sequence()), (0, 1));
}

#[test]
fn parts_out_of_range_are_refused() {
    assert_eq!(
        TimeSortableId::new(1 << 42, 0),
        Err(IdError::TimestampOutOfRange(1 << 42))
    );
    assert_eq!(
        TimeSortableId::new(0, 1 << 22),
        Err(IdError::SequenceOutOfRange(1 << 22))
    );
    assert_eq!(TimeSortableId::new(0, 0), Err(IdError::Zero));
    assert_eq!(
        TimeSortableId::from_unix_ms(EPOCH_UNIX_MS - 1, 0),
        Err(IdError::BeforeEpoch(EPOCH_UNIX_MS - 1))
    );
}

#[test]
fn zero_is_none() {
    assert_eq!(TimeSortableId::from_raw(0), None);
    assert_eq!(
        TimeSortableId::from_raw(1).map(TimeSortableId::get),
        Some(1)
    );
    assert!(UserId::NONE.is_none());
    assert_eq!(UserId::NONE.time_sortable(), None);
    assert!(!UserId(5).is_none());
    assert!(TrackId::NONE.is_none());
    assert!(SessionId(0).is_none());
    // The niche of the non-zero value keeps an optional id at eight bytes.
    assert_eq!(size_of::<Option<TimeSortableId>>(), 8);
}

#[test]
fn the_epoch_is_the_first_of_january_2026() {
    // 56 years from 1970 to 2026, 14 of them leap years (1972 to 2024).
    let days = 56 * 365 + 14;
    assert_eq!(EPOCH_UNIX_MS, days * 24 * 60 * 60 * 1000);

    // 2026-11-13T12:00:00Z is 316.5 days later.
    let unix_ms = EPOCH_UNIX_MS + 316 * 86_400_000 + 12 * 3_600_000;
    let id = TimeSortableId::from_unix_ms(unix_ms, 7).expect("after the epoch");
    assert_eq!(id.unix_ms(), unix_ms);
    assert_eq!(id.timestamp_ms(), unix_ms - EPOCH_UNIX_MS);
    assert_eq!(id.sequence(), 7);
}

#[test]
fn ids_fit_a_signed_64_bit_integer_until_2095() {
    // Bit 63 is the top bit of the timestamp: it is set from 2^41 ms after
    // the epoch on, which is in the year 2095.
    let ms_per_year = 31_556_952_000_u64; // 365.2425 days
    let years = (1_u64 << 41) / ms_per_year;
    assert_eq!(2026 + years, 2095);

    let before =
        TimeSortableId::new((1 << 41) - 1, TimeSortableId::MAX_SEQUENCE).expect("in range");
    assert!(i64::try_from(before.get()).is_ok());
    let after = TimeSortableId::new(1 << 41, 0).expect("in range");
    assert!(i64::try_from(after.get()).is_err());
}

#[test]
fn ids_sort_by_time_then_by_sequence() {
    let id =
        |timestamp_ms, sequence| TimeSortableId::new(timestamp_ms, sequence).expect("in range");
    let mut ids = [
        id(2000, 0),
        id(1000, 5),
        id(1000, 4),
        id(3000, TimeSortableId::MAX_SEQUENCE),
        id(3001, 0),
        id(1, 1),
    ];
    ids.sort();
    let parts: Vec<_> = ids
        .iter()
        .map(|id| (id.timestamp_ms(), id.sequence()))
        .collect();
    assert_eq!(
        parts,
        [
            (1, 1),
            (1000, 4),
            (1000, 5),
            (2000, 0),
            (3000, TimeSortableId::MAX_SEQUENCE),
            (3001, 0),
        ]
    );
    // The integer form sorts the same way, which is what a database index
    // relies on.
    assert!(ids.windows(2).all(|pair| pair[0].get() < pair[1].get()));
}

#[test]
fn typed_ids_wrap_the_integer_of_the_messages() {
    let raw = TimeSortableId::new(123_456, 9).expect("in range");
    let user = UserId::from(raw);
    assert_eq!(user, UserId(raw.get()));
    assert_eq!(user.get(), raw.get());
    assert_eq!(u64::from(user), raw.get());
    assert_eq!(UserId::from(raw.get()), user);
    assert_eq!(user.time_sortable(), Some(raw));
    assert_eq!(user.to_string(), raw.get().to_string());

    // Every kind of time-sortable id converts the same way.
    assert_eq!(RoleId::from(raw).time_sortable(), Some(raw));
    assert_eq!(ChannelId::from(raw).time_sortable(), Some(raw));
    assert_eq!(MessageId::from(raw).time_sortable(), Some(raw));
    assert_eq!(JoinCodeId::from(raw).time_sortable(), Some(raw));
    assert_eq!(AttachmentId::from(raw).time_sortable(), Some(raw));
    assert_eq!(AuditEntryId::from(raw).time_sortable(), Some(raw));

    // They order like their integers.
    assert!(ChannelId(1) < ChannelId(2));
    assert!(RoleId(7) > RoleId(3));
}

#[test]
fn session_and_track_ids() {
    assert_eq!(SessionId::from(77).get(), 77);
    assert_eq!(u64::from(SessionId(77)), 77);
    assert_eq!(TrackId::from(9).get(), 9);
    assert_eq!(u32::from(TrackId(u32::MAX)), u32::MAX);
    assert_eq!(TrackId(12).to_string(), "12");
    assert_eq!(size_of::<TrackId>(), 4);
}
