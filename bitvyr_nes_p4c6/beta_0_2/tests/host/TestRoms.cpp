#include "Machine.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
/** @brief Кодирует диагностическую строку JSON без изменения исходных байтов. */
std::string jsonString(const std::string& text) {
    std::ostringstream out;
    out << '"';
    for (const unsigned char ch : text) {
        if (ch == '"' || ch == '\\') out << '\\' << ch;
        else if (ch == '\n') out << "\\n";
        else if (ch == '\r') out << "\\r";
        else if (ch == '\t') out << "\\t";
        else if (ch >= 32) out << ch;
    }
    out << '"';
    return out.str();
}

/** @brief Читает ограниченный результат тестового ROM из его протокольной памяти. */
std::string message(nes::core::Machine& machine) {
    std::string result;
    for (unsigned i = 0; i < 4096; ++i) {
        const auto ch = machine.debugRead(static_cast<std::uint16_t>(0x6004 + i));
        if (!ch) break;
        if (ch == 10 || ch == 13 || ch == 9 || (ch >= 32 && ch < 127)) result += static_cast<char>(ch);
    }
    return result;
}

/**
 * @brief Выполняет внешний test-ROM до bounded maximumFrames, читая аппаратный blargg-протокол$6000.
 * @param path Входной host-ROM; до8MiB загружается в собственный vector, живущий дольше Machine.
 * @param record Необязательный заимствованный JSONL-поток; может быть nullptr.
 * @return true только при явном протокольном code0; отсутствие signature, reset request, jam/clock-limit и timeout успехом не считаются.
 * @details Visual-only тест без signature требует отдельного oracle; host_seconds не является аппаратным FPS.
 */
bool run(const std::filesystem::path& path, unsigned maximumFrames, nes::core::Region region, std::ostream* record) {
    const auto start = std::chrono::steady_clock::now();
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> rom((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    auto machine = std::make_unique<nes::core::Machine>();
    std::string status = "load_failed", text;
    int resultCode = -1;
    unsigned frames = 0;
    bool signatureSeen = false;
    if (rom.size() > 8 * 1024 * 1024 || rom.empty()) text = "ROM missing, empty or larger than 8 MiB";
    else {
        const auto loaded = machine->load(rom.data(), rom.size(), region);
        if (loaded != nes::core::LoadResult::Ok) text = machine->cartridge().errorText(loaded);
        else {
            status = "timeout";
            for (; frames < maximumFrames; ++frames) {
                const auto frameStatus = machine->stepFrame();
                const bool signature = machine->debugRead(0x6001) == 0xDE && machine->debugRead(0x6002) == 0xB0 && machine->debugRead(0x6003) == 0x61;
                signatureSeen = signatureSeen || signature;
                if (signature) {
                    const auto code = machine->debugRead(0x6000);
                    if (code < 0x80) {
                        resultCode = code;
                        status = code == 0 ? "pass" : "fail";
                        ++frames;
                        break;
                    }
                    if (code == 0x81) { status = "reset_requested"; ++frames; break; }
                }
                if (frameStatus == nes::core::RunStatus::CpuJammed) { status = "cpu_jammed"; ++frames; break; }
                if (frameStatus == nes::core::RunStatus::ClockLimit) { status = "clock_limit"; ++frames; break; }
            }
            if (signatureSeen) text = message(*machine);
            else if (text.empty()) text = "No blargg $6000 signature observed; visual-only tests need a separate oracle";
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::ostringstream json;
    json << "{\"rom\":" << jsonString(path.generic_string()) << ",\"status\":" << jsonString(status)
         << ",\"code\":" << resultCode << ",\"frames\":" << frames
         << ",\"cpu_cycles\":" << machine->cpuCycles() << ",\"host_seconds\":" << seconds
         << ",\"message\":" << jsonString(text) << '}';
    std::cout << json.str() << '\n' << std::flush;
    if (record) *record << json.str() << '\n' << std::flush;
    return status == "pass";
}
}  // namespace

/**
 * @brief Собирает заданные ROM/каталоги, применяет NTSC/PAL и лимит кадров, выполняет каждый протокольный test-ROM.
 * @return 0 только если каждый ROM вернул pass;2 означает неверный запуск/выходной файл,1 — хотя бы один непрошедший ROM.
 * @details Внешние тестовые данные не входят в собственную реализацию эмулятора; JSONL содержит отдельный результат каждого файла.
 */
int main(int argc, char** argv) {
    unsigned maximumFrames = 1800;
    nes::core::Region region = nes::core::Region::Ntsc;
    std::filesystem::path output;
    std::vector<std::filesystem::path> paths;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--frames" && i + 1 < argc) maximumFrames = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        else if (argument == "--output" && i + 1 < argc) output = argv[++i];
        else if (argument == "--pal") region = nes::core::Region::Pal;
        else {
            const std::filesystem::path path(argument);
            if (std::filesystem::is_directory(path)) {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(path))
                    if (entry.is_regular_file() && entry.path().extension() == ".nes") paths.push_back(entry.path());
            } else paths.push_back(path);
        }
    }
    if (paths.empty() || !maximumFrames || maximumFrames > 36000) {
        std::cerr << "Usage: nes_test_roms [--frames 1800] [--pal] [--output report.jsonl] ROM-or-directory ...\n";
        return 2;
    }
    std::sort(paths.begin(), paths.end());
    std::ofstream record;
    if (!output.empty()) { record.open(output); if (!record) { std::cerr << "Cannot write report\n"; return 2; } }
    unsigned passed = 0;
    for (const auto& path : paths) passed += run(path, maximumFrames, region, record.is_open() ? &record : nullptr) ? 1U : 0U;
    std::cerr << "Test ROMs: " << passed << '/' << paths.size() << " passed\n";
    return passed == paths.size() ? 0 : 1;
}
