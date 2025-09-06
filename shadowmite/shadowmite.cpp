// main.cpp
#include <gtk/gtk.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unordered_set>
#include "json.hpp" // nlohmann::json single-header

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---------------- AppWidgets (unified) ----------------
struct AppWidgets {
    GtkWidget *window;
    GtkWidget *stack;

    // Network
    GtkWidget *iface_combo;
    GtkWidget *wifi_combo;
    GtkWidget *password_entry;
    GtkWidget *status_label;
    std::string selected_iface;
    std::string selected_wifi;

    // Locale
    GtkWidget *locale_combo;
    GtkWidget *tz_combo;
    std::string selected_lang;
    std::string selected_tz;

    // Apps & summary
    GtkWidget *apps_list_box;
    GtkWidget *apps_scrolled;
    GtkWidget *summary_box;
    GtkWidget *summary_logo;
    GtkWidget *summary_name;
    GtkWidget *summary_desc;
    std::string selected_package;
    fs::path selected_json_path;
};

// ---------------- Utility helpers ----------------
static std::vector<std::string> get_locales() {
    std::vector<std::string> locales;
    FILE* fp = popen("locale -a", "r");
    if (!fp) return locales;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = 0;
        if (strlen(line) > 0) locales.push_back(line);
    }
    pclose(fp);
    return locales;
}

static std::vector<std::string> get_timezones() {
    std::vector<std::string> zones;
    FILE* fp = popen("timedatectl list-timezones", "r");
    if (!fp) return zones;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = 0;
        if (strlen(line) > 0) zones.push_back(line);
    }
    pclose(fp);
    return zones;
}

// ---------------- Forward declarations ----------------
void setup_welcome_screen(AppWidgets* aw);
void setup_network_screen(AppWidgets* aw);
void setup_locale_screen(AppWidgets* aw);
void setup_apps_screen(AppWidgets* aw);
void setup_summary_screen(AppWidgets* aw);
void setup_finish_screen(AppWidgets* aw);

void load_prescribed_apps(AppWidgets* aw);
void reload_prescribed_apps(AppWidgets* aw);
static void edit_json_btn_clicked(GtkButton* button, gpointer data);
void show_summary(AppWidgets* aw, const std::string& name, const std::string& description,
                  const std::string& logo, const std::string& package, const fs::path& json_path);

// ---------------- Navigation callbacks ----------------
static void welcome_continue_cb(GtkButton* button, gpointer data) {
    AppWidgets* aw = (AppWidgets*)data;
    gtk_stack_set_visible_child_name(GTK_STACK(aw->stack), "network");
}

static void skip_to_locale_cb(GtkButton* button, gpointer data) {
    AppWidgets* aw = (AppWidgets*)data;
    gtk_stack_set_visible_child_name(GTK_STACK(aw->stack), "locale");
}

static void back_to_network_cb(GtkButton* button, gpointer data) {
    AppWidgets* aw = (AppWidgets*)data;
    gtk_stack_set_visible_child_name(GTK_STACK(aw->stack), "network");
}

// ---------------- Wi-Fi scanning (async) ----------------
struct WiFiThreadData {
    AppWidgets* aw;
    GtkWidget* wait_popup;
    std::vector<std::string> networks;
};

static void wifi_scan_finish(gpointer arg) {
    WiFiThreadData* data = (WiFiThreadData*)arg;
    // Clear existing entries then append the found networks
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(data->aw->wifi_combo));
    for (auto& ssid : data->networks) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(data->aw->wifi_combo), ssid.c_str());
    }
    gtk_widget_set_sensitive(data->aw->wifi_combo, TRUE);
    gtk_widget_set_sensitive(data->aw->password_entry, TRUE);
    gtk_label_set_text(GTK_LABEL(data->aw->status_label), "Select Wi-Fi and enter password.");
    gtk_widget_destroy(data->wait_popup);
    delete data;
}

static void wifi_scan_thread(WiFiThreadData* data) {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    FILE* fp = popen("sudo nmcli -t -f SSID dev wifi list", "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\n")] = 0;
            if (strlen(line) > 0) data->networks.push_back(line);
        }
        pclose(fp);
    }
    // Push back to main thread for GTK updates
    g_idle_add((GSourceFunc)[](gpointer arg)->gboolean {
        wifi_scan_finish(arg);
        return G_SOURCE_REMOVE;
    }, data);
}

