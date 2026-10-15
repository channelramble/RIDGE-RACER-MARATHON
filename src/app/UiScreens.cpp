#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <ctime>
#include <mutex>

#include "app/Ui.h"
#include "core/Platform.h"

namespace rrm::ui {

namespace {

const ImU32 kYellow = IM_COL32(245, 196, 0, 255);
const ImU32 kGreen = IM_COL32(52, 211, 120, 255);
const ImU32 kAmber = IM_COL32(255, 166, 0, 255);
const ImU32 kBlue = IM_COL32(80, 160, 255, 255);
const ImU32 kGrey = IM_COL32(110, 118, 136, 255);
const ImU32 kRed = IM_COL32(255, 80, 80, 255);

ImU32 statusColor(App::GameStatus s) {
    switch (s) {
        case App::GameStatus::Ready: return kGreen;
        case App::GameStatus::Running: return kYellow;
        case App::GameStatus::NeedsFiles: return kAmber;
        case App::GameStatus::NeedsBackend: return kBlue;
        case App::GameStatus::NeedsSystemFiles: return kAmber;
        case App::GameStatus::Unsupported: return kGrey;
    }
    return kGrey;
}

ImU32 withAlpha(ImU32 c, int a) { return (c & 0x00FFFFFF) | ((ImU32)a << 24); }

const char* osName(Os os) {
    switch (os) {
        case Os::MacOS: return "macOS";
        case Os::Windows: return "Windows";
        case Os::Linux: return "Linux";
        case Os::Android: return "Android";
        case Os::iOS: return "iOS";
    }
    return "?";
}

std::string installStateText(App& app, Backend* b) {
    switch (b->installState(app.library)) {
        case InstallState::Installed: {
            const BackendInstall* bi = app.library.install(b->def().id);
            return "Installed" + (bi && !bi->version.empty() && bi->version != "direct" ? " (" + bi->version + ")" : std::string());
        }
        case InstallState::UserProvided: return "Linked to your copy";
        case InstallState::NotInstalled: return "Not installed";
        case InstallState::Unavailable: return "Not available here";
    }
    return "";
}

// Install / link / remove controls for one backend. Returns true if a job is running.
void backendActions(App& app, Backend* b, const char* idSuffix) {
    ImGui::PushID(b->def().id.c_str());
    ImGui::PushID(idSuffix);
    Job* job = app.job("install:" + b->def().id);
    bool busy = job && !job->finished;
    auto st = b->installState(app.library);
    const BackendSource* src = b->def().sourceFor(currentPlatformKey());
    bool canDownload = b->def().kind == "libretro" ? isDesktop() : (src && src->kind != "user" && src->kind != "external-app");
    if (busy) {
        progressLine(*job->progress, S(260));
        ImGui::SameLine();
        if (secondaryButton("Cancel")) job->progress->cancel = true;
    } else {
        if (st == InstallState::NotInstalled && canDownload) {
            if (secondaryButton("Install")) app.installBackend(b->def().id);
            ImGui::SameLine();
        } else if (st == InstallState::Installed && canDownload) {
            if (secondaryButton("Update")) app.installBackend(b->def().id);
            ImGui::SameLine();
        }
        if (isDesktop() && b->def().kind != "libretro" && st != InstallState::Unavailable) {
            if (secondaryButton(st == InstallState::UserProvided ? "Relink..." : "Link existing...")) app.pickFolder("link:" + b->def().id);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Use a copy you already have (choose its folder).");
            ImGui::SameLine();
        }
        if (st == InstallState::Installed || st == InstallState::UserProvided) {
            if (secondaryButton("Remove")) b->uninstall(app.library);
            ImGui::SameLine();
        }
        if (!b->def().homepage.empty() && secondaryButton("Website")) openExternal(b->def().homepage);
    }
    ImGui::PopID();
    ImGui::PopID();
}

void systemFileRow(App& app, const char* key, const char* label, const char* help,
                   const std::vector<std::pair<std::string, std::string>>& filters) {
    ImGui::PushID(key);
    std::string path = app.library.systemFile(key);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(S(190));
    bool optional = std::string(key) == "ps1-bios";
    if (path.empty()) {
        if (optional) chip("not set", withAlpha(kGrey, 48), kGrey);
        else chip("missing", withAlpha(kAmber, 48), kAmber);
    } else {
        chip("ready", withAlpha(kGreen, 48), kGreen);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", ellipsizePath(path, ImGui::GetContentRegionAvail().x - S(200)).c_str());
    }
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - S(190)));
    if (secondaryButton("Choose file...")) app.pickFile(std::string("system:") + key, filters);
    if (!path.empty()) {
        ImGui::SameLine();
        if (secondaryButton("Clear")) {
            app.library.setSystemFile(key, "");
            app.library.save();
        }
    }
    if (help) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", help);
        ImGui::PopTextWrapPos();
    }
    ImGui::PopID();
}

}  // namespace

