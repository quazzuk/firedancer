use std::env;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    let cargo_manifest_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
    let mut fd_root = PathBuf::from(&cargo_manifest_dir);
    // Navigate from contrib/accstr-consumer to firedancer root
    fd_root.pop(); // accstr-consumer -> contrib
    fd_root.pop(); // contrib -> firedancer root

    // Build output path
    let mut build_path = fd_root.clone();
    build_path.push("build");
    build_path.push("native");
    build_path.push("gcc");

    let mut lib_path = build_path.clone();
    lib_path.push("lib");
    println!("cargo:rustc-link-search={}", lib_path.to_str().unwrap());

    // Link against required firedancer libraries
    for lib in &[
        "fd_disco", // accstr module
        "fd_tango", // mcache/dcache/fseq
        "fd_util",  // base utilities
    ] {
        println!("cargo:rustc-link-lib=static={}", lib);
        println!(
            "cargo:rerun-if-changed={}/lib{}.a",
            lib_path.to_str().unwrap(),
            lib
        );
    }

    // Source path for includes
    let mut src_path = fd_root.clone();
    src_path.push("src");

    // Find GCC include path dynamically
    let gcc_include = find_gcc_include_path();

    // Build clang args
    let mut clang_args = vec![
        format!("-I{}", fd_root.to_str().unwrap()),
        format!("-I{}", src_path.to_str().unwrap()),
        "-std=gnu17".to_string(),
        "-D_GNU_SOURCE".to_string(),
    ];

    if let Some(gcc_inc) = gcc_include {
        clang_args.push(format!("-I{}", gcc_inc));
    }

    // Generate bindings - only for functions we actually call
    // We define our own Rust structs for the data types
    let bindings = bindgen::Builder::default()
        .header("wrapper.h")
        .clang_args(&clang_args)
        .detect_include_paths(true)
        // Only allow the specific functions and types we need
        .allowlist_function("fd_wksp_attach")
        .allowlist_function("fd_wksp_detach")
        .allowlist_function("fd_wksp_tag_query")
        .allowlist_type("fd_wksp_t")
        .allowlist_type("fd_wksp_tag_query_info_t")
        // Block packed struct types - we define these in Rust
        .blocklist_type("fd_accstr_account_update")
        .blocklist_type("fd_accstr_slot_boundary")
        .blocklist_type("fd_accstr_shmem_hdr")
        .blocklist_type("fd_frag_meta")
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()
        .expect("Unable to generate bindings");

    println!("cargo:rerun-if-changed=wrapper.h");

    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings!");
}

/// Find the GCC include path containing stdalign.h
fn find_gcc_include_path() -> Option<String> {
    // Try to get GCC's include path
    let output = Command::new("gcc")
        .args(["-print-file-name=include"])
        .output()
        .ok()?;

    if output.status.success() {
        let path = String::from_utf8_lossy(&output.stdout).trim().to_string();
        if !path.is_empty() && path != "include" {
            return Some(path);
        }
    }

    // Fallback: search common locations
    for version in ["13", "12", "11", "10", "9"] {
        let path = format!("/usr/lib/gcc/x86_64-linux-gnu/{}/include", version);
        if std::path::Path::new(&path).exists() {
            return Some(path);
        }
    }

    None
}