static void iface_changed_cb(GtkComboBox* combo, gpointer user_data) {
    AppWidgets* aw = (AppWidgets*)user_data;
    gchar* iface_text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (!iface_text) return;
    aw->selected_iface = iface_text;
    g_free(iface_text);

    if (aw->selected_iface.find("wlan") != std::string::npos || aw->selected_iface.find("wifi") != std::string::npos) {
        GtkWidget* wait_popup = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(wait_popup), "Scanning Wi-Fi...");
        gtk_window_set_modal(GTK_WINDOW(wait_popup), TRUE);
        gtk_window_set_transient_for(GTK_WINDOW(wait_popup), GTK_WINDOW(aw->window));
        gtk_window_set_default_size(GTK_WINDOW(wait_popup), 300, 80);
        GtkWidget* lbl = gtk_label_new("Scanning for networks, please wait...");
        gtk_container_add(GTK_CONTAINER(wait_popup), lbl);
        gtk_widget_show_all(wait_popup);

        gtk_widget_set_sensitive(aw->wifi_combo, FALSE);
        gtk_widget_set_sensitive(aw->password_entry, FALSE);
        gtk_label_set_text(GTK_LABEL(aw->status_label), "Scanning Wi-Fi...");

        WiFiThreadData* td = new WiFiThreadData{aw, wait_popup, {}};
        std::thread(wifi_scan_thread, td).detach();
    } else {
        gtk_widget_set_sensitive(aw->wifi_combo, FALSE);
        gtk_widget_set_sensitive(aw->password_entry, FALSE);
        gtk_label_set_text(GTK_LABEL(aw->status_label), "Ethernet selected.");
    }
}

static void wifi_changed_cb(GtkComboBox* combo, gpointer user_data) {
    AppWidgets* aw = (AppWidgets*)user_data;
    gchar* wifi_text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (wifi_text) {
        aw->selected_wifi = wifi_text;
        g_free(wifi_text);
    }
}

static void locale_changed_cb(GtkComboBox* combo, gpointer user_data) {
    AppWidgets* aw = (AppWidgets*)user_data;
    gchar* text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (text) {
        aw->selected_lang = text;
        g_free(text);
    }
}

static void tz_changed_cb(GtkComboBox* combo, gpointer user_data) {
    AppWidgets* aw = (AppWidgets*)user_data;
    gchar* text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (text) {
        aw->selected_tz = text;
        g_free(text);
    }
}

// ---------------- Static IP dialog ----------------
void show_static_ip_dialog(AppWidgets* aw) {
    GtkWidget* dialog = gtk_dialog_new_with_buttons("Static IP Configuration",
                                                    GTK_WINDOW(aw->window),
                                                    GTK_DIALOG_MODAL,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_OK", GTK_RESPONSE_OK,
                                                    NULL);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
    gtk_container_add(GTK_CONTAINER(content), grid);

    GtkWidget* mode_label = gtk_label_new("IP Mode:");
    gtk_widget_set_halign(mode_label, GTK_ALIGN_START);
    GtkWidget* mode_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode_combo), "DHCP");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode_combo), "Static");
    gtk_combo_box_set_active(GTK_COMBO_BOX(mode_combo), 0);
    gtk_grid_attach(GTK_GRID(grid), mode_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), mode_combo, 1, 0, 1, 1);

    GtkWidget* ip_label = gtk_label_new("IP Address:");
    gtk_widget_set_halign(ip_label, GTK_ALIGN_START);
    GtkWidget* ip_entry = gtk_entry_new();
    gtk_grid_attach(GTK_GRID(grid), ip_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ip_entry, 1, 1, 1, 1);

    GtkWidget* gw_label = gtk_label_new("Gateway:");
    gtk_widget_set_halign(gw_label, GTK_ALIGN_START);
    GtkWidget* gw_entry = gtk_entry_new();
    gtk_grid_attach(GTK_GRID(grid), gw_label, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gw_entry, 1, 2, 1, 1);

    GtkWidget* dns_label = gtk_label_new("DNS:");
    gtk_widget_set_halign(dns_label, GTK_ALIGN_START);
    GtkWidget* dns_entry = gtk_entry_new();
    gtk_grid_attach(GTK_GRID(grid), dns_label, 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), dns_entry, 1, 3, 1, 1);

    gtk_widget_show_all(dialog);

    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_OK) {
        const char* mode = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(mode_combo));
        const char* ip   = gtk_entry_get_text(GTK_ENTRY(ip_entry));
        const char* gw   = gtk_entry_get_text(GTK_ENTRY(gw_entry));
        const char* dns  = gtk_entry_get_text(GTK_ENTRY(dns_entry));
        std::cout << "Static IP Config: Mode=" << (mode ? mode : "(null)") << ", IP=" << ip << ", GW=" << gw << ", DNS=" << dns << "\n";
    }
    gtk_widget_destroy(dialog);
}