// ---- top bar ------------------------------------------------------------------------------------

void drawTopBar(App& app) {
    float h = S(64);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(13, 16, 24, 255));
    dl->AddRectFilledMultiColor(ImVec2(p.x, p.y + h - S(3)), ImVec2(p.x + w, p.y + h), kYellow, IM_COL32(255, 59, 48, 255),
                                IM_COL32(255, 59, 48, 255), kYellow);
    // Logo
    ImFont* f = app.fonts.display;
    float fs = S(30);
    dl->AddText(f, fs, ImVec2(p.x + S(24), p.y + (h - fs) * 0.5f - S(2)), kYellow, "RIDGE RACER");
    float lw = f->CalcTextSizeA(fs, FLT_MAX, 0, "RIDGE RACER ").x;
    dl->AddText(f, fs, ImVec2(p.x + S(24) + lw, p.y + (h - fs) * 0.5f - S(2)), IM_COL32(255, 255, 255, 235), "MARATHON");

    struct Tab {
        const char* label;
        Screen screen;
    } tabs[] = {{"LIBRARY", Screen::Library},
                {"ADD GAMES", Screen::AddGames},
                {"BACKENDS", Screen::Backends},
                {"SETTINGS", Screen::Settings},
                {"ABOUT", Screen::About}};
    float tabFs = S(19);
    float x = p.x + w - S(20);
    for (int i = (int)(sizeof(tabs) / sizeof(tabs[0])) - 1; i >= 0; --i) {
        float tw = f->CalcTextSizeA(tabFs, FLT_MAX, 0, tabs[i].label).x + S(28);
        x -= tw;
        ImGui::SetCursorScreenPos(ImVec2(x, p.y + S(12)));
        ImGui::PushID(i);
        bool active = app.screen == tabs[i].screen || (tabs[i].screen == Screen::Library && app.screen == Screen::Game);
        if (ImGui::InvisibleButton("tab", ImVec2(tw, h - S(24)))) app.screen = tabs[i].screen;
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        ImU32 col = active ? kYellow : (hov ? IM_COL32(255, 255, 255, 255) : IM_COL32(170, 178, 196, 255));
        dl->AddText(f, tabFs, ImVec2(x + S(14), p.y + (h - tabFs) * 0.5f - S(1)), col, tabs[i].label);
        if (active) dl->AddRectFilled(ImVec2(x + S(14), p.y + h - S(12)), ImVec2(x + tw - S(14), p.y + h - S(9)), kYellow, 2);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::Dummy(ImVec2(w, 0));
}

// ---- library ---------------------------------------------------------------------------------------

