use serde_json::Value;
use std::{
    collections::HashSet,
    fs,
    path::{Path, PathBuf},
    sync::LazyLock,
};

pub(crate) const REVISION: &str = "_action75Revision";
pub(crate) const VERSIONS: &str = ".action75-versions";

static BUNDLED_IDS: LazyLock<HashSet<String>> = LazyLock::new(|| {
    let inventory: Value = serde_json::from_str(include_str!("../../../tools/release-assets.json"))
        .expect("release-assets.json must be valid JSON");
    inventory["wallpapers"]
        .as_object()
        .expect("the release inventory must list wallpapers")
        .keys()
        .map(|id| id.to_ascii_lowercase())
        .collect()
});

pub(crate) fn bundled_id(id: &str) -> bool {
    BUNDLED_IDS.contains(&id.to_ascii_lowercase())
}

pub(crate) fn bundled_manifest(directory: &Path, manifest: &Value) -> bool {
    directory
        .file_name()
        .and_then(|name| name.to_str())
        .is_some_and(|name| {
            bundled_id(name)
                && manifest["id"]
                    .as_str()
                    .is_some_and(|id| id.eq_ignore_ascii_case(name))
        })
        && manifest["version"] == 1
        && manifest.get(REVISION).is_none()
}

pub(crate) fn valid_revision(value: &str) -> bool {
    value.strip_prefix("v-").is_some_and(|value| {
        value.len() == 40 && value.bytes().all(|byte| byte.is_ascii_hexdigit())
    })
}

pub(crate) fn sources(root: &Path, configured: PathBuf) -> Vec<(PathBuf, bool)> {
    let bundled = root.join("wallpapers-native");
    let same_library = configured == bundled
        || fs::canonicalize(&configured)
            .ok()
            .zip(fs::canonicalize(&bundled).ok())
            .is_some_and(|(left, right)| left == right);
    let mut directories = vec![
        (configured, false),
        (root.join("data/native-wallpapers"), false),
    ];
    if !same_library {
        directories.push((bundled, true));
    }
    directories
}