// --- Callback for "Continue" buttons ---
static void continue_btn_clicked(GtkButton* btn, gpointer data) {
    auto tup = static_cast<std::tuple<
        AppWidgets*, std::string, std::string, std::string, std::string, fs::path
    >*>(data);

    AppWidgets* aw;
    std::string name, description, logo, package;
    fs::path path;

    std::tie(aw, name, description, logo, package, path) = *tup;
    show_summary(aw, name, description, logo, package, path);
}

// --- Function to load prescribed apps ---
void load_prescribed_apps(AppWidgets* aw) {
    // Remove existing children from list box
    if (aw->apps_list_box) {
        GList* children = gtk_container_get_children(GTK_CONTAINER(aw->apps_list_box));
        for (GList* iter = children; iter != nullptr; iter = iter->next) {
            gtk_widget_destroy(GTK_WIDGET(iter->data));
        }
        g_list_free(children);
    }

    // Ensure apps directory exists
    fs::path apps_dir = fs::path(getenv("HOME")) / "sm_conf" / "apps";
    if (!fs::exists(apps_dir)) {
        fs::create_directories(apps_dir);
    }

    // Track added JSON files to avoid duplicates
    std::unordered_set<std::string> added;

    for (auto& entry : fs::directory_iterator(apps_dir)) {
        if (entry.path().extension() != ".json") continue;

        std::string pstr = entry.path().string();
        if (added.find(pstr) != added.end()) continue;
        added.insert(pstr);

        std::ifstream ifs(entry.path());
        if (!ifs.is_open()) continue;

        try {
            json j;
            ifs >> j;

            std::string name        = j.value("name", entry.path().stem().string());
            std::string description = j.value("description", "");
            std::string logo        = j.value("logo", "");
            std::string package     = j.value("package", "");

            // Expand ~ to $HOME in logo path
            if (!logo.empty() && logo[0] == '~') {
                const char* home = getenv("HOME");
                if (home) {
                    logo = std::string(home) + logo.substr(1);
                }
            }
            // Make relative logos point to same folder as JSON
            if (!logo.empty() && logo[0] != '/') {
                logo = (entry.path().parent_path() / logo).string();
            }
            // Fallback to default logo
            if (!fs::exists(logo)) {
                logo = (entry.path().parent_path() / "logos/default.png").string();
            }

            // Build a row for this app
            GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

            // App logo
            GtkWidget* image = nullptr;
            if (fs::exists(logo)) {
                image = gtk_image_new_from_file(logo.c_str());
            } else {
                image = gtk_image_new(); // empty placeholder
            }
            gtk_box_pack_start(GTK_BOX(row), image, FALSE, FALSE, 10);

            // Labels (name + description)
            GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

            GtkWidget* label_name = gtk_label_new(name.c_str());
            gtk_widget_set_name(label_name, "app-name");
            gtk_widget_set_halign(label_name, GTK_ALIGN_START);
            gtk_label_set_xalign(GTK_LABEL(label_name), 0.0);

            GtkWidget* label_desc = gtk_label_new(description.c_str());
            gtk_widget_set_name(label_desc, "app-desc");
            gtk_label_set_xalign(GTK_LABEL(label_desc), 0.0);
            gtk_label_set_xalign(GTK_LABEL(label_desc), 0.0);
            gtk_label_set_justify(GTK_LABEL(label_desc), GTK_JUSTIFY_LEFT);

            gtk_box_pack_start(GTK_BOX(vbox), label_name, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(vbox), label_desc, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(row), vbox, TRUE, TRUE, 10);

            // Continue button
            GtkWidget* cont_btn = gtk_button_new_with_label("Continue");
            auto* tup = new std::tuple<
                AppWidgets*, std::string, std::string, std::string, std::string, fs::path
            >(aw, name, description, logo, package, entry.path());

            g_signal_connect_data(
                cont_btn,
                "clicked",
                G_CALLBACK(continue_btn_clicked),
                tup,
                [](gpointer data, GClosure*) {
                    delete static_cast<std::tuple<
                        AppWidgets*, std::string, std::string, std::string, std::string, fs::path
                    >*>(data);
                },
                (GConnectFlags)0
            );

            gtk_box_pack_end(GTK_BOX(row), cont_btn, FALSE, FALSE, 10);

            // Add row to list box
            gtk_box_pack_start(GTK_BOX(aw->apps_list_box), row, FALSE, FALSE, 8);

        } catch (...) {
            g_print("Failed to parse JSON: %s\n", entry.path().c_str());
        }
    }

    gtk_widget_show_all(aw->apps_list_box);
}