void drawLibrary(App& app) {
    int ready = 0, total = 0;
    for (auto& g : app.catalog.games()) {
        auto st = app.statusOf(g);
        if (st == App::GameStatus::Unsupported && !app.global.showUnsupported) continue;
        ++total;
        if (st == App::GameStatus::Ready || st == App::GameStatus::Running) ++ready;
    }
    titleText(app, "Your Ridge Racer garage", S(34), IM_COL32(255, 255, 255, 255));
    ImGui::TextDisabled("%d games, %d ready to race on this %s. Marathon uses native recompilations where they exist and "
                        "emulation everywhere else. Bring your own game files.",
                        total, ready, osName(currentOs()));
    ImGui::Dummy(ImVec2(0, S(6)));

    float avail = ImGui::GetContentRegionAvail().x;
    float minCard = app.touchUi ? S(300) : S(340);
    int cols = std::max(1, (int)((avail + S(18)) / (minCard + S(18))));
    float cardW = (avail - S(18) * (cols - 1)) / cols;
    float cardH = cardW * 0.56f;
    int i = 0;
    for (auto& g : app.catalog.games()) {
        auto st = app.statusOf(g);
        if (st == App::GameStatus::Unsupported && !app.global.showUnsupported) continue;
        if (i % cols != 0) ImGui::SameLine(0, S(18));
        ImGui::PushID(g.id.c_str());
        ImVec2 a = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::InvisibleButton("card", ImVec2(cardW, cardH));
        bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        if (clicked) {
            app.selectedGame = g.id;
            app.screen = Screen::Game;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && st == App::GameStatus::Ready)
            app.play(g.id);
        ImVec2 b(a.x + cardW, a.y + cardH);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float r = S(14);
        dl->AddRectFilled(a, b, IM_COL32(16, 20, 30, 255), r);
        drawCardArt(dl, a, b, g, r, hov);
        float pad = S(18);
        ImFont* f = app.fonts.display;
        // Title (wrap long titles)
        float titleSize = cardW > S(380) ? S(38) : S(32);
        dl->AddText(f, titleSize, ImVec2(a.x + pad, a.y + pad), IM_COL32(255, 255, 255, 255), g.title.c_str(), nullptr,
                    cardW - pad * 2);
        char sub[128];
        snprintf(sub, sizeof(sub), "%s  |  %d  |  %s", g.systemLabel.c_str(), g.year, g.region.c_str());
        float titleH = f->CalcTextSizeA(titleSize, FLT_MAX, cardW - pad * 2, g.title.c_str()).y;
        dl->AddText(app.fonts.body, S(17), ImVec2(a.x + pad, a.y + pad + titleH + S(2)), IM_COL32(220, 226, 238, 210), sub);
        // Status chip
        std::string status = app.statusText(g);
        ImVec2 ts = app.fonts.bodyBold->CalcTextSizeA(S(16), FLT_MAX, 0, status.c_str());
        ImVec2 cp(a.x + pad, b.y - pad - ts.y - S(6));
        ImU32 sc = statusColor(st);
        dl->AddRectFilled(cp, ImVec2(cp.x + ts.x + S(20), cp.y + ts.y + S(6)), withAlpha(sc, 46), (ts.y + S(6)) * 0.5f);
        dl->AddCircleFilled(ImVec2(cp.x + S(9), cp.y + (ts.y + S(6)) * 0.5f), S(3.5f), sc);
        dl->AddText(app.fonts.bodyBold, S(16), ImVec2(cp.x + S(16), cp.y + S(3)), sc, status.c_str());
        // Backend name
        if (const GameBackendOption* o = app.chosenOption(g)) {
            if (const BackendDef* bd = app.catalog.backend(o->backend)) {
                std::string bn = bd->name;
                ImVec2 bs = app.fonts.body->CalcTextSizeA(S(14), FLT_MAX, 0, bn.c_str());
                if (bs.x < cardW * 0.55f)
                    dl->AddText(app.fonts.body, S(14), ImVec2(b.x - pad - bs.x, b.y - pad - bs.y - S(3)), IM_COL32(200, 206, 220, 170),
                                bn.c_str());
            }
        }
        ImGui::PopID();
        ++i;
    }
    ImGui::Dummy(ImVec2(0, S(12)));
}

// ---- game page ----------------------------------------------------------------------------------------

