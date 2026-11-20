use std::{env, fs, path::PathBuf, process::Command};

#[path = "src/branding.rs"]
mod branding;
use branding::APP_NAME;

fn main() {
    println!("cargo:rerun-if-changed=../../assets/app.ico");
    println!("cargo:rerun-if-changed=src/branding.rs");
    println!("cargo:rerun-if-changed=manager.manifest");
    if !env::var("TARGET")
        .unwrap_or_default()
        .contains("windows-msvc")
    {
        return;
    }
    let root = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    let icon = root
        .join("../../assets/app.ico")
        .canonicalize()
        .expect("application icon");
    let kits = PathBuf::from(env::var_os("ProgramFiles(x86)").expect("Windows SDK location"))
        .join("Windows Kits/10/bin");
    let mut versions: Vec<_> = fs::read_dir(kits)
        .expect("Windows SDK installed")
        .flatten()
        .map(|p| p.path())
        .filter(|p| p.join("x64/rc.exe").is_file())
        .collect();
    versions.sort();
    let compiler = versions
        .last()
        .expect("Windows resource compiler")
        .join("x64/rc.exe");
    let source = out.join("app.rc");
    let resource = out.join("app.res");
    let icon = icon.to_string_lossy().replace('\\', "/");
    let manifest = root
        .join("manager.manifest")
        .to_string_lossy()
        .replace('\\', "/");
    let version = env!("CARGO_PKG_VERSION");
    let numeric_version = concat!(
        env!("CARGO_PKG_VERSION_MAJOR"),
        ",",
        env!("CARGO_PKG_VERSION_MINOR"),
        ",",
        env!("CARGO_PKG_VERSION_PATCH"),
        ",0"
    );
    fs::write(
        &source,
        format!(
            r#"1 ICON "{icon}"
1 24 "{manifest}"
1 VERSIONINFO
FILEVERSION {numeric_version}
PRODUCTVERSION {numeric_version}
FILEOS 0x00040004
FILETYPE 0x1
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904B0"
        BEGIN
            VALUE "FileDescription", "{APP_NAME}\0"
            VALUE "FileVersion", "{version}\0"
            VALUE "OriginalFilename", "Action75Manager.exe\0"
            VALUE "ProductName", "{APP_NAME}\0"
            VALUE "ProductVersion", "{version}\0"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x0409, 1200
    END
END
"#
        ),
    )
    .unwrap();
    assert!(
        Command::new(compiler)
            .arg("/nologo")
            .arg("/fo")
            .arg(&resource)
            .arg(source)
            .status()
            .expect("resource compiler")
            .success()
    );
    println!("cargo:rustc-link-arg-bins={}", resource.display());
}
