#include "PackSettings.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;
using action75::PackSettingsStore;

namespace {
void require(bool ok, const std::string& message) {
  if (!ok) throw std::runtime_error(message);
}
void near(float actual, float expected, const std::string& message) {
  require(std::abs(actual-expected)<1e-5f, message);
}
template<class F> void rejects(F&& function, const std::string& message) {
  bool rejected=false;
  try { function(); } catch (const std::exception&) { rejected=true; }
  require(rejected,message);
}
struct Temporary {
  fs::path root;
  Temporary() {
    const auto parent=fs::weakly_canonical(fs::temp_directory_path());
    root=parent/("Action75-PackSettings-Tests-"+std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
    require(root.parent_path()==parent,"Temp path must stay under the system temporary directory");
    require(fs::create_directory(root),"Test directory must be newly created");
  }
  ~Temporary() {
    std::error_code error;
    const auto parent=fs::weakly_canonical(fs::temp_directory_path(),error);
    if(!error && root.parent_path()==parent &&
       root.filename().string().find("Action75-PackSettings-Tests-")==0)
      fs::remove_all(root,error);
  }
};
void write(const fs::path& file, const json& value) {
  fs::create_directories(file.parent_path());
  std::ofstream output(file,std::ios::binary|std::ios::trunc);
  output<<value.dump(2);
  require(static_cast<bool>(output),"Test file write failed");
}
json read(const fs::path& file) {
  std::ifstream input(file,std::ios::binary);return json::parse(input);
}
json slider(const std::string& id,const std::string& target,double minimum,
            double maximum,double initial,json map=nullptr) {
  json binding={{"target",target}};
  if(!map.is_null()) binding["mapping"]=map;
  return {{"id",id},{"type","slider"},{"label",id},{"min",minimum},
    {"max",maximum},{"step",.01},{"default",initial},{"binding",binding}};
}
json schema(json controls) {
  return {{"version",1},{"groups",json::array({
    {{"id","appearance"},{"label","Appearance"},{"controls",std::move(controls)}}})}};
}
json brightnessControl() {
  return slider("brightness","effectBrightness",0,100,50,
    {{"type","power"},{"inputMin",0},{"inputMax",100},
      {"outputMin",0},{"outputMax",4},{"exponent",2}});
}
json allControls() {
  json uniform=slider("futureAmplitude","material.uniform",0,1,.5);
  uniform["binding"]["name"]="FutureAmplitude";
  uniform["binding"]["component"]=2;
  uniform["visibleWhen"]={{"id","enabled"},{"equals",true}};
  return schema(json::array({brightnessControl(),
    slider("size","effectScale",0,100,50,{{"type","linear"},{"inputMin",0},
      {"inputMax",100},{"outputMin",.5},{"outputMax",3}}),
    {{"id","enabled"},{"type","toggle"},{"label","Enable"},{"default",true},
      {"binding",{{"target","effectIntensity"}}}},
    {{"id","saturation"},{"type","select"},{"label","Saturation"},{"default",1},
      {"options",json::array({{{"value",0},{"label","Gray"}},
        {{"value",1},{"label","Original"}},{{"value",2.5},{"label","Vivid"}}})},
      {"binding",{{"target","effectSaturation"}}}},
    {{"id","filter"},{"type","color"},{"label","Filter"},{"default",json::array({10,20,30})},
      {"binding",{{"target","effectColor"}}}},
    {{"id","palette"},{"type","palette"},{"label","Palette"},{"maxColors",4},
      {"presets",json::array({"original","custom","ocean"})},
      {"default",{{"preset","custom"},{"colors",json::array({json::array({255,0,0}),json::array({0,255,0})})}}},
      {"binding",{{"target","effectPalette"}}}},
    slider("duration","sprite.duration",.12,3,.82),
    slider("speed","effect.speed",0,100,50,
      {{"type","piecewise"},{"points",json::array({json::array({0,.25}),json::array({50,1}),json::array({100,3})})}}),
    uniform}));
}
json variantPlaybackSchema() {
  return json::parse(R"json({
    "version": 1,
    "groups": [{
      "id": "rhythm", "label": "Rhythm", "presentation": "variant-playback",
      "playback": {
        "selection": "flowerSet", "randomize": "pickRandom", "interval": "tapsPerShell",
        "order": [8.0, 4]
      },
      "controls": [
        {"id":"flowerSet","label":"Flowers","type":"select","default":0,"options":[
          {"value":0,"label":"Combination"},
          {"value":4,"label":"Palm","toggleId":"silver"},
          {"value":8,"label":"Heart","toggleId":"ribbon"}]},
        {"id":"pickRandom","label":"Randomize","type":"toggle","default":false},
        {"id":"tapsPerShell","label":"Taps","type":"slider","default":3,"min":1,"max":100,"step":1},
        {"id":"silver","label":"Silver","type":"toggle","default":true},
        {"id":"ribbon","label":"Ribbon","type":"toggle","default":true}
      ]
    },{
      "id":"appearance","label":"Appearance","controls":[
        {"id":"size","label":"Spread","type":"slider","default":1,"min":0.5,"max":6,"step":0.05,
         "binding":{"target":"fireworks.spread"}}
      ]
    }]
  })json");
}
json localPaletteSchema(const action75::EffectRgb& rgb) {
  auto control=allControls()["groups"][0]["controls"][5];
  control.erase("presets");
  control["palettes"]=json::array({
    {{"id","same-local"},{"name","File color"},
      {"colors",json::array({json::array({rgb[0],rgb[1],rgb[2]})})}},
    {{"id","source-ink"},{"name","Original ink"},{"source",true},
      {"colors",json::array({json::array({82,106,137})})}}});
  // This is the saved custom fallback, deliberately different from the descriptor.
  control["default"]={{"preset","same-local"},
    {"colors",json::array({json::array({10,20,30})})}};
  return schema(json::array({control}));
}
fs::path install(Temporary& t,const std::string& id,const json& data) {
  const auto directory=t.root/id;
  write(directory/"settings.json",data);return directory;
}
void registerAt(PackSettingsStore& store,const std::string& id,const fs::path& directory) {
  store.registerPack(id,directory,directory/"settings.json");
  require(store.hasSchema(id),"Valid schema rejected: "+store.schemaError(id));
}
}