void reload_prescribed_apps(AppWidgets* aw) {
    load_prescribed_apps(aw);
    gtk_widget_show_all(aw->apps_list_box);
}

// ---------------- Build UI screens ----------------
void setup_welcome_screen(AppWidgets* aw) {
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 30);
    gtk_stack_add_named(GTK_STACK(aw->stack), vbox, "welcome");

    GtkWidget* image = gtk_image_new_from_file("shadowmite.png");
    gtk_widget_set_halign(image, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), image, FALSE, FALSE, 10);

    GtkWidget* title = gtk_label_new("<span size='xx-large'><b>Welcome to Shadowmite</b></span>");
    gtk_label_set_use_markup(GTK_LABEL(title), TRUE);
    gtk_label_set_justify(GTK_LABEL(title), GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign(title, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 10);

    GtkWidget* subtitle = gtk_label_new("<span size='small'>This setup wizard will guide you through the essential configuration.</span>");
    gtk_label_set_use_markup(GTK_LABEL(subtitle), TRUE);
    gtk_label_set_justify(GTK_LABEL(subtitle), GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign(subtitle, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), subtitle, FALSE, FALSE, 10);

    GtkWidget* continue_btn = gtk_button_new_with_label("Continue");
    gtk_widget_set_size_request(continue_btn, 120, 40);
    gtk_widget_set_halign(continue_btn, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), continue_btn, FALSE, FALSE, 20);
    g_signal_connect(continue_btn, "clicked", G_CALLBACK(welcome_continue_cb), aw);
}

