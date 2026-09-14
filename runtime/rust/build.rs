use std::{env, path::PathBuf};
fn main() {
    println!("cargo:rerun-if-env-changed=IMAPIPE_MEDIA_PREFIX");
    let prefix = PathBuf::from(env::var_os("IMAPIPE_MEDIA_PREFIX").expect(
        "Build the pinned private media artifact with runtime/build_native.py and set IMAPIPE_MEDIA_PREFIX; system GStreamer plugins are not supported",
    ));
    let lib = prefix.join("lib");
    assert!(
        prefix.join("build.json").is_file(),
        "Missing private media build provenance"
    );
    println!("cargo:rustc-link-search=native={}", lib.display());
    println!("cargo:rustc-link-lib=dylib=gstreamer-full-1.0");
    println!("cargo:metadata=native-prefix={}", prefix.display());
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib.display());
    }
}
