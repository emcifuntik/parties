//! Join codes and join links: the alphabet, the grammar, and everything a
//! parser of text from outside has to refuse.

mod common;

use std::net::{IpAddr, Ipv4Addr, Ipv6Addr};

use common::key;
use parties_identity::join::{JOIN_CODE_ALPHABET, JOIN_CODE_LEN, LINK_SCHEME, MAX_LINK_LEN};
use parties_identity::{
    Host, JoinCode, JoinCodeError, JoinLink, KeyError, LengthError, LinkError, PublicKey,
};

const KEY_TEXT: &str = "6kpsY-KcUgq-9VB7Ey7F-ZVHdq6-vnuSQh7qaRRG0iw";

fn server() -> PublicKey {
    key(7).public_key()
}

fn code(text: &str) -> JoinCode {
    JoinCode::parse(text).unwrap_or_else(|error| panic!("{text}: {error}"))
}

// ---------------------------------------------------------------------------
// Join codes
// ---------------------------------------------------------------------------

#[test]
fn alphabet_is_crockford_base32() {
    assert_eq!(JOIN_CODE_LEN, 10);
    assert_eq!(JOIN_CODE_ALPHABET, b"0123456789ABCDEFGHJKMNPQRSTVWXYZ");
    assert_eq!(JOIN_CODE_ALPHABET.len(), 32);
    let mut sorted = JOIN_CODE_ALPHABET.to_vec();
    sorted.sort_unstable();
    sorted.dedup();
    assert_eq!(sorted.len(), 32, "32 different symbols");
    for look_alike in b"ILOU" {
        assert!(!JOIN_CODE_ALPHABET.contains(look_alike));
    }
}

#[test]
fn codes_are_shown_in_two_groups_of_five() {
    let parsed = code("7KQ2M-9X01T");
    assert_eq!(parsed.expose().as_str(), "7KQ2M-9X01T");
    assert_eq!(parsed.expose_compact().as_str(), "7KQ2M9X01T");
    assert_eq!(code("7KQ2M9X01T"), parsed);
    assert_eq!("7KQ2M-9X01T".parse::<JoinCode>(), Ok(parsed));
}

#[test]
fn codes_are_read_without_regard_to_case_and_look_alikes() {
    let canonical = code("0123456789");
    assert_eq!(canonical.expose().as_str(), "01234-56789");
    // O for 0; I and L for 1; any case.
    for typed in [
        "O123456789",
        "o123456789",
        "0I23456789",
        "0l23456789",
        "OL234-56789",
    ] {
        assert_eq!(code(typed), canonical, "{typed}");
    }
    assert_eq!(code("abcde-fghjk").expose().as_str(), "ABCDE-FGHJK");
    assert_eq!(code("mnpqrstvwx").expose().as_str(), "MNPQR-STVWX");
    assert_eq!(code("YZ000-00000").expose_compact().as_str(), "YZ00000000");
}

#[test]
fn every_symbol_round_trips() {
    for (value, &symbol) in JOIN_CODE_ALPHABET.iter().enumerate() {
        let value = u8::try_from(value).expect("fewer than 256 symbols");
        let from_value = JoinCode::from_random(&[value; 10]);
        let text: String = std::iter::repeat_n(char::from(symbol), 10).collect();
        assert_eq!(from_value.expose_compact().as_str(), text);
        assert_eq!(code(&text), from_value);
        assert_eq!(code(&text.to_ascii_lowercase()), from_value);
    }
}

