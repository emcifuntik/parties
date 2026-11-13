//! Conversions that wrap a message in the frame that carries it, so that a
//! sender writes `Hello { .. }.into()` instead of spelling out the `oneof`.

use crate::v2::{
    AuthResult, Authenticate, ClientHandshake, Event, Hello, HelloAck, Ready, Response,
    ServerFrame, ServerHandshake, client_handshake, server_frame, server_handshake,
};

impl From<Hello> for ClientHandshake {
    fn from(hello: Hello) -> Self {
        Self {
            kind: Some(client_handshake::Kind::Hello(hello)),
        }
    }
}

impl From<Authenticate> for ClientHandshake {
    fn from(authenticate: Authenticate) -> Self {
        Self {
            kind: Some(client_handshake::Kind::Authenticate(authenticate)),
        }
    }
}

impl From<HelloAck> for ServerHandshake {
    fn from(hello_ack: HelloAck) -> Self {
        Self {
            kind: Some(server_handshake::Kind::HelloAck(hello_ack)),
        }
    }
}

impl From<AuthResult> for ServerHandshake {
    fn from(auth_result: AuthResult) -> Self {
        Self {
            kind: Some(server_handshake::Kind::AuthResult(auth_result)),
        }
    }
}

impl From<Ready> for ServerHandshake {
    fn from(ready: Ready) -> Self {
        Self {
            kind: Some(server_handshake::Kind::Ready(ready)),
        }
    }
}

impl From<Response> for ServerFrame {
    fn from(response: Response) -> Self {
        Self {
            kind: Some(server_frame::Kind::Response(response)),
        }
    }
}

impl From<Event> for ServerFrame {
    fn from(event: Event) -> Self {
        Self {
            kind: Some(server_frame::Kind::Event(event)),
        }
    }
}