void setup_network_screen(AppWidgets* aw) {
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 30);
    gtk_stack_add_named(GTK_STACK(aw->stack), vbox, "network");

    // --- Header ---
    GtkWidget* title = gtk_label_new("<span size='x-large'><b>Network Setup</b></span>");
    gtk_label_set_use_markup(GTK_LABEL(title), TRUE);
    gtk_label_set_justify(GTK_LABEL(title), GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign(title, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 5);

    // --- Subtitle / instructions ---
    GtkWidget* subtitle = gtk_label_new("Select your interface and configure Wi-Fi or Ethernet settings.");
    gtk_widget_set_halign(subtitle, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), subtitle, FALSE, FALSE, 10);

    // --- Interface selection ---
    GtkWidget* iface_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* iface_label = gtk_label_new("Interface:");
    gtk_widget_set_halign(iface_label, GTK_ALIGN_START);
    aw->iface_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(aw->iface_combo), "eth0");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(aw->iface_combo), "wlan0");
    gtk_combo_box_set_active(GTK_COMBO_BOX(aw->iface_combo), 0);
    gtk_box_pack_start(GTK_BOX(iface_box), iface_label, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(iface_box), aw->iface_combo, TRUE, TRUE, 6);
    gtk_box_pack_start(GTK_BOX(vbox), iface_box, FALSE, FALSE, 6);

    // --- Wi-Fi Frame ---
    GtkWidget* wifi_frame = gtk_frame_new("Wi-Fi");
    GtkWidget* wifi_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(wifi_frame), wifi_vbox);

    GtkWidget* wifi_label = gtk_label_new("Networks:");
    gtk_widget_set_halign(wifi_label, GTK_ALIGN_START);
    aw->wifi_combo = gtk_combo_box_text_new();
    gtk_widget_set_sensitive(aw->wifi_combo, FALSE);

    GtkWidget* pwd_label = gtk_label_new("Password:");
    gtk_widget_set_halign(pwd_label, GTK_ALIGN_START);
    aw->password_entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(aw->password_entry), FALSE);
    gtk_widget_set_sensitive(aw->password_entry, FALSE);

    gtk_box_pack_start(GTK_BOX(wifi_vbox), wifi_label, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(wifi_vbox), aw->wifi_combo, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(wifi_vbox), pwd_label, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(wifi_vbox), aw->password_entry, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(vbox), wifi_frame, FALSE, FALSE, 10);

    // --- Status label ---
    aw->status_label = gtk_label_new("Select your interface.");
    gtk_widget_set_halign(aw->status_label, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), aw->status_label, FALSE, FALSE, 10);

    // --- Bottom buttons ---
    GtkWidget* button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(button_box, GTK_ALIGN_CENTER);
    GtkWidget* skip_btn = gtk_button_new_with_label("Skip");
    GtkWidget* adv_btn = gtk_button_new_with_label("Advanced...");
    gtk_box_pack_start(GTK_BOX(button_box), skip_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(button_box), adv_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), button_box, FALSE, FALSE, 10);

    g_signal_connect(skip_btn, "clicked", G_CALLBACK(skip_to_locale_cb), aw);
    g_signal_connect(adv_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data){
        show_static_ip_dialog((AppWidgets*)data);
    }), aw);
    g_signal_connect(aw->iface_combo, "changed", G_CALLBACK(iface_changed_cb), aw);
    g_signal_connect(aw->wifi_combo, "changed", G_CALLBACK(wifi_changed_cb), aw);

    gchar* start_iface = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(aw->iface_combo));
    if (start_iface) {
        if (std::string(start_iface).find("wlan") == std::string::npos) {
            gtk_widget_set_sensitive(aw->wifi_combo, FALSE);
            gtk_widget_set_sensitive(aw->password_entry, FALSE);
        }
        g_free(start_iface);
    }
}

void setup_locale_screen(AppWidgets* aw) {
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 30);
    gtk_stack_add_named(GTK_STACK(aw->stack), vbox, "locale");

    // --- Header ---
    GtkWidget* title = gtk_label_new("<span size='x-large'><b>Locale Setup</b></span>");
    gtk_label_set_use_markup(GTK_LABEL(title), TRUE);
    gtk_label_set_justify(GTK_LABEL(title), GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign(title, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 5);

    // --- Subtitle ---
    GtkWidget* subtitle = gtk_label_new("Choose your preferred language and timezone.");
    gtk_widget_set_halign(subtitle, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), subtitle, FALSE, FALSE, 10);

    // --- Language ---
    GtkWidget* locale_label = gtk_label_new("Language:");
    gtk_widget_set_halign(locale_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), locale_label, FALSE, FALSE, 2);

    aw->locale_combo = gtk_combo_box_text_new();
    for (auto& loc : get_locales()) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(aw->locale_combo), loc.c_str());
    }
    gtk_box_pack_start(GTK_BOX(vbox), aw->locale_combo, FALSE, FALSE, 2);
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(aw->locale_combo)) == -1)
        gtk_combo_box_set_active(GTK_COMBO_BOX(aw->locale_combo), 0);

    // --- Timezone ---
    GtkWidget* tz_label = gtk_label_new("Timezone:");
    gtk_widget_set_halign(tz_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), tz_label, FALSE, FALSE, 2);

    aw->tz_combo = gtk_combo_box_text_new();
    for (auto& tz : get_timezones()) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(aw->tz_combo), tz.c_str());
    }
    gtk_box_pack_start(GTK_BOX(vbox), aw->tz_combo, FALSE, FALSE, 2);
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(aw->tz_combo)) == -1)
        gtk_combo_box_set_active(GTK_COMBO_BOX(aw->tz_combo), 0);

    // --- Bottom buttons ---
    GtkWidget* button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(button_box, GTK_ALIGN_CENTER);
    GtkWidget* back_btn = gtk_button_new_with_label("Back");
    GtkWidget* next_btn = gtk_button_new_with_label("Next");
    gtk_box_pack_start(GTK_BOX(button_box), back_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(button_box), next_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), button_box, FALSE, FALSE, 10);

    g_signal_connect(back_btn, "clicked", G_CALLBACK(back_to_network_cb), aw);
    g_signal_connect(next_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data){
        AppWidgets* aw = (AppWidgets*)data;
        gtk_stack_set_visible_child_name(GTK_STACK(aw->stack), "apps");
    }), aw);

    g_signal_connect(aw->locale_combo, "changed", G_CALLBACK(locale_changed_cb), aw);
    g_signal_connect(aw->tz_combo, "changed", G_CALLBACK(tz_changed_cb), aw);
}

