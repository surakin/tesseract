use std::sync::{Arc, Mutex};

use anyhow::Context as _;
use cxx::UniquePtr;
use matrix_sdk::{
    config::SyncSettings,
    ruma::events::room::message::{MessageType, RoomMessageEventContent},
    Client,
};
use tokio::runtime::Runtime;
use tokio::sync::watch;

use crate::ffi::{EventHandlerBridge, OpResult, RoomInfo, TimelineEvent};

// ---------------------------------------------------------------------------

fn ok(msg: impl Into<String>) -> OpResult {
    OpResult { ok: true, message: msg.into() }
}

fn err(msg: impl Into<String>) -> OpResult {
    OpResult { ok: false, message: msg.into() }
}

// ---------------------------------------------------------------------------

pub struct MatrixClientFfi {
    rt:          Runtime,
    client:      Option<Client>,
    stop_tx:     Option<watch::Sender<bool>>,
}

impl MatrixClientFfi {
    pub fn new() -> Self {
        tracing_subscriber::fmt()
            .with_env_filter(
                tracing_subscriber::EnvFilter::from_default_env()
                    .add_directive("matrix_sdk=info".parse().unwrap()),
            )
            .init();

        Self {
            rt:      Runtime::new().expect("tokio runtime"),
            client:  None,
            stop_tx: None,
        }
    }

    pub fn login_password(
        &mut self,
        homeserver: &str,
        username:   &str,
        password:   &str,
    ) -> OpResult {
        let hs = homeserver.to_owned();
        let un = username.to_owned();
        let pw = password.to_owned();

        let result = self.rt.block_on(async move {
            let client = Client::builder()
                .homeserver_url(&hs)
                .build()
                .await
                .context("build client")?;

            client
                .matrix_auth()
                .login_username(&un, &pw)
                .send()
                .await
                .context("login")?;

            anyhow::Ok(client)
        });

        match result {
            Ok(c)  => { self.client = Some(c); ok("") }
            Err(e) => err(e.to_string()),
        }
    }

    pub fn restore_session(&mut self, session_json: &str) -> OpResult {
        // TODO: deserialise a previously exported MatrixSession and restore it.
        // Placeholder until session persistence format is finalised.
        let _ = session_json;
        err("restore_session: not yet implemented")
    }

    pub fn export_session(&self) -> String {
        // TODO: serialise client.session() to JSON.
        String::new()
    }

    pub fn start_sync(
        &mut self,
        handler: UniquePtr<EventHandlerBridge>,
    ) {
        let Some(client) = self.client.clone() else { return };

        let (stop_tx, mut stop_rx) = watch::channel(false);
        self.stop_tx = Some(stop_tx);

        // The handler is not Send, so we wrap it in a Mutex and only call it
        // from the single sync task.
        let handler = Arc::new(Mutex::new(handler));

        self.rt.spawn(async move {
            let settings = SyncSettings::default();

            // Attach a room-message event handler.
            {
                let h = Arc::clone(&handler);
                client.add_event_handler(
                    move |ev: matrix_sdk::ruma::events::SyncMessageLikeEvent<
                        RoomMessageEventContent,
                    >,
                          room: matrix_sdk::Room| {
                        let h = Arc::clone(&h);
                        async move {
                            if let matrix_sdk::ruma::events::SyncMessageLikeEvent::Original(e) = ev
                            {
                                if let MessageType::Text(t) = e.content.msgtype {
                                    let event = TimelineEvent {
                                        event_id:  e.event_id.to_string(),
                                        room_id:   room.room_id().to_string(),
                                        sender:    e.sender.to_string(),
                                        body:      t.body,
                                        timestamp: e
                                            .origin_server_ts
                                            .as_secs()
                                            .into(),
                                        msg_type:  "m.text".to_owned(),
                                    };
                                    if let Ok(guard) = h.lock() {
                                        guard.on_message_event(&event);
                                    }
                                }
                            }
                        }
                    },
                );
            }

            loop {
                tokio::select! {
                    _ = stop_rx.changed() => {
                        if *stop_rx.borrow() { break; }
                    }
                    result = client.sync_once(settings.clone()) => {
                        match result {
                            Ok(resp) => {
                                if let Ok(guard) = handler.lock() {
                                    guard.on_sync_token(resp.next_batch.as_str());
                                }
                            }
                            Err(e) => {
                                if let Ok(guard) = handler.lock() {
                                    guard.on_error("sync", &e.to_string());
                                }
                                tokio::time::sleep(std::time::Duration::from_secs(5)).await;
                            }
                        }
                    }
                }
            }
        });
    }