void drawGame(App& app) {
    const GameDef* gp = app.catalog.game(app.selectedGame);
    if (!gp) {
        app.screen = Screen::Library;
        return;
    }
    const GameDef& g = *gp;
    if (secondaryButton("< Library")) app.screen = Screen::Library;

    // Hero banner
    float w = ImGui::GetContentRegionAvail().x;
    float heroH = S(190);
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImVec2 b(a.x + w, a.y + heroH);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, IM_COL32(16, 20, 30, 255), S(16));
    drawCardArt(dl, a, b, g, S(16), false);
    dl->AddText(app.fonts.display, S(60), ImVec2(a.x + S(28), a.y + S(22)), IM_COL32(255, 255, 255, 255), g.title.c_str());
    char sub[160];
    snprintf(sub, sizeof(sub), "%s  |  %d  |  %s  |  %s", g.systemLabel.c_str(), g.year, g.region.c_str(), g.serial.c_str());
    dl->AddText(app.fonts.bodyBold, S(18), ImVec2(a.x + S(30), a.y + S(90)), IM_COL32(230, 234, 244, 230), sub);
    dl->AddText(app.fonts.body, S(17), ImVec2(a.x + S(30), a.y + S(118)), IM_COL32(220, 226, 238, 190), g.blurb.c_str(), nullptr,
                w * 0.62f);
    ImGui::Dummy(ImVec2(w, heroH + S(8)));

    auto status = app.statusOf(g);
    const GameBackendOption* chosen = app.chosenOption(g);
    Backend* backend = chosen ? app.backend(chosen->backend) : nullptr;
    LibraryEntry& entry = app.library.entry(g.id);

    // Play row
    {
        float bw = S(260), bh = S(58);
        bool canPlay = status == App::GameStatus::Ready;
        bool running = status == App::GameStatus::Running;
        if (running) {
            if (primaryButton("STOP", ImVec2(bw, bh), kRed)) {
                if (app.playingInProcess()) app.exitPlayer();
                else app.stopRunning();
            }
        } else if (primaryButton("RACE!", ImVec2(bw, bh), hexColor(g.accent), canPlay)) {
            app.play(g.id);
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0, S(2)));
        chip(app.statusText(g).c_str(), withAlpha(statusColor(status), 46), statusColor(status));
        if (backend) ImGui::TextDisabled("Runs with %s", backend->def().name.c_str());
        ImGui::EndGroup();
        if (entry.playSeconds > 0 || entry.lastPlayed > 0) {
            ImGui::SameLine(0, S(30));
            ImGui::BeginGroup();
            ImGui::Dummy(ImVec2(0, S(2)));
            ImGui::TextDisabled("Played %s", formatDuration(entry.playSeconds).c_str());
            if (entry.lastPlayed > 0) {
                std::time_t t = (std::time_t)entry.lastPlayed;
                char buf[64];
                std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&t));
                ImGui::TextDisabled("Last raced %s", buf);
            }
            ImGui::EndGroup();
        }
    }
    ImGui::Dummy(ImVec2(0, S(6)));

    float colW = (w - S(20)) * 0.5f;
    if (w < S(900)) colW = w;  // single column on narrow screens

    // ---- left column ----
    ImGui::BeginGroup();
    ImGui::PushItemWidth(colW);
    beginPanel("files", ImVec2(colW, 0));
    sectionHeader(app, "YOUR FILES");
    if (entry.hasFiles()) {
        ImGui::TextWrapped("%s", entry.files.path.c_str());
        ImGui::TextDisabled("Format: %s   Detected by: %s", entry.files.format.c_str(), entry.files.detectedBy.c_str());
        if (entry.files.verified.rfind("match", 0) == 0) chip("Verified good dump", withAlpha(kGreen, 46), kGreen);
        else if (entry.files.verified == "mismatch") {
            chip("Differs from the known dump", withAlpha(kAmber, 46), kAmber);
            ImGui::SameLine();
            helpMarker("Your dump doesn't match the reference hashes. It may still work in emulators, but recompilations "
                       "that are generated from the disc usually need the exact Redump dump.");
        } else if (entry.files.verified == "partial") chip("Incomplete set", withAlpha(kAmber, 46), kAmber);
        else chip("Not verified yet", withAlpha(kGrey, 46), kGrey);
    } else {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", g.fileHelp.c_str());
        ImGui::PopTextWrapPos();
    }
    Job* vjob = app.job("verify:" + g.id);
    if (vjob && !vjob->finished) {
        progressLine(*vjob->progress, colW - S(60));
    } else {
        if (secondaryButton("Choose file...")) {
            std::vector<std::pair<std::string, std::string>> filters;
            std::string exts;
            for (auto& f : g.acceptedFormats)
                if (f != "folder" && f != "ps3-folder" && f != "xex-folder") exts += (exts.empty() ? "" : ";") + f;
            if (!exts.empty()) filters.push_back({g.title + " files", exts});
            app.pickFile("file:" + g.id, filters);
        }
        ImGui::SameLine();
        if (secondaryButton("Scan a folder...")) app.pickFolder("scan-folder");
        if (entry.hasFiles()) {
            ImGui::SameLine();
            if (secondaryButton("Verify")) app.verifyGame(g.id);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hash your files and compare them with the known good dump.");
        }
    }
    endPanel();

    // System files
    bool needsPs2 = std::find(g.requirements.begin(), g.requirements.end(), "bios:ps2") != g.requirements.end();
    bool needsPs3 = std::find(g.requirements.begin(), g.requirements.end(), "firmware:ps3") != g.requirements.end();
    if (needsPs2 || needsPs3) {
        ImGui::Dummy(ImVec2(0, S(4)));
        beginPanel("system", ImVec2(colW, 0));
        sectionHeader(app, "SYSTEM FILES");
        if (needsPs2)
            systemFileRow(app, "ps2-bios", "PS2 BIOS", "Needed by PCSX2 and ARMSX2 (not by the Fukami recomp). Dump it from your own console.",
                          {{"PS2 BIOS", "bin;rom;rom0"}});
        if (needsPs3)
            systemFileRow(app, "ps3-firmware", "PS3 firmware",
                          "RPCS3 needs PS3UPDAT.PUP, the official system software from playstation.com.", {{"PS3 firmware", "PUP"}});
        endPanel();
    }

    // Backend options
    ImGui::Dummy(ImVec2(0, S(4)));
    beginPanel("backends", ImVec2(colW, 0));
    sectionHeader(app, "HOW IT RUNS");
    auto options = app.catalog.optionsFor(g, currentOs());
    GameSettings s = app.settingsFor(g.id);
    if (options.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAmber), "No backend can run %s on %s yet.", g.title.c_str(),
                           osName(currentOs()));
        ImGui::PopTextWrapPos();
    }
    {
        bool autoSel = s.backend == "auto";
        if (ImGui::RadioButton("Automatic (best available)", autoSel) && !autoSel) {
            s.backend = "auto";
            LibraryEntry& e = app.library.entry(g.id);
            e.settings.backend = "auto";
            app.library.save();
        }
    }
    for (auto* o : options) {
        Backend* b = app.backend(o->backend);
        if (!b) continue;
        ImGui::PushID(o->backend.c_str());
        ImGui::Separator();
        bool sel = s.backend == o->backend;
        if (ImGui::RadioButton(b->def().name.c_str(), sel) && !sel) {
            LibraryEntry& e = app.library.entry(g.id);
            e.settings.backend = o->backend;
            app.library.save();
        }
        ImGui::SameLine();
        ImU32 tagCol = o->status == "recommended" ? kGreen : o->status == "experimental" ? kAmber : kBlue;
        chip(o->status.c_str(), withAlpha(tagCol, 40), tagCol);
        if (chosen == o) {
            ImGui::SameLine();
            chip("in use", withAlpha(kYellow, 40), kYellow);
        }
        ImGui::TextDisabled("%s", installStateText(app, b).c_str());
        if (!o->note.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(o->note.c_str());
            ImGui::PopTextWrapPos();
        }
        backendActions(app, b, "game");
        ImGui::NewLine();
        ImGui::PopID();
    }
    // Other platforms, so users know what exists elsewhere.
    std::string elsewhere;
    for (auto& o : g.backends) {
        bool here = std::find(o.os.begin(), o.os.end(), currentOs()) != o.os.end();
        if (here) continue;
        const BackendDef* bd = app.catalog.backend(o.backend);
        if (!bd) continue;
        std::string oses;
        for (Os os : o.os) oses += (oses.empty() ? "" : ", ") + std::string(osName(os));
        elsewhere += "- " + bd->name + " (" + oses + ")\n";
    }
    if (!elsewhere.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Also available on other platforms:");
        ImGui::TextDisabled("%s", elsewhere.c_str());
    }
    endPanel();
    ImGui::PopItemWidth();
    ImGui::EndGroup();

    // ---- right column ----
    if (colW < w) ImGui::SameLine(0, S(20));
    ImGui::BeginGroup();
    beginPanel("settings", ImVec2(colW, 0));
    sectionHeader(app, "ENHANCEMENTS");
    bool custom = entry.customSettings;
    if (ImGui::Checkbox("Custom settings for this game", &custom)) {
        GameSettings cur = app.settingsFor(g.id);
        app.setGameSettings(g.id, cur, custom);
    }
    if (!custom) ImGui::TextDisabled("Using your global defaults (Settings tab). Tick the box to tune this game.");
    json caps = backend ? backend->capabilities(chosen) : json::object();
    GameSettings edit = app.settingsFor(g.id);
    ImGui::BeginDisabled(!custom);
    if (settingsEditor(app, edit, &caps, backend ? backend->def().name : std::string("this backend")))
        app.setGameSettings(g.id, edit, true);
    ImGui::EndDisabled();
    endPanel();
    ImGui::EndGroup();
}

