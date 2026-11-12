//! The chat panel's data: messages, attachments, reactions, system lines.

use super::{Avatar, ChannelId, MessageId, Picture};

#[derive(Clone, Debug, PartialEq)]
pub struct Chat {
    pub channel: ChannelId,
    pub name: String,
    pub topic: String,
    pub items: Vec<ChatItem>,
}

#[derive(Clone, Debug, PartialEq)]
pub enum ChatItem {
    /// A rule with the day's name.
    DayDivider(String),
    /// Boxed: a message is several times the size of the other rows.
    Message(Box<Message>),
    System(SystemLine),
    /// The rule above the first unread message.
    NewMessages,
}

impl From<Message> for ChatItem {
    fn from(message: Message) -> Self {
        Self::Message(Box::new(message))
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct Message {
    pub id: MessageId,
    pub author: String,
    pub avatar: Avatar,
    pub time: String,
    pub text: String,
    pub reply_to: Option<ReplyReference>,
    pub attachment: Option<Attachment>,
    pub reactions: Vec<Reaction>,
}

/// The message a reply answers, shown as one line above it.
#[derive(Clone, Debug, PartialEq)]
pub struct ReplyReference {
    pub author: String,
    pub avatar: Avatar,
    pub excerpt: String,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Attachment {
    Image {
        name: String,
        size: String,
        picture: Option<Picture>,
    },
    File {
        name: String,
        size: String,
    },
}

#[derive(Clone, Debug, PartialEq)]
pub struct Reaction {
    pub kind: ReactionKind,
    pub count: u32,
    /// The user reacted with it.
    pub mine: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ReactionKind {
    ThumbsUp,
    Fire,
}

/// A line the party wrote itself, such as "Nadia joined with a join code
/// from Sara".
#[derive(Clone, Debug, PartialEq)]
pub struct SystemLine {
    pub parts: Vec<SystemPart>,
    pub time: String,
}

#[derive(Clone, Debug, PartialEq)]
pub enum SystemPart {
    /// A name, drawn stronger.
    Name(String),
    Text(String),
}
