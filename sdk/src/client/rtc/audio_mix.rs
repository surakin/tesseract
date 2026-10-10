//! Mixes every remote audio track of a call into one 48 kHz mono stream.
//!
//! The UI plays a single stream per call, so tracks (a participant's mic and
//! their screen-share audio, or several participants) must be summed here:
//! pushed straight to the sink they interleave sample-by-sample and sound
//! distorted. Each track feeds a small queue; a 10 ms ticker pulls one frame
//! from every queue through libwebrtc's `AudioMixer` and emits the result.

use std::{
    borrow::Cow,
    collections::VecDeque,
    sync::{
        atomic::{AtomicI32, Ordering},
        Arc,
    },
    time::Duration,
};

use livekit::webrtc::{
    audio_frame::AudioFrame,
    native::audio_mixer::{AudioMixer, AudioMixerSource},
};
use parking_lot::Mutex;
use tokio::task::AbortHandle;

use super::RtcEventSink;

const SAMPLE_RATE: u32 = 48_000;
/// Samples per 10 ms frame at 48 kHz mono — the unit libwebrtc's mixer pulls.
const FRAME_SAMPLES: usize = 480;
/// ~100 ms: a stalled consumer drops the oldest audio rather than growing.
const MAX_QUEUED: usize = FRAME_SAMPLES * 10;
/// ~30 ms buffered before a track starts being consumed, to absorb jitter.
const PREBUFFER: usize = FRAME_SAMPLES * 3;

#[derive(Default)]
struct TrackQueue {
    samples: VecDeque<i16>,
    /// False until `PREBUFFER` samples have accumulated (and again after an underrun).
    primed: bool,
}

impl TrackQueue {
    fn push(&mut self, samples: &[i16]) {
        self.samples.extend(samples.iter().copied());
        let excess = self.samples.len().saturating_sub(MAX_QUEUED);
        if excess > 0 {
            self.samples.drain(..excess);
        }
    }

    /// One 10 ms frame, or `None` while buffering / after an underrun.
    fn pop_frame(&mut self) -> Option<Vec<i16>> {
        if !self.primed {
            if self.samples.len() < PREBUFFER {
                return None;
            }
            self.primed = true;
        }
        if self.samples.len() < FRAME_SAMPLES {
            self.primed = false;
            return None;
        }
        Some(self.samples.drain(..FRAME_SAMPLES).collect())
    }
}

struct QueueSource {
    ssrc: i32,
    queue: Arc<Mutex<TrackQueue>>,
}

impl AudioMixerSource for QueueSource {
    fn ssrc(&self) -> i32 {
        self.ssrc
    }

    fn preferred_sample_rate(&self) -> u32 {
        SAMPLE_RATE
    }

    fn get_audio_frame_with_info(&self, target_sample_rate: u32) -> Option<AudioFrame<'_>> {
        // The tracks are requested at 48 kHz mono; the mixer asserts on any
        // other rate, so refuse rather than panic.
        if target_sample_rate != SAMPLE_RATE {
            return None;
        }
        let data = self.queue.lock().pop_frame()?;
        Some(AudioFrame {
            data: Cow::Owned(data),
            sample_rate: SAMPLE_RATE,
            num_channels: 1,
            samples_per_channel: FRAME_SAMPLES as u32,
        })
    }
}

/// One remote track's input to the mixer. Dropping it removes the track.
pub struct TrackHandle {
    ssrc: i32,
    queue: Arc<Mutex<TrackQueue>>,
    mixer: Arc<Mutex<AudioMixer>>,
    participant_id: String,
    sink: Arc<dyn RtcEventSink>,
}

impl TrackHandle {
    /// Queue decoded 48 kHz mono samples. Dropped when the sink rejects this
    /// participant (not a real publisher on this SFU).
    pub fn push(&self, samples: &[i16]) {
        if self.sink.accepts_participant(&self.participant_id) {
            self.queue.lock().push(samples);
        }
    }
}

impl Drop for TrackHandle {
    fn drop(&mut self) {
        self.mixer.lock().remove_source(self.ssrc);
    }
}

pub struct RemoteAudioMixer {
    mixer: Arc<Mutex<AudioMixer>>,
    sink: Arc<dyn RtcEventSink>,
    next_ssrc: AtomicI32,
    ticker: AbortHandle,
}

