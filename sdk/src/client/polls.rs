//! MSC3381 poll sending: start, respond, end. Wire format is the unstable
//! `org.matrix.msc3381.poll.*` set.

use crate::ffi::OpResult;
#[cfg(test)]
use super::err;
use super::ClientFfi;
#[cfg(not(test))]
use super::{err, ok, parse_room_id, try_op};

use matrix_sdk::ruma::events::poll::{
    start::PollKind,
    unstable_start::{
        NewUnstablePollStartEventContent, UnstablePollAnswer, UnstablePollAnswers,
        UnstablePollStartContentBlock,
    },
};

const MAX_POLL_ANSWERS: usize = 20;

pub(crate) fn build_poll_start(
    question: &str,
    answers: &[String],
    max_selections: u32,
    disclosed: bool,
) -> Result<NewUnstablePollStartEventContent, String> {
    let question = question.trim();
    if question.is_empty() {
        return Err("poll question is empty".to_owned());
    }
    let texts: Vec<&str> = answers.iter().map(|a| a.trim()).collect();
    if texts.iter().any(|t| t.is_empty()) {
        return Err("poll option is empty".to_owned());
    }
    if texts.len() < 2 || texts.len() > MAX_POLL_ANSWERS {
        return Err(format!("a poll needs 2 to {MAX_POLL_ANSWERS} options"));
    }
    // Opaque, unique per poll.
    let list: Vec<UnstablePollAnswer> = texts
        .iter()
        .map(|t| UnstablePollAnswer::new(format!("{:016x}", rand::random::<u64>()), *t))
        .collect();
    let answers = UnstablePollAnswers::try_from(list).map_err(|e| e.to_string())?;
    let mut block = UnstablePollStartContentBlock::new(question, answers);
    block.kind = if disclosed { PollKind::Disclosed } else { PollKind::Undisclosed };
    let max = max_selections.clamp(1, texts.len() as u32);
    block.max_selections = u8::try_from(max).unwrap_or(1).into();

    let mut fallback = question.to_owned();
    for (i, t) in texts.iter().enumerate() {
        fallback.push_str(&format!("\n{}. {t}", i + 1));
    }
    Ok(NewUnstablePollStartEventContent::plain_text(fallback, block))
}

impl ClientFfi {
    /// Start a poll in `room_id`.
    #[cfg(not(test))]
    pub fn send_poll(
        &self,
        room_id: &str,
        question: &str,
        answers: &Vec<String>,
        max_selections: u32,
        disclosed: bool,
    ) -> OpResult {
        use matrix_sdk::ruma::events::{
            poll::unstable_start::UnstablePollStartEventContent, AnyMessageLikeEventContent,
        };
        let content = match build_poll_start(question, answers, max_selections, disclosed) {
            Ok(c) => AnyMessageLikeEventContent::UnstablePollStart(
                UnstablePollStartEventContent::New(c),
            ),
            Err(e) => return err(e),
        };
        self.dispatch_room_content_(room_id, content, "send/poll")
    }

    #[cfg(test)]
    pub fn send_poll(
        &self,
        _room_id: &str,
        _question: &str,
        _answers: &Vec<String>,
        _max_selections: u32,
        _disclosed: bool,
    ) -> OpResult {
        err("not logged in")
    }

    /// Cast or replace the current user's vote.
    #[cfg(not(test))]
    pub fn send_poll_response(
        &self,
        room_id: &str,
        poll_event_id: &str,
        answer_ids: &Vec<String>,
    ) -> OpResult {
        use matrix_sdk::ruma::events::{
            poll::unstable_response::UnstablePollResponseEventContent, AnyMessageLikeEventContent,
        };
        self.send_poll_event_(room_id, poll_event_id, |id| {
            AnyMessageLikeEventContent::UnstablePollResponse(
                UnstablePollResponseEventContent::new(answer_ids.clone(), id),
            )
        })
    }

    #[cfg(test)]
    pub fn send_poll_response(
        &self,
        _room_id: &str,
        _poll_event_id: &str,
        _answer_ids: &Vec<String>,
    ) -> OpResult {
        err("not logged in")
    }

