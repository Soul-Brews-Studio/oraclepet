fn main() {
    // The bot's settings come from the build environment (tools/build.sh), never from a file in git.
    for var in ["DISCORD_BOT_TOKEN", "DISCORD_GUILD_ID", "DISCORD_CHANNEL_ID"] {
        println!("cargo:rerun-if-env-changed={var}");
    }
    embuild::espidf::sysenv::output();
}
