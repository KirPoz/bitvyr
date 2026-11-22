#include "Machine.h"
#include "cartridge/Mmc5.h"
#include "cartridge/Mmc5Audio.h"
#include "Palette.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {
/**
 * @brief Полное состояние pad, применяемое перед гостевым кадром frame, начиная с нуля.
 * @details mask имеет8 бит и удерживается до следующего явного movie-события; запись не означает нажатие только изменённых кнопок.
 */
struct PadFrameEvent { unsigned frame, mask; };
/** @brief Проверяет числовую маску контроллера из текстового входа. */
bool padNumber(const char* first, const char* last, unsigned limit, unsigned& result, bool allowHex = false) noexcept {
    if (first == last) return false;
    unsigned base = 10;
    if (allowHex && last - first > 2 && first[0] == '0' && (first[1] == 'x' || first[1] == 'X')) { base = 16; first += 2; }
    else if (allowHex && last - first > 1 && first[0] == '0') return false;
    result = 0;
    for (; first != last; ++first) {
        unsigned digit = 99;
        if (*first >= '0' && *first <= '9') digit = static_cast<unsigned>(*first - '0');
        else if (*first >= 'a' && *first <= 'f') digit = static_cast<unsigned>(*first - 'a' + 10);
        else if (*first >= 'A' && *first <= 'F') digit = static_cast<unsigned>(*first - 'A' + 10);
        if (digit >= base || digit > limit || result > (limit - digit) / base) return false;
        result = result * base + digit;
    }
    return true;
}
/** @brief Разбирает привязанное к гостевому кадру изменение полного состояния pad. */
bool padFrameEvent(const char* text, PadFrameEvent& event) noexcept {
    const char* colon = std::strchr(text, ':');
    if (!colon || std::strchr(colon + 1, ':')) return false;
    return padNumber(text, colon, 360000, event.frame) &&
        padNumber(colon + 1, text + std::strlen(text), 255, event.mask, true);
}
/** @brief Считает диагностический хеш уже готового PCM, не изменяя владельца и sample-фазу. */
std::uint64_t pcmStateHash(const std::int16_t* samples, std::size_t count) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t i = 0; i < count; ++i) {
        const auto sample = static_cast<std::uint16_t>(samples[i]);
        hash = (hash ^ static_cast<std::uint8_t>(sample)) * 1099511628211ULL;
        hash = (hash ^ static_cast<std::uint8_t>(sample >> 8)) * 1099511628211ULL;
    }
    return hash;
}
/** @brief Считает хеш доступного кадрового состояния после возврата владельца. */
std::uint64_t frameStateHash(const nes::core::Machine& machine) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto* pixels = machine.framebuffer();
    for (unsigned i = 0; i < 256 * 240; ++i) hash = (hash ^ pixels[i]) * 1099511628211ULL;
    for (unsigned row = 0; row < 240; ++row) hash = (hash ^ machine.emphasis(row)) * 1099511628211ULL;
    return hash;
}
/**
 * @brief Сохраняет готовый NES-кадр как RGB PPM P6 размером256×240.
 * @param path Выходной host-файл; операция не пишет SD/ROM платы.
 * @param m Завершившая кадр const Machine; framebuffer/emphasis читаются после возврата владельца.
 * @details displayRgb применяет реальные palette/emphasis/region, а не экспортирует исходные индексы как RGB.
 */
