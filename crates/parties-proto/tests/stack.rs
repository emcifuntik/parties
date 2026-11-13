//! Decoding a message from the network needs little stack, whatever the
//! bytes say.
//!
//! A panic can be caught; a stack overflow ends the process. The schema has
//! no message that contains itself, so known fields nest only a few levels
//! deep, and `prost` stops at 100 levels of nested unknown groups. This test
//! holds the decoders to that on a thread whose stack is a fraction of the
//! one a connection task runs on.

mod common;

use common::fixtures;
use parties_proto::frame::FrameCodec;
use parties_proto::prost::Message;
use parties_proto::v2::{ClientFrame, ClientHandshake, ServerFrame, ServerHandshake};

/// The stack of the thread the decoders run on. A debug build needs less
/// than half of it; a Tokio worker has 2 MiB.
const STACK_BYTES: usize = 256 * 1024;

fn decode_as_every_frame_type(bytes: &[u8]) -> usize {
    usize::from(ClientHandshake::decode(bytes).is_ok())
        + usize::from(ServerHandshake::decode(bytes).is_ok())
        + usize::from(ClientFrame::decode(bytes).is_ok())
        + usize::from(ServerFrame::decode(bytes).is_ok())
}

fn on_a_small_stack<T: Send + 'static>(run: impl FnOnce() -> T + Send + 'static) -> T {
    std::thread::Builder::new()
        .stack_size(STACK_BYTES)
        .spawn(run)
        .expect("the thread starts")
        .join()
        .expect("the decoders do not panic")
}

/// A small deterministic generator (xorshift64), so that the test needs no
/// seed and fails the same way every time.
struct Random(u64);

impl Random {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }

    /// A number below `bound`.
    fn below(&mut self, bound: usize) -> usize {
        let bound = u64::try_from(bound).expect("a length");
        usize::try_from(self.next() % bound).expect("below a length")
    }
}

#[test]
fn nested_unknown_groups_stop_at_the_recursion_limit() {
    // Field 1000 with wire type 3, "start group": the key is c3 3e. A group
    // inside a group inside a group, never closed.
    let nested = |depth: usize| [0xC3, 0x3E].repeat(depth);
    let accepted = on_a_small_stack(move || {
        [1, 50, 99, 100, 101, 1000, FrameCodec::CONTROL.max_len() / 2]
            .map(|depth| decode_as_every_frame_type(&nested(depth)))
    });
    // None of them is a message: the groups are not closed. What matters is
    // that the decoder came back.
    assert_eq!(accepted, [0; 7]);

    // Closed groups are skipped as unknown fields, up to the limit.
    let closed = |depth: usize| {
        let mut bytes = [0xC3, 0x3E].repeat(depth);
        // Wire type 4, "end group", of the same field: c4 3e.
        bytes.extend([0xC4, 0x3E].repeat(depth));
        bytes
    };
    let accepted = on_a_small_stack(move || {
        [1, 50, 100, 101, 1000].map(|depth| decode_as_every_frame_type(&closed(depth)) == 4)
    });
    assert_eq!(accepted[..2], [true, true], "shallow groups are skipped");
    assert_eq!(accepted[3..], [false, false], "past the limit is an error");
}

#[test]
fn damaged_snapshots_decode_or_fail_on_a_small_stack() {
    let (decoded, tried) = on_a_small_stack(|| {
        let valid = ServerHandshake::from(fixtures::ready()).encode_to_vec();
        let mut random = Random(0x9E37_79B9_7F4A_7C15);
        let mut decoded = 0;
        let tried = 20_000;
        for _ in 0..tried {
            let mut bytes = valid.clone();
            for _ in 0..=random.below(6) {
                let position = random.below(bytes.len());
                bytes[position] = random.next().to_le_bytes()[0];
            }
            bytes.truncate(random.below(bytes.len() + 1));
            decoded += usize::from(decode_as_every_frame_type(&bytes) > 0);
        }
        (decoded, tried)
    });
    // Both outcomes occur: the damage is neither always fatal nor harmless.
    assert!(decoded > 0, "no damaged snapshot decoded");
    assert!(decoded < tried, "every damaged snapshot decoded");
}
