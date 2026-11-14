//! esp32-discord-bot: the OraclePet (ESP32-S3, the 2.8in board) as its own Discord voice bot.
//! Discord gateway v10 → voice gateway v8 → UDP/RTP, with DAVE end-to-end encryption from the
//! `davey` crate, which Discord has required of bots outside Stage channels since 2026-03-02.
//!
//! WiFi: the network the pet was given over Improv (NVS "wifi": "ssid", "pass"), read at boot.
//! Discord: the bot token, guild and voice channel come from the build environment
//! (tools/build.sh), never from a file in git.

mod audio;
mod crypto;
mod dave;
mod gateway;
mod opus;
mod voice;
mod ws;

use std::time::Duration;

use esp_idf_svc::eventloop::EspSystemEventLoop;
use esp_idf_svc::hal::peripherals::Peripherals;
use esp_idf_svc::nvs::{EspDefaultNvsPartition, EspNvs, NvsDefault};
use esp_idf_svc::sys;
use esp_idf_svc::wifi::{AuthMethod, BlockingWifi, ClientConfiguration, Configuration, EspWifi};
use log::{error, info};

fn main() -> anyhow::Result<()> {
    sys::link_patches();
    esp_idf_svc::log::EspLogger::initialize_default();
    info!("esp32-discord-bot {}", env!("CARGO_PKG_VERSION"));

    let settings = gateway::Settings {
        token: option_env!("DISCORD_BOT_TOKEN").unwrap_or("").to_string(),
        guild_id: option_env!("DISCORD_GUILD_ID").unwrap_or("").to_string(),
        channel_id: option_env!("DISCORD_CHANNEL_ID").unwrap_or("").to_string(),
    };
    if settings.token.is_empty() || settings.guild_id.is_empty() || settings.channel_id.is_empty() {
        error!("built without DISCORD_BOT_TOKEN, DISCORD_GUILD_ID or DISCORD_CHANNEL_ID: see tools/build.sh");
        loop {
            std::thread::sleep(Duration::from_secs(60));
        }
    }

    let peripherals = Peripherals::take()?;
    let sysloop = EspSystemEventLoop::take()?;
    let nvs = EspDefaultNvsPartition::take()?;
    let (ssid, pass) = pet_wifi(&nvs)?;
    let mut wifi = BlockingWifi::wrap(EspWifi::new(peripherals.modem, sysloop.clone(), Some(nvs))?, sysloop)?;
    wifi.set_configuration(&Configuration::Client(ClientConfiguration {
        ssid: ssid.as_str().try_into().map_err(|_| anyhow::anyhow!("SSID too long"))?,
        password: pass.as_str().try_into().map_err(|_| anyhow::anyhow!("WiFi password too long"))?,
        auth_method: if pass.is_empty() { AuthMethod::None } else { AuthMethod::WPA2Personal },
        ..Default::default()
    }))?;
    wifi.start()?;
    wifi.connect()?;
    wifi.wait_netif_up()?;
    // no power save: the 20 ms audio cadence stays smooth (no BLE in this firmware)
    unsafe { sys::esp_wifi_set_ps(sys::wifi_ps_type_t_WIFI_PS_NONE) };
    info!("wifi: {ssid} up, {}", wifi.wifi().sta_netif().get_ip_info()?.ip);

    psram_stacks(); // threads created from here on get their stacks in PSRAM
    let voice = voice::Voice::start()?;
    let v = voice.clone();
    std::thread::Builder::new()
        .name("gateway".into())
        .stack_size(24 * 1024)
        .spawn(move || gateway::run(settings, move |creds| v.connect(creds)))?;
    let mic = audio::start_mic();
    std::thread::Builder::new()
        .name("audio".into())
        .stack_size(48 * 1024)
        .spawn(move || if mic { audio::mic(voice) } else { audio::melody(voice) })?;

    loop {
        std::thread::sleep(Duration::from_secs(30));
        unsafe {
            info!(
                "heap: internal {} free (min {}), psram {} free",
                sys::heap_caps_get_free_size(sys::MALLOC_CAP_INTERNAL),
                sys::heap_caps_get_minimum_free_size(sys::MALLOC_CAP_INTERNAL),
                sys::heap_caps_get_free_size(sys::MALLOC_CAP_SPIRAM)
            );
        }
    }
}

/// The WiFi the pet firmware stored when it was provisioned over Improv.
fn pet_wifi(nvs: &EspDefaultNvsPartition) -> anyhow::Result<(String, String)> {
    let store = EspNvs::<NvsDefault>::new(nvs.clone(), "wifi", false)?;
    let (mut a, mut b) = ([0u8; 64], [0u8; 96]);
    let ssid = store
        .get_str("ssid", &mut a)?
        .ok_or_else(|| anyhow::anyhow!("no WiFi in NVS: give the pet its WiFi over Improv first"))?
        .to_string();
    let pass = store.get_str("pass", &mut b)?.unwrap_or("").to_string();
    Ok((ssid, pass))
}

/// Stacks of the threads created after this call come from PSRAM: the voice thread's MLS work
/// needs a big one, and internal RAM is kept for WiFi and TLS.
fn psram_stacks() {
    unsafe {
        let mut cfg = sys::esp_pthread_get_default_config();
        cfg.stack_alloc_caps = sys::MALLOC_CAP_SPIRAM | sys::MALLOC_CAP_8BIT;
        sys::esp_pthread_set_cfg(&cfg);
    }
}