impl RemoteAudioMixer {
    /// Start the 10 ms mixing loop. Mixed frames go to
    /// `sink.on_audio_frame(session_id, out_identity, ..)`; `out_identity` must be
    /// an id the sink lets through (our own local identity).
    pub fn spawn(sink: Arc<dyn RtcEventSink>, session_id: u64, out_identity: String) -> Self {
        let mixer = Arc::new(Mutex::new(AudioMixer::new()));
        let ticker = {
            let mixer = Arc::clone(&mixer);
            let sink = Arc::clone(&sink);
            tokio::spawn(async move {
                let mut tick = tokio::time::interval(Duration::from_millis(10));
                tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
                loop {
                    tick.tick().await;
                    let mixed: Vec<i16> = {
                        let mut m = mixer.lock();
                        m.mix(1).to_vec()
                    };
                    // No audible source this tick: the playback ring underruns to silence.
                    if mixed.iter().all(|&s| s == 0) {
                        continue;
                    }
                    sink.on_audio_frame(session_id, &out_identity, &mixed, SAMPLE_RATE, 1);
                }
            })
            .abort_handle()
        };
        Self {
            mixer,
            sink,
            next_ssrc: AtomicI32::new(1),
            ticker,
        }
    }

    pub fn add_track(&self, participant_id: String) -> TrackHandle {
        let ssrc = self.next_ssrc.fetch_add(1, Ordering::Relaxed);
        let queue = Arc::new(Mutex::new(TrackQueue::default()));
        self.mixer.lock().add_source(QueueSource {
            ssrc,
            queue: Arc::clone(&queue),
        });
        TrackHandle {
            ssrc,
            queue,
            mixer: Arc::clone(&self.mixer),
            participant_id,
            sink: Arc::clone(&self.sink),
        }
    }
}

impl Drop for RemoteAudioMixer {
    fn drop(&mut self) {
        self.ticker.abort();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame(v: i16) -> Vec<i16> {
        vec![v; FRAME_SAMPLES]
    }

    #[test]
    fn queue_prebuffers_before_first_frame() {
        let mut q = TrackQueue::default();
        q.push(&frame(1));
        q.push(&frame(1));
        assert!(q.pop_frame().is_none(), "below the prebuffer threshold");
        q.push(&frame(1));
        assert_eq!(q.pop_frame().unwrap().len(), FRAME_SAMPLES);
        assert!(q.pop_frame().is_some());
    }

    #[test]
    fn queue_reprimes_after_underrun() {
        let mut q = TrackQueue::default();
        for _ in 0..3 {
            q.push(&frame(1));
        }
        assert!(q.pop_frame().is_some());
        assert!(q.pop_frame().is_some());
        assert!(q.pop_frame().is_some());
        assert!(q.pop_frame().is_none(), "underrun");
        q.push(&frame(1));
        assert!(q.pop_frame().is_none(), "must re-buffer after an underrun");
    }

    #[test]
    fn queue_drops_oldest_on_overflow() {
        let mut q = TrackQueue::default();
        q.push(&vec![1; MAX_QUEUED]);
        q.push(&frame(2));
        assert_eq!(q.samples.len(), MAX_QUEUED);
        assert_eq!(*q.samples.back().unwrap(), 2);
        assert_eq!(*q.samples.front().unwrap(), 1);
    }

    #[test]
    fn mixer_sums_two_sources_and_stops_after_removal() {
        let mut mixer = AudioMixer::new();
        let qa = Arc::new(Mutex::new(TrackQueue::default()));
        let qb = Arc::new(Mutex::new(TrackQueue::default()));
        for (ssrc, q) in [(1, &qa), (2, &qb)] {
            mixer.add_source(QueueSource {
                ssrc,
                queue: Arc::clone(q),
            });
        }
        for _ in 0..4 {
            qa.lock().push(&frame(1000));
            qb.lock().push(&frame(2000));
        }
        let mixed = mixer.mix(1).to_vec();
        assert_eq!(mixed.len(), FRAME_SAMPLES);
        assert!(
            mixed.iter().all(|&s| (2900..=3100).contains(&s)),
            "expected ~3000, got {}",
            mixed[0]
        );

        mixer.remove_source(2);
        let mixed = mixer.mix(1).to_vec();
        assert!(
            mixed.iter().all(|&s| (900..=1100).contains(&s)),
            "expected ~1000 after removal, got {}",
            mixed[0]
        );
    }
}