#[test]
fn random_bytes_use_their_low_five_bits() {
    let low = JoinCode::from_random(&[0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
    assert_eq!(low.expose().as_str(), "01234-56789");
    let high = JoinCode::from_random(&[0xE0, 0xE1, 0x22, 0x43, 0x64, 0x85, 0xA6, 0xC7, 0xE8, 0x09]);
    assert_eq!(high, low);
    assert_eq!(
        JoinCode::from_random(&[0xFF; 10]).expose().as_str(),
        "ZZZZZ-ZZZZZ"
    );
    assert_eq!(
        JoinCode::from_random(&[31; 10]).expose().as_str(),
        "ZZZZZ-ZZZZZ"
    );
}

#[test]
fn codes_of_another_shape_are_refused() {
    let error = |text: &str| JoinCode::parse(text).err();
    for text in [
        "",
        "7KQ2M",
        "7KQ2M-9X0",
        "7KQ2M-9X01TT",
        "7KQ2M9X01TT",
        "7KQ2-M9X01T",
        "7KQ2M--9X01T",
        "7KQ2M 9X01T",
        "7KQ2M_9X01T",
        " 7KQ2M9X01T",
        "7KQ2M9X01T ",
        "7KQ2M-9X01T\n",
        "-7KQ2M9X01T",
        "7KQ2M9X01T-",
    ] {
        assert_eq!(error(text), Some(JoinCodeError::Format), "{text:?}");
    }
}

#[test]
fn symbols_outside_the_alphabet_are_refused() {
    let error = |text: &str| JoinCode::parse(text).err();
    assert_eq!(
        error("U123456789"),
        Some(JoinCodeError::Character { position: 1 })
    );
    assert_eq!(
        error("0123u-56789"),
        Some(JoinCodeError::Character { position: 5 })
    );
    assert_eq!(
        error("01234-5678!"),
        Some(JoinCodeError::Character { position: 10 })
    );
    assert_eq!(
        error("01234-56-89"),
        Some(JoinCodeError::Character { position: 8 })
    );
    assert_eq!(
        error("0123456 89"),
        Some(JoinCodeError::Character { position: 8 })
    );
    // Ten characters with the hyphen among them: one symbol is missing.
    assert_eq!(
        error("7KQ2M-9X01"),
        Some(JoinCodeError::Character { position: 6 })
    );
    // Ten bytes, but a two-byte character among them.
    assert_eq!(
        error("01234567\u{e9}"),
        Some(JoinCodeError::Character { position: 9 })
    );
}

#[cfg(feature = "os-rng")]
#[test]
fn generated_codes_differ_and_parse() {
    let first = JoinCode::generate();
    let second = JoinCode::generate();
    // 50 random bits: equal codes mean a broken generator.
    assert_ne!(first, second);
    assert_eq!(code(&first.expose()), first);
    assert_eq!(code(&first.expose_compact()), first);
}

// ---------------------------------------------------------------------------
// Join links: what is accepted
// ---------------------------------------------------------------------------

#[test]
fn link_with_a_join_code() {
    let text = format!("parties://voice.example.org:7800/j/7KQ2M-9X01T#k={KEY_TEXT}");
    let link = JoinLink::parse(&text).expect("valid link");
    assert_eq!(link.host().name(), Some("voice.example.org"));
    assert_eq!(link.host().ip(), None);
    assert_eq!(link.port(), 7800);
    assert_eq!(link.join_code(), Some(&code("7KQ2M9X01T")));
    assert_eq!(link.server_key(), &server());
    assert_eq!(link.to_uri().as_str(), text);
    assert_eq!(text.parse::<JoinLink>(), Ok(link));
}

/// `partiesctl --server parties://host:7800#k=<key>`: an address and a pin.
#[test]
fn link_without_a_join_code() {
    let text = format!("parties://example.org:7800#k={KEY_TEXT}");
    let link = JoinLink::parse(&text).expect("valid link");
    assert_eq!(link.join_code(), None);
    assert_eq!(link.to_uri().as_str(), text);

    let with_slash = format!("parties://example.org:7800/#k={KEY_TEXT}");
    assert_eq!(JoinLink::parse(&with_slash), Ok(link));
}

#[test]
fn link_is_normalised() {
    let typed = format!("PARTIES://Voice.Example.ORG:7800/j/7kq2m9xolt#k={KEY_TEXT}");
    let link = JoinLink::parse(&typed).expect("valid link");
    assert_eq!(
        link.to_uri().as_str(),
        format!("parties://voice.example.org:7800/j/7KQ2M-9X01T#k={KEY_TEXT}")
    );
    assert_eq!(LINK_SCHEME, "parties");
}

#[test]
fn link_hosts_can_be_addresses() {
    let v4 = JoinLink::parse(&format!("parties://203.0.113.7:7800#k={KEY_TEXT}")).expect("IPv4");
    assert_eq!(
        v4.host().ip(),
        Some(IpAddr::V4(Ipv4Addr::new(203, 0, 113, 7)))
    );
    assert_eq!(v4.host().name(), None);
    assert_eq!(v4.host().to_string(), "203.0.113.7");

    let v6 = JoinLink::parse(&format!(
        "parties://[2001:db8::7]:7800/j/7KQ2M-9X01T#k={KEY_TEXT}"
    ))
    .expect("IPv6");
    let address: Ipv6Addr = "2001:db8::7".parse().expect("address");
    assert_eq!(v6.host().ip(), Some(IpAddr::V6(address)));
    assert_eq!(v6.host().to_string(), "[2001:db8::7]");
    assert_eq!(
        v6.to_uri().as_str(),
        format!("parties://[2001:db8::7]:7800/j/7KQ2M-9X01T#k={KEY_TEXT}")
    );

    // An address is written in its canonical form.
    let long = JoinLink::parse(&format!(
        "parties://[2001:0DB8:0000:0000:0000:0000:0000:0007]:7800#k={KEY_TEXT}"
    ))
    .expect("IPv6, written out");
    assert_eq!(long.host(), v6.host());

    let loopback = JoinLink::parse(&format!("parties://localhost:1#k={KEY_TEXT}")).expect("name");
    assert_eq!(loopback.host().name(), Some("localhost"));
    assert_eq!(loopback.port(), 1);
    let highest = JoinLink::parse(&format!("parties://[::1]:65535#k={KEY_TEXT}")).expect("IPv6");
    assert_eq!(highest.port(), 65535);
}

#[test]
fn unknown_fragment_parameters_are_ignored() {
    let plain = JoinLink::parse(&format!("parties://example.org:7800#k={KEY_TEXT}")).expect("link");
    for fragment in [
        format!("k={KEY_TEXT}&n=My%20server"),
        format!("v=2&k={KEY_TEXT}"),
        format!("x&k={KEY_TEXT}&&y=1"),
        format!("kk=1&k={KEY_TEXT}&K=2"),
    ] {
        let text = format!("parties://example.org:7800#{fragment}");
        assert_eq!(JoinLink::parse(&text), Ok(plain.clone()), "{fragment}");
    }
}

#[test]
fn links_are_built_from_parts() {
    let host = Host::parse("Example.ORG").expect("name");
    let link = JoinLink::new(host.clone(), 7800, Some(code("7KQ2M9X01T")), server()).expect("link");
    assert_eq!(
        link.to_uri().as_str(),
        format!("parties://example.org:7800/j/7KQ2M-9X01T#k={KEY_TEXT}")
    );
    assert_eq!(JoinLink::parse(&link.to_uri()), Ok(link));

    let by_address = JoinLink::new(
        Host::from_ip(IpAddr::V6(Ipv6Addr::LOCALHOST)).expect("loopback"),
        7800,
        None,
        server(),
    )
    .expect("link");
    assert_eq!(
        by_address.to_uri().as_str(),
        format!("parties://[::1]:7800#k={KEY_TEXT}")
    );

    assert_eq!(JoinLink::new(host, 0, None, server()), Err(LinkError::Port));
}

// ---------------------------------------------------------------------------
// Join links: what is refused
// ---------------------------------------------------------------------------

fn refused(text: &str) -> LinkError {
    JoinLink::parse(text).expect_err(text)
}

#[test]
fn links_with_another_scheme_are_refused() {
    for text in [
        String::new(),
        "parties".to_owned(),
        format!("https://example.org:7800/j/7KQ2M-9X01T#k={KEY_TEXT}"),
        format!("party://example.org:7800#k={KEY_TEXT}"),
        format!("parties:example.org:7800#k={KEY_TEXT}"),
        format!("parties:/example.org:7800#k={KEY_TEXT}"),
        format!("//example.org:7800#k={KEY_TEXT}"),
        format!("example.org:7800#k={KEY_TEXT}"),
    ] {
        assert_eq!(refused(&text), LinkError::Scheme, "{text}");
    }
}

#[test]
fn links_with_forbidden_characters_or_length_are_refused() {
    let good = format!("parties://example.org:7800/j/7KQ2M-9X01T#k={KEY_TEXT}");
    for text in [
        format!(" {good}"),
        format!("{good} "),
        format!("{good}\n"),
        good.replace("example", "exa mple"),
        good.replace("example", "ex\u{e4}mple"),
        good.replace("example", "exa\u{0}mple"),
        good.replace("example", "exa\tmple"),
        // A right-to-left override, as used to disguise a link.
        good.replace("example", "\u{202e}example"),
    ] {
        assert_eq!(refused(&text), LinkError::Character, "{text:?}");
    }

    let padding = "a".repeat(MAX_LINK_LEN);
    assert_eq!(refused(&format!("{good}&x={padding}")), LinkError::TooLong);
    assert_eq!(refused(&"x".repeat(1_000_000)), LinkError::TooLong);
    // Up to the limit, unknown parameters are fine.
    let filler = "a".repeat(MAX_LINK_LEN - good.len() - 3);
    let longest = format!("{good}&x={filler}");
    assert_eq!(longest.len(), MAX_LINK_LEN);
    assert!(JoinLink::parse(&longest).is_ok());
}

#[test]
fn links_with_user_info_or_query_are_refused() {
    assert_eq!(
        refused(&format!("parties://user@example.org:7800#k={KEY_TEXT}")),
        LinkError::UserInfo
    );
    // The classic disguise: the real host hides behind an at sign.
    assert_eq!(
        refused(&format!(
            "parties://example.org:7800@evil.example:7800#k={KEY_TEXT}"
        )),
        LinkError::UserInfo
    );
    assert_eq!(
        refused(&format!("parties://example.org:7800?k={KEY_TEXT}")),
        LinkError::Query
    );
    assert_eq!(
        refused(&format!(
            "parties://example.org:7800/j/7KQ2M-9X01T?x=1#k={KEY_TEXT}"
        )),
        LinkError::Query
    );
}

#[test]
fn links_with_a_bad_host_are_refused() {
    for host in [
        "",
        ".",
        "example..org",
        ".example.org",
        "example.org.",
        "-example.org",
        "example-.org",
        "exa_mple.org",
        "exa%20mple.org",
        "example.org\\evil",
        // Shorthand and out-of-range IPv4 forms other software reads as addresses.
        "127.1",
        "1.2.3",
        "256.0.0.1",
        "1.2.3.4.5",
        "0x7f.0.0.1",
        "0x7f000001",
        "2130706433",
        "01.2.3.4",
        // IPv6 without brackets, with a zone, or malformed.
        "::1",
        "2001:db8::7",
        "[::1",
        "[fe80::1%eth0]",
        "[::1]x",
        "[1.2.3.4]",
        "[]",
    ] {
        let text = format!("parties://{host}:7800#k={KEY_TEXT}");
        assert_eq!(refused(&text), LinkError::Host, "{host:?}");
    }

    let long_label = format!("{}.org", "a".repeat(64));
    assert_eq!(
        refused(&format!("parties://{long_label}:7800#k={KEY_TEXT}")),
        LinkError::Host
    );
    let long_name = ["abcdefgh"; 29].join(".");
    assert_eq!(long_name.len(), 260);
    assert_eq!(
        refused(&format!("parties://{long_name}:7800#k={KEY_TEXT}")),
        LinkError::Host
    );
    // The longest label and a long name are fine.
    let label = format!("{}.org", "a".repeat(63));
    assert!(JoinLink::parse(&format!("parties://{label}:7800#k={KEY_TEXT}")).is_ok());
}

/// A link must point at one server. The unspecified address reaches the
/// local machine on many systems, and multicast and broadcast addresses
/// reach whoever listens.
#[test]
fn links_to_addresses_that_are_not_one_host_are_refused() {
    for host in [
        "0.0.0.0",
        "255.255.255.255",
        "224.0.0.1",
        "239.255.255.250",
        "[::]",
        "[ff02::1]",
        "[ff0e::101]",
        "[::ffff:0.0.0.0]",
        "[::ffff:224.0.0.1]",
        "[::ffff:255.255.255.255]",
    ] {
        let text = format!("parties://{host}:7800#k={KEY_TEXT}");
        assert_eq!(refused(&text), LinkError::Host, "{host:?}");
        assert_eq!(Host::parse(host), Err(LinkError::Host), "{host:?}");
    }
    for address in [
        IpAddr::V4(Ipv4Addr::UNSPECIFIED),
        IpAddr::V4(Ipv4Addr::BROADCAST),
        IpAddr::V4(Ipv4Addr::new(224, 0, 0, 251)),
        IpAddr::V6(Ipv6Addr::UNSPECIFIED),
        IpAddr::V6(Ipv6Addr::new(0xff02, 0, 0, 0, 0, 0, 0, 0xfb)),
    ] {
        assert_eq!(Host::from_ip(address), Err(LinkError::Host), "{address}");
    }

    // Ordinary addresses, private ones and loopback stay usable.
    for host in [
        "127.0.0.1",
        "10.0.0.5",
        "192.168.1.20",
        "203.0.113.7",
        "0.0.0.1",
        "223.255.255.255",
        "[::1]",
        "[fe80::1]",
        "[fd00::5]",
        "[2001:db8::7]",
        "[::ffff:192.0.2.1]",
    ] {
        let text = format!("parties://{host}:7800#k={KEY_TEXT}");
        assert!(JoinLink::parse(&text).is_ok(), "{host:?}");
    }
}

/// Join codes compare by value, whatever form they were typed in.
#[test]
fn join_codes_compare_by_value() {
    assert_eq!(code("7KQ2M-9X01T"), code("7kq2m9xolt"));
    assert_ne!(code("7KQ2M-9X01T"), code("7KQ2M-9X01V"));
    assert_ne!(code("7KQ2M-9X01T"), code("8KQ2M-9X01T"));
    let copy = code("7KQ2M-9X01T").clone();
    assert_eq!(copy, code("7KQ2M-9X01T"));
}

#[test]
fn links_with_a_bad_port_are_refused() {
    for authority in [
        "example.org",
        "example.org:",
        "example.org:0",
        "example.org:65536",
        "example.org:99999999999999999999",
        "example.org:07800",
        "example.org:+7800",
        "example.org:-1",
        "example.org:78oo",
        "example.org:7800:7800x",
        "[::1]",
        "[::1]:",
        "[2001:db8::7]",
    ] {
        let text = format!("parties://{authority}#k={KEY_TEXT}");
        assert_eq!(refused(&text), LinkError::Port, "{authority:?}");
    }
}

#[test]
fn links_with_another_path_are_refused() {
    for path in [
        "/x",
        "/j",
        "/J/7KQ2M-9X01T",
        "/join/7KQ2M-9X01T",
        "//j/7KQ2M-9X01T",
        "/claim/abcdef",
        "/7KQ2M-9X01T",
    ] {
        let text = format!("parties://example.org:7800{path}#k={KEY_TEXT}");
        assert_eq!(refused(&text), LinkError::Path, "{path:?}");
    }
}

#[test]
fn links_with_a_bad_join_code_are_refused() {
    let link = |code: &str| format!("parties://example.org:7800/j/{code}#k={KEY_TEXT}");
    assert_eq!(
        refused(&link("")),
        LinkError::JoinCode(JoinCodeError::Format)
    );
    assert_eq!(
        refused(&link("7KQ2M")),
        LinkError::JoinCode(JoinCodeError::Format)
    );
    assert_eq!(
        refused(&link("7KQ2M-9X01T/")),
        LinkError::JoinCode(JoinCodeError::Format)
    );
    assert_eq!(
        refused(&link("7KQ2M-9X01T/extra")),
        LinkError::JoinCode(JoinCodeError::Format)
    );
    assert_eq!(
        refused(&link("7KQ2M-9XU1T")),
        LinkError::JoinCode(JoinCodeError::Character { position: 8 })
    );
}

#[test]
fn links_without_exactly_one_valid_key_are_refused() {
    let base = "parties://example.org:7800/j/7KQ2M-9X01T";
    assert_eq!(refused(base), LinkError::MissingKey);
    assert_eq!(refused(&format!("{base}#")), LinkError::MissingKey);
    assert_eq!(refused(&format!("{base}#x=1")), LinkError::MissingKey);
    assert_eq!(
        refused(&format!("{base}#K={KEY_TEXT}")),
        LinkError::MissingKey
    );
    assert_eq!(refused(&format!("{base}#k")), LinkError::MissingKey);
    assert_eq!(
        refused(&format!("{base}#k={KEY_TEXT}&k={KEY_TEXT}")),
        LinkError::DuplicateKey
    );

    let length = |found| {
        LinkError::Key(KeyError::Length(LengthError {
            expected: 43,
            found,
        }))
    };
    assert_eq!(refused(&format!("{base}#k=")), length(0));
    assert_eq!(
        refused(&format!("{base}#k={}", &KEY_TEXT[..42])),
        length(42)
    );
    assert_eq!(refused(&format!("{base}#k={KEY_TEXT}=")), length(44));
    // A second fragment marker ends up in the key and breaks it.
    assert_eq!(refused(&format!("{base}#k={KEY_TEXT}#x")), length(45));
    assert_eq!(
        refused(&format!("{base}#k={}", KEY_TEXT.replace('-', "+"))),
        LinkError::Key(KeyError::Encoding)
    );
    // A key that is valid text but not a usable key: the identity point.
    assert_eq!(
        refused(&format!("{base}#k=AQ{}", "A".repeat(41))),
        LinkError::Key(KeyError::WeakKey)
    );
    // A hex key is not the text form.
    let hex_key = common::to_hex(server().as_bytes());
    assert_eq!(refused(&format!("{base}#k={hex_key}")), length(64));
}

#[test]
fn hosts_parse_on_their_own() {
    assert_eq!(
        "Example.ORG".parse::<Host>().map(|host| host.to_string()),
        Ok("example.org".to_owned())
    );
    assert_eq!(
        Host::parse("[::ffff:192.0.2.1]").map(|host| host.to_string()),
        Ok("[::ffff:192.0.2.1]".to_owned())
    );
    assert_eq!(Host::parse("exa mple"), Err(LinkError::Host));
    assert_eq!(
        format!("{:?}", Host::parse("example.org").expect("name")),
        "Host(example.org)"
    );
}