static int runPackSettingsTests() {
  unsigned passed=0;
  auto test=[&](const char* name,const std::function<void()>& run) {
    try {run();++passed;std::cout<<"PASS "<<name<<'\n';}
    catch(const std::exception& e) {std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';throw;}
  };
  try {
    test("dynamic schema and numeric mappings",[] {
      Temporary t;PackSettingsStore store(t.root/"state");
      registerAt(store,"dynamic",install(t,"dynamic",allControls()));
      auto current=store.current("dynamic");
      require(current.values.size()==9,"All file-declared controls must be loaded");
      auto appearance=store.evaluate(current);
      near(appearance.brightness,1,"Power mapping neutral");
      near(appearance.scale,1.75,"Linear mapping midpoint");
      near(appearance.spriteDuration,.82,"Identity mapping");
      near(appearance.speed,1,"Piecewise mapping neutral");
      for(const auto& values : json::array({json{{"brightness",0},{"size",0},{"speed",0}},
                                            json{{"brightness",100},{"size",100},{"speed",100}}})) {
        const auto evaluated=store.evaluate(store.prepare("dynamic",values));
        near(evaluated.brightness,values["brightness"]==0?0.f:4.f,"Power mapping endpoint");
        near(evaluated.scale,values["size"]==0?.5f:3.f,"Linear mapping endpoint");
        near(evaluated.speed,values["speed"]==0?.25f:3.f,"Piecewise mapping endpoint");
      }
    });
    test("toggle select color palette and future uniform",[] {
      Temporary t;PackSettingsStore store(t.root/"state");
      registerAt(store,"types",install(t,"types",allControls()));
      const auto candidate=store.prepare("types",{{"enabled",false},{"saturation",2.5},
        {"filter",json::array({255,128,0})},{"futureAmplitude",.75},
        {"palette",{{"preset","ocean"},{"colors",json::array({json::array({10,20,30})})}}}});
      const auto a=store.evaluate(candidate);
      near(a.intensity,0,"Toggle false maps to zero");near(a.saturation,2.5,"Numeric select binding");
      require(a.color==action75::EffectRgb{255,128,0},"RGB color binding");
      require(a.palette=="ocean" && a.paletteColors.size()==1,"Palette object binding");
      require(a.uniforms.size()==1 && a.uniforms[0].name=="FutureAmplitude" && a.uniforms[0].component==2,
        "Future uniform name and component are data-driven");
      near(a.uniforms[0].value,.75,"Future uniform value");
    });
    test("invalid updates never mutate current values",[] {
      Temporary t;PackSettingsStore store(t.root/"state");
      registerAt(store,"invalid",install(t,"invalid",allControls()));
      const auto original=store.current("invalid");
      const std::vector<json> bad={{{"brightness",-1}},{{"brightness",101}},{{"brightness","bright"}},
        {{"brightness",std::numeric_limits<double>::infinity()}},{{"enabled",1}},{{"saturation",1.5}},
        {{"filter",json::array({256,0,0})}},{{"filter",json::array({1.5,0,0})}},
        {{"filter",json::array({1,2})}},{{"unknown",3}},
        {{"palette",{{"preset","missing"},{"colors",json::array({json::array({1,2,3})})}}}},
        {{"palette",{{"preset","neon"},{"colors",json::array({json::array({1,2,3})})}}}},
        {{"palette",{{"preset","custom"},{"colors",json::array()}}}},
        {{"palette",{{"preset","custom"},{"colors",json::array({json::array({1,2,3})})},{"extra",true}}}},
        {{"brightness",60},{"size",999}}};
      for(const auto& patch:bad) {
        rejects([&]{store.prepare("invalid",patch);},"Invalid update accepted");
        const auto after=store.current("invalid");
        require(after.values==original.values && after.revision==original.revision,"Partial update leaked");
      }
    });
    test("wallpaper profiles remain independent and reset locally",[] {
      Temporary t;const auto state=t.root/"state";PackSettingsStore store(state);
      registerAt(store,"A",install(t,"A",schema(json::array({brightnessControl()}))));
      auto other=schema(json::array({brightnessControl()}));other["groups"][0]["controls"][0]["default"]=80;
      registerAt(store,"B",install(t,"B",other));
      store.commit(store.prepare("A",{{"brightness",73}}));
      store.commit(store.prepare("B",{{"brightness",21}}));
      require(store.current("A").values["brightness"]==73,"A polluted by B");
      require(store.current("B").values["brightness"]==21,"B save failed");
      store.commit(store.prepare("A",json::object(),{},true));
      require(store.current("A").values["brightness"]==50,"A did not reset to its default");
      require(store.current("B").values["brightness"]==21,"Reset A reset B");
      require(read(state/"wallpaper-settings/A.json")["values"]["brightness"]==50,"A profile file incorrect");
      require(read(state/"wallpaper-settings/B.json")["values"]["brightness"]==21,"B profile file incorrect");
    });
    test("revision stale update and restart persistence",[] {
      Temporary t;const auto directory=install(t,"rev",allControls());
      PackSettingsStore store(t.root/"state");registerAt(store,"rev",directory);
      const auto initial=store.current("rev");
      const auto unchanged=store.prepare("rev",{{"brightness",50}},initial.revision);
      require(unchanged.revision==initial.revision,"No-op advanced revision");
      auto changed=store.prepare("rev",{{"brightness",75}},initial.revision);store.commit(changed);
      require(changed.revision==initial.revision+1,"Changed revision did not advance");
      const auto file=t.root/"state/wallpaper-settings/rev.json";
      fs::last_write_time(file,fs::last_write_time(file)-std::chrono::hours(1));
      const auto stamp=fs::last_write_time(file);
      store.commit(store.prepare("rev",{{"brightness",75}}));
      require(fs::last_write_time(file)==stamp,"Unchanged settings rewrote the profile");
      rejects([&]{store.prepare("rev",{{"brightness",80}},initial.revision);},"Stale revision accepted");
      PackSettingsStore restarted(t.root/"state");registerAt(restarted,"rev",directory);
      const auto restored=restarted.current("rev");
      require(restored.values==changed.values && restored.revision==changed.revision && restored.stored,
        "Profile did not survive restart");
    });
    test("large native palettes survive compact saves and legacy formatted profiles",[] {
      Temporary t;const auto state=t.root/"state";
      json items=json::array(),patch=json::object();
      for(unsigned i=0;i<64;++i) {
        const auto id="palette"+std::to_string(i);
        items.push_back({{"id",id},{"type","palette"},{"label","Palette"},{"maxColors",9},
          {"presets",json::array({"custom"})},
          {"default",{{"preset","custom"},{"colors",json::array({json::array({255,128,64})})}}}});
        auto& value=patch[id];value={{"preset","custom"},{"colors",json::array()}};
        for(unsigned c=0;c<9;++c) value["colors"].push_back(json::array({255,128,64}));
      }
      const auto definition=schema(items);
      require(definition.dump().size()<32*1024,"Large-palette fixture exceeded the schema limit");
      const auto directory=install(t,"large-palettes",definition);
      PackSettingsStore store(state);store.registerPack("large-palettes",directory,directory/"settings.json","native");
      const auto updated=store.prepare("large-palettes",patch);
      store.commit(updated);
      const auto file=state/"wallpaper-settings/large-palettes.json";
      require(fs::file_size(file)<32*1024,"New profile did not use compact serialization");
      PackSettingsStore reopened(state);reopened.registerPack("large-palettes",directory,directory/"settings.json","native");
      require(reopened.current("large-palettes").values==updated.values,"Large saved palette values did not survive cold reload");
      const auto persisted=read(file);write(file,persisted);
      require(fs::file_size(file)>32*1024 && fs::file_size(file)<=64*1024,"Legacy formatting fixture did not exercise the expanded profile budget");
      std::cout<<"INFO legacy formatted profile bytes="<<fs::file_size(file)<<'\n';
      PackSettingsStore legacy(state);legacy.registerPack("large-palettes",directory,directory/"settings.json","native");
      require(legacy.current("large-palettes").values==updated.values,"Previously written formatted profile could not be recovered without resetting");
    });
    test("damaged profiles require explicit reset and retain an exact recovery backup",[] {
      Temporary t;const auto state=t.root/"state";
      const std::vector<std::string> damaged={"{",R"({"version":999,"revision":1,"values":{}})",
        R"({"version":1,"revision":0,"values":{}})",R"({"version":1,"revision":-1,"values":{}})",
        R"({"version":1,"revision":1.5,"values":{}})",std::string(64*1024+1,'x')};
      const auto bytes=[](const fs::path& file) {
        std::ifstream input(file,std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
      };
      for(std::size_t i=0;i<damaged.size();++i) {
        const auto id="damaged-"+std::to_string(i);
        const auto directory=install(t,id,schema(json::array({brightnessControl()})));
        PackSettingsStore store(state);registerAt(store,id,directory);
        const auto file=state/"wallpaper-settings"/(id+".json");
        fs::create_directories(file.parent_path());
        {std::ofstream output(file,std::ios::binary);output<<damaged[i];}
        rejects([&]{store.current(id);},"Damaged profile became a normal editable profile");
        rejects([&]{store.prepare(id,{{"brightness",75}});},"A normal patch overwrote a damaged profile");
        rejects([&]{store.prepare(id,json::object(),7,true);},"A stale explicit revision bypassed recovery validation");
        require(bytes(file)==damaged[i],"Inspecting or rejecting a bad profile changed the original");
        const auto recovery=store.prepare(id,json::object(),{},true);
        require(!recovery.recoveryBackup.empty() && bytes(recovery.recoveryBackup)==damaged[i],"Recovery did not preserve the exact damaged bytes");
        require(bytes(file)==damaged[i],"Preparing recovery replaced the profile before commit");
        store.commit(recovery);
        require(store.current(id).values["brightness"]==50,"Explicit recovery did not restore declared defaults");
        PackSettingsStore reopened(state);registerAt(reopened,id,directory);
        require(reopened.current(id).values["brightness"]==50,"Recovered profile failed cold reload");
        require(bytes(recovery.recoveryBackup)==damaged[i],"Recovery backup changed after the commit");
      }
    });
    test("recovery commit preserves a profile repaired after reset was requested",[] {
      Temporary t;const auto state=t.root/"state";
      const auto directory=install(t,"repair-race",schema(json::array({brightnessControl()})));
      PackSettingsStore store(state);registerAt(store,"repair-race",directory);
      const auto file=state/"wallpaper-settings/repair-race.json";
      fs::create_directories(file.parent_path());
      {std::ofstream output(file);output<<"{";}
      const auto recovery=store.prepare("repair-race",json::object(),{},true);
      write(file,{{"version",1},{"revision",17},{"values",{{"brightness",82}}}});
      const auto repaired=read(file);
      rejects([&]{store.commit(recovery);},"Recovery overwrote a newer repaired profile");
      require(read(file)==repaired,"Failed recovery changed the repaired file");
      PackSettingsStore reopened(state);registerAt(reopened,"repair-race",directory);
      require(reopened.current("repair-race").values["brightness"]==82,"Repaired profile was not usable");
    });
    test("unreadable profiles and invalid schemas cannot bypass reset protection",[] {
      Temporary t;const auto state=t.root/"state";
      const auto directory=install(t,"not-a-file",schema(json::array({brightnessControl()})));
      PackSettingsStore store(state);registerAt(store,"not-a-file",directory);
      const auto file=state/"wallpaper-settings/not-a-file.json";
      fs::create_directories(file);write(file/"keep.json",{{"preserved",true}});
      rejects([&]{store.prepare("not-a-file",json::object(),{},true);},"An unreadable profile bypassed recovery backup");
      require(read(file/"keep.json")["preserved"]==true,"An unreadable profile path was changed");
      auto invalid=schema(json::array({brightnessControl()}));
      invalid["groups"][0]["controls"][0]["binding"]["target"]="missing.renderer.target";
      const auto bad=install(t,"bad-schema-reset",invalid);store.registerPack("bad-schema-reset",bad,bad/"settings.json");
      rejects([&]{store.prepare("bad-schema-reset",json::object(),{},true);},"Reset bypassed schema validation");
    });
    test("legacy mapping migrates only targeted wallpaper",[] {
      Temporary t;PackSettingsStore store(t.root/"state");
      registerAt(store,"old",install(t,"old",allControls()));
      registerAt(store,"other",install(t,"other",allControls()));
      const json legacy={{"effectBrightness",4},{"effectScale",2.75},{"effectIntensity",0},
        {"effectSaturation",2.5},{"effectPalette","custom"},
        {"effectPaletteColors",json::array({json::array({40,180,220})})}};
      const auto migrated=store.migrate("old",legacy);store.commit(migrated);
      near(store.evaluate(migrated).brightness,4,"Legacy power inverse");
      near(store.evaluate(migrated).scale,2.75,"Legacy linear inverse");
      near(store.evaluate(migrated).intensity,0,"Legacy toggle inverse");
      require(store.current("other").values["brightness"]==50,"Migration polluted another wallpaper");
      const auto again=store.migrate("old",{{"effectBrightness",0}});
      require(again.values==migrated.values,"Migration replaced an existing independent profile");
      const auto patch=store.legacyPatch("old",{{"effectBrightness",1}});
      near(static_cast<float>(patch["brightness"].get<double>()),50,"Legacy patch inverse conversion");
    });
    test("schema evolves with preserved known values and backup",[] {
      Temporary t;const auto directory=install(t,"evolve",allControls());
      const auto state=t.root/"state";PackSettingsStore store(state);registerAt(store,"evolve",directory);
      auto saved=store.prepare("evolve",{{"brightness",73},{"filter",json::array({8,9,10})}});store.commit(saved);
      auto replacement=schema(json::array({brightnessControl(),slider("newLifetime","sprite.duration",.12,3,.44)}));
      write(directory/"settings.json",replacement);
      const auto evolved=store.current("evolve");
      require(evolved.values["brightness"]==73,"Schema update lost a valid setting");
      require(evolved.values["newLifetime"]==.44 && !evolved.values.contains("filter"),"Schema defaults/removal not reconciled");
      require(evolved.revision>saved.revision,"Schema update did not invalidate stale revisions");
      store.commit(evolved);
      bool backedUp=false;
      for(const auto& f:fs::directory_iterator(state/"wallpaper-settings"))
        if(f.path().filename().string().find("evolve.json.before-schema-")==0) {
          backedUp=true;require(read(f.path())["values"].contains("filter"),"Backup did not retain old settings");
        }
      require(backedUp,"Evolved schema overwrote user settings without backup");
    });
    test("schema default repair replaces only invalid values",[] {
      Temporary t;const auto directory=install(t,"repair",allControls());PackSettingsStore store(t.root/"state");
      registerAt(store,"repair",directory);store.commit(store.prepare("repair",{{"brightness",90},{"duration",2.5}}));
      auto changed=allControls();changed["groups"][0]["controls"][6]["max"]=1.5;
      const auto file=directory/"settings.json";
      const auto previousTime=fs::last_write_time(file);
      write(file,changed);
      // This edit keeps the file size, so give the schema cache a distinct timestamp.
      fs::last_write_time(file,previousTime-std::chrono::hours(1));
      const auto repaired=store.current("repair");
      require(repaired.values["brightness"]==90,"Unrelated valid value changed");
      near(static_cast<float>(repaired.values["duration"].get<double>()),.82,"Invalid old value did not use the declared default");
    });
    test("pack IDs and schema paths cannot escape",[] {
      Temporary t;PackSettingsStore store(t.root/"state");const auto directory=install(t,"safe",allControls());
      for(const auto& id:std::vector<std::string>{"../bad","A/B","CON","COM1","NUL","",std::string(81,'x')})
        rejects([&]{store.registerPack(id,directory,directory/"settings.json");},"Unsafe wallpaper ID accepted");
      require(PackSettingsStore::validPackId("valid-id_42"),"Valid wallpaper ID rejected");
      write(t.root/"outside.json",allControls());store.registerPack("escape",directory,t.root/"outside.json");
      require(!store.hasSchema("escape") && !store.schemaError("escape").empty(),"Outside schema path accepted");
      rejects([&]{store.current("escape");},"Outside schema became editable");
      registerAt(store,"safe",directory);require(store.current("safe").values.size()==9,"Bad neighbor prevented valid pack");
    });
    test("invalid schema layouts and mappings are isolated",[] {
      Temporary t;PackSettingsStore store(t.root/"state");std::vector<json> bad;
      auto item=allControls();item["version"]=2;bad.push_back(item);
      item=allControls();item["groups"][0]["controls"].push_back(item["groups"][0]["controls"][0]);bad.push_back(item);
      item=allControls();item["groups"][0]["controls"][0]["binding"]["target"]="unknown.renderer";bad.push_back(item);
      item=allControls();item["groups"][0]["controls"][0]["type"]="script";bad.push_back(item);
      item=allControls();item["groups"][0]["controls"][0]["default"]=101;bad.push_back(item);
      item=allControls();item["groups"][0]["controls"][0]["binding"]["mapping"]["exponent"]=0;bad.push_back(item);
      item=allControls();item["groups"][0]["controls"][7]["binding"]["mapping"]["points"]=json::array({json::array({50,1}),json::array({0,.25})});bad.push_back(item);
      item=allControls();item["groups"][0]["controls"][8]["binding"]["component"]=4;bad.push_back(item);
      item=allControls();item["groups"][0]["controls"][8]["visibleWhen"]={{"id","missing"},{"equals",true}};bad.push_back(item);
      for(std::size_t i=0;i<bad.size();++i) {
        const auto id="bad_"+std::to_string(i);const auto directory=install(t,id,bad[i]);
        store.registerPack(id,directory,directory/"settings.json");
        require(!store.hasSchema(id) && !store.schemaError(id).empty(),"Invalid schema accepted");
        rejects([&]{store.current(id);},"Invalid schema produced values");
      }
    });
    test("revisiting an evicted schema preserves in-memory revisions and failed profile reads",[] {
      Temporary t;PackSettingsStore store(t.root/"state");
      for(const auto* id:{"one","two"}) registerAt(store,id,install(t,id,allControls()));
      const auto draft=store.prepare("one",{{"brightness",90}});
      store.commit(draft,false);
      const auto file=t.root/"state/wallpaper-settings/one.json";
      require(!fs::exists(file),"In-memory values were written");
      // A loaded profile must not suddenly consult disk when only its schema was evicted.
      write(file,{{"version",999},{"values",json::object()}});
      (void)store.current("two");
      const auto again=store.current("one");
      require(again.schema==draft.schema && again.values==draft.values &&
              again.revision==draft.revision && !again.stored,"Cached state lost on revisit");
      write(t.root/"state/wallpaper-settings/broken.json",{{"version",999}});
      registerAt(store,"broken",install(t,"broken",allControls()));
      rejects([&]{store.current("broken");},"Malformed profile accepted");
      (void)store.current("one");
      rejects([&]{store.current("broken");},"Failed profile read incorrectly cached");
    });
    test("removing a catalog entry releases its schema without removing saved values",[] {
      Temporary t;PackSettingsStore store(t.root/"state");
      const auto dir=install(t,"removed",allControls());
      registerAt(store,"removed",dir);
      store.commit(store.prepare("removed",{{"brightness",90}}));
      store.retainPacks({});
      rejects([&]{store.current("removed");},"Removed schema still present");
      registerAt(store,"removed",dir);
      require(store.current("removed").values.at("brightness")==90,"Reinstall lost saved values");
    });
    test("publishing a catalog preserves a live change and an older active import schema",[] {
      Temporary t;
      PackSettingsStore live(t.root/"state"), scanned(t.root/"state");
      const auto stable=install(t,"stable",allControls());
      const auto active=install(t,"old-revision",allControls());
      const auto updated=install(t,"published-revision",schema(json::array({brightnessControl()})));
      registerAt(live,"stable",stable);
      registerAt(live,"active",active);registerAt(scanned,"active",updated);
      const auto patched=live.prepare("stable",{{"brightness",87}});
      live.commit(patched,false);
      const auto prior=live.current("active");
      require(PackSettingsStore::unchangedSchema(live.catalogSchemas().at("stable"),
                stable,stable/"settings.json",""),"Unchanged settings certificate missed");
      live.adoptCatalog(scanned,{"stable"},"active","");
      const auto current=live.current("stable");
      require(current.values==patched.values && current.revision==patched.revision && !current.stored,
              "Publishing scan lost newer in-memory settings");
      require(live.current("active").schema==prior.schema,
              "Publishing a pending update replaced the active imported schema");
    });
    test("file bounds malformed profiles and no-schema fallback",[] {
      Temporary t;PackSettingsStore store(t.root/"state");
      const auto huge=t.root/"huge";fs::create_directories(huge);
      {std::ofstream out(huge/"settings.json");out<<std::string(32769,' ');}
      store.registerPack("huge",huge,huge/"settings.json");require(!store.hasSchema("huge"),"Oversized schema accepted");
      const auto empty=t.root/"no-settings";fs::create_directory(empty);store.registerPack("empty",empty,{});
      const auto noSchema=store.current("empty");require(noSchema.schema.is_null() && noSchema.values.empty(),"Legacy pack should have an empty settings page");
      const auto good=install(t,"malformed",allControls());
      write(t.root/"state/wallpaper-settings/malformed.json",{{"version",999},{"values",json::object()}});
      registerAt(store,"malformed",good);rejects([&]{store.current("malformed");},"Malformed profile accepted");
    });
    test("variant playback is compiled from package-owned control IDs and order",[] {
      Temporary t;PackSettingsStore store(t.root/"state");
      const auto schema=variantPlaybackSchema();
      const auto directory=install(t,"custom-fireworks",schema);
      registerAt(store,"custom-fireworks",directory);
      const auto original=store.current("custom-fireworks");
      const auto defaults=store.evaluate(original).fireworks;
      const std::array<unsigned,action75::FireworksAppearance::shapeCount> order{8,4};
      require(defaults.shape==0 && defaults.keyInterval==3 && !defaults.shuffle &&
              defaults.selected==0b10001000 && defaults.order==order,
              "Metadata did not supply the two enabled shapes, order and interval");
      require(store.bundle(original)["schema"]==schema,"Playback metadata changed in the bundle");
      const auto changed=store.prepare("custom-fireworks",{{"flowerSet",4.0},{"pickRandom",true},
          {"tapsPerShell",5},{"ribbon",false},{"size",2.6}});
      const auto appearance=store.evaluate(changed);
      require(appearance.fireworks.shape==4 && appearance.fireworks.keyInterval==5 &&
              appearance.fireworks.shuffle && appearance.fireworks.selected==0b00001000 &&
              appearance.fireworks.order==order,"Playback controls did not reach the renderer");
      near(appearance.scale,2.6f,"Regular renderer binding stopped working alongside playback");
      require(store.legacyPatch("custom-fireworks",{{"effectScale",3.0}})["size"]==3.0,
              "Legacy appearance command failed on controls without bindings");
      for(const auto& invalid:{json(0),json(2.5),json(101)})
        rejects([&]{store.prepare("custom-fireworks",{{"tapsPerShell",invalid}});},
                "Invalid custom interval was accepted");
      rejects([&]{store.prepare("custom-fireworks",{{"flowerSet",1}});},
              "Undeclared shape was accepted");
      require(store.current("custom-fireworks").values==original.values,
              "Rejected or preview-only patch escaped to saved settings");
      store.commit(changed);
      PackSettingsStore reopened(t.root/"state");registerAt(reopened,"custom-fireworks",directory);
      require(reopened.current("custom-fireworks").values==changed.values,
              "Custom-named values did not persist");
    });
    test("legacy fireworks keep fixed selection and migrate stock IDs when a package renames them",[] {
      Temporary t;
      const auto state=t.root/"state";
      const auto file=state/"wallpaper-settings/night-fireworks.json";
      auto legacy=variantPlaybackSchema();
      auto& group=legacy["groups"][0];
      group.erase("playback");
      group["presentation"]="fireworks-playback";
      auto& controls=group["controls"];
      const std::array<const char*,5> ids{"shape","shuffle","keyInterval","usePalm","useHeart"};
      const std::array<const char*,5> bindings{
        "fireworks.shape","fireworks.shuffle","fireworks.keyInterval","fireworks.palm","fireworks.heart"};
      controls[0]["options"][1]["toggleId"]=ids[3];
      controls[0]["options"][2]["toggleId"]=ids[4];
      for(std::size_t i=0;i<ids.size();++i) {
        controls[i]["id"]=ids[i];
        controls[i]["binding"]={{"target", bindings[i]}};
      }
      const json saved={{"version",1},{"revision",8},{"values",{{"shape",8},{"shuffle",true},
          {"keyInterval",5},{"usePalm",false},{"useHeart",true},{"size",2.6}}}};
      write(file,saved);
      const auto directory=install(t,"night-fireworks",legacy);
      PackSettingsStore store(state);registerAt(store,"night-fireworks",directory);
      const auto old=store.current("night-fireworks");
      const auto oldRules=store.evaluate(old).fireworks;
      require(old.values==saved["values"] && old.revision==8 && oldRules.shape==8 &&
              oldRules.shuffle && oldRules.keyInterval==5 &&
              oldRules.order==action75::FireworksAppearance::legacyOrder,
              "Legacy package or old profile stopped working");

      write(directory/"settings.json",variantPlaybackSchema());
      const auto renamed=store.current("night-fireworks");
      require(renamed.values==json({{"flowerSet",8},{"pickRandom",true},{"tapsPerShell",5},
              {"silver",false},{"ribbon",true},{"size",2.6}}),
              "Folder update lost an old user value");
      const auto rules=store.evaluate(renamed).fireworks;
      require(rules.shape==8 && rules.selected==0b10000000 && rules.order[0]==8 &&
              rules.shuffle && rules.keyInterval==5,"Renamed values did not drive playback");
      store.commit(renamed);
      const auto backup=file.parent_path()/("night-fireworks.json.before-schema-"+
          std::to_string(renamed.revision)+".json");
      require(read(backup)==saved,"Schema migration lost the original profile backup");
    });
    test("variant playback validates references, distinct slots and integer intervals",[] {
      Temporary t;PackSettingsStore store(t.root/"state");
      const std::vector<std::function<void(json&)>> corrupt{
        [](json& group){group["playback"]["selection"]="missing";},
        [](json& group){group["playback"]["interval"]="pickRandom";},
        [](json& group){group["playback"]["order"]={8,8};},
        [](json& group){group["playback"]["order"]={4};},
        [](json& group){group["controls"][0]["options"][2]["value"]=9;},
        [](json& group){group["controls"][0]["options"][2]["value"]=1.5;},
        [](json& group){group["controls"][0]["options"][2]["toggleId"]="silver";},
        [](json& group){group["controls"][2]["min"]=0;},
        [](json& group){group["controls"][2]["default"]=1.5;},
      };
      for(std::size_t i=0;i<corrupt.size();++i) {
        auto schema=variantPlaybackSchema();corrupt[i](schema["groups"][0]);
        const auto id="invalid-playback-"+std::to_string(i);
        const auto directory=install(t,id,schema);
        store.registerPack(id,directory,directory/"settings.json");
        require(!store.hasSchema(id),"Malformed playback rule was accepted");
      }
    });
    test("reply bundle remains plain validated data",[] {
      Temporary t;PackSettingsStore store(t.root/"state");registerAt(store,"bundle",install(t,"bundle",allControls()));
      const auto candidate=store.current("bundle");const auto result=store.bundle(candidate);
      require(result["packId"]=="bundle" && result["schema"]==candidate.schema &&
        result["values"]==candidate.values && result["revision"]==candidate.revision,"Settings bundle lost protocol fields");
      require(result.dump().size()<60*1024,"Bounded schema did not fit the named-pipe reply");
    });
    test("native controls retain their author IDs and values without renderer bindings",[] {
      Temporary t;
      const auto state=t.root/"state";
      const std::string id="native-sparks";
      auto author=allControls();
      auto& controls=author["groups"][0]["controls"];
      for(auto& control:controls) control.erase("binding");
      controls[0]["binding"]={{"target","author.optionalAnnotation"}};
      controls[3]["default"]="calm";
      controls[3]["options"]=json::array({{{"value","calm"},{"label","Calm"}},
                                         {{"value","radiant"},{"label","Radiant"}}});
      controls[8]["visibleWhen"]={{"id","saturation"},{"equals","radiant"}};
      const auto directory=install(t,id,author);
      const auto file=directory/"settings.json";
      PackSettingsStore store(state);
      store.registerPack(id,directory,file,"effekseer");
      require(!store.hasSchema(id),"Unbound controls weakened a mapped schema");
      store.registerPack(id,directory,file,"native");
      require(store.hasSchema(id),store.schemaError(id));
      const auto defaults=store.current(id);
      require(defaults.authorValues && store.bundle(defaults)["schema"]==author &&
              defaults.values["saturation"]=="calm" && !defaults.playback,
              "The native schema was translated to host controls");
      const auto updated=store.prepare(id,{{"brightness",82},{"enabled",false},{"saturation","radiant"},
          {"filter",{100,150,200}},{"palette",{{"preset","ocean"},{"colors",{{5,15,25}}}}}});
      const auto result=store.bundle(updated);
      require(result["values"]==updated.values && updated.values.size()==9 &&
              updated.values["brightness"]==82 && updated.values["saturation"]=="radiant",
              "The plugin values lost author IDs or original types");
      near(store.evaluate(updated).brightness,1,"A native author binding changed host rendering");
      require(store.migrate(id,{{"effectBrightness",3}}).values==defaults.values,
              "Legacy global settings were written to a plugin");
      rejects([&]{store.prepare(id,{{"saturation","unknown"}});},"Undeclared native select option accepted");
      rejects([&]{store.prepare(id,{{"size",101}});},"Out-of-range native slider accepted");
      rejects([&]{store.prepare(id,{{"enabled",1}});},"Non-boolean native toggle accepted");
      rejects([&]{store.prepare(id,{{"filter",{1,2,300}}});},"Invalid native color accepted");
      rejects([&]{store.prepare(id,{{"unregistered",5}});},"Unregistered native control accepted");
      store.commit(updated);
      PackSettingsStore restored(state);
      restored.registerPack(id,directory,file,"native");
      require(restored.current(id).values==updated.values,"Plugin settings were changed on restart");
      require(restored.prepare(id,json::object(),updated.revision,true).values==defaults.values,
              "Native reset did not restore author defaults");
    });
    test("ordered range defaults agree on first load and reset",[] {
      Temporary t;PackSettingsStore store(t.root/"state");
      auto definition=schema(json::array({
        slider("lower","effectScale",0,2,1.8),
        slider("upper","effectBrightness",0,2,.2)}));
      definition["orderedRange"]=json::array({"lower","upper"});
      const auto directory=install(t,"ordered",definition);
      store.registerPack("ordered",directory,directory/"settings.json","native");
      const auto initial=store.current("ordered");
      require(initial.values==json{{"lower",.2},{"upper",1.8}},
              "An ordered range must be normalized before the first render");
      const auto changed=store.prepare("ordered",{{"lower",1.9}});
      require(changed.values==json{{"lower",1.9},{"upper",1.9}},
              "Moving a range endpoint should move its peer when it crosses it");
      store.commit(changed);
      require(store.prepare("ordered",json::object(),{},true).values==initial.values,
              "Reset did not restore the first-load range");
    });
    test("case-insensitive Windows profile names cannot collide",[] {
      Temporary t;PackSettingsStore store(t.root/"state");
      const auto a=install(t,"A",allControls());registerAt(store,"A",a);
      const auto other=t.root/"second-directory";write(other/"settings.json",allControls());
      rejects([&]{store.registerPack("a",other,other/"settings.json");},
        "Case-only pack IDs would share the same Windows profile file");
      store.commit(store.prepare("A",{{"brightness",72}}));
      require(store.current("A").values["brightness"]==72,"Rejected case collision mutated the original profile");
    });
    test("wallpaper-local palette descriptors resolve independently",[] {
      Temporary t;PackSettingsStore store(t.root/"state");
      const auto red=localPaletteSchema({255,0,0}),blue=localPaletteSchema({0,0,255});
      registerAt(store,"local-A",install(t,"local-A",red));
      registerAt(store,"local-B",install(t,"local-B",blue));
      const auto a=store.evaluate(store.current("local-A"));
      const auto b=store.evaluate(store.current("local-B"));
      require(a.palette=="same-local" && b.palette=="same-local","Local ID was rewritten to a global preset");
      require(a.resolvedPaletteColors==std::vector<action75::EffectRgb>{{255,0,0}} &&
        b.resolvedPaletteColors==std::vector<action75::EffectRgb>{{0,0,255}},
        "Same local palette ID leaked colors across wallpaper schemas");
      require(a.paletteOverride && b.paletteOverride && !a.sourcePalette && !b.sourcePalette,
        "Named local palettes must enable their file-defined colors");
      const auto customCandidate=store.prepare("local-A",{{"palette",{{"preset","custom"},
        {"colors",json::array({json::array({17,118,209}),json::array({142,34,106})})}}}});
      const auto custom=store.evaluate(customCandidate);
      require(custom.paletteOverride && !custom.sourcePalette &&
        custom.resolvedPaletteColors==std::vector<action75::EffectRgb>{{17,118,209},{142,34,106}},
        "Custom palette did not use its own saved RGB values");
      rejects([&]{store.prepare("local-A",{{"palette",{{"preset","original"},
        {"colors",json::array({json::array({1,2,3})})}}}});},
        "Unlisted global original palette was accepted by a local-only schema");
      const auto sourceCandidate=store.prepare("local-A",{{"palette",{{"preset","source-ink"},
        {"colors",json::array({json::array({10,20,30})})}}}});
      const auto source=store.evaluate(sourceCandidate);
      require(source.sourcePalette && !source.paletteOverride &&
        source.resolvedPaletteColors==std::vector<action75::EffectRgb>{{82,106,137}},
        "Source palette must retain swatches while leaving the original renderer colors active");
      require(store.bundle(sourceCandidate)["schema"]["groups"][0]["controls"][0]["palettes"]==
        red["groups"][0]["controls"][0]["palettes"],"Source descriptor colors were discarded from the GUI bundle");
      store.commit(customCandidate);
      require(store.evaluate(store.current("local-B")).resolvedPaletteColors==
        std::vector<action75::EffectRgb>{{0,0,255}},"Saving A changed B's local palette");
    });
    test("background controls are independent of effect parameters",[] {
      Temporary t;PackSettingsStore store(t.root/"state");
      const auto backgroundBrightness=slider("backgroundBrightness","background.brightness",0,100,50,
        {{"type","power"},{"inputMin",0},{"inputMax",100},{"outputMin",0},{"outputMax",4},{"exponent",2}});
      const auto backgroundSaturation=slider("backgroundSaturation","background.saturation",0,100,50,
        {{"type","piecewise"},{"points",json::array({json::array({0,0}),json::array({50,1}),json::array({100,2.5})})}});
      const json backgroundColor={{"id","backgroundColor"},{"type","color"},{"label","Color"},
        {"default",json::array({255,255,255})},{"binding",{{"target","background.color"}}}};
      const auto definition=schema(json::array({brightnessControl(),backgroundBrightness,backgroundSaturation,
        slider("backgroundHue","background.hue",0,360,0),backgroundColor}));
      registerAt(store,"layers-A",install(t,"layers-A",definition));
      registerAt(store,"layers-B",install(t,"layers-B",definition));
      const auto defaults=store.evaluate(store.current("layers-A"));
      near(defaults.backgroundBrightness,1,"Default background brightness must retain the original");
      near(defaults.backgroundSaturation,1,"Default background saturation must retain the original");
      near(defaults.backgroundHue,0,"Default background hue must retain the original");
      require(defaults.backgroundColor==action75::EffectRgb{255,255,255},"Default background tint must be white");
      auto changed=store.prepare("layers-A",{{"backgroundBrightness",25},{"backgroundSaturation",0},
        {"backgroundHue",120},{"backgroundColor",json::array({64,128,255})}});
      const auto appearance=store.evaluate(changed);
      near(appearance.backgroundBrightness,.25f,"Background brightness binding was not evaluated");
      near(appearance.backgroundSaturation,0,"Background saturation binding was not evaluated");
      near(appearance.backgroundHue,120,"Background hue binding was not evaluated");
      require(appearance.backgroundColor==action75::EffectRgb{64,128,255},"Background tint binding was not evaluated");
      near(appearance.brightness,defaults.brightness,"Background settings changed effect brightness");
      require(appearance.color==defaults.color && appearance.palette==defaults.palette,"Background settings changed effect colors");
      store.commit(changed);
      const auto saved=store.current("layers-A");
      for(const auto& patch:std::vector<json>{{{"backgroundBrightness",101}},{{"backgroundHue",361}},
          {{"backgroundSaturation",-1}},{{"backgroundColor",json::array({256,0,0})}}}) {
        rejects([&]{store.prepare("layers-A",patch);},"Invalid background setting accepted");
        require(store.current("layers-A").values==saved.values,"Rejected background patch changed saved settings");
      }
      near(store.evaluate(store.current("layers-B")).backgroundBrightness,1,"Background settings leaked into another wallpaper");
      PackSettingsStore reopened(t.root/"state");registerAt(reopened,"layers-A",t.root/"layers-A");
      require(reopened.current("layers-A").values==saved.values,"Independent background values did not survive reload");
    });
    test("adding background controls preserves existing effect values",[] {
      Temporary t;PackSettingsStore store(t.root/"state");
      auto definition=schema(json::array({brightnessControl()}));
      const auto directory=install(t,"evolve-background",definition);registerAt(store,"evolve-background",directory);
      store.commit(store.prepare("evolve-background",{{"brightness",73}}));
      definition["groups"][0]["controls"].push_back(slider("backgroundHue","background.hue",0,360,0));
      write(directory/"settings.json",definition);store.refresh("evolve-background");
      const auto evolved=store.current("evolve-background");
      require(evolved.values["brightness"]==73 && evolved.values["backgroundHue"]==0,"New background control reset the existing effect");
      store.commit(evolved);
      require(read(t.root/"state/wallpaper-settings/evolve-background.json")["values"]["brightness"]==73,"Schema evolution persisted a changed effect value");
    });
    test("idle controls preserve saved appearance and bind independently",[] {
      Temporary t;PackSettingsStore store(t.root/"state");
      auto definition=schema(json::array({brightnessControl()}));
      const auto directory=install(t,"idle-settings",definition);registerAt(store,"idle-settings",directory);
      store.commit(store.prepare("idle-settings",{{"brightness",73}}));
      definition["groups"][0]["controls"].push_back(slider("idleFps","runtime.idleFps",15,120,30));
      definition["groups"][0]["controls"].push_back(slider("idleDelay","runtime.idleDelay",1,15,3));
      write(directory/"settings.json",definition);store.refresh("idle-settings");
      auto migrated=store.current("idle-settings");require(migrated.values["brightness"]==73,"Idle controls changed saved brightness");
      auto candidate=store.prepare("idle-settings",{{"idleFps",15},{"idleDelay",5}});
      auto appearance=store.evaluate(candidate);require(appearance.idleFps==15,"Idle frame-rate binding failed");
      near(appearance.idleDelay,5,"Idle delay binding failed");
      rejects([&]{store.prepare("idle-settings",{{"idleFps",0}});},"Unbounded idle rate accepted");
      store.commit(candidate);PackSettingsStore reopened(t.root/"state");registerAt(reopened,"idle-settings",directory);
      require(reopened.current("idle-settings").values==candidate.values,"Idle settings did not survive reload");
    });
    test("invalid local palette descriptors are rejected at schema load",[] {
      Temporary t;PackSettingsStore store(t.root/"state");std::vector<json> bad;
      auto item=localPaletteSchema({255,0,0});
      item["groups"][0]["controls"][0]["palettes"].push_back(
        item["groups"][0]["controls"][0]["palettes"][0]);bad.push_back(item);
      item=localPaletteSchema({255,0,0});
      item["groups"][0]["controls"][0]["palettes"][0]["id"]="custom";bad.push_back(item);
      item=localPaletteSchema({255,0,0});
      item["groups"][0]["controls"][0]["palettes"][0]["colors"][0][0]=256;bad.push_back(item);
      item=localPaletteSchema({255,0,0});
      item["groups"][0]["controls"][0]["palettes"][0]["colors"][0][1]=1.5;bad.push_back(item);
      item=localPaletteSchema({255,0,0});
      item["groups"][0]["controls"][0]["palettes"][0]["source"]="true";bad.push_back(item);
      for(std::size_t i=0;i<bad.size();++i) {
        const auto id="bad-local-"+std::to_string(i);const auto directory=install(t,id,bad[i]);
        store.registerPack(id,directory,directory/"settings.json");
        require(!store.hasSchema(id) && !store.schemaError(id).empty(),"Invalid local palette schema accepted");
        rejects([&]{store.current(id);},"Invalid local palette became editable");
      }
    });
  } catch(const std::exception&) {return 1;}
  std::cout<<"PackSettings: "<<passed<<" tests passed; no UI, Raw Input, or production files touched.\n";
  return 0;
}

#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
#else
int main(int argc,char** argv) {
#endif
  if(argc==1) return runPackSettingsTests();
  const bool hasKind=argc>2 && fs::path(argv[2])=="--kind";
  const int firstFile=hasKind ? 4 : 2;
  if(argc<=firstFile || fs::path(argv[1])!="--check-settings") {
    std::cerr<<"Usage: action75_pack_settings_tests --check-settings [--kind <manifest-kind>] <settings.json>...\n";
    return 2;
  }
  try {
    const std::string kind=hasKind ? fs::path(argv[3]).u8string() : std::string{};
    if(hasKind && kind.empty()) throw std::runtime_error("Specify the wallpaper manifest kind.");
    Temporary temporary;
    PackSettingsStore store(temporary.root/"state");
    for(int i=firstFile;i<argc;++i) {
      const auto file=fs::canonical(fs::path(argv[i]));
      const std::string id="schema-check";
      store.registerPack(id,file.parent_path(),file,kind);
      if(!store.hasSchema(id))
        throw std::runtime_error(file.u8string()+": "+store.schemaError(id));
      store.evaluate(store.current(id));
      std::cout<<"PASS "<<file.u8string()<<'\n';
    }
    return 0;
  } catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';
    return 1;
  }
}
