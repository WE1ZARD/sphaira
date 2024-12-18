#include "ui/menus/main_menu.hpp"
#include "ui/menus/irs_menu.hpp"
#include "ui/menus/themezer.hpp"

#include "ui/sidebar.hpp"
#include "ui/popup_list.hpp"
#include "ui/option_box.hpp"
#include "ui/progress_box.hpp"
#include "ui/error_box.hpp"

#include "app.hpp"
#include "log.hpp"
#include "download.hpp"
#include "defines.hpp"
#include "web.hpp"
#include "i18n.hpp"

#include <cstring>
#include <minizip/unzip.h>
#include <yyjson.h>

namespace sphaira::ui::menu::main {
namespace {

auto InstallUpdate(ProgressBox* pbox, const std::string url, const std::string version) -> bool {
    static fs::FsPath zip_out{"/config/sphaira/cache/update.zip"};
    constexpr auto chunk_size = 1024 * 512; // 512KiB

    fs::FsNativeSd fs;
    R_TRY_RESULT(fs.GetFsOpenResult(), false);

    // 1. download the zip
    if (!pbox->ShouldExit()) {
        pbox->NewTransfer("Downloading "_i18n + version);
        log_write("starting download: %s\n", url.c_str());

        DownloadClearCache(url);
        if (!DownloadFile(url, zip_out, "", [pbox](u32 dltotal, u32 dlnow, u32 ultotal, u32 ulnow){
            if (pbox->ShouldExit()) {
                return false;
            }
            pbox->UpdateTransfer(dlnow, dltotal);
            return true;
        })) {
            log_write("error with download\n");
            // push popup error box
            return false;
        }
    }

    ON_SCOPE_EXIT(fs.DeleteFile(zip_out));

    // 2. extract the zip
    if (!pbox->ShouldExit()) {
        auto zfile = unzOpen64(zip_out);
        if (!zfile) {
            log_write("failed to open zip: %s\n", zip_out);
            return false;
        }
        ON_SCOPE_EXIT(unzClose(zfile));

        unz_global_info64 pglobal_info;
        if (UNZ_OK != unzGetGlobalInfo64(zfile, &pglobal_info)) {
            return false;
        }

        for (int i = 0; i < pglobal_info.number_entry; i++) {
            if (i > 0) {
                if (UNZ_OK != unzGoToNextFile(zfile)) {
                    log_write("failed to unzGoToNextFile\n");
                    return false;
                }
            }

            if (UNZ_OK != unzOpenCurrentFile(zfile)) {
                log_write("failed to open current file\n");
                return false;
            }
            ON_SCOPE_EXIT(unzCloseCurrentFile(zfile));

            unz_file_info64 info;
            fs::FsPath file_path;
            if (UNZ_OK != unzGetCurrentFileInfo64(zfile, &info, file_path, sizeof(file_path), 0, 0, 0, 0)) {
                log_write("failed to get current info\n");
                return false;
            }

            if (file_path[0] != '/') {
                file_path = fs::AppendPath("/", file_path);
            }

            Result rc;
            if (file_path[strlen(file_path) -1] == '/') {
                if (R_FAILED(rc = fs.CreateDirectoryRecursively(file_path)) && rc != FsError_ResultPathAlreadyExists) {
                    log_write("failed to create folder: %s 0x%04X\n", file_path, rc);
                    return false;
                }
            } else {
                Result rc;
                if (R_FAILED(rc = fs.CreateFile(file_path, info.uncompressed_size, 0)) && rc != FsError_ResultPathAlreadyExists) {
                    log_write("failed to create file: %s 0x%04X\n", file_path, rc);
                    return false;
                }

                FsFile f;
                if (R_FAILED(rc = fs.OpenFile(file_path, FsOpenMode_Write, &f))) {
                    log_write("failed to open file: %s 0x%04X\n", file_path, rc);
                    return false;
                }
                ON_SCOPE_EXIT(fsFileClose(&f));

                if (R_FAILED(rc = fsFileSetSize(&f, info.uncompressed_size))) {
                    log_write("failed to set file size: %s 0x%04X\n", file_path, rc);
                    return false;
                }

                std::vector<char> buf(chunk_size);
                u64 offset{};
                while (offset < info.uncompressed_size) {
                    if (pbox->ShouldExit()) {
                        return false;
                    }

                    const auto bytes_read = unzReadCurrentFile(zfile, buf.data(), buf.size());
                    if (bytes_read <= 0) {
                        // log_write("failed to read zip file: %s\n", inzip.c_str());
                        return false;
                    }

                    if (R_FAILED(rc = fsFileWrite(&f, offset, buf.data(), bytes_read, FsWriteOption_None))) {
                        log_write("failed to write file: %s 0x%04X\n", file_path, rc);
                        return false;
                    }

                    pbox->UpdateTransfer(offset, info.uncompressed_size);
                    offset += bytes_read;
                }
            }
        }
    }

    log_write("finished update :)\n");
    return true;
}

} // namespace

MainMenu::MainMenu() {
    DownloadMemoryAsync("https://api.github.com/repos/we1zard/sphaira/releases/latest", "", [this](std::vector<u8>& data, bool success){
        m_update_state = UpdateState::None;
        auto json = yyjson_read((const char*)data.data(), data.size(), 0);
        R_UNLESS(json, false);
        ON_SCOPE_EXIT(yyjson_doc_free(json));

        auto root = yyjson_doc_get_root(json);
        R_UNLESS(root, false);

        auto tag_key = yyjson_obj_get(root, "tag_name");
        R_UNLESS(tag_key, false);

        const auto version = yyjson_get_str(tag_key);
        R_UNLESS(version, false);
        R_UNLESS(std::strcmp(APP_VERSION, version) < 0, false);

        auto assets = yyjson_obj_get(root, "assets");
        R_UNLESS(assets, false);

        auto idx0 = yyjson_arr_get(assets, 0);
        R_UNLESS(idx0, false);

        auto url_key = yyjson_obj_get(idx0, "browser_download_url");
        R_UNLESS(url_key, false);

        const auto url = yyjson_get_str(url_key);
        R_UNLESS(url, false);

        m_update_version = version;
        m_update_url = url;
        m_update_state = UpdateState::Update;
        log_write("found url: %s\n", url);
        App::Notify("Update avaliable: "_i18n + m_update_version);

        return true;
    });

    AddOnLPress();
    AddOnRPress();

    this->SetActions(
        std::make_pair(Button::START, Action{App::Exit}),
        std::make_pair(Button::Y, Action{"Menu"_i18n, [this](){
            auto options = std::make_shared<Sidebar>("Menu Options"_i18n, "v" APP_VERSION_HASH, Sidebar::Side::LEFT);
            ON_SCOPE_EXIT(App::Push(options));


            SidebarEntryArray::Items language_items;
            language_items.push_back("Auto"_i18n);
            language_items.push_back("English");
            language_items.push_back("Japanese"_i18n);
            language_items.push_back("French"_i18n);
            language_items.push_back("German"_i18n);
            language_items.push_back("Italian"_i18n);
            language_items.push_back("Spanish"_i18n);
            language_items.push_back("Chinese"_i18n);
            language_items.push_back("Korean"_i18n);
            language_items.push_back("Dutch"_i18n);
            language_items.push_back("Portuguese"_i18n);
            language_items.push_back("Russian"_i18n);

            options->AddHeader("Header"_i18n);
            options->AddSpacer();
            options->Add(std::make_shared<SidebarEntryCallback>("Theme"_i18n, [this](){
                SidebarEntryArray::Items theme_items{};
                const auto theme_meta = App::GetThemeMetaList();
                for (auto& p : theme_meta) {
                    theme_items.emplace_back(p.name);
                }

                auto options = std::make_shared<Sidebar>("Theme Options"_i18n, Sidebar::Side::LEFT);
                ON_SCOPE_EXIT(App::Push(options));

                options->Add(std::make_shared<SidebarEntryArray>("Select Theme"_i18n, theme_items, [this, theme_items](std::size_t& index_out){
                    App::SetTheme(index_out);
                }, App::GetThemeIndex()));

                options->Add(std::make_shared<SidebarEntryBool>("Shuffle"_i18n, App::GetThemeShuffleEnable(), [this](bool& enable){
                    App::SetThemeShuffleEnable(enable);
                }, "Enabled"_i18n, "Disabled"_i18n));

                options->Add(std::make_shared<SidebarEntryBool>("Music"_i18n, App::GetThemeMusicEnable(), [this](bool& enable){
                    App::SetThemeMusicEnable(enable);
                }, "Enabled"_i18n, "Disabled"_i18n));
            }));

            options->Add(std::make_shared<SidebarEntryCallback>("Network"_i18n, [this](){
                auto options = std::make_shared<Sidebar>("Network Options"_i18n, Sidebar::Side::LEFT);
                ON_SCOPE_EXIT(App::Push(options));

                options->Add(std::make_shared<SidebarEntryBool>("Nxlink"_i18n, App::GetNxlinkEnable(), [this](bool& enable){
                    App::SetNxlinkEnable(enable);
                }, "Enabled"_i18n, "Disabled"_i18n));

                if (m_update_state == UpdateState::Update) {
                    options->Add(std::make_shared<SidebarEntryCallback>("Download update: "_i18n + m_update_version, [this](){
                        App::Push(std::make_shared<ProgressBox>("Downloading "_i18n + m_update_version, [this](auto pbox){
                            return InstallUpdate(pbox, m_update_url, m_update_version);
                        }, [this](bool success){
                            if (success) {
                                m_update_state = UpdateState::None;
                            } else {
                                App::Push(std::make_shared<ui::ErrorBox>(MAKERESULT(351, 1), "Failed to download update"));
                            }
                        }, 2));
                    }));
                }
            }));

            options->Add(std::make_shared<SidebarEntryArray>("Language"_i18n, language_items, [this, language_items](std::size_t& index_out){
                App::SetLanguage(index_out);
            }, (std::size_t)App::GetLanguage()));

            options->Add(std::make_shared<SidebarEntryBool>("Logging"_i18n, App::GetLogEnable(), [this](bool& enable){
                App::SetLogEnable(enable);
            }, "Enabled"_i18n, "Disabled"_i18n));
            //options->Add(std::make_shared<SidebarEntryBool>("Replace hbmenu on exit"_i18n, App::GetReplaceHbmenuEnable(), [this](bool& enable){
            //    App::SetReplaceHbmenuEnable(enable);
            //}, "Enabled"_i18n, "Disabled"_i18n));

            options->Add(std::make_shared<SidebarEntryCallback>("Misc"_i18n, [this](){
                auto options = std::make_shared<Sidebar>("Misc Options"_i18n, Sidebar::Side::LEFT);
                ON_SCOPE_EXIT(App::Push(options));

                options->Add(std::make_shared<SidebarEntryCallback>("Themezer"_i18n, [](){
                    App::Push(std::make_shared<menu::themezer::Menu>());
                }));
                options->Add(std::make_shared<SidebarEntryCallback>("Irs"_i18n, [](){
                    App::Push(std::make_shared<menu::irs::Menu>());
                }));
                options->Add(std::make_shared<SidebarEntryCallback>("Web"_i18n, [](){
                    WebShow("https://lite.duckduckgo.com/lite");
                }));
            }));
        }})
    );

    m_homebrew_menu = std::make_shared<homebrew::Menu>();
    m_filebrowser_menu = std::make_shared<filebrowser::Menu>(m_homebrew_menu->GetHomebrewList());
    m_app_store_menu = std::make_shared<appstore::Menu>(m_homebrew_menu->GetHomebrewList());
    m_current_menu = m_homebrew_menu;

    for (auto [button, action] : m_actions) {
        m_current_menu->SetAction(button, action);
    }
}

MainMenu::~MainMenu() {

}

void MainMenu::Update(Controller* controller, TouchInfo* touch) {
    m_current_menu->Update(controller, touch);
}

void MainMenu::Draw(NVGcontext* vg, Theme* theme) {
    m_current_menu->Draw(vg, theme);
}

void MainMenu::OnFocusGained() {
    Widget::OnFocusGained();
    this->SetHidden(false);
    m_current_menu->OnFocusGained();
}

void MainMenu::OnFocusLost() {
    m_current_menu->OnFocusLost();
}

void MainMenu::OnLRPress(std::shared_ptr<MenuBase> menu, Button b) {
    m_current_menu->OnFocusLost();
    if (m_current_menu == m_homebrew_menu) {
        m_current_menu = menu;
        RemoveAction(b);
    } else {
        m_current_menu = m_homebrew_menu;
    }

    m_current_menu->OnFocusGained();

    for (auto [button, action] : m_actions) {
        m_current_menu->SetAction(button, action);
    }

    if (b == Button::L) {
        AddOnRPress();
    } else {
        AddOnLPress();
    }
}

void MainMenu::AddOnLPress() {
    SetAction(Button::L, Action{"Fs"_i18n, [this]{
        OnLRPress(m_filebrowser_menu, Button::L);
    }});
}

void MainMenu::AddOnRPress() {
    SetAction(Button::R, Action{"App"_i18n, [this]{
        OnLRPress(m_app_store_menu, Button::R);
    }});
}

} // namespace sphaira::ui::menu::main
