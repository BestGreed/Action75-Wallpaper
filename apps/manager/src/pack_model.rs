use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};
use std::collections::BTreeSet;
pub fn defaults(schema: &Value) -> Map<String, Value> {
    schema["groups"]
        .as_array()
        .into_iter()
        .flatten()
        .flat_map(|g| g["controls"].as_array().into_iter().flatten())
        .filter_map(|c| Some((c["id"].as_str()?.to_owned(), c.get("default")?.clone())))
        .collect()
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Bundle {
    #[serde(rename = "packId")]
    pub pack_id: String,
    pub schema: Value,
    pub values: Map<String, Value>,
    #[serde(default)]
    pub revision: Value,
}

#[derive(Debug)]
struct PendingWrite {
    edit: u64,
    keys: BTreeSet<String>,
    edited_after_send: BTreeSet<String>,
}

#[derive(Default, Debug)]
pub struct Draft {
    pub selected: String,
    pub selection: u64,
    pub bundle: Option<Bundle>,
    pub values: Map<String, Value>,
    pub edit: u64,
    in_flight: Option<PendingWrite>,
}
impl Draft {
    pub fn select(&mut self, id: String) -> u64 {
        self.selection = self.selection.wrapping_add(1);
        if self.selected != id {
            self.selected = id;
            self.bundle = None;
            self.values.clear();
        }
        self.in_flight = None;
        self.edit = 0;
        self.selection
    }
    pub fn load(&mut self, id: &str, selection: u64, bundle: Bundle) -> bool {
        if id != self.selected || selection != self.selection || bundle.pack_id != id {
            return false;
        }
        self.values = bundle.values.clone();
        self.bundle = Some(bundle);
        true
    }
    pub fn changed(&mut self, fields: impl IntoIterator<Item = String>) {
        self.edit = self.edit.wrapping_add(1);
        if let Some(write) = &mut self.in_flight {
            write.edited_after_send.extend(fields);
        }
    }
    pub fn begin_write(&mut self, keys: BTreeSet<String>) -> u64 {
        self.in_flight = Some(PendingWrite {
            edit: self.edit,
            keys,
            edited_after_send: BTreeSet::new(),
        });
        self.edit
    }
    pub fn writing(&self) -> bool {
        self.in_flight.is_some()
    }
    pub fn patch(&self) -> Map<String, Value> {
        let Some(bundle) = &self.bundle else {
            return Map::new();
        };
        self.values
            .iter()
            .filter(|(k, v)| bundle.values.get(*k) != Some(*v))
            .map(|(k, v)| (k.clone(), v.clone()))
            .collect()
    }
    pub fn final_patch(&self) -> Map<String, Value> {
        let mut patch = self.patch();
        if let Some(write) = &self.in_flight {
            // Include an explicit edit back to the old baseline even if the pending write
            // changed that field indirectly (for example the other end of a range).
            for key in write.keys.union(&write.edited_after_send) {
                if let Some(value) = self.values.get(key) {
                    patch.insert(key.clone(), value.clone());
                }
            }
        }
        patch
    }
    pub fn ack(&mut self, id: &str, selection: u64, edit: u64, bundle: Bundle) -> bool {
        if bundle.pack_id != id {
            return false;
        }
        let Some(write) = self.take_write(id, selection, edit) else {
            return false;
        };
        let mut values = bundle.values.clone();
        for key in write.edited_after_send {
            if let (Some(confirmed), Some(local)) = (values.get_mut(&key), self.values.remove(&key))
            {
                *confirmed = local;
            }
        }
        self.values = values;
        self.bundle = Some(bundle);
        true
    }
    pub fn fail(&mut self, id: &str, selection: u64, edit: u64) -> bool {
        self.take_write(id, selection, edit).is_some()
    }
    fn take_write(&mut self, id: &str, selection: u64, edit: u64) -> Option<PendingWrite> {
        if id == self.selected
            && selection == self.selection
            && self
                .in_flight
                .as_ref()
                .is_some_and(|write| write.edit == edit)
        {
            self.in_flight.take()
        } else {
            None
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    fn bundle(id: &str, value: i32) -> Bundle {
        serde_json::from_value(json!({"packId":id,"schema":{"version":1,"groups":[]},"values":{"arbitrary":value},"revision":value})).unwrap()
    }
    #[test]
    fn switching_profiles_restores_each_value() {
        let mut d = Draft::default();
        let a = d.select("a".into());
        d.load("a", a, bundle("a", 80));
        d.values.insert("arbitrary".into(), json!(95));
        d.changed(["arbitrary".into()]);
        let edit = d.begin_write(["arbitrary".into()].into());
        assert!(d.ack("a", a, edit, bundle("a", 95)));
        let b = d.select("b".into());
        d.load("b", b, bundle("b", 20));
        assert_eq!(d.values["arbitrary"], 20);
        let a2 = d.select("a".into());
        d.load("a", a2, bundle("a", 95));
        assert_eq!(d.values["arbitrary"], 95);
    }
    #[test]
    fn old_profile_ack_and_load_cannot_pollute_new_selection() {
        let mut d = Draft::default();
        let a = d.select("a".into());
        d.load("a", a, bundle("a", 80));
        let e = d.begin_write(BTreeSet::new());
        let b = d.select("b".into());
        d.load("b", b, bundle("b", 20));
        assert!(!d.ack("a", a, e, bundle("a", 90)));
        assert!(!d.load("a", a, bundle("a", 5)));
        assert_eq!(d.values["arbitrary"], 20);
    }
    #[test]
    fn newer_draft_survives_older_ack_and_revision_updates() {
        let mut d = Draft::default();
        let s = d.select("a".into());
        d.load("a", s, bundle("a", 50));
        d.values.insert("arbitrary".into(), json!(60));
        d.changed(["arbitrary".into()]);
        let e = d.begin_write(["arbitrary".into()].into());
        d.values.insert("arbitrary".into(), json!(70));
        d.changed(["arbitrary".into()]);
        assert!(d.ack("a", s, e, bundle("a", 60)));
        assert_eq!(d.values["arbitrary"], 70);
        assert_eq!(d.bundle.as_ref().unwrap().revision, 60);
        assert_eq!(d.patch()["arbitrary"], 70);
    }

    #[test]
    fn normalized_range_survives_editing_another_field_during_write() {
        let id = "glass-shatter";
        let mut d = Draft::default();
        let selection = d.select(id.into());
        let mut initial = bundle(id, 1);
        initial.values = Map::from_iter([
            ("impactMin".into(), json!(0.60)),
            ("impactMax".into(), json!(1.45)),
            ("spread".into(), json!(1.00)),
        ]);
        assert!(d.load(id, selection, initial.clone()));
        d.values.insert("impactMin".into(), json!(2.00));
        d.changed(["impactMin".into()]);
        let edit = d.begin_write(["impactMin".into()].into());

        d.values.insert("spread".into(), json!(1.20));
        d.changed(["spread".into()]);
        let mut confirmed = initial;
        confirmed.values.insert("impactMin".into(), json!(2.00));
        confirmed.values.insert("impactMax".into(), json!(2.00));
        confirmed.revision = json!(2);
        assert!(d.ack(id, selection, edit, confirmed));

        assert_eq!(d.values["impactMax"], json!(2.00));
        assert_eq!(d.patch(), Map::from_iter([("spread".into(), json!(1.20))]));
    }

    #[test]
    fn failed_write_releases_only_its_guard() {
        let mut d = Draft::default();
        let s = d.select("a".into());
        d.load("a", s, bundle("a", 20));
        let edit = d.begin_write(BTreeSet::new());
        assert!(!d.fail("a", s, edit + 1));
        assert!(d.writing());
        assert!(d.fail("a", s, edit));
        assert!(!d.writing());
    }
}