void image(const char* path, const nes::core::Machine& m) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n256 240\n255\n";
    const auto* pixels = m.framebuffer();
    for (unsigned y = 0; y < 240; ++y) for (unsigned x = 0; x < 256; ++x) {
        const auto c = nes::core::displayRgb(pixels[y*256+x], m.emphasis(y), m.region() == nes::core::Region::Pal);
        const char b[]{char(c >> 16),char(c >> 8),char(c)};
        f.write(b, 3);
    }
}
/** @brief Сохраняет готовые PCM-отсчёты в WAV без изменения гостевого времени. */
void wave(const char* path, const std::vector<std::int16_t>& samples) {
    std::ofstream f(path, std::ios::binary);
    /**
     * @brief Записывает uint32 в WAV как четыре little-endian байта без native struct padding.
     * @details Поток f заимствован из текущего wave; отказ файла обрабатывается стандартным состоянием ofstream.
     */
    auto le32 = [&f](std::uint32_t v) { for (unsigned i = 0; i < 4; ++i) f.put(char(v >> (i*8))); };
    /**
     * @brief Записывает uint16 PCM/header слово в WAV двумя little-endian байтами.
     * @details Используется и для подписанного PCM через сохранение его16-битного представления.
     */
    auto le16 = [&f](std::uint16_t v) { f.put(char(v)); f.put(char(v >> 8)); };
    f.write("RIFF", 4); le32(36+static_cast<std::uint32_t>(samples.size()*2));
    f.write("WAVEfmt ", 8); le32(16); le16(1); le16(1); le32(48000); le32(96000); le16(2); le16(16);
    f.write("data",4); le32(static_cast<std::uint32_t>(samples.size()*2));
    for (const auto s : samples) le16(static_cast<std::uint16_t>(s));
}
}
/**
 * @brief Исполняет заданный host-ROM с bounded frames и movie-событиями pad, при запросе сохраняет PPM/WAV.
 * @details Host-таймер и диагностические hashes не являются FPS P4. ROM vector остаётся живым до завершения Machine и всех экспортов.
 */