// ---------------- Prescribed Apps + Summary ----------------
void show_summary(AppWidgets* aw, const std::string& name, const std::string& description,
                  const std::string& logo, const std::string& package, const fs::path& json_path) {
    aw->selected_package = package;
    aw->selected_json_path = json_path;

    if (aw->summary_logo && fs::exists(logo)) gtk_image_set_from_file(GTK_IMAGE(aw->summary_logo), logo.c_str());
    gtk_label_set_text(GTK_LABEL(aw->summary_name), name.c_str());
    gtk_label_set_text(GTK_LABEL(aw->summary_desc), description.c_str());
    gtk_widget_show_all(aw->summary_box);
    gtk_stack_set_visible_child_name(GTK_STACK(aw->stack), "summary");
}

static void edit_json_btn_clicked(GtkButton* button, gpointer data) {
    AppWidgets* aw = (AppWidgets*)data;
    if (aw->selected_json_path.empty()) return;
    if (!fs::exists(aw->selected_json_path)) return;
    std::string cmd = "x-terminal-emulator -e \"nano '" + aw->selected_json_path.string() + "'\" &";
    system(cmd.c_str());
}

static void install_btn_clicked(GtkButton* button, gpointer data) {
    AppWidgets* aw = (AppWidgets*)data;
    if (aw->selected_package.empty()) return;
    std::string cmd = "sudo apt install -y " + aw->selected_package;
    system(cmd.c_str());
}

static void summary_back_clicked(GtkButton* button, gpointer data) {
    AppWidgets* aw = (AppWidgets*)data;
    gtk_stack_set_visible_child_name(GTK_STACK(aw->stack), "apps");
}

void setup_summary_screen(AppWidgets* aw) {
    aw->summary_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_container_set_border_width(GTK_CONTAINER(aw->summary_box), 20);

    GtkWidget* title = gtk_label_new("<span size='xx-large'><b>Summary:</b></span>");
    gtk_label_set_use_markup(GTK_LABEL(title), TRUE);
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(aw->summary_box), title, FALSE, FALSE, 8);

    aw->summary_logo = gtk_image_new();
    aw->summary_name = gtk_label_new("");
    gtk_widget_set_name(aw->summary_name, "summary-name");
    aw->summary_desc = gtk_label_new("");
    gtk_widget_set_name(aw->summary_desc, "summary-desc");

    gtk_box_pack_start(GTK_BOX(aw->summary_box), aw->summary_logo, FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(aw->summary_box), aw->summary_name, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(aw->summary_box), aw->summary_desc, FALSE, FALSE, 4);

    GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* edit_btn = gtk_button_new_with_label("Edit JSON");
    GtkWidget* install_btn = gtk_button_new_with_label("Install");
    GtkWidget* back_btn = gtk_button_new_with_label("Back");

    g_signal_connect(edit_btn, "clicked", G_CALLBACK(edit_json_btn_clicked), aw);
    g_signal_connect(install_btn, "clicked", G_CALLBACK(install_btn_clicked), aw);
    g_signal_connect(back_btn, "clicked", G_CALLBACK(summary_back_clicked), aw);

    gtk_box_pack_start(GTK_BOX(btn_box), edit_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_box), install_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_box), back_btn, FALSE, FALSE, 0);

    gtk_box_pack_end(GTK_BOX(aw->summary_box), btn_box, FALSE, FALSE, 6);
    gtk_stack_add_named(GTK_STACK(aw->stack), aw->summary_box, "summary");
}