    /// Close a poll.
    #[cfg(not(test))]
    pub fn end_poll(&self, room_id: &str, poll_event_id: &str) -> OpResult {
        use matrix_sdk::ruma::events::{
            poll::unstable_end::UnstablePollEndEventContent, AnyMessageLikeEventContent,
        };
        self.send_poll_event_(room_id, poll_event_id, |id| {
            AnyMessageLikeEventContent::UnstablePollEnd(UnstablePollEndEventContent::new(
                "The poll has ended.",
                id,
            ))
        })
    }

    #[cfg(test)]
    pub fn end_poll(&self, _room_id: &str, _poll_event_id: &str) -> OpResult {
        err("not logged in")
    }

    /// Shared routing for respond/end: send `make(poll_event_id)` through
    /// whichever subscribed timeline (room or thread) holds the poll.
    #[cfg(not(test))]
    fn send_poll_event_(
        &self,
        room_id: &str,
        poll_event_id: &str,
        make: impl FnOnce(
            matrix_sdk::ruma::OwnedEventId,
        ) -> matrix_sdk::ruma::events::AnyMessageLikeEventContent,
    ) -> OpResult {
        if self.client.is_none() {
            return err("not logged in");
        }
        let room_id = try_op!(parse_room_id(room_id));
        let event_id: matrix_sdk::ruma::OwnedEventId = match poll_event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
        };
        let tl = match self
            .rt
            .block_on(async { self.timeline_for_event(&room_id, &event_id).await })
        {
            Some(tl) => tl,
            None => return err("poll not found in any subscribed timeline"),
        };
        let content = make(event_id);
        match self.block_on_cancellable(async move { tl.send(content).await }) {
            Some(Ok(_)) => ok(""),
            Some(Err(e)) => err(e.to_string()),
            None => err("cancelled"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn s(v: &[&str]) -> Vec<String> {
        v.iter().map(|x| x.to_string()).collect()
    }

    #[test]
    fn builds_a_valid_poll() {
        let c = build_poll_start("Lunch?", &s(&["Pizza", "Sushi"]), 1, true).unwrap();
        assert_eq!(c.poll_start.question.text, "Lunch?");
        assert_eq!(c.poll_start.answers.len(), 2);
        assert_eq!(c.poll_start.max_selections, 1u8.into());
        // plain-text fallback for clients without poll support
        let fb = c.text.clone().unwrap();
        assert!(fb.starts_with("Lunch?"));
        assert!(fb.contains("1. Pizza") && fb.contains("2. Sushi"));
    }

    #[test]
    fn answer_ids_are_unique_and_non_empty() {
        let c = build_poll_start("q", &s(&["a", "a", "b"]), 1, false).unwrap();
        let ids: Vec<&str> = c.poll_start.answers.iter().map(|a| a.id.as_str()).collect();
        assert!(ids.iter().all(|i| !i.is_empty()));
        let mut u = ids.clone();
        u.sort();
        u.dedup();
        assert_eq!(u.len(), 3);
    }

    #[test]
    fn rejects_bad_input() {
        assert!(build_poll_start("", &s(&["a", "b"]), 1, true).is_err());
        assert!(build_poll_start("q", &s(&["a"]), 1, true).is_err());
        let many: Vec<String> = (0..21).map(|i| format!("o{i}")).collect();
        assert!(build_poll_start("q", &many, 1, true).is_err());
        assert!(build_poll_start("q", &s(&["a", " "]), 1, true).is_err());
    }

    #[test]
    fn max_selections_is_clamped_to_answer_count() {
        let c = build_poll_start("q", &s(&["a", "b", "c"]), 9, true).unwrap();
        assert_eq!(c.poll_start.max_selections, 3u8.into());
        let c = build_poll_start("q", &s(&["a", "b"]), 0, true).unwrap();
        assert_eq!(c.poll_start.max_selections, 1u8.into());
    }

    #[test]
    fn disclosed_flag_maps_to_kind() {
        use matrix_sdk::ruma::events::poll::start::PollKind;
        let d = build_poll_start("q", &s(&["a", "b"]), 1, true).unwrap();
        let u = build_poll_start("q", &s(&["a", "b"]), 1, false).unwrap();
        assert_eq!(d.poll_start.kind, PollKind::Disclosed);
        assert_eq!(u.poll_start.kind, PollKind::Undisclosed);
    }
}