int main(int argc, char** argv) {
    if (argc < 2) { std::fputs("nes_run ROM [--frames N] [--pal] [--autoplay | --pad-frame N:MASK ...] [--ppm file] [--wav file]\n", stderr); return 1; }
    unsigned frames = 600;
    bool pal = false, autoplay = false;
    unsigned playAt = 450;
    unsigned captureAt = 0;
    unsigned playButtons = nes::core::Right | nes::core::A | nes::core::B;
    std::vector<unsigned> starts;
    std::vector<PadFrameEvent> padFrames;
    bool legacyInputOption = false;
    const char *ppm = nullptr, *wav = nullptr;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--pad-frame")) {
            if (i + 1 >= argc || padFrames.size() == 16) return 1;
            PadFrameEvent event{};
            if (!padFrameEvent(argv[++i], event) || (!padFrames.empty() && event.frame <= padFrames.back().frame)) return 1;
            padFrames.push_back(event);
        }
        else if (!std::strcmp(argv[i], "--pal")) pal = true;
        else if (!std::strcmp(argv[i], "--autoplay")) { autoplay = true; legacyInputOption = true; }
        else if (!std::strcmp(argv[i], "--start") && i+1 < argc) { starts.push_back(static_cast<unsigned>(std::strtoul(argv[++i],nullptr,10))); autoplay=true; legacyInputOption=true; }
        else if (!std::strcmp(argv[i], "--play-at") && i+1 < argc) { playAt=static_cast<unsigned>(std::strtoul(argv[++i],nullptr,10)); legacyInputOption=true; }
        else if (!std::strcmp(argv[i], "--capture-at") && i+1 < argc) captureAt=static_cast<unsigned>(std::strtoul(argv[++i],nullptr,10));
        else if (!std::strcmp(argv[i], "--play-buttons") && i+1 < argc) { playButtons=static_cast<unsigned>(std::strtoul(argv[++i],nullptr,0)); legacyInputOption=true; }
        else if (!std::strcmp(argv[i], "--frames") && i+1 < argc) frames = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        else if (!std::strcmp(argv[i], "--ppm") && i+1 < argc) ppm = argv[++i];
        else if (!std::strcmp(argv[i], "--wav") && i+1 < argc) wav = argv[++i];
        else { std::fprintf(stderr, "Unknown argument: %s\n", argv[i]); return 1; }
    }
    if (!frames || frames > 360000 || captureAt > frames || playButtons > 255) return 1;
    if (!padFrames.empty() && (legacyInputOption || padFrames.back().mask != 0)) return 1;
    if (starts.empty()) starts.push_back(360);
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) { std::fputs("Cannot open ROM\n", stderr); return 1; }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    // Владельцы MMC5 созданы до load и живут дольше всех шагов Machine.
    std::unique_ptr<nes::core::Mmc5> mmc5;
    std::unique_ptr<nes::core::Mmc5Audio> mmc5Audio;
    auto machine = std::make_unique<nes::core::Machine>();
    if(bytes.size()>=16 && ((bytes[6]>>4)|(bytes[7]&0xf0))==5){
        mmc5=std::make_unique<nes::core::Mmc5>();mmc5Audio=std::make_unique<nes::core::Mmc5Audio>();
        machine->configureMmc5(mmc5.get(),mmc5Audio.get());
    }
    const auto loaded = machine->load(bytes.data(), bytes.size(), pal ? nes::core::Region::Pal : nes::core::Region::Ntsc);
    if (loaded != nes::core::LoadResult::Ok) {
        std::printf("{\"load\":%u,\"mapper\":%u,\"error\":\"%s\"}\n", unsigned(loaded), machine->cartridge().mapper(), machine->cartridge().errorText(loaded));
        return 3;
    }
    std::uint64_t hash = 1469598103934665603ull, samples = 0;
    std::uint64_t audioEnergy = 0;
    unsigned changes = 0, done = 0, unique = 0;
    std::uint64_t previous = 0;
    std::vector<std::int16_t> audio;
    const auto start = std::chrono::steady_clock::now();
    auto status = nes::core::RunStatus::NoCartridge;
    std::size_t padCursor = 0;
    std::uint8_t padMask = 0;
    for (unsigned n = 0; n < frames; ++n) {
        std::uint8_t buttons = 0;
        if (!padFrames.empty()) {
            // Гостевой индекс n начинается с нуля: полное состояние pad применяется ДО
            // stepFrame(n) и удерживается до следующего явного фронта ввода.
            if (padCursor < padFrames.size() && padFrames[padCursor].frame == n)
                padMask = static_cast<std::uint8_t>(padFrames[padCursor++].mask);
            buttons = padMask;
        } else if (autoplay) {
            for (const auto pulse : starts) if (n >= pulse && n < pulse+3) buttons |= nes::core::Start;
            if (n > playAt) {
                buttons |= static_cast<std::uint8_t>(playButtons & ~(nes::core::A | nes::core::B));
                if (n % 40 < 12) buttons |= static_cast<std::uint8_t>(playButtons & nes::core::A);
                if (n % 12 < 6) buttons |= static_cast<std::uint8_t>(playButtons & nes::core::B);
            }
        }
        machine->setController(0, buttons);
        status = machine->stepFrame();
        if (status != nes::core::RunStatus::FrameReady) break;
        ++done;
        if (ppm && captureAt == done) image(ppm, *machine);
        std::array<bool,64> colors{};
        std::uint64_t frameHash = 1469598103934665603ull;
        for (unsigned p = 0; p < 256*240; ++p) {
            const auto c = machine->framebuffer()[p];
            colors[c & 63] = true;
            frameHash = (frameHash ^ c) * 1099511628211ull;
        }
        unique = 0; for (bool c : colors) if (c) ++unique;
        if (n && frameHash != previous) ++changes;
        previous = frameHash; hash = (hash ^ frameHash) * 1099511628211ull;
        const auto* pcm = machine->samples();
        samples += machine->sampleCount();
        for (std::size_t i = 0; i < machine->sampleCount(); ++i) audioEnergy += std::uint64_t(std::int64_t(pcm[i])*pcm[i]);
        if (wav) audio.insert(audio.end(), pcm, pcm+machine->sampleCount());
    }
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    if (ppm && (!captureAt || done < captureAt)) image(ppm, *machine);
    if (wav) wave(wav, audio);
    const auto finalFrameHash = frameStateHash(*machine);
    const auto finalPcmCount = machine->sampleCount();
    const auto finalPcmHash = pcmStateHash(machine->samples(), finalPcmCount);
    std::printf("{\"frames\":%u,\"requested\":%u,\"mapper\":%u,\"pal\":%s,\"status\":%u,\"pc\":%u,\"cpu_cycles\":%llu,\"video_hash\":\"%016llx\",\"frame_hash\":\"%016llx\",\"pcm_count\":%u,\"pcm_hash\":\"%016llx\",\"changed_frames\":%u,\"last_colors\":%u,\"samples\":%llu,\"audio_energy\":%llu,\"host_seconds\":%.3f,\"host_fps\":%.1f}\n",
        done,frames,machine->cartridge().mapper(),pal?"true":"false",unsigned(status),machine->cpuState().pc,
        static_cast<unsigned long long>(machine->cpuCycles()),static_cast<unsigned long long>(hash),
        static_cast<unsigned long long>(finalFrameHash),unsigned(finalPcmCount),
        static_cast<unsigned long long>(finalPcmHash),changes,unique,
        static_cast<unsigned long long>(samples),static_cast<unsigned long long>(audioEnergy),seconds,done/seconds);
    return done == frames ? 0 : 2;
}