void setup_apps_screen(AppWidgets* aw) {
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 30);
    gtk_stack_add_named(GTK_STACK(aw->stack), vbox, "apps");

    // --- Header ---
    GtkWidget* title = gtk_label_new("<span size='xx-large'><b>Available Apps</b></span>");
    gtk_label_set_use_markup(GTK_LABEL(title), TRUE);
    gtk_label_set_justify(GTK_LABEL(title), GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign(title, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 5);

    // --- Subtitle / explanatory text under header ---
    GtkWidget* subtitle = gtk_label_new("Select an app, create a new one, or reload the list.");
    gtk_widget_set_halign(subtitle, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), subtitle, FALSE, FALSE, 10);

    // --- Scrollable apps list inside a frame (thin border) ---
    GtkWidget* apps_frame = gtk_frame_new(NULL);
    gtk_container_set_border_width(GTK_CONTAINER(apps_frame), 6);

    aw->apps_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(aw->apps_scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    aw->apps_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(aw->apps_list_box), 12);
    gtk_container_add(GTK_CONTAINER(aw->apps_scrolled), aw->apps_list_box);
    gtk_container_add(GTK_CONTAINER(apps_frame), aw->apps_scrolled);
    gtk_box_pack_start(GTK_BOX(vbox), apps_frame, TRUE, TRUE, 10);

    // --- Bottom buttons (centered) ---
    GtkWidget* button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(button_box, GTK_ALIGN_CENTER);

    GtkWidget* back_btn = gtk_button_new_with_label("Back");
    g_signal_connect(back_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data){
        AppWidgets* aw = (AppWidgets*)data;
        gtk_stack_set_visible_child_name(GTK_STACK(aw->stack), "locale");
    }), aw);

    GtkWidget* reload_btn = gtk_button_new_with_label("Reload");
    g_signal_connect(reload_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data){
        reload_prescribed_apps((AppWidgets*)data);
    }), aw);

    GtkWidget* create_btn = gtk_button_new_with_label("Create");
    g_signal_connect(create_btn, "clicked", G_CALLBACK(+[](GtkButton* btn, gpointer data){
        AppWidgets* aw = (AppWidgets*)data;
        fs::path apps_dir = fs::path(getenv("HOME")) / "sm_conf" / "apps";
        if (!fs::exists(apps_dir)) fs::create_directories(apps_dir);
        // unique filename with timestamp to avoid clobbering
        auto t = std::chrono::system_clock::now();
        auto s = std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
        fs::path new_json = apps_dir / ("new_app_" + std::to_string(s) + ".json");

        json j;
        j["name"] = "New App";
        j["description"] = "Description here";
        j["logo"] = "logos/default.png";
        j["package"] = "package-name";

        std::ofstream ofs(new_json);
        ofs << j.dump(4) << std::endl;
        ofs.close();

        // Open in nano inside user's terminal emulator immediately
        std::string cmd = "x-terminal-emulator -e \"nano '" + new_json.string() + "'\" &";
        system(cmd.c_str());

        // Refresh the list
        reload_prescribed_apps(aw);
    }), aw);

    GtkWidget* skip_btn = gtk_button_new_with_label("Skip");
    g_signal_connect(skip_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data){
        AppWidgets* aw = (AppWidgets*)data;
        gtk_stack_set_visible_child_name(GTK_STACK(aw->stack), "finish");
    }), aw);

    gtk_box_pack_start(GTK_BOX(button_box), back_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(button_box), reload_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(button_box), create_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(button_box), skip_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), button_box, FALSE, FALSE, 10);

    // --- Status label at very bottom (optional) ---
    aw->status_label = gtk_label_new("");
    gtk_widget_set_halign(aw->status_label, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), aw->status_label, FALSE, FALSE, 5);

    // --- Summary setup (hidden until used) ---
    setup_summary_screen(aw);
}