// ---- add games ------------------------------------------------------------------------------------------

void drawAddGames(App& app) {
    titleText(app, "Add your games", S(34), IM_COL32(255, 255, 255, 255));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Marathon never downloads games. Point it at your own dumps: CUE/BIN, ISO, CHD, MAME romsets (folder or "
                        "zip), decrypted PS3 ISOs or game folders and Xbox 360 ISOs. Files are recognised by their contents, not "
                        "their names.");
    ImGui::PopTextWrapPos();

    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 a = ImGui::GetCursorScreenPos();
    float h = S(120);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRect(a, ImVec2(a.x + w, a.y + h), IM_COL32(245, 196, 0, 110), S(16), 0, 2.0f);
    const char* dropText = app.touchUi ? "Put your game files in Marathon's folder, then tap Scan" : "Drop game files or folders here";
    ImVec2 ts = app.fonts.display->CalcTextSizeA(S(28), FLT_MAX, 0, dropText);
    dl->AddText(app.fonts.display, S(28), ImVec2(a.x + (w - ts.x) * 0.5f, a.y + S(24)), IM_COL32(255, 255, 255, 220), dropText);
    ImGui::SetCursorScreenPos(ImVec2(a.x + w * 0.5f - S(250), a.y + S(68)));
    if (secondaryButton("Scan a folder...", ImVec2(S(240), 0))) app.pickFolder("scan-folder");
    ImGui::SameLine();
    if (app.touchUi) {
        if (secondaryButton("Scan Marathon's folder", ImVec2(S(240), 0))) app.scanFolder(pathToUtf8(paths().userFiles));
    } else if (secondaryButton("Rescan known folders", ImVec2(S(240), 0), !app.library.scanFolders.empty())) {
        for (auto& f : app.library.scanFolders) app.scanFolder(f);
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h + S(10)));
    if (app.touchUi) {
        ImGui::TextDisabled("Marathon's folder: %s", pathToUtf8(paths().userFiles).c_str());
        ImGui::TextDisabled(currentOs() == Os::iOS ? "Open the Files app > On My iPhone > Ridge Racer Marathon > Games."
                                                   : "Copy files there over USB (Android/data/.../files/Games) or pick another folder.");
    }

    Job* scan = app.job("scan");
    if (scan && !scan->finished) progressLine(*scan->progress, w);

    std::vector<ScanHit> hits;
    {
        std::lock_guard<std::mutex> lock(app.scanMutex);
        hits = app.lastScanHits;
    }
    if (!hits.empty()) {
        ImGui::Dummy(ImVec2(0, S(6)));
        sectionHeader(app, "FOUND");
        if (secondaryButton("Use the first match for every game")) {
            std::vector<std::string> done;
            for (auto& h2 : hits) {
                if (h2.gameId.empty() || std::find(done.begin(), done.end(), h2.gameId) != done.end()) continue;
                app.acceptHit(h2);
                done.push_back(h2.gameId);
            }
            app.toast("Added " + std::to_string(done.size()) + " games");
        }
        if (ImGui::BeginTable("hits", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("Detected by", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(150));
            ImGui::TableHeadersRow();
            int row = 0;
            for (auto& h2 : hits) {
                const GameDef* g = app.catalog.game(h2.gameId);
                if (!g) continue;
                ImGui::PushID(row++);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(g->title.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(ellipsizePath(h2.files.path, ImGui::GetContentRegionAvail().x).c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", h2.files.path.c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", h2.files.detectedBy.c_str());
                ImGui::TableNextColumn();
                const LibraryEntry* e = app.library.find(h2.gameId);
                if (e && e->files.path == h2.files.path) chip("in library", withAlpha(kGreen, 46), kGreen);
                else if (secondaryButton(e && e->hasFiles() ? "Use this copy" : "Add")) app.acceptHit(h2);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    std::vector<std::pair<std::string, std::string>> installs;
    {
        std::lock_guard<std::mutex> lock(app.scanMutex);
        installs = app.foundInstalls;
    }
    if (!installs.empty()) {
        ImGui::Dummy(ImVec2(0, S(6)));
        sectionHeader(app, "EXISTING BUILDS FOUND");
        ImGui::TextDisabled("Recompilations or ports you already have. Linking one makes Marathon use it as-is.");
        int row = 0;
        for (auto& [id, path] : installs) {
            Backend* b = app.backend(id);
            if (!b) continue;
            ImGui::PushID(row++);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(b->def().name.c_str());
            ImGui::SameLine(S(330));
            ImGui::TextDisabled("%s", ellipsizePath(path, ImGui::GetContentRegionAvail().x - S(170)).c_str());
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - S(150)));
            const BackendInstall* bi = app.library.install(id);
            if (bi && bi->executable == path) chip("linked", withAlpha(kGreen, 46), kGreen);
            else if (secondaryButton("Link")) {
                std::string error;
                if (b->linkExisting(app.library, path, error)) app.toast(b->def().name + " linked");
                else app.toast(error, true);
            }
            ImGui::PopID();
        }
    }

    ImGui::Dummy(ImVec2(0, S(10)));
    beginPanel("sysfiles", ImVec2(w, 0));
    sectionHeader(app, "SYSTEM FILES");
    ImGui::TextDisabled("Only some backends need these. They come from your own hardware or the console maker.");
    systemFileRow(app, "ps2-bios", "PS2 BIOS", "Used by PCSX2 and ARMSX2 for Ridge Racer V (Fukami needs none).",
                  {{"PS2 BIOS", "bin;rom;rom0"}});
    systemFileRow(app, "ps3-firmware", "PS3 firmware", "PS3UPDAT.PUP from playstation.com, installed into RPCS3 on first use.",
                  {{"PS3 firmware", "PUP"}});
    systemFileRow(app, "ps1-bios", "PS1 BIOS (optional)",
                  "PS1 cores fall back to the open-source OpenBIOS when no BIOS is given; a real BIOS improves compatibility.",
                  {{"PS1 BIOS", "bin"}});
    endPanel();
}

// ---- backends ---------------------------------------------------------------------------------------------

void drawBackends(App& app) {
    titleText(app, "Backends", S(34), IM_COL32(255, 255, 255, 255));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Recompilations, native ports and emulators that Marathon drives. They're downloaded from their official "
                        "sources on request and keep their own licences. Nothing here contains game data.");
    ImGui::PopTextWrapPos();
    struct Group {
        const char* title;
        std::vector<std::string> kinds;
    } groups[] = {{"NATIVE RECOMPILATIONS & PORTS", {"recomp", "port"}},
                  {"BUILT-IN CORES (run inside Marathon)", {"libretro"}},
                  {"EMULATORS", {"external"}},
                  {"OTHER APPS", {"android-app"}}};
    float w = ImGui::GetContentRegionAvail().x;
    for (auto& grp : groups) {
        bool header = false;
        for (auto& def : app.catalog.backends()) {
            if (std::find(grp.kinds.begin(), grp.kinds.end(), def.kind) == grp.kinds.end()) continue;
            Backend* b = app.backend(def.id);
            if (!b) continue;
            if (!b->availableHere() && !app.global.showUnsupported) continue;
            if (!header) {
                ImGui::Dummy(ImVec2(0, S(6)));
                sectionHeader(app, grp.title);
                header = true;
            }
            ImGui::PushID(def.id.c_str());
            beginPanel("b", ImVec2(w, 0));
            ImGui::PushFont(app.fonts.bodyBold, 0.0f);
            ImGui::TextUnformatted(def.name.c_str());
            ImGui::PopFont();
            ImGui::SameLine();
            auto st = b->installState(app.library);
            ImU32 c = st == InstallState::Installed || st == InstallState::UserProvided ? kGreen
                      : st == InstallState::Unavailable                                ? kGrey
                                                                                       : kBlue;
            chip(installStateText(app, b).c_str(), withAlpha(c, 40), c);
            // Games using it
            std::string used;
            for (auto& g : app.catalog.games())
                for (auto& o : g.backends)
                    if (o.backend == def.id) {
                        used += (used.empty() ? "" : ", ") + g.title;
                        break;
                    }
            if (!def.description.empty()) {
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(def.description.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::TextDisabled("Used by: %s   |   Licence: %s", used.c_str(), def.license.empty() ? "see website" : def.license.c_str());
            if (const BackendSource* src = def.sourceFor(currentPlatformKey()); src && !src->note.empty()) ImGui::TextDisabled("%s", src->note.c_str());
            if (b->availableHere()) backendActions(app, b, "list");
            else ImGui::TextDisabled("Not available on %s.", osName(currentOs()));
            endPanel();
            ImGui::PopID();
        }
    }
}

// ---- settings ---------------------------------------------------------------------------------------------------

void drawSettings(App& app) {
    titleText(app, "Settings", S(34), IM_COL32(255, 255, 255, 255));
    float w = ImGui::GetContentRegionAvail().x;
    float colW = w < S(900) ? w : (w - S(20)) * 0.5f;
    ImGui::BeginGroup();
    beginPanel("defaults", ImVec2(colW, 0));
    sectionHeader(app, "DEFAULT ENHANCEMENTS");
    ImGui::TextDisabled("Applied to every game without custom settings. Each backend uses the closest option it supports.");
    if (settingsEditor(app, app.global.defaults, nullptr, "")) app.saveAll();
    endPanel();
    ImGui::EndGroup();
    if (colW < w) ImGui::SameLine(0, S(20));
    ImGui::BeginGroup();
    beginPanel("app", ImVec2(colW, 0));
    sectionHeader(app, "MARATHON");
    float scale = app.global.uiScale > 0 ? app.global.uiScale : app.uiScale;
    ImGui::SetNextItemWidth(S(220));
    if (ImGui::SliderFloat("Interface size", &scale, 0.75f, 2.0f, "%.2fx")) {
        app.global.uiScale = scale;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        app.saveAll();
        app.toast("Interface size applies after restarting Marathon.");
    }
    if (ImGui::Checkbox("Show games and backends this device can't run", &app.global.showUnsupported)) app.saveAll();
    ImGui::Separator();
    ImGui::TextDisabled("Data folder (saves, configs, downloaded backends):");
    ImGui::TextWrapped("%s", pathToUtf8(paths().data).c_str());
    if (secondaryButton("Open data folder")) openExternal(pathToUtf8(paths().data));
    ImGui::Separator();
    sectionHeader(app, "CONTROLS");
    ImGui::TextDisabled("While playing inside Marathon:");
    ImGui::BulletText("Esc / Guide button / two-finger tap: pause menu");
    ImGui::BulletText("Tab (hold) / right-stick click: fast-forward");
    ImGui::BulletText("F11: fullscreen   F5 / F8: save / load state");
    ImGui::BulletText("Keyboard: arrows, Z/X/A/S, Q/W, Enter (Start)");
    ImGui::TextDisabled("External recomps and emulators use their own controls.");
    endPanel();
    ImGui::EndGroup();
}

// ---- about ---------------------------------------------------------------------------------------------------------

void drawAbout(App& app) {
    titleText(app, "About Ridge Racer Marathon", S(34), IM_COL32(255, 255, 255, 255));
    ImGui::TextDisabled("Version %s  |  %s", RRM_VERSION, currentPlatformKey().c_str());
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(
        "One launcher for the Ridge Racer series. Marathon recognises your own dumps, picks the best way to run each game "
        "on your device (a native recompilation when one exists, otherwise an emulator) and applies one set of "
        "enhancement settings - resolution, frame rate, speed and experimental redraws - to all of them.");
    ImGui::Dummy(ImVec2(0, S(6)));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAmber),
                       "Marathon is an unofficial fan project. Ridge Racer and all related titles are trademarks of Bandai "
                       "Namco Entertainment. Marathon contains no game code, BIOS or game data; you must supply files you "
                       "own.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, S(8)));
    sectionHeader(app, "BUILT ON THE WORK OF");
    const char* credits[][2] = {
        {"Ridge Racer PS1 Recomp - RobGreenUK", "https://github.com/RobGreenUK/Ridge-Racer-PS1-Recomp"},
        {"R4 Recomp - tetrisgm / Shokunin", "https://github.com/tetrisgm/RidgeRacerType4Recomp"},
        {"psxrecomp & recomp-ui - RetroPortingToolKit", "https://github.com/RetroPortingToolKit/psxrecomp"},
        {"Rage Racer PC - khasinski", "https://github.com/khasinski/rage-racer-pc"},
        {"namco22-decompile (Rave Racer) - spacestate1", "https://github.com/spacestate1/namco22-decompile"},
        {"Fukami (Ridge Racer V) - danisandoval", "https://github.com/danisandoval/fukami"},
        {"Ridge Racer 6 Recompilation - Sirhalo23", "https://github.com/Sirhalo23/RidgeRacer6-Recompilation"},
        {"PPSSPP", "https://www.ppsspp.org"},
        {"PCSX2", "https://pcsx2.net"},
        {"RPCS3", "https://rpcs3.net"},
        {"Xenia", "https://xenia.jp"},
        {"MAME", "https://www.mamedev.org"},
        {"libretro cores: SwanStation, PCSX-ReARMed, PPSSPP, MAME", "https://www.libretro.com"},
        {"SDL3, Dear ImGui, nlohmann/json, toml++, miniz", "https://libsdl.org"},
        {"Fonts: Rajdhani (ITF) and Titillium Web, SIL Open Font License", "https://fonts.google.com"},
    };
    for (auto& c : credits) {
        ImGui::PushID(c[0]);
        ImGui::BulletText("%s", c[0]);
        ImGui::SameLine();
        if (ImGui::SmallButton("link")) openExternal(c[1]);
        ImGui::PopID();
    }
}

// ---- overlays: running banner, modal, toasts ---------------------------------------------------------------------------

void drawOverlays(App& app) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    // Running external game banner
    if (const RunningGame* r = app.running()) {
        const GameDef* g = app.catalog.game(r->gameId);
        const BackendDef* bd = app.catalog.backend(r->backendId);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + S(76)), ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::SetNextWindowBgAlpha(0.96f);
        ImGui::Begin("##running", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kYellow), "%s is running in %s", g ? g->title.c_str() : r->gameId.c_str(),
                           bd ? bd->name.c_str() : r->backendId.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop")) app.stopRunning();
        ImGui::SameLine();
        if (ImGui::SmallButton("Log")) openExternal(r->logFile);
        ImGui::End();
    }
    // Modal
    if (!app.modalTitle.empty()) {
        ImGui::OpenPopup("##modal");
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(S(420), 0), ImVec2(S(720), vp->WorkSize.y * 0.8f));
        if (ImGui::BeginPopupModal("##modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            titleText(app, app.modalTitle.c_str(), S(28), kYellow);
            ImGui::PushTextWrapPos(S(680));
            ImGui::TextUnformatted(app.modalText.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, S(6)));
            if (primaryButton("OK", ImVec2(S(140), S(40)), kYellow) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                app.modalTitle.clear();
                app.modalText.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    // Toasts
    double t = app.now();
    app.toasts.erase(std::remove_if(app.toasts.begin(), app.toasts.end(), [&](const App::Toast& x) { return x.until < t; }),
                     app.toasts.end());
    float y = vp->WorkPos.y + vp->WorkSize.y - S(20);
    int idx = 0;
    for (auto it = app.toasts.rbegin(); it != app.toasts.rend(); ++it, ++idx) {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - S(20), y), ImGuiCond_Always, ImVec2(1, 1));
        ImGui::SetNextWindowBgAlpha(0.95f);
        char name[32];
        snprintf(name, sizeof(name), "##toast%d", idx);
        ImGui::Begin(name, nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
        ImGui::PushTextWrapPos(S(460));
        ImGui::TextColored(it->error ? ImGui::ColorConvertU32ToFloat4(kRed) : ImVec4(0.92f, 0.94f, 0.97f, 1), "%s", it->text.c_str());
        ImGui::PopTextWrapPos();
        y -= ImGui::GetWindowHeight() + S(8);
        ImGui::End();
    }
}

}  // namespace rrm::ui
