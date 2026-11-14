//! Opus at 16 kHz mono through Espressif's encoder (esp_audio_codec, the one xiaozhi runs on this
//! board). A 16 kHz stream is valid Opus; Discord's decoders play it at 48 kHz.

use core::ffi::c_void;

use esp_idf_svc::sys::opus as ffi;

/// 20 ms at 16 kHz.
pub const FRAME_SAMPLES: usize = 320;

pub struct Encoder {
    handle: *mut c_void,
    out: Vec<u8>,
}

// the handle is only used by the thread that owns the encoder
unsafe impl Send for Encoder {}

impl Encoder {
    pub fn new() -> anyhow::Result<Self> {
        let mut cfg = ffi::esp_opus_enc_config_t {
            sample_rate: 16000,
            channel: 1,
            bits_per_sample: 16,
            bitrate: 32000,
            frame_duration: ffi::esp_opus_enc_frame_duration_t_ESP_OPUS_ENC_FRAME_DURATION_20_MS,
            application_mode: ffi::esp_opus_enc_application_t_ESP_OPUS_ENC_APPLICATION_VOIP,
            complexity: 0,
            enable_fec: false,
            enable_dtx: false,
            enable_vbr: true,
        };
        let mut handle: *mut c_void = core::ptr::null_mut();
        let rc = unsafe {
            ffi::esp_opus_enc_open(&mut cfg as *mut _ as *mut c_void, core::mem::size_of_val(&cfg) as _, &mut handle)
        };
        anyhow::ensure!(rc == ffi::esp_audio_err_t_ESP_AUDIO_ERR_OK && !handle.is_null(), "esp_opus_enc_open: {rc}");
        let (mut in_size, mut out_size) = (0, 0);
        unsafe { ffi::esp_opus_enc_get_frame_size(handle, &mut in_size, &mut out_size) };
        log::info!("opus: {in_size} bytes in, up to {out_size} out per 20 ms");
        Ok(Encoder { handle, out: vec![0; (out_size as usize).max(512)] })
    }

    pub fn encode(&mut self, pcm: &[i16; FRAME_SAMPLES]) -> anyhow::Result<&[u8]> {
        let mut input = ffi::esp_audio_enc_in_frame_t {
            buffer: pcm.as_ptr() as *mut u8,
            len: (FRAME_SAMPLES * 2) as _,
            ..Default::default()
        };
        let mut output = ffi::esp_audio_enc_out_frame_t {
            buffer: self.out.as_mut_ptr(),
            len: self.out.len() as _,
            ..Default::default()
        };
        let rc = unsafe { ffi::esp_opus_enc_process(self.handle, &mut input, &mut output) };
        anyhow::ensure!(rc == ffi::esp_audio_err_t_ESP_AUDIO_ERR_OK, "opus encode: {rc}");
        Ok(&self.out[..output.encoded_bytes as usize])
    }
}

impl Drop for Encoder {
    fn drop(&mut self) {
        unsafe { ffi::esp_opus_enc_close(self.handle) };
    }
}