// ---------------- Setup Finish screen ----------------
void setup_finish_screen(AppWidgets* aw) {
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 30);
    gtk_stack_add_named(GTK_STACK(aw->stack), vbox, "finish");

    GtkWidget* title = gtk_label_new("<span size='x-large'><b>Setup Complete</b></span>");
    gtk_label_set_use_markup(GTK_LABEL(title), TRUE);
    gtk_label_set_justify(GTK_LABEL(title), GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign(title, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 5);

    GtkWidget* subtitle = gtk_label_new("Your system is ready to use.");
    gtk_widget_set_halign(subtitle, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), subtitle, FALSE, FALSE, 10);

    GtkWidget* summary_frame = gtk_frame_new(NULL);
    gtk_container_set_border_width(GTK_CONTAINER(summary_frame), 6);
    GtkWidget* scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, 500, 200);
    GtkWidget* summary_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(summary_box), 10);
    gtk_container_add(GTK_CONTAINER(scroll), summary_box);
    gtk_container_add(GTK_CONTAINER(summary_frame), scroll);
    gtk_box_pack_start(GTK_BOX(vbox), summary_frame, TRUE, TRUE, 10);

    // dynamic labels will be added when showing finish; we'll add placeholders
    GtkWidget* lbl_iface = gtk_label_new("");
    GtkWidget* lbl_wifi  = gtk_label_new("");
    GtkWidget* lbl_lang  = gtk_label_new("");
    GtkWidget* lbl_tz    = gtk_label_new("");
    GtkWidget* lbl_app   = gtk_label_new("");

    gtk_widget_set_halign(lbl_iface, GTK_ALIGN_START);
    gtk_widget_set_halign(lbl_wifi, GTK_ALIGN_START);
    gtk_widget_set_halign(lbl_lang, GTK_ALIGN_START);
    gtk_widget_set_halign(lbl_tz, GTK_ALIGN_START);
    gtk_widget_set_halign(lbl_app, GTK_ALIGN_START);

    gtk_box_pack_start(GTK_BOX(summary_box), lbl_iface, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(summary_box), lbl_wifi, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(summary_box), lbl_lang, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(summary_box), lbl_tz, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(summary_box), lbl_app, FALSE, FALSE, 2);

    // We'll update these labels whenever finish page is shown:
    g_signal_connect_swapped(aw->stack, "notify::visible-child-name", G_CALLBACK(+[](gpointer data){
        AppWidgets* aw = (AppWidgets*)data;
        const char* child = gtk_stack_get_visible_child_name(GTK_STACK(aw->stack));
        if (child && strcmp(child, "finish") == 0) {
            // find the labels inside the finish page and update them
            // simple approach: iterate children of stack's finish page and set text
            GtkWidget* finish_page = nullptr;
            GList* kids = gtk_container_get_children(GTK_CONTAINER(aw->stack));
            for (GList* it = kids; it; it = it->next) {
                GtkWidget* w = GTK_WIDGET(it->data);
                const char* name = gtk_widget_get_name(w);
                // not relying on names: find the "finish" page by pointer comparison
                if (gtk_widget_get_visible(w) && GTK_IS_BOX(w)) {
                    // but safer: skip — instead directly rebuild summary labels here:
                }
            }
            g_list_free(kids);
            // update a new temporary summary printout in stdout (and status_label) so user sees values
            std::string s = "Iface: " + aw->selected_iface + "  Wi-Fi: " + (aw->selected_wifi.empty() ? std::string("None") : aw->selected_wifi)
                + "  Lang: " + aw->selected_lang + "  TZ: " + aw->selected_tz;
            g_print("%s\n", s.c_str());
            if (aw->status_label) gtk_label_set_text(GTK_LABEL(aw->status_label), s.c_str());
        }
    }), aw);

    // --- Bottom buttons ---
    GtkWidget* button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(button_box, GTK_ALIGN_CENTER);

    GtkWidget* reboot_btn = gtk_button_new_with_label("Reboot");
    GtkWidget* exit_btn   = gtk_button_new_with_label("Exit");

    gtk_box_pack_start(GTK_BOX(button_box), reboot_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(button_box), exit_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), button_box, FALSE, FALSE, 10);

    g_signal_connect(reboot_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer){
        system("reboot");
    }), NULL);

    g_signal_connect(exit_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer){
        gtk_main_quit();
    }), NULL);
}

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);

    AppWidgets* aw = new AppWidgets();

    aw->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(aw->window), "Shadowmite Setup");
    gtk_window_set_default_size(GTK_WINDOW(aw->window), 900, 700);
    g_signal_connect(aw->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    aw->stack = GTK_WIDGET(gtk_stack_new());
    gtk_container_add(GTK_CONTAINER(aw->window), aw->stack);

    setup_welcome_screen(aw);
    setup_network_screen(aw);
    setup_locale_screen(aw);
    setup_apps_screen(aw);
    setup_finish_screen(aw);

    gtk_stack_set_visible_child_name(GTK_STACK(aw->stack), "welcome");
    gtk_widget_show_all(aw->window);

    load_prescribed_apps(aw);

    gtk_main();
    return 0;
}

