//! DAVE, Discord's end-to-end encryption for voice, around the `davey` crate. The state machine
//! follows @discordjs/voice 0.19.2 (src/networking/DAVESession.ts) step by step, because that is
//! what Discord's voice servers are known to accept from a bot.
//!
//! Binary voice-gateway messages: server → us `[seq u16][op u8][payload]`, us → server
//! `[op u8][payload]`. The opcodes:
//!   25 external sender (bin)   26 key package (bin, ours)   27 proposals (bin)
//!   28 commit + welcome (bin, ours)   29 announce commit (bin)   30 welcome (bin)
//!   21 prepare transition   22 execute transition   23 transition ready (ours)
//!   24 prepare epoch   31 invalid commit/welcome (ours)

use std::collections::{HashMap, HashSet};
use std::num::NonZeroU16;

use davey::{DaveSession, ProposalsOperationType};
use log::{debug, info, warn};

pub const OP_EXTERNAL_SENDER: u8 = 25;
pub const OP_KEY_PACKAGE: u8 = 26;
pub const OP_PROPOSALS: u8 = 27;
pub const OP_COMMIT_WELCOME: u8 = 28;
pub const OP_ANNOUNCE_COMMIT: u8 = 29;
pub const OP_WELCOME: u8 = 30;

const SILENCE: [u8; 3] = [0xF8, 0xFF, 0xFE];

/// What the voice connection has to send after a DAVE step.
#[derive(Default)]
pub struct Reply {
    /// binary messages: (opcode, payload)
    pub binary: Vec<(u8, Vec<u8>)>,
    /// op 23 transition ready, for this transition id
    pub ready_for: Option<u16>,
    /// op 31 invalid commit/welcome, for this transition id
    pub invalid: Option<u16>,
}

pub struct Dave {
    protocol_version: u16,
    user_id: u64,
    channel_id: u64,
    session: Option<DaveSession>,
    pending: HashMap<u16, u16>,
    downgraded: bool,
    reinitializing: bool,
    pub last_transition: Option<u16>,
}

impl Dave {
    /// `protocol_version` from the Session Description (op 4). Returns the session and what to send
    /// (the key package, op 26, when DAVE is on).
    pub fn new(protocol_version: u16, user_id: u64, channel_id: u64) -> (Self, Reply) {
        let mut d = Dave {
            protocol_version,
            user_id,
            channel_id,
            session: None,
            pending: HashMap::new(),
            downgraded: false,
            reinitializing: false,
            last_transition: None,
        };
        let r = d.reinit();
        (d, r)
    }

    pub fn protocol_version(&self) -> u16 {
        self.protocol_version
    }

    pub fn ready(&self) -> bool {
        self.session.as_ref().is_some_and(|s| s.is_ready())
    }

    pub fn privacy_code(&self) -> Option<String> {
        self.session.as_ref().and_then(|s| s.voice_privacy_code().map(str::to_string))
    }

    fn reinit(&mut self) -> Reply {
        let mut reply = Reply::default();
        if let Some(version) = NonZeroU16::new(self.protocol_version) {
            let result = match self.session.as_mut() {
                Some(s) => s.reinit(version, self.user_id, self.channel_id, None).map_err(|e| format!("{e:?}")),
                None => DaveSession::new(version, self.user_id, self.channel_id, None)
                    .map(|s| self.session = Some(s))
                    .map_err(|e| format!("{e:?}")),
            };
            if let Err(e) = result {
                warn!("dave: (re)init failed: {e}");
                return reply;
            }
            info!("dave: session for protocol v{}", self.protocol_version);
            match self.session.as_mut().map(|s| s.create_key_package()) {
                Some(Ok(kp)) => reply.binary.push((OP_KEY_PACKAGE, kp)),
                Some(Err(e)) => warn!("dave: key package failed: {e:?}"),
                None => {}
            }
        } else if let Some(s) = self.session.as_mut() {
            let _ = s.reset();
            s.set_passthrough_mode(true, Some(10));
            info!("dave: session reset (protocol 0)");
        }
        reply
    }

    pub fn set_external_sender(&mut self, payload: &[u8]) {
        match self.session.as_mut().map(|s| s.set_external_sender(payload)) {
            Some(Ok(())) => debug!("dave: external sender set"),
            Some(Err(e)) => warn!("dave: external sender: {e:?}"),
            None => warn!("dave: external sender without a session"),
        }
    }

    /// op 21. Returns whether to answer with op 23 (transition ready).
    pub fn prepare_transition(&mut self, transition_id: u16, protocol_version: u16) -> bool {
        info!("dave: prepare transition {transition_id} to v{protocol_version}");
        self.pending.insert(transition_id, protocol_version);
        if transition_id == 0 {
            self.execute_transition(0);
            false
        } else {
            if protocol_version == 0 {
                if let Some(s) = self.session.as_mut() {
                    s.set_passthrough_mode(true, Some(24));
                }
            }
            true
        }
    }