    pub fn stop_sync(&mut self) {
        if let Some(tx) = self.stop_tx.take() {
            let _ = tx.send(true);
        }
    }

    pub fn list_rooms(&self) -> Vec<RoomInfo> {
        let Some(client) = &self.client else { return Vec::new() };

        client
            .joined_rooms()
            .into_iter()
            .map(|r| RoomInfo {
                id:           r.room_id().to_string(),
                name:         r.name().unwrap_or_default(),
                topic:        r.topic().unwrap_or_default(),
                unread_count: r.unread_notification_counts().notification_count,
                is_direct:    r.is_direct().unwrap_or(false),
            })
            .collect()
    }

    pub fn room_messages(&self, room_id: &str, limit: u64) -> Vec<TimelineEvent> {
        use matrix_sdk::ruma::{RoomId, api::client::message::get_message_events};

        let Some(client) = &self.client else { return Vec::new() };

        let room_id = match RoomId::parse(room_id) {
            Ok(id) => id,
            Err(_) => return Vec::new(),
        };

        let client  = client.clone();
        let room_id = room_id.to_owned();

        self.rt.block_on(async move {
            let room = client.get_room(&room_id)?;
            let request = get_message_events::v3::Request::backward(room_id.clone())
                .limit(matrix_sdk::ruma::UInt::new(limit).unwrap_or_default());

            let resp = room.messages(request).await.ok()?;

            let events = resp
                .chunk
                .into_iter()
                .filter_map(|ev| {
                    use matrix_sdk::ruma::events::AnyTimelineEvent;
                    let ev = ev.deserialize().ok()?;
                    if let AnyTimelineEvent::MessageLike(
                        matrix_sdk::ruma::events::AnyMessageLikeEvent::RoomMessage(
                            matrix_sdk::ruma::events::MessageLikeEvent::Original(e),
                        ),
                    ) = ev
                    {
                        let (body, msg_type) = match e.content.msgtype {
                            MessageType::Text(t)  => (t.body, "m.text".to_owned()),
                            MessageType::Image(i) => (i.body, "m.image".to_owned()),
                            MessageType::File(f)  => (f.body, "m.file".to_owned()),
                            other                 => (other.body().to_owned(), "m.other".to_owned()),
                        };
                        Some(TimelineEvent {
                            event_id:  e.event_id.to_string(),
                            room_id:   room_id.to_string(),
                            sender:    e.sender.to_string(),
                            body,
                            timestamp: e.origin_server_ts.as_secs().into(),
                            msg_type,
                        })
                    } else {
                        None
                    }
                })
                .collect();

            Some(events)
        })
        .unwrap_or_default()
    }

    pub fn send_message(&self, room_id: &str, body: &str) -> OpResult {
        let Some(client) = &self.client else {
            return err("not logged in");
        };

        use matrix_sdk::ruma::RoomId;
        let room_id = match RoomId::parse(room_id) {
            Ok(id) => id,
            Err(e) => return err(e.to_string()),
        };

        let client  = client.clone();
        let body    = body.to_owned();
        let room_id = room_id.to_owned();

        let result = self.rt.block_on(async move {
            let room = client.get_room(&room_id).context("room not found")?;
            room.send(RoomMessageEventContent::text_plain(body))
                .await
                .context("send")?;
            anyhow::Ok(())
        });

        match result {
            Ok(_)  => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    pub fn logout(&mut self) -> OpResult {
        let Some(client) = self.client.take() else {
            return ok("");
        };

        self.stop_sync();

        let result = self.rt.block_on(async move {
            client.matrix_auth().logout().await.context("logout")
        });

        match result {
            Ok(_)  => ok(""),
            Err(e) => err(e.to_string()),
        }
    }
}
