#include "Readback12.hpp"
#include <nlohmann/json.hpp>

using nlohmann::json;

int wmain(int argc, wchar_t** argv) {
    using namespace readback;
    try {
        check(argc == 4 || argc == 5,
              "usage: visual_check [ABI 2 pack] [calibration file] [output folder] [settings file]");
        const auto pack = fs::absolute(argv[1]);
        const fs::path output(argv[3]);
        std::ifstream calibration{fs::path(argv[2])};
        const auto points = json::parse(calibration).at("points");
        fs::create_directories(output);
        constexpr UINT width = 1280, height = 480;
        Warp12 reader(pack, width, height);
        if (argc == 5) {
            std::ifstream settings{fs::path(argv[4])};
            const auto values = json::parse(settings).dump();
            const A75Event event{sizeof(event), A75_SETTINGS, width, height, 0, 0, 0, nullptr, values.c_str()};
            reader.plugin.event(event);
        }

        struct Beat { int frame; const char* key; UINT type; };
        constexpr Beat sequence[] = {
            {28,"KeyA",A75_KEY_DOWN}, {105,"KeyL",A75_KEY_DOWN}, {124,"KeyE",A75_KEY_DOWN},
            {201,"KeyE",A75_KEY_UP}, {254,"KeyL",A75_KEY_UP}, {300,"KeyA",A75_KEY_UP}
        };
        Bytes pixels;
        bool awake = true;
        for (int frame = 0; frame < (argc == 5 ? 1 : 500); ++frame) {
            for (const auto& beat : sequence) {
                if (beat.frame != frame) continue;
                const auto& p = points.at(beat.key);
                const A75Event event{sizeof(event), beat.type, width, height, frame / 60.,
                                     p.at("x").get<float>(), p.at("y").get<float>(), beat.key};
                reader.plugin.event(event);
                awake = true;
            }
            if (awake) {
                const auto delay = reader.render(frame / 60., pixels);
                check(delay >= -1, "render failed");
                awake = delay >= 0;
            }
            if (frame % 15 && frame != 20 && frame != 100 && frame != 155 && frame != 230 && frame != 499)
                continue;
            std::ofstream frameFile(output / (std::to_string(frame) + ".rgba"), std::ios::binary);
            frameFile.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
            check(bool(frameFile), "write frame");
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 2;
    }
    return 0;
}