    /// op 22.
    pub fn execute_transition(&mut self, transition_id: u16) -> bool {
        let Some(version) = self.pending.remove(&transition_id) else {
            debug!("dave: execute transition {transition_id} that we never prepared");
            return false;
        };
        let old = self.protocol_version;
        self.protocol_version = version;
        if old != version && version == 0 {
            self.downgraded = true;
            info!("dave: downgraded to protocol 0");
        } else if transition_id > 0 && self.downgraded {
            self.downgraded = false;
            if let Some(s) = self.session.as_mut() {
                s.set_passthrough_mode(true, Some(10));
            }
            info!("dave: upgraded");
        }
        self.reinitializing = false;
        self.last_transition = Some(transition_id);
        info!(
            "dave: transition {transition_id} executed (v{old} -> v{version}), privacy code {:?}",
            self.privacy_code()
        );
        true
    }

    /// op 24. Epoch 1 means a new group: start over with a new key package.
    pub fn prepare_epoch(&mut self, epoch: u64, protocol_version: u16) -> Reply {
        info!("dave: prepare epoch {epoch} (v{protocol_version})");
        if epoch == 1 {
            self.protocol_version = protocol_version;
            return self.reinit();
        }
        Reply::default()
    }

    fn recover(&mut self, transition_id: u16) -> Reply {
        if self.reinitializing {
            return Reply::default();
        }
        warn!("dave: invalidating transition {transition_id}, re-initializing");
        self.reinitializing = true;
        let mut reply = self.reinit();
        reply.invalid = Some(transition_id);
        reply
    }

    /// op 27: `[operation type u8][proposals]`. May answer with op 28 (our commit, plus a welcome
    /// when it adds members).
    pub fn process_proposals(&mut self, payload: &[u8], connected: &HashSet<u64>) -> Reply {
        let mut reply = Reply::default();
        let (Some(s), Some((&op, rest))) = (self.session.as_mut(), payload.split_first()) else {
            return reply;
        };
        let op = if op == 1 { ProposalsOperationType::REVOKE } else { ProposalsOperationType::APPEND };
        let mut expected: Vec<u64> = connected.iter().copied().collect();
        expected.push(self.user_id);
        match s.process_proposals(op, rest, Some(&expected)) {
            Ok(Some(cw)) => {
                let mut out = cw.commit;
                if let Some(w) = cw.welcome {
                    out.extend_from_slice(&w);
                }
                info!("dave: proposals processed, sending our commit ({} bytes)", out.len());
                reply.binary.push((OP_COMMIT_WELCOME, out));
            }
            Ok(None) => debug!("dave: proposals processed, nothing to commit"),
            Err(e) => warn!("dave: proposals: {e:?}"),
        }
        reply
    }

    /// op 29: `[transition id u16][commit]`.
    pub fn process_commit(&mut self, payload: &[u8]) -> Reply {
        self.process_transition(payload, true)
    }

    /// op 30: `[transition id u16][welcome]`.
    pub fn process_welcome(&mut self, payload: &[u8]) -> Reply {
        self.process_transition(payload, false)
    }

    fn process_transition(&mut self, payload: &[u8], commit: bool) -> Reply {
        if payload.len() < 2 || self.session.is_none() {
            return Reply::default();
        }
        let tid = u16::from_be_bytes([payload[0], payload[1]]);
        let s = self.session.as_mut().unwrap();
        let result = if commit {
            s.process_commit(&payload[2..]).map_err(|e| format!("{e:?}"))
        } else {
            s.process_welcome(&payload[2..]).map_err(|e| format!("{e:?}"))
        };
        match result {
            Ok(()) => {
                info!(
                    "dave: {} processed (transition {tid}), epoch {:?}",
                    if commit { "commit" } else { "welcome" },
                    self.session.as_ref().and_then(|s| s.epoch()).map(|e| e.as_u64())
                );
                let mut reply = Reply::default();
                if tid == 0 {
                    self.reinitializing = false;
                    self.last_transition = Some(0);
                } else {
                    self.pending.insert(tid, self.protocol_version);
                    reply.ready_for = Some(tid);
                }
                reply
            }
            Err(e) => {
                warn!("dave: {} for transition {tid} failed: {e}", if commit { "commit" } else { "welcome" });
                self.recover(tid)
            }
        }
    }

    /// End-to-end encrypt one Opus frame (passthrough before the group is ready, on protocol 0, and
    /// for the silence frame, as discord.js does).
    pub fn encrypt(&mut self, opus: &[u8]) -> Vec<u8> {
        if self.protocol_version == 0 || opus == SILENCE {
            return opus.to_vec();
        }
        match self.session.as_mut() {
            Some(s) if s.is_ready() => match s.encrypt_opus(opus) {
                Ok(out) => out.into_owned(),
                Err(e) => {
                    warn!("dave: encrypt: {e:?}");
                    opus.to_vec()
                }
            },
            _ => opus.to_vec(),
        }
    }
}
