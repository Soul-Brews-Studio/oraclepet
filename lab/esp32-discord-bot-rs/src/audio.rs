//! What the bot says.
//!
//! - `mic`: the pet's ES8311 mic (M2). 200 Hz high-pass (below it was WiFi rumble on this board),
//!   +15 dB, and a voice gate tuned like the pet's (lab/oraclepet-lib/VOICE.md §3): speaking starts
//!   on 3 loud frames in a row and ends after 1.2 s without voice, so Discord shows the bot talking
//!   only when someone is.
//! - `melody`: M1's test sound made on the chip, the fallback when the mic does not open.

use std::f32::consts::PI;
use std::sync::Arc;
use std::time::{Duration, Instant};

use esp_idf_svc::sys::mic;
use log::{info, warn};

use crate::opus::{Encoder, FRAME_SAMPLES};
use crate::voice::Voice;

const GAIN: f32 = 5.6; // +15 dB: the mic at 30 dB gives speech around RMS 250

pub fn start_mic() -> bool {
    let rc = unsafe { mic::mic_start() };
    if rc != 0 {
        warn!("mic: start failed ({rc}), playing the melody instead");
    }
    rc == 0
}

pub fn mic(voice: Arc<Voice>) -> ! {
    let mut enc = Encoder::new().expect("opus encoder");
    let mut pcm = [0i16; FRAME_SAMPLES];
    // 2nd-order high-pass at 200 Hz (RBJ, Q 0.707), the pet's filter
    let (b0, b1, b2, a1, a2) = (0.945976f32, -1.891953f32, 0.945976f32, -1.889032f32, 0.894873f32);
    let (mut x1, mut x2, mut y1, mut y2) = (0f32, 0f32, 0f32, 0f32);
    let mut mins = [0f32; 150]; // frame RMS of the last 3 s: the floor is their minimum
    let mut mi = 0;
    let (mut r1, mut r2) = (0f32, 0f32);
    let mut hist = [0u8; 60]; // voiced frames of the last 1.2 s
    let mut hi = 0;
    let mut on = false;
    let mut frames: u32 = 0;
    loop {
        if unsafe { mic::mic_read(pcm.as_mut_ptr(), FRAME_SAMPLES as i32) } != 0 {
            std::thread::sleep(Duration::from_millis(20));
            continue;
        }
        let mut e = 0f64;
        for s in pcm.iter_mut() {
            let x = *s as f32;
            let y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            (x2, x1, y2, y1) = (x1, x, y1, y);
            e += (y * y) as f64;
            *s = (y * GAIN).clamp(-32768.0, 32767.0) as i16;
        }
        let rms = (e / FRAME_SAMPLES as f64).sqrt() as f32;
        mins[mi] = rms;
        mi = (mi + 1) % mins.len();
        let floor = mins.iter().copied().filter(|&m| m > 0.0).fold(f32::MAX, f32::min).max(1.0);
        let on_th = (2.0 * floor).max(floor + 60.0);
        let hold_th = (1.5 * floor).max(floor + 30.0);
        let speechy = rms.min(r1).min(r2) > on_th;
        let voiced = rms.min(r1) > hold_th;
        (r2, r1) = (r1, rms);
        hist[hi] = voiced as u8;
        hi = (hi + 1) % hist.len();
        frames = frames.wrapping_add(1);
        if frames % 250 == 0 {
            info!("mic: rms {rms:.0}, floor {floor:.0}, {}", if on { "talking" } else { "quiet" });
        }
        if !on && speechy {
            on = true;
            hist = [1; 60];
        } else if on && hist.iter().map(|&v| v as u32).sum::<u32>() < 3 {
            on = false;
            voice.silence();
        }
        if on && voice.ready() {
            match enc.encode(&pcm) {
                Ok(opus) => voice.send_opus(opus),
                Err(e) => warn!("{e}"),
            }
        }
    }
}

pub fn melody(voice: Arc<Voice>) -> ! {
    let mut enc = Encoder::new().expect("opus encoder");
    const NOTES: [f32; 6] = [261.63, 329.63, 392.00, 523.25, 392.00, 329.63];
    const NOTE: usize = 33 * FRAME_SAMPLES; // 0.66 s per note
    const FADE: usize = 400; // 25 ms fade in and out, per sample (frame steps clicked)
    let mut pcm = [0i16; FRAME_SAMPLES];
    let mut phase = 0f32;
    let mut next = Instant::now();
    let mut frame: u32 = 0;
    loop {
        next += Duration::from_millis(20);
        let now = Instant::now();
        if next > now {
            std::thread::sleep(next - now);
        } else if now - next > Duration::from_millis(200) {
            next = now;
        }
        frame = frame.wrapping_add(1);
        if !voice.ready() {
            continue;
        }
        let t = frame % 250; // 200 frames of sound, 50 quiet
        if t >= 200 {
            if t == 200 {
                voice.silence();
            }
            continue;
        }
        for (i, s) in pcm.iter_mut().enumerate() {
            let k = t as usize * FRAME_SAMPLES + i; // sample index within the 4 s
            let (note, pos) = (k / NOTE, k % NOTE);
            if pos == 0 {
                phase = 0.0;
            }
            let env = (pos.min(NOTE - 1 - pos) as f32 / FADE as f32).min(1.0);
            *s = (6000.0 * env * phase.sin()) as i16;
            phase += 2.0 * PI * NOTES[note % 6] / 16000.0;
            if phase > 2.0 * PI {
                phase -= 2.0 * PI;
            }
        }
        match enc.encode(&pcm) {
            Ok(opus) => voice.send_opus(opus),
            Err(e) => log::warn!("{e}"),
        }
    }
}
