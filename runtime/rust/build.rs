use sha2::{Digest, Sha256};
use std::{env, fs, path::PathBuf, process::Command};
fn main() {
    println!("cargo:rerun-if-env-changed=IMAPIPE_MEDIA_PREFIX");
    let prefix = PathBuf::from(env::var_os("IMAPIPE_MEDIA_PREFIX").expect(
        "Build the pinned private media artifact with runtime/build_native.py and set IMAPIPE_MEDIA_PREFIX; system GStreamer plugins are not supported",
    ));
    let manifest = prefix.join("build.json");
    println!("cargo:rerun-if-changed={}", manifest.display());
    let provenance: serde_json::Value = serde_json::from_slice(
        &fs::read(&manifest).expect("Missing private media build provenance"),
    )
    .expect("Invalid private media build provenance");
    let source = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let git = |argument: &str| {
        let output = Command::new("git")
            .arg("-C")
            .arg(&source)
            .args(["rev-parse", argument])
            .output()
            .expect("Git is required to verify the pinned media source");
        assert!(output.status.success(), "Cannot verify pinned media source");
        String::from_utf8(output.stdout)
            .expect("Invalid Git identity")
            .trim()
            .to_owned()
    };
    let repository = PathBuf::from(git("--show-toplevel"));
    assert_eq!(
        repository.join("runtime/rust").canonicalize().unwrap(),
        source.canonicalize().unwrap(),
        "Media source must be the pinned native repository checkout"
    );
    assert_eq!(
        provenance["source_commit"].as_str(),
        Some(git("HEAD").as_str()),
        "Private media artifact does not match the pinned source ABI"
    );
    let artifacts = provenance["artifacts"]
        .as_object()
        .expect("Missing media artifact digests");
    assert!(!artifacts.is_empty(), "Empty media artifact set");
    for (relative, expected) in artifacts {
        let path = std::path::Path::new(relative);
        assert!(
            path.components()
                .all(|c| matches!(c, std::path::Component::Normal(_))),
            "Invalid artifact path"
        );
        let artifact = prefix.join(path);
        println!("cargo:rerun-if-changed={}", artifact.display());
        let digest = format!(
            "{:x}",
            Sha256::digest(fs::read(&artifact).expect("Missing media artifact"))
        );
        assert_eq!(
            expected.as_str(),
            Some(digest.as_str()),
            "Private media artifact digest mismatch: {relative}"
        );
    }
    let lib = prefix.join("lib");
    let filename = match env::var("CARGO_CFG_TARGET_OS").unwrap().as_str() {
        "macos" => "lib/libgstreamer-full-1.0.dylib",
        "windows" => "lib/gstreamer-full-1.0.dll",
        _ => "lib/libgstreamer-full-1.0.so",
    };
    assert!(
        artifacts.contains_key(filename),
        "Linked native media library is missing from provenance"
    );
    println!("cargo:rustc-link-search=native={}", lib.display());
    println!("cargo:rustc-link-lib=dylib=gstreamer-full-1.0");
    println!("cargo::metadata=native_prefix={}", prefix.display());
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib.display());
    }
}
