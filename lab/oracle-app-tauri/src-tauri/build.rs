use std::path::PathBuf;

fn main() {
    guard_frontend_dist();
    tauri_build::build()
}

/// Preflight guard against the `frontendDist` runaway (PR #17 / RCA).
///
/// `tauri-codegen`'s asset embedder recursively walks `frontendDist` and
/// bakes every file into the binary. If `frontendDist` resolves to a
/// directory that *contains* the cargo build output (`target/`), each build
/// embeds the previous build's embedded assets into a new, bigger bin — a
/// geometric blowup that once ballooned `target/` to ~250 GB.
///
/// This runs in `build.rs`, i.e. BEFORE the codegen macro expands, so a bad
/// `frontendDist` fails fast on build #1 and never accumulates anything.
fn guard_frontend_dist() {
    println!("cargo:rerun-if-changed=tauri.conf.json");

    let manifest = PathBuf::from(env_or_bail("CARGO_MANIFEST_DIR"));
    let conf = match std::fs::read_to_string(manifest.join("tauri.conf.json")) {
        Ok(c) => c,
        Err(_) => return, // no config here → let tauri-build report it
    };

    let Some(dist) = extract_frontend_dist(&conf) else {
        return; // devUrl-only / array / absent → nothing to embed from disk
    };

    // Resolve frontendDist relative to src-tauri/ and canonicalize to a real path.
    let dist_path = manifest.join(&dist);
    let Ok(dist_abs) = dist_path.canonicalize() else {
        return; // doesn't exist yet → tauri-build will surface that
    };

    // OUT_DIR always lives under target/. If it sits *inside* frontendDist,
    // the embedder will ingest the entire build tree.
    let out_dir = PathBuf::from(env_or_bail("OUT_DIR"));
    let out_abs = out_dir.canonicalize().unwrap_or(out_dir);

    if out_abs.starts_with(&dist_abs) {
        panic!(
            "\n\n  ✗ frontendDist (\"{dist}\") contains the build output.\n\
            \n      {} \n    is inside\n      {}\n\n  \
            tauri-codegen would recursively embed target/ into the binary —\n  \
            the ~250 GB runaway (see PR #17). Point frontendDist at a folder\n  \
            that holds ONLY the web assets, e.g. \"../ui\".\n\n",
            out_abs.display(),
            dist_abs.display(),
        );
    }
}

fn env_or_bail(key: &str) -> String {
    std::env::var(key).unwrap_or_else(|_| panic!("{key} not set — build must run under cargo"))
}

/// Minimal extractor for the `"frontendDist": "<path>"` string value, so the
/// guard needs no serde_json build-dependency. Returns None if the value
/// isn't a plain string (e.g. a devUrl or an array of paths).
fn extract_frontend_dist(conf: &str) -> Option<String> {
    let key = "\"frontendDist\"";
    let after_key = &conf[conf.find(key)? + key.len()..];
    let after_colon = &after_key[after_key.find(':')? + 1..];
    // first non-space char must be a quote for this to be a plain string value
    let trimmed = after_colon.trim_start();
    let inner = trimmed.strip_prefix('"')?;
    let end = inner.find('"')?;
    Some(inner[..end].to_string())
}
