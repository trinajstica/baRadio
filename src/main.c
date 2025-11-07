#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <libayatana-appindicator/app-indicator.h>
#include <sqlite3.h>
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>


#include <gio/gio.h>

void on_play_clicked(GtkButton *button, gpointer user_data);

// Verzija aplikacije
static const char *version = "baRadio v2.2, 2025";

// Forward deklaracije
static void on_gst_message(GstBus *bus, GstMessage *msg, gpointer user_data);
static const char *get_db_path();
static void save_last_played(const char *name);
static void refresh_active_station_color();
static void play_station(const char *name);
static void load_station_urls();
static void update_play_item_label();
static void on_tray_menu_show(GtkWidget *menu, gpointer user_data);
static void update_current_playing_item();
// Settings / favorites helpers
static char *get_setting(const char *key);
static void set_setting(const char *key, const char *value);
void on_toggle_favorite(GtkMenuItem *item, gpointer user_data);
static void on_fav_toggle_clicked(GtkButton *button, gpointer user_data);
// Globals for favorite filter
static gboolean favorite_filter_enabled = FALSE;
static GtkWidget *fav_toggle_button = NULL;

// Extern deklaracije za globalne spremenljivke (definirane nižje v kodi)
extern GstElement *pipeline;
extern GtkWidget *label;
extern char current_song[256];
extern char current_station[512];
extern GtkWidget *treeview;
extern GtkWidget *main_window;

// Globalna hash tabela za hitrejši dostop do URL-jev
GHashTable *station_urls = NULL;

// --- MPRIS D-Bus --- //
static GDBusNodeInfo *introspection_data = NULL;
static const gchar introspection_xml[] =
    "<node>\n"
    "  <interface name='org.mpris.MediaPlayer2'>\n"
    "    <method name='Raise'/>\n"
    "    <method name='Quit'/>\n"
    "    <property name='CanQuit' type='b' access='read'/>\n"
    "    <property name='CanRaise' type='b' access='read'/>\n"
    "    <property name='HasTrackList' type='b' access='read'/>\n"
    "    <property name='Identity' type='s' access='read'/>\n"
    "    <property name='SupportedUriSchemes' type='as' access='read'/>\n"
    "    <property name='SupportedMimeTypes' type='as' access='read'/>\n"
    "  </interface>\n"
    "  <interface name='org.mpris.MediaPlayer2.Player'>\n"
    "    <method name='Play'/>\n"
    "    <method name='Pause'/>\n"
    "    <method name='PlayPause'/>\n"
    "    <method name='Stop'/>\n"
    "    <method name='Next'/>\n"
    "    <method name='Previous'/>\n"
    "    <property name='PlaybackStatus' type='s' access='read'/>\n"
    "  </interface>\n"
    "</node>\n";

static void mpris_handle_method_call(GDBusConnection *conn, const gchar *sender, const gchar *obj_path, const gchar *iface, const gchar *method, GVariant *params, GDBusMethodInvocation *invocation, gpointer user_data) {
    (void)conn; (void)sender; (void)obj_path; (void)params; (void)user_data;
    if (g_strcmp0(iface, "org.mpris.MediaPlayer2.Player") == 0) {
        if (g_strcmp0(method, "Play") == 0 || g_strcmp0(method, "Pause") == 0 || g_strcmp0(method, "PlayPause") == 0 || g_strcmp0(method, "Stop") == 0) {
            // Vsi ti ukazi naj sprožijo on_play_clicked (toggle play/stop)
            on_play_clicked(NULL, NULL);
            g_dbus_method_invocation_return_value(invocation, NULL);
        } else if (g_strcmp0(method, "Next") == 0) {
            // Naslednja postaja
            GtkTreeModel *filter_model = GTK_TREE_MODEL(gtk_tree_view_get_model(GTK_TREE_VIEW(treeview)));
            GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
            GtkTreeIter iter;
            if (gtk_tree_selection_get_selected(selection, NULL, &iter)) {
                if (gtk_tree_model_iter_next(filter_model, &iter)) {
                    GtkTreePath *path = gtk_tree_model_get_path(filter_model, &iter);
                    gtk_tree_view_set_cursor(GTK_TREE_VIEW(treeview), path, NULL, FALSE);
                    gtk_tree_view_row_activated(GTK_TREE_VIEW(treeview), path, NULL);
                    gtk_tree_path_free(path);
                }
            }
            g_dbus_method_invocation_return_value(invocation, NULL);
        } else if (g_strcmp0(method, "Previous") == 0) {
            // Prejšnja postaja
            GtkTreeModel *filter_model = GTK_TREE_MODEL(gtk_tree_view_get_model(GTK_TREE_VIEW(treeview)));
            GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
            GtkTreeIter iter;
            GtkTreePath *path = NULL;
            if (gtk_tree_selection_get_selected(selection, NULL, &iter)) {
                path = gtk_tree_model_get_path(filter_model, &iter);
                if (path && gtk_tree_path_prev(path)) {
                    gtk_tree_view_set_cursor(GTK_TREE_VIEW(treeview), path, NULL, FALSE);
                    gtk_tree_view_row_activated(GTK_TREE_VIEW(treeview), path, NULL);
                }
                if (path) gtk_tree_path_free(path);
            }
            g_dbus_method_invocation_return_value(invocation, NULL);
        }
    } else if (g_strcmp0(iface, "org.mpris.MediaPlayer2") == 0) {
        if (g_strcmp0(method, "Raise") == 0) {
            if (main_window) {
                gtk_window_present(GTK_WINDOW(main_window));
            }
            g_dbus_method_invocation_return_value(invocation, NULL);
        } else if (g_strcmp0(method, "Quit") == 0) {
            gtk_main_quit();
            g_dbus_method_invocation_return_value(invocation, NULL);
        }
    }
}

static GVariant *mpris_handle_get_property(GDBusConnection *conn, const gchar *sender, const gchar *obj_path, const gchar *iface, const gchar *prop, GError **error, gpointer user_data) {
    (void)conn; (void)sender; (void)obj_path; (void)error; (void)user_data;
    if (g_strcmp0(iface, "org.mpris.MediaPlayer2.Player") == 0) {
        if (g_strcmp0(prop, "PlaybackStatus") == 0) {
            if (pipeline) {
                GstState state;
                gst_element_get_state(pipeline, &state, NULL, 0);
                if (state == GST_STATE_PLAYING) return g_variant_new_string("Playing");
                if (state == GST_STATE_PAUSED) return g_variant_new_string("Paused");
            }
            return g_variant_new_string("Stopped");
        }
    } else if (g_strcmp0(iface, "org.mpris.MediaPlayer2") == 0) {
        if (g_strcmp0(prop, "CanQuit") == 0) return g_variant_new_boolean(TRUE);
        if (g_strcmp0(prop, "CanRaise") == 0) return g_variant_new_boolean(TRUE);
        if (g_strcmp0(prop, "HasTrackList") == 0) return g_variant_new_boolean(FALSE);
    if (g_strcmp0(prop, "Identity") == 0) return g_variant_new_string(version);
        if (g_strcmp0(prop, "SupportedUriSchemes") == 0) return g_variant_new_strv(NULL, 0);
        if (g_strcmp0(prop, "SupportedMimeTypes") == 0) return g_variant_new_strv(NULL, 0);
    }
    return NULL;
}

static const gchar *mpris_obj_path = "/org/mpris/MediaPlayer2";

static void mpris_register(void) {
    introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, NULL);
    GError *error = NULL;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!conn) {
        fprintf(stderr, "Napaka pri povezavi na D-Bus: %s\n", error->message);
        g_error_free(error);
        return;
    }
    (void)g_dbus_connection_register_object(
        conn,
        mpris_obj_path,
        introspection_data->interfaces[0],
        &(GDBusInterfaceVTable){
            .method_call = mpris_handle_method_call,
            .get_property = mpris_handle_get_property,
        },
        NULL, NULL, NULL
    );
    (void)g_dbus_connection_register_object(
        conn,
        mpris_obj_path,
        introspection_data->interfaces[1],
        &(GDBusInterfaceVTable){
            .method_call = mpris_handle_method_call,
            .get_property = mpris_handle_get_property,
        },
        NULL, NULL, NULL
    );
    // Registriraj ime na D-Bus
    g_bus_own_name(G_BUS_TYPE_SESSION, "org.mpris.MediaPlayer2.baradio", G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL, NULL);
}

// Vrne polno pot do baze v ~/.config/baradio/baradio.db in poskrbi, da mapa obstaja
static const char *get_db_path() {
    static char db_path[512] = "";
    if (db_path[0] == '\0') {
        const char *home = getenv("HOME");
        if (!home) home = ".";
        snprintf(db_path, sizeof(db_path), "%s/.config/baradio", home);
        // Ustvari mapo, če ne obstaja
        struct stat st = {0};
        if (stat(db_path, &st) == -1) {
            mkdir(db_path, 0700);
        }
        // Dodaj ime baze
        strncat(db_path, "/baradio.db", sizeof(db_path) - strlen(db_path) - 1);
    }
    return db_path;
}

// Vrne polno pot do lock datoteke ~/.config/baradio/baradio.lock
static const char *get_lock_path() {
    static char lock_path[512] = "";
    if (lock_path[0] == '\0') {
        const char *home = getenv("HOME");
        if (!home) home = ".";
        /* Ustvari mapo ~/.config/baradio, če ne obstaja, nato sestavi lock_path */
        char dir_path[512];
        snprintf(dir_path, sizeof(dir_path), "%s/.config/baradio", home);
        struct stat st = {0};
        if (stat(dir_path, &st) == -1) {
            mkdir(dir_path, 0700);
        }
        /* Sestavimo lock_path v enem klicu - varno */
        int r = snprintf(lock_path, sizeof(lock_path), "%s/baradio.lock", dir_path);
        if (r < 0 || (size_t)r >= sizeof(lock_path)) {
            /* Če je prišlo do trimanja, zagotovimo terminacijo in uporabimo zadnjih 511 znakov */
            lock_path[sizeof(lock_path)-1] = '\0';
        }
    }
    return lock_path;
}

// Simple settings helpers (store small key/value pairs in settings table)
static char *get_setting(const char *key) {
    sqlite3 *db;
    char *result = NULL;
    if (sqlite3_open(get_db_path(), &db) != SQLITE_OK) return NULL;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "SELECT value FROM settings WHERE key = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *val = sqlite3_column_text(stmt, 0);
            if (val) result = g_strdup((const char*)val);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return result;
}

static void set_setting(const char *key, const char *value) {
    sqlite3 *db;
    if (sqlite3_open(get_db_path(), &db) != SQLITE_OK) return;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "REPLACE INTO settings (key, value) VALUES (?, ?);", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
}

// Naloži ikono iz teme kot GdkPixbuf velikosti 'size'
static GdkPixbuf *load_icon_pixbuf(const char *icon_name, int size) {
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    GError *err = NULL;
    GdkPixbuf *pb = gtk_icon_theme_load_icon(theme, icon_name, size, 0, &err);
    if (!pb) {
        if (err) g_clear_error(&err);
        return NULL;
    }
    return pb;
}

// Recolor pixbuf: nastavi RGB del slikovnih pik (ohrani alpha)
static GdkPixbuf *recolor_pixbuf(GdkPixbuf *src, GdkRGBA color) {
    if (!src) return NULL;
    int w = gdk_pixbuf_get_width(src);
    int h = gdk_pixbuf_get_height(src);
    int n = gdk_pixbuf_get_n_channels(src);
    int rowstride = gdk_pixbuf_get_rowstride(src);
    gboolean has_alpha = gdk_pixbuf_get_has_alpha(src);
    if (n < 3) return NULL;
    GdkPixbuf *dst = gdk_pixbuf_copy(src);
    guchar *pixels = gdk_pixbuf_get_pixels(dst);
    for (int y = 0; y < h; y++) {
        guchar *p = pixels + y * rowstride;
        for (int x = 0; x < w; x++) {
            guchar a = has_alpha ? p[3] : 255;
            if (a != 0) {
                p[0] = (guchar)(color.red * 255.0);
                p[1] = (guchar)(color.green * 255.0);
                p[2] = (guchar)(color.blue * 255.0);
            }
            p += n;
        }
    }
    return dst;
}

// Vrne GtkImage za gumb priljubljenih glede na stanje (enabled -> rumena filled icon)
static GtkWidget *get_fav_image_for_state(gboolean enabled) {
    const int size = 20; // velikost ikone v px
    // poskusi naložiti symbolic emblem
    GdkPixbuf *pb = load_icon_pixbuf("emblem-favorite-symbolic", size);
    if (!pb) pb = load_icon_pixbuf("emblem-favorite", size);
    if (!pb) {
        // fallback na ime ikone (gtk will handle)
        return gtk_image_new_from_icon_name("emblem-favorite-symbolic", GTK_ICON_SIZE_BUTTON);
    }
    if (enabled) {
        GdkRGBA yellow = {0};
        yellow.red = 1.0; yellow.green = 0.84; yellow.blue = 0.0; yellow.alpha = 1.0;
        GdkPixbuf *col = recolor_pixbuf(pb, yellow);
        g_object_unref(pb);
        if (col) {
            GtkWidget *img = gtk_image_new_from_pixbuf(col);
            g_object_unref(col);
            return img;
        }
    }
    // disabled: vrni original (symbolic) kot image
    GtkWidget *img = gtk_image_new_from_pixbuf(pb);
    g_object_unref(pb);
    return img;
}

// Preveri ali je proces s podanim PID še živ in ali je to naša aplikacija
static int is_pid_running_and_ours(pid_t pid, const char *expected_cmdline) {
    if (pid <= 0) return 0;
    char proc_cmdline[512] = "";
    snprintf(proc_cmdline, sizeof(proc_cmdline), "/proc/%d/cmdline", pid);
    FILE *f = fopen(proc_cmdline, "r");
    if (!f) return 0;
    char cmdline[512] = "";
    size_t n = fread(cmdline, 1, sizeof(cmdline)-1, f);
    fclose(f);
    if (n == 0) return 0;
    // cmdline je null-separated, expected_cmdline je argv[0] ali /proc/self/exe
    if (strstr(cmdline, expected_cmdline) != NULL) return 1;
    return 0;
}

// Ustvari lock file z PID-om in cmdline, vrne 1 če uspe, 0 če že obstaja in je proces še živ in je naša aplikacija
static int create_lock_file() {
    const char *lock_path = get_lock_path();
    FILE *f = fopen(lock_path, "r");
    pid_t old_pid = 0;
    char old_cmdline[512] = "";
    int found = 0;
    if (f) {
        // Preberi PID in cmdline iz lock file
        if (fscanf(f, "%d\n%511[^\n]", &old_pid, old_cmdline) >= 1) {
            found = 1;
        }
        fclose(f);
    }
    // Pridobi trenutni cmdline (realpath do /proc/self/exe)
    char self_cmdline[512] = "";
    ssize_t len = readlink("/proc/self/exe", self_cmdline, sizeof(self_cmdline)-1);
    if (len > 0) self_cmdline[len] = '\0';
    else strncpy(self_cmdline, "baradio", sizeof(self_cmdline)-1);

    if (found && is_pid_running_and_ours(old_pid, old_cmdline)) {
        // Že teče naša instanca
        return 0;
    }
    // Zapiši svoj PID in cmdline
    f = fopen(lock_path, "w");
    if (!f) return 0;
    fprintf(f, "%d\n%s\n", getpid(), self_cmdline);
    fclose(f);
    return 1;
}

// Pobriši lock file
static void remove_lock_file() {
    const char *lock_path = get_lock_path();
    unlink(lock_path);
}

// Forward deklaracije za funkcije, ki jih uporabljajo handlerji menija
void fill_station_store(GtkListStore *store, const char *filter);
static char *load_last_played();
static void clear_last_played();

// Forward deklaracije za kontrolne gumbe
void on_play_clicked(GtkButton *button, gpointer user_data);
// static void on_pause_clicked(GtkButton *button, gpointer user_data);
// static void on_stop_clicked(GtkButton *button, gpointer user_data);
static void on_next_clicked(GtkButton *button, gpointer user_data);
static void on_previous_clicked(GtkButton *button, gpointer user_data);

// --- HANDLERJI ZA KONTEKSTNI MENI --- //
static void on_add_station(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *treeview = GTK_WIDGET(user_data);
    // Prikaži lep dialog za dodajanje
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Dodaj postajo", NULL, GTK_DIALOG_MODAL, ("Prekliči"), GTK_RESPONSE_CANCEL, ("Shrani"), GTK_RESPONSE_OK, NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 16);

    // Velik naslov
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<span size='large' weight='bold'>Dodaj postajo</span>");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(title), 0.5f);
    gtk_box_pack_start(GTK_BOX(content), title, FALSE, FALSE, 8);

    // Opis
    GtkWidget *desc = gtk_label_new("Vnesi podatke nove postaje:");
    gtk_label_set_xalign(GTK_LABEL(desc), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(desc), 0.5f);
    gtk_box_pack_start(GTK_BOX(content), desc, FALSE, FALSE, 8);

    // Vnosna polja v boxu
    GtkWidget *form_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *name_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *url_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget *name_label = gtk_label_new("Ime:");
    gtk_widget_set_size_request(name_label, 60, -1);
    GtkWidget *name_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(name_entry), 32);
    gtk_box_pack_start(GTK_BOX(name_box), name_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(name_box), name_entry, TRUE, TRUE, 0);

    GtkWidget *url_label = gtk_label_new("URL:");
    gtk_widget_set_size_request(url_label, 60, -1);
    GtkWidget *url_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(url_entry), 32);
    gtk_box_pack_start(GTK_BOX(url_box), url_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(url_box), url_entry, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(form_box), name_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(form_box), url_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), form_box, FALSE, FALSE, 12);

    // Nastavi Shrani kot privzeti gumb
    GtkWidget *save_btn = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_widget_grab_default(save_btn);

    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(name_entry);
    int resp = gtk_dialog_run(GTK_DIALOG(dialog));
    if (resp == GTK_RESPONSE_OK) {
        const char *new_name = gtk_entry_get_text(GTK_ENTRY(name_entry));
        const char *new_url = gtk_entry_get_text(GTK_ENTRY(url_entry));
        if (strlen(new_name) == 0 || strlen(new_url) == 0) {
            GtkWidget *md = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "Ime in URL ne smeta biti prazna!");
            gtk_dialog_run(GTK_DIALOG(md));
            gtk_widget_destroy(md);
        } else {
            // Vstavi v bazo
            sqlite3 *db;
            if (sqlite3_open(get_db_path(), &db) == SQLITE_OK) {
                sqlite3_stmt *stmt;
                if (sqlite3_prepare_v2(db, "INSERT INTO stations (name, url) VALUES (?, ?);", -1, &stmt, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 2, new_url, -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(stmt) != SQLITE_DONE) {
                        GtkWidget *md = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Napaka pri shranjevanju postaje!");
                        gtk_dialog_run(GTK_DIALOG(md));
                        gtk_widget_destroy(md);
                    }
                    sqlite3_finalize(stmt);
                }
                sqlite3_close(db);
            }
            // Osveži seznam
            GtkTreeModel *filter_model = gtk_tree_view_get_model(GTK_TREE_VIEW(treeview));
            fill_station_store(GTK_LIST_STORE(gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(filter_model))), NULL);
            load_station_urls();  // Posodobi hash tabelo
            gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(filter_model));
            // Osveži ikono aktivne postaje (če predvaja)
            refresh_active_station_color();
            // Izberi novo postajo in nastavi fokus
            GtkTreeIter iter;
            
            gboolean valid = gtk_tree_model_get_iter_first(filter_model, &iter);
            while (valid) {
                gchar *name = NULL;
                gtk_tree_model_get(filter_model, &iter, 1, &name, -1);
                if (name && strcmp(name, new_name) == 0) {
                    GtkTreePath *path = gtk_tree_model_get_path(filter_model, &iter);
                    gtk_tree_view_set_cursor(GTK_TREE_VIEW(treeview), path, NULL, FALSE);
                    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(treeview), path, NULL, TRUE, 0.5, 0.0);
                    gtk_tree_path_free(path);
                    g_free(name);
                    break;
                }
                g_free(name);
                valid = gtk_tree_model_iter_next(filter_model, &iter);
            }
            // Nastavi fokus na treeview
            gtk_widget_grab_focus(treeview);
        }
    }
    gtk_widget_destroy(dialog);
}

static void on_edit_station(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *treeview = GTK_WIDGET(user_data);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
    GtkTreeModel *filter_model = gtk_tree_view_get_model(GTK_TREE_VIEW(treeview));
    GtkTreeIter filter_iter;
    if (!gtk_tree_selection_get_selected(selection, NULL, &filter_iter)) {
        GtkWidget *md = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Najprej izberi postajo!");
        gtk_dialog_run(GTK_DIALOG(md));
        gtk_widget_destroy(md);
        return;
    }
    // Pridobi ime iz filter_model (stolpec 1, ker je stolpec 0 ikona)
    gchar *name = NULL;
    gtk_tree_model_get(filter_model, &filter_iter, 1, &name, -1);
    // Pridobi URL iz baze
    char url[512] = "";
    sqlite3 *db;
    if (sqlite3_open(get_db_path(), &db) == SQLITE_OK) {
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, "SELECT url FROM stations WHERE name = ?;", -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *url_db = (const char *)sqlite3_column_text(stmt, 0);
                strncpy(url, url_db, sizeof(url)-1);
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }
    // Prikaži lepši dialog za urejanje
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Uredi postajo", NULL, GTK_DIALOG_MODAL, ("Prekliči"), GTK_RESPONSE_CANCEL, ("Shrani"), GTK_RESPONSE_OK, NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 16);

    // Velik naslov
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<span size='large' weight='bold'>Uredi postajo</span>");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(title), 0.5f);
    gtk_box_pack_start(GTK_BOX(content), title, FALSE, FALSE, 8);

    // Opis
    GtkWidget *desc = gtk_label_new("Spremeni podatke postaje:");
    gtk_label_set_xalign(GTK_LABEL(desc), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(desc), 0.5f);
    gtk_box_pack_start(GTK_BOX(content), desc, FALSE, FALSE, 8);

    // Vnosna polja v boxu
    GtkWidget *form_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *name_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *url_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget *name_label = gtk_label_new("Ime:");
    gtk_widget_set_size_request(name_label, 60, -1);
    GtkWidget *name_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(name_entry), name);
    gtk_entry_set_width_chars(GTK_ENTRY(name_entry), 32);
    gtk_box_pack_start(GTK_BOX(name_box), name_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(name_box), name_entry, TRUE, TRUE, 0);

    GtkWidget *url_label = gtk_label_new("URL:");
    gtk_widget_set_size_request(url_label, 60, -1);
    GtkWidget *url_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(url_entry), url);
    gtk_entry_set_width_chars(GTK_ENTRY(url_entry), 32);
    gtk_box_pack_start(GTK_BOX(url_box), url_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(url_box), url_entry, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(form_box), name_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(form_box), url_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), form_box, FALSE, FALSE, 12);

    // Nastavi Shrani kot privzeti gumb
    GtkWidget *save_btn = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_widget_grab_default(save_btn);

    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(name_entry);
    int resp = gtk_dialog_run(GTK_DIALOG(dialog));
    if (resp == GTK_RESPONSE_OK) {
        const char *new_name = gtk_entry_get_text(GTK_ENTRY(name_entry));
        const char *new_url = gtk_entry_get_text(GTK_ENTRY(url_entry));
        if (strlen(new_name) == 0 || strlen(new_url) == 0) {
            GtkWidget *md = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "Ime in URL ne smeta biti prazna!");
            gtk_dialog_run(GTK_DIALOG(md));
            gtk_widget_destroy(md);
        } else {
            // Posodobi bazo
            if (sqlite3_open(get_db_path(), &db) == SQLITE_OK) {
                sqlite3_stmt *stmt;
                if (sqlite3_prepare_v2(db, "UPDATE stations SET name = ?, url = ? WHERE name = ?;", -1, &stmt, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 2, new_url, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(stmt) != SQLITE_DONE) {
                        GtkWidget *md = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Napaka pri shranjevanju sprememb!");
                        gtk_dialog_run(GTK_DIALOG(md));
                        gtk_widget_destroy(md);
                    }
                    sqlite3_finalize(stmt);
                }
                sqlite3_close(db);
            }
            // Če je bila to zadnja predvajana postaja, posodobi tudi last_played
            char *last = load_last_played();
            if (last && strcmp(last, name) == 0) {
                save_last_played(new_name);
            }
            if (last) g_free(last);
            // Osveži seznam
            GtkTreeModel *filter_model = gtk_tree_view_get_model(GTK_TREE_VIEW(treeview));
            fill_station_store(GTK_LIST_STORE(gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(filter_model))), NULL);
            load_station_urls();  // Posodobi hash tabelo
            gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(filter_model));
            // Osveži ikono aktivne postaje (če predvaja)
            refresh_active_station_color();
            // Ponovno izberi urejeno postajo (z novim imenom) in nastavi fokus
            GtkTreeIter iter;
            
            gboolean valid = gtk_tree_model_get_iter_first(filter_model, &iter);
            while (valid) {
                gchar *iter_name = NULL;
                gtk_tree_model_get(filter_model, &iter, 1, &iter_name, -1);
                if (iter_name && strcmp(iter_name, new_name) == 0) {
                    GtkTreePath *path = gtk_tree_model_get_path(filter_model, &iter);
                    gtk_tree_view_set_cursor(GTK_TREE_VIEW(treeview), path, NULL, FALSE);
                    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(treeview), path, NULL, TRUE, 0.5, 0.0);
                    gtk_tree_path_free(path);
                    g_free(iter_name);
                    break;
                }
                g_free(iter_name);
                valid = gtk_tree_model_iter_next(filter_model, &iter);
            }
            // Nastavi fokus na treeview
            gtk_widget_grab_focus(treeview);
        }
    }
    gtk_widget_destroy(dialog);
    g_free(name);
}

static void on_delete_station(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *treeview = GTK_WIDGET(user_data);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
    GtkTreeModel *filter_model = gtk_tree_view_get_model(GTK_TREE_VIEW(treeview));
    GtkTreeIter filter_iter;
    if (!gtk_tree_selection_get_selected(selection, NULL, &filter_iter)) {
        GtkWidget *md = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Najprej izberi postajo!");
        gtk_dialog_run(GTK_DIALOG(md));
        gtk_widget_destroy(md);
        return;
    }
    // Pridobi ime iz filter_model (stolpec 1, ker je stolpec 0 ikona)
    gchar *name = NULL;
    gtk_tree_model_get(filter_model, &filter_iter, 1, &name, -1);
    // Potrditveno vprašanje
    gchar *msg = g_strdup_printf("Ali res želiš izbrisati postajo '%s'?", name);
    GtkWidget *confirm = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, "%s", msg);
    g_free(msg);
    int resp = gtk_dialog_run(GTK_DIALOG(confirm));
    gtk_widget_destroy(confirm);
    if (resp == GTK_RESPONSE_YES) {
        // Izbriši iz baze
        sqlite3 *db;
    if (sqlite3_open(get_db_path(), &db) == SQLITE_OK) {
            sqlite3_stmt *stmt;
            if (sqlite3_prepare_v2(db, "DELETE FROM stations WHERE name = ?;", -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
            sqlite3_close(db);
        }
        // Če je bila to zadnja predvajana postaja, pobriši last_played
        char *last = load_last_played();
        if (last && strcmp(last, name) == 0) {
            clear_last_played();
        }
        if (last) g_free(last);
        // Poišči vrstico nad izbrisano
    GtkTreePath *deleted_path = NULL;
    GtkTreeIter iter, prev_iter;
    gboolean prev_valid = FALSE;
        gboolean valid = gtk_tree_model_get_iter_first(filter_model, &iter);
        while (valid) {
            gchar *row_name = NULL;
            gtk_tree_model_get(filter_model, &iter, 1, &row_name, -1);
            if (row_name && strcmp(row_name, name) == 0) {
                deleted_path = gtk_tree_model_get_path(filter_model, prev_valid ? &prev_iter : &iter);
                g_free(row_name);
                break;
            }
            if (prev_valid) prev_valid = FALSE;
            prev_iter = iter;
            prev_valid = TRUE;
            g_free(row_name);
            valid = gtk_tree_model_iter_next(filter_model, &iter);
        }
        // Osveži seznam
        fill_station_store(GTK_LIST_STORE(gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(filter_model))), NULL);
        load_station_urls();  // Posodobi hash tabelo
        gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(filter_model));
        // Osveži ikono aktivne postaje (če predvaja)
        refresh_active_station_color();
        // Nastavi fokus na vrstico nad izbrisano (če obstaja)
        if (deleted_path) {
            gtk_tree_view_set_cursor(GTK_TREE_VIEW(treeview), deleted_path, NULL, FALSE);
            gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(treeview), deleted_path, NULL, TRUE, 0.5, 0.0);
            gtk_tree_path_free(deleted_path);
        }
    }
    g_free(name);
}

// Handler za desni klik na treeview
gboolean on_treeview_button_press(GtkWidget *treeview, GdkEventButton *event, gpointer user_data) {
    (void)user_data;
    if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
        GtkWidget *menu = gtk_menu_new();
        // Pred desnim klikom poskrbimo, da je vrstica pod kazalcem izbrana
        int x = (int)event->x;
        int y = (int)event->y;
        GtkTreePath *path = NULL;
        GtkTreeViewColumn *col = NULL;
        if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview), x, y, &path, &col, NULL, NULL)) {
            GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
            gtk_tree_selection_select_path(sel, path);
            gtk_tree_path_free(path);
        }
        GtkWidget *add_item = gtk_menu_item_new_with_label("Dodaj postajo");
        GtkWidget *edit_item = gtk_menu_item_new_with_label("Uredi postajo");
        GtkWidget *delete_item = gtk_menu_item_new_with_label("Izbriši postajo");
        // Dodaj opcijo za priljubljene glede na trenutno stanje izbrane vrstice
        GtkWidget *fav_item = NULL;
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
        GtkTreeModel *filter_model = GTK_TREE_MODEL(gtk_tree_view_get_model(GTK_TREE_VIEW(treeview)));
        GtkTreeIter iter;
        if (gtk_tree_selection_get_selected(selection, NULL, &iter)) {
            gchar *fav = NULL;
            gtk_tree_model_get(filter_model, &iter, 3, &fav, -1);
            if (fav && strlen(fav) > 0) fav_item = gtk_menu_item_new_with_label("Odstrani iz priljubljenih");
            else fav_item = gtk_menu_item_new_with_label("Dodaj v priljubljene");
            g_free(fav);
        }
        g_signal_connect(add_item, "activate", G_CALLBACK(on_add_station), treeview);
        g_signal_connect(edit_item, "activate", G_CALLBACK(on_edit_station), treeview);
        g_signal_connect(delete_item, "activate", G_CALLBACK(on_delete_station), treeview);
        if (fav_item) g_signal_connect(fav_item, "activate", G_CALLBACK(on_toggle_favorite), treeview);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), add_item);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), edit_item);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), delete_item);
        if (fav_item) gtk_menu_shell_append(GTK_MENU_SHELL(menu), fav_item);
        gtk_widget_show_all(menu);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent*)event);
        return TRUE;
    }
    return FALSE;
}

// Toggle favorite for currently selected station
void on_toggle_favorite(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *treeview = GTK_WIDGET(user_data);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
    GtkTreeModel *filter_model = GTK_TREE_MODEL(gtk_tree_view_get_model(GTK_TREE_VIEW(treeview)));
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(selection, NULL, &iter)) return;
    gchar *name = NULL;
    gtk_tree_model_get(filter_model, &iter, 1, &name, -1);
    if (!name) return;
    /* Shrani indeks izbrane vrstice in trenutno stanje filtra preden spremenimo DB */
    int old_index = -1;
    GtkTreePath *old_path = gtk_tree_model_get_path(filter_model, &iter);
    if (old_path) {
        int depth = gtk_tree_path_get_depth(old_path);
        int *indices = gtk_tree_path_get_indices(old_path);
        if (indices && depth > 0) old_index = indices[0];
        gtk_tree_path_free(old_path);
    }
    gboolean viewing_only_favs = favorite_filter_enabled;
    // Toggle value in DB
    sqlite3 *db;
    int newfav = -1;
    if (sqlite3_open(get_db_path(), &db) == SQLITE_OK) {
        sqlite3_stmt *stmt;
        // Preberi trenutno vrednost
        if (sqlite3_prepare_v2(db, "SELECT favorite FROM stations WHERE name = ? LIMIT 1;", -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
            int fav = 0;
            if (sqlite3_step(stmt) == SQLITE_ROW) fav = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
            newfav = fav ? 0 : 1;
            if (sqlite3_prepare_v2(db, "UPDATE stations SET favorite = ? WHERE name = ?;", -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, newfav);
                sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }
        sqlite3_close(db);
    }
    // Osveži store in filter
    fill_station_store(GTK_LIST_STORE(gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(filter_model))), NULL);
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(filter_model));
    // Osveži UI
    refresh_active_station_color();

    /* Če smo odstranili priljubljenost medtem ko smo gledali samo priljubljene, element izgine iz modela:
       v tem primeru izberemo vrstico nad staro pozicijo (old_index - 1) če obstaja, sicer počistimo selekcijo.
       V vseh drugih primerih poskušamo ponovno izbrati isto postajo po imenu. */
    if (viewing_only_favs && newfav == 0) {
        // poskusi izbrati vrstico nad prejšnjo
        if (old_index > 0) {
            GtkTreeIter prev_iter;
            if (gtk_tree_model_iter_nth_child(filter_model, &prev_iter, NULL, old_index - 1)) {
                GtkTreePath *path = gtk_tree_model_get_path(filter_model, &prev_iter);
                gtk_tree_view_set_cursor(GTK_TREE_VIEW(treeview), path, NULL, FALSE);
                gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(treeview), path, NULL, TRUE, 0.5, 0.0);
                gtk_tree_path_free(path);
            } else {
                // Ni prejšnje vrstice - počistimo selekcijo
                gtk_tree_selection_unselect_all(selection);
            }
        } else {
            // bila je prva vrstica - izbrišemo selekcijo
            gtk_tree_selection_unselect_all(selection);
        }
    } else {
        // poskusi ponovno izbrati isto postajo (deluje tako pri dodajanju kot pri odstranitvi, če smo v glavnem seznamu)
        GtkTreeIter re_iter;
        gboolean found = FALSE;
        gboolean valid = gtk_tree_model_get_iter_first(filter_model, &re_iter);
        while (valid) {
            gchar *row_name = NULL;
            gtk_tree_model_get(filter_model, &re_iter, 1, &row_name, -1);
            if (row_name && strcmp(row_name, name) == 0) {
                GtkTreePath *path = gtk_tree_model_get_path(filter_model, &re_iter);
                gtk_tree_view_set_cursor(GTK_TREE_VIEW(treeview), path, NULL, FALSE);
                gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(treeview), path, NULL, TRUE, 0.5, 0.0);
                gtk_tree_path_free(path);
                g_free(row_name);
                found = TRUE;
                break;
            }
            g_free(row_name);
            valid = gtk_tree_model_iter_next(filter_model, &re_iter);
        }
        if (!found) {
            // če ne najdemo (npr. redka situacija), poskusimo izbrati prvo vrstico
            GtkTreeIter first_iter;
            if (gtk_tree_model_get_iter_first(filter_model, &first_iter)) {
                GtkTreePath *path = gtk_tree_model_get_path(filter_model, &first_iter);
                gtk_tree_view_set_cursor(GTK_TREE_VIEW(treeview), path, NULL, FALSE);
                gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(treeview), path, NULL, TRUE, 0.5, 0.0);
                gtk_tree_path_free(path);
            }
        }
    }
    g_free(name);
}

// Handler za klik na headerbar favorite toggle
static void on_fav_toggle_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    favorite_filter_enabled = !favorite_filter_enabled;
    // Shrani v settings
    set_setting("show_favorites_only", favorite_filter_enabled ? "1" : "0");
    // Posodobi ikono gumba (lahko uporabimo isti simbol, tu ne spreminjamo ikone barve)
    if (fav_toggle_button) {
        GtkWidget *img = get_fav_image_for_state(favorite_filter_enabled);
        gtk_button_set_image(GTK_BUTTON(fav_toggle_button), img);
    }
    // Refiltiraj model
    GtkTreeModel *filter_model = GTK_TREE_MODEL(gtk_tree_view_get_model(GTK_TREE_VIEW(treeview)));
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(filter_model));
}

static GtkWidget *global_filter_entry = NULL;

// Globalne spremenljivke za kontrolne gumbe
static GtkWidget *play_button = NULL;
//static GtkWidget *pause_button = NULL; // ni več v rabi

// --- KONTROLNI GUMBI --- //

void on_play_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    if (pipeline) {
        // Če že predvaja, ustavi (kot je bila logika v stop)
        {
            GstBus *bus = gst_element_get_bus(pipeline);
            if (bus) {
                gst_bus_remove_signal_watch(bus);
                gst_object_unref(bus);
            }
        }
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        pipeline = NULL;
        current_station[0] = '\0';
        current_song[0] = '\0';
        gtk_label_set_text(GTK_LABEL(label), "Ni predvajanja...");
        // Odstrani barvo iz vseh postaj
        GtkTreeModel *filter_model = GTK_TREE_MODEL(gtk_tree_view_get_model(GTK_TREE_VIEW(treeview)));
        GtkTreeModel *model = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(filter_model));
        GtkTreeIter iter;
        gboolean valid = gtk_tree_model_get_iter_first(filter_model, &iter);
        while (valid) {
            GtkTreeIter child_iter;
            gtk_tree_model_filter_convert_iter_to_child_iter(GTK_TREE_MODEL_FILTER(filter_model), &child_iter, &iter);
            gtk_list_store_set(GTK_LIST_STORE(model), &child_iter, 0, "", -1);  // Odstrani ikono
            valid = gtk_tree_model_iter_next(filter_model, &iter);
        }
        // Nastavi ikono na Play
            if (play_button) {
                GtkWidget *img = gtk_image_new_from_icon_name("media-playback-start-symbolic", GTK_ICON_SIZE_BUTTON);
                gtk_button_set_image(GTK_BUTTON(play_button), img);
                gtk_widget_set_tooltip_text(play_button, "Predvajaj");
                update_play_item_label();
                update_current_playing_item();
            }
    } else {
        // Če pipeline ne obstaja, predvajaj izbrano postajo v treeview
        GtkTreeModel *filter_model = GTK_TREE_MODEL(gtk_tree_view_get_model(GTK_TREE_VIEW(treeview)));
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
        GtkTreeIter iter;
        if (gtk_tree_selection_get_selected(selection, NULL, &iter)) {
            gchar *name = NULL;
            gtk_tree_model_get(filter_model, &iter, 1, &name, -1);
            if (name) {
                play_station(name);
                g_free(name);
                // Nastavi ikono na Pause
                if (play_button) {
                    GtkWidget *img = gtk_image_new_from_icon_name("media-playback-pause-symbolic", GTK_ICON_SIZE_BUTTON);
                    gtk_button_set_image(GTK_BUTTON(play_button), img);
                    gtk_widget_set_tooltip_text(play_button, "Pavza");
                    update_play_item_label();
                    update_current_playing_item();
                }
            }
        }
    }
    gtk_widget_grab_focus(treeview);
}

// Funkcija za pavzo in stop odstranjena, logika bo prenesena v play

static void on_next_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    GtkTreeModel *filter_model = GTK_TREE_MODEL(gtk_tree_view_get_model(GTK_TREE_VIEW(treeview)));
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(selection, NULL, &iter)) {
        if (gtk_tree_model_iter_next(filter_model, &iter)) {
            GtkTreePath *path = gtk_tree_model_get_path(filter_model, &iter);
            gtk_tree_view_set_cursor(GTK_TREE_VIEW(treeview), path, NULL, FALSE);
            gtk_tree_view_row_activated(GTK_TREE_VIEW(treeview), path, NULL);
            gtk_tree_path_free(path);
        }
    }
    gtk_widget_grab_focus(treeview);
}

static void on_previous_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    GtkTreeModel *filter_model = GTK_TREE_MODEL(gtk_tree_view_get_model(GTK_TREE_VIEW(treeview)));
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
    GtkTreeIter iter;
    GtkTreePath *path = NULL;
    if (gtk_tree_selection_get_selected(selection, NULL, &iter)) {
        path = gtk_tree_model_get_path(filter_model, &iter);
        if (path && gtk_tree_path_prev(path)) {
            gtk_tree_view_set_cursor(GTK_TREE_VIEW(treeview), path, NULL, FALSE);
            gtk_tree_view_row_activated(GTK_TREE_VIEW(treeview), path, NULL);
        }
        if (path) gtk_tree_path_free(path);
    }
    gtk_widget_grab_focus(treeview);
}

// Pot do baze

// Pomožna funkcija za osvežitev barve aktivne postaje
static void refresh_active_station_color() {
    if (!pipeline || strlen(current_station) == 0) return;
    
    GtkTreeModel *filter_model = GTK_TREE_MODEL(gtk_tree_view_get_model(GTK_TREE_VIEW(treeview)));
    GtkTreeModel *model = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(filter_model));
    GtkTreeIter iter;
    // Iteriraj skozi base model
        gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    
    while (valid) {
        gchar *name = NULL;
        gtk_tree_model_get(model, &iter, 1, &name, -1);  // Stolpec 1 je ime
        
        if (name) {
            const char *url = g_hash_table_lookup(station_urls, name);
            // Če je URL enak trenutni postaji, nastavi ikono ▶, sicer odstrani ikono
            if (url && strcmp(url, current_station) == 0) {
                gtk_list_store_set(GTK_LIST_STORE(model), &iter, 0, "▶", -1);
            } else {
                gtk_list_store_set(GTK_LIST_STORE(model), &iter, 0, "", -1);
            }
            g_free(name);
        }
        valid = gtk_tree_model_iter_next(model, &iter);
    }
}

// Funkcije za zadnjo predvajano postajo
static void save_last_played(const char *name) {
    sqlite3 *db;
    if (sqlite3_open(get_db_path(), &db) != SQLITE_OK) return;
    sqlite3_exec(db, "DELETE FROM last_played;", 0, 0, 0);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "INSERT INTO last_played (name) VALUES (?);", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
}

static char *load_last_played() {
    sqlite3 *db;
    char *result = NULL;
    if (sqlite3_open(get_db_path(), &db) != SQLITE_OK) return NULL;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "SELECT name FROM last_played LIMIT 1;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(stmt, 0);
            if (name) result = g_strdup(name);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return result;
}

// Pobriši vnos iz tabele last_played
static void clear_last_played() {
    sqlite3 *db;
    if (sqlite3_open(get_db_path(), &db) != SQLITE_OK) return;
    sqlite3_exec(db, "DELETE FROM last_played;", 0, 0, 0);
    sqlite3_close(db);
}

// Globalni iskalni niz za filter
static char search_string[128] = "";

// Globalni handler za ESC na glavnem oknu
gboolean on_main_window_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    (void)widget;
    // Prestrezanje multimedijskih tipk
    switch (event->keyval) {
        case GDK_KEY_Escape:
            if (global_filter_entry) {
                gtk_entry_set_text(GTK_ENTRY(global_filter_entry), "");
                gtk_entry_set_placeholder_text(GTK_ENTRY(global_filter_entry), NULL); // odstrani placeholder
                gtk_widget_hide(global_filter_entry);
                search_string[0] = '\0';
                gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(user_data));
                gtk_widget_grab_focus(treeview); // Postavi fokus na treeview
                return TRUE;
            }
            break;
        case GDK_KEY_AudioPlay:
        case GDK_KEY_AudioPause:
            // Play/Pause toggle ali predvajaj izbrano postajo, če pipeline == NULL
            if (pipeline) {
                GstState state;
                gst_element_get_state(pipeline, &state, NULL, 0);
                if (state == GST_STATE_PLAYING) {
                    gst_element_set_state(pipeline, GST_STATE_PAUSED);
                    gtk_label_set_text(GTK_LABEL(label), "Pavza...");
                } else if (state == GST_STATE_PAUSED) {
                    gst_element_set_state(pipeline, GST_STATE_PLAYING);
                    gtk_label_set_text(GTK_LABEL(label), strlen(current_song) > 0 ? current_song : "Predvajanje...");
                    refresh_active_station_color(); // Osveži barvo aktivne postaje
                }
            } else {
                // Če pipeline ne obstaja, predvajaj izbrano postajo v treeview
                GtkTreeModel *filter_model = GTK_TREE_MODEL(gtk_tree_view_get_model(GTK_TREE_VIEW(treeview)));
                GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
                GtkTreeIter iter;
                if (gtk_tree_selection_get_selected(selection, NULL, &iter)) {
                    gchar *name = NULL;
                    gtk_tree_model_get(filter_model, &iter, 1, &name, -1);
                    if (name) {
                        play_station(name);
                        g_free(name);
                    }
                }
            }
            return TRUE;
        case GDK_KEY_AudioStop:
            // Stop
            if (pipeline) {
                GstBus *bus = gst_element_get_bus(pipeline);
                if (bus) {
                    gst_bus_remove_signal_watch(bus);
                    gst_object_unref(bus);
                }
                gst_element_set_state(pipeline, GST_STATE_NULL);
                gst_object_unref(pipeline);
                pipeline = NULL;
                current_station[0] = '\0';
                current_song[0] = '\0';
                gtk_label_set_text(GTK_LABEL(label), "Ni predvajanja...");
                // Odstrani ikone iz vseh postaj
                GtkTreeModel *filter_model = GTK_TREE_MODEL(gtk_tree_view_get_model(GTK_TREE_VIEW(treeview)));
                GtkTreeModel *model = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(filter_model));
                GtkTreeIter iter2;
                gboolean valid = gtk_tree_model_get_iter_first(filter_model, &iter2);
                while (valid) {
                    GtkTreeIter child_iter2;
                    gtk_tree_model_filter_convert_iter_to_child_iter(GTK_TREE_MODEL_FILTER(filter_model), &child_iter2, &iter2);
                    gtk_list_store_set(GTK_LIST_STORE(model), &child_iter2, 0, "", -1);  // Odstrani ikono
                    valid = gtk_tree_model_iter_next(filter_model, &iter2);
                }
            }
            return TRUE;
        case GDK_KEY_AudioNext:
        case GDK_KEY_AudioForward:
            // Naslednja postaja
            {
                GtkTreeModel *filter_model = GTK_TREE_MODEL(gtk_tree_view_get_model(GTK_TREE_VIEW(treeview)));
                GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
                GtkTreeIter iter;
                if (gtk_tree_selection_get_selected(selection, NULL, &iter)) {
                    if (gtk_tree_model_iter_next(filter_model, &iter)) {
                        GtkTreePath *path = gtk_tree_model_get_path(filter_model, &iter);
                        gtk_tree_view_set_cursor(GTK_TREE_VIEW(treeview), path, NULL, FALSE);
                        gtk_tree_view_row_activated(GTK_TREE_VIEW(treeview), path, NULL);
                        gtk_tree_path_free(path);
                    }
                }
            }
            return TRUE;
        case GDK_KEY_AudioPrev:
        case GDK_KEY_AudioRewind:
            // Prejšnja postaja
            {
                GtkTreeModel *filter_model = GTK_TREE_MODEL(gtk_tree_view_get_model(GTK_TREE_VIEW(treeview)));
                GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
                GtkTreeIter iter;
                GtkTreePath *path = NULL;
                if (gtk_tree_selection_get_selected(selection, NULL, &iter)) {
                    path = gtk_tree_model_get_path(filter_model, &iter);
                    if (path && gtk_tree_path_prev(path)) {
                        gtk_tree_view_set_cursor(GTK_TREE_VIEW(treeview), path, NULL, FALSE);
                        gtk_tree_view_row_activated(GTK_TREE_VIEW(treeview), path, NULL);
                    }
                    if (path) gtk_tree_path_free(path);
                }
            }
            return TRUE;
        default:
            break;
    }
    return FALSE;
}

// Handler za ESC v filter_entry
gboolean on_filter_entry_key_press(GtkWidget *entry, GdkEventKey *event, gpointer user_data) {
    if (event->keyval == GDK_KEY_Escape) {
        gtk_entry_set_text(GTK_ENTRY(entry), "");
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry), NULL); // odstrani placeholder
        gtk_widget_hide(entry);
        search_string[0] = '\0';
        gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(user_data));
        gtk_widget_grab_focus(treeview); // Postavi fokus na treeview
        
        // Premakni selekcijo na trenutno aktivno postajo, če obstaja
        if (pipeline && strlen(current_station) > 0) {
            GtkTreeModel *filter_model = GTK_TREE_MODEL(gtk_tree_view_get_model(GTK_TREE_VIEW(treeview)));
            GtkTreeIter iter;
            gboolean valid = gtk_tree_model_get_iter_first(filter_model, &iter);
            while (valid) {
                gchar *name = NULL;
                gtk_tree_model_get(filter_model, &iter, 1, &name, -1);
                if (name) {
                    const char *url = g_hash_table_lookup(station_urls, name);
                    if (url && strcmp(url, current_station) == 0) {
                        GtkTreePath *path = gtk_tree_model_get_path(filter_model, &iter);
                        gtk_tree_view_set_cursor(GTK_TREE_VIEW(treeview), path, NULL, FALSE);
                        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(treeview), path, NULL, TRUE, 0.5, 0.0);
                        gtk_tree_path_free(path);
                        g_free(name);
                        break;
                    }
                    g_free(name);
                }
                valid = gtk_tree_model_iter_next(filter_model, &iter);
            }
        }
        return TRUE;
    } else if (event->keyval == GDK_KEY_Up) {
        // Tipka gor - premakni selekcijo v treeview gor
        GtkTreeModel *filter_model = GTK_TREE_MODEL(gtk_tree_view_get_model(GTK_TREE_VIEW(treeview)));
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
        GtkTreeIter iter;
        GtkTreePath *path = NULL;
        if (gtk_tree_selection_get_selected(selection, NULL, &iter)) {
            path = gtk_tree_model_get_path(filter_model, &iter);
            if (path && gtk_tree_path_prev(path)) {
                gtk_tree_view_set_cursor(GTK_TREE_VIEW(treeview), path, NULL, FALSE);
                gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(treeview), path, NULL, TRUE, 0.5, 0.0);
            }
            if (path) gtk_tree_path_free(path);
        }
        return TRUE;
    } else if (event->keyval == GDK_KEY_Down) {
        // Tipka dol - premakni selekcijo v treeview dol
        GtkTreeModel *filter_model = GTK_TREE_MODEL(gtk_tree_view_get_model(GTK_TREE_VIEW(treeview)));
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
        GtkTreeIter iter;
        if (gtk_tree_selection_get_selected(selection, NULL, &iter)) {
            if (gtk_tree_model_iter_next(filter_model, &iter)) {
                GtkTreePath *path = gtk_tree_model_get_path(filter_model, &iter);
                gtk_tree_view_set_cursor(GTK_TREE_VIEW(treeview), path, NULL, FALSE);
                gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(treeview), path, NULL, TRUE, 0.5, 0.0);
                gtk_tree_path_free(path);
            }
        }
        return TRUE;
    } else if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
        // Enter - toggle play/stop for selected station, update icon
        GtkTreeModel *filter_model = GTK_TREE_MODEL(gtk_tree_view_get_model(GTK_TREE_VIEW(treeview)));
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
        GtkTreeIter iter;
        if (gtk_tree_selection_get_selected(selection, NULL, &iter)) {
            gchar *name = NULL;
            gtk_tree_model_get(filter_model, &iter, 1, &name, -1);
            if (name) {
                const char *url = g_hash_table_lookup(station_urls, name);
                if (url && pipeline && strcmp(current_station, url) == 0) {
                    // Ustavi predvajanje (kot on_play_clicked)
                    gst_element_set_state(pipeline, GST_STATE_NULL);
                    gst_object_unref(pipeline);
                    pipeline = NULL;
                    current_station[0] = '\0';
                    current_song[0] = '\0';
                    gtk_label_set_text(GTK_LABEL(label), "Ni predvajanja...");
                    // Odstrani barvo iz vseh postaj
                    gboolean valid = gtk_tree_model_get_iter_first(filter_model, &iter);
                    while (valid) {
                        GtkTreeIter child_iter2;
                        gtk_tree_model_filter_convert_iter_to_child_iter(GTK_TREE_MODEL_FILTER(filter_model), &child_iter2, &iter);
                        gtk_list_store_set(GTK_LIST_STORE(gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(filter_model))), &child_iter2, 0, "", -1);
                        valid = gtk_tree_model_iter_next(filter_model, &iter);
                    }
                    // Nastavi ikono na Play
                    if (play_button) {
                        GtkWidget *img = gtk_image_new_from_icon_name("media-playback-start-symbolic", GTK_ICON_SIZE_BUTTON);
                        gtk_button_set_image(GTK_BUTTON(play_button), img);
                        gtk_widget_set_tooltip_text(play_button, "Predvajaj");
                        update_play_item_label();
                        update_current_playing_item();
                    }
                } else {
                    // Predvajaj izbrano postajo
                    play_station(name);
                    // Nastavi ikono na Pause
                    if (play_button) {
                        GtkWidget *img = gtk_image_new_from_icon_name("media-playback-pause-symbolic", GTK_ICON_SIZE_BUTTON);
                        gtk_button_set_image(GTK_BUTTON(play_button), img);
                        gtk_widget_set_tooltip_text(play_button, "Pavza");
                        update_play_item_label();
                        update_current_playing_item();
                    }
                }
                g_free(name);
            }
        }
        return TRUE;
    }
    return FALSE;
}

// Handler za tipke v treeview (prikaži filter_entry in fokusiraj, vstavi znak)
gboolean on_treeview_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    (void)widget;
    GtkWidget *entry = GTK_WIDGET(user_data);
    if (g_unichar_isprint(gdk_keyval_to_unicode(event->keyval)) && !(event->state & GDK_CONTROL_MASK)) {
        gboolean was_visible = gtk_widget_get_visible(entry);
        gtk_widget_show(entry);
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Išči postajo..."); // nastavi placeholder
        gtk_widget_grab_focus(entry);
        if (!was_visible || strlen(gtk_entry_get_text(GTK_ENTRY(entry))) == 0) {
            // Če je vrstica bila prej skrita ali prazna, vstavi znak
            gunichar c = gdk_keyval_to_unicode(event->keyval);
            char buf[8] = {0};
            g_unichar_to_utf8(c, buf);
            gtk_entry_set_text(GTK_ENTRY(entry), buf);
            gtk_editable_set_position(GTK_EDITABLE(entry), -1);
            return TRUE;
        }
    }
    return FALSE;
}

// Funkcija za filtriranje vrstic glede na search_string
gboolean station_filter_func(GtkTreeModel *model, GtkTreeIter *iter, gpointer data) {
    (void)data;
    // Če je vključen filter za priljubljene, preveri stolpec 3
    if (favorite_filter_enabled) {
        gchar *fav = NULL;
        gtk_tree_model_get(model, iter, 3, &fav, -1);
        gboolean isfav = (fav && strlen(fav) > 0);
        g_free(fav);
        if (!isfav) return FALSE;
    }
    if (search_string[0] == '\0') return TRUE;
    gchar *name = NULL;
    gtk_tree_model_get(model, iter, 1, &name, -1);  // Stolpec 1 je ime postaje
    gboolean match = FALSE;
    if (name) {
        // Case-insensitive substring match
        char *haystack = g_utf8_strdown(name, -1);
        char *needle = g_utf8_strdown(search_string, -1);
        if (strstr(haystack, needle)) match = TRUE;
        g_free(haystack);
        g_free(needle);
    }
    g_free(name);
    return match;
}

// Handler za spremembo search entry
void on_search_entry_changed(GtkEntry *entry, gpointer user_data) {
    const char *txt = gtk_entry_get_text(entry);
    strncpy(search_string, txt, sizeof(search_string)-1);
    search_string[sizeof(search_string)-1] = '\0';
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(user_data));
}

// Glavno okno
GtkWidget *main_window = NULL;
GtkWidget *label = NULL;
GtkListStore *station_store = NULL;
GtkWidget *treeview = NULL;
char *last_played_name = NULL;
// GStreamer pipeline in trenutno predvajana postaja
GstElement *pipeline = NULL;
char current_station[512] = "";
char current_song[256] = "";
// Handler za GStreamer bus message (prikaz metapodatkov skladbe)
static void on_gst_message(GstBus *bus, GstMessage *msg, gpointer user_data) {
    (void)bus;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_TAG) {
        GstTagList *tags = NULL;
        gst_message_parse_tag(msg, &tags);
        gchar *title = NULL;
        gchar *artist = NULL;
        if (gst_tag_list_get_string(tags, GST_TAG_TITLE, &title)) {
            strncpy(current_song, title, sizeof(current_song)-1);
        }
        if (gst_tag_list_get_string(tags, GST_TAG_ARTIST, &artist)) {
            // Če je na voljo tudi izvajalec, združi
            gchar song_info[256];
            if (title) {
                snprintf(song_info, sizeof(song_info), "%s – %s", artist, title);
                strncpy(current_song, song_info, sizeof(current_song)-1);
            } else {
                strncpy(current_song, artist, sizeof(current_song)-1);
            }
        }
        g_free(title);
        g_free(artist);
        // Posodobi label
        gchar info[512];
        const gchar *station_name = (const gchar *)user_data;
        if (strlen(current_song) > 0)
            snprintf(info, sizeof(info), "%s", current_song);
        else
            snprintf(info, sizeof(info), "%s", station_name);
        gtk_label_set_text(GTK_LABEL(label), info);
        gst_tag_list_unref(tags);
        update_current_playing_item();
    }
}

// Preveri ali obstaja in je pravilna SQLite baza
int check_or_create_db(const char *db_path) {
    sqlite3 *db;
    int rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Napaka pri odpiranju baze: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 0;
    }
    // Pravilna shema - vključimo polje favorite (INTEGER 0/1)
    const char *sql = "CREATE TABLE IF NOT EXISTS stations (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, url TEXT NOT NULL, favorite INTEGER DEFAULT 0);";
    rc = sqlite3_exec(db, sql, 0, 0, 0);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Napaka pri ustvarjanju tabele: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 0;
    }
    /* Če je tabela obstajala brez stolpca 'favorite', dodamo stolpec (varen način: preverimo PRAGMA table_info) */
    int has_fav = 0;
    sqlite3_stmt *pstmt;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(stations);", -1, &pstmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(pstmt) == SQLITE_ROW) {
            const unsigned char *colname = sqlite3_column_text(pstmt, 1);
            if (colname && strcmp((const char*)colname, "favorite") == 0) {
                has_fav = 1;
                break;
            }
        }
        sqlite3_finalize(pstmt);
    }
    if (!has_fav) {
        /* Poskusimo dodati stolpec; če je to neuspešno, nadaljujemo, ker CREATE TABLE zgoraj že poskrbi za novo shemo */
        const char *addcol = "ALTER TABLE stations ADD COLUMN favorite INTEGER DEFAULT 0;";
        (void)sqlite3_exec(db, addcol, 0, 0, 0);
    }
    // Dodaj še tabelo za zadnjo predvajano postajo
    const char *sql2 = "CREATE TABLE IF NOT EXISTS last_played (name TEXT);";
    rc = sqlite3_exec(db, sql2, 0, 0, 0);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Napaka pri ustvarjanju tabele last_played: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 0;
    }
    /* Tabela za shranjevanje enostavnih nastavitev (shrani state gumba "priljubljeni") */
    const char *sql3 = "CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT);";
    rc = sqlite3_exec(db, sql3, 0, 0, 0);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Napaka pri ustvarjanju tabele settings: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 0;
    }
    sqlite3_close(db);
    return 1;
}

// Napolni GtkListStore z imeni postaj iz baze
void fill_station_store(GtkListStore *store, const char *filter) {
    sqlite3 *db;
    if (sqlite3_open(get_db_path(), &db) != SQLITE_OK) return;
    const char *sql_all = "SELECT name, favorite FROM stations;";
    const char *sql_like = "SELECT name, favorite FROM stations WHERE name LIKE ?;";
    sqlite3_stmt *stmt;
    gtk_list_store_clear(store);
    if (filter && strlen(filter) > 0) {
        if (sqlite3_prepare_v2(db, sql_like, -1, &stmt, NULL) == SQLITE_OK) {
            char like_pattern[256];
            snprintf(like_pattern, sizeof(like_pattern), "%%%s%%", filter);
            sqlite3_bind_text(stmt, 1, like_pattern, -1, SQLITE_TRANSIENT);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *name = (const char *)sqlite3_column_text(stmt, 0);
                int fav = sqlite3_column_int(stmt, 1);
                GtkTreeIter iter;
                gtk_list_store_append(store, &iter);
                gtk_list_store_set(store, &iter, 0, "", 1, name, 2, NULL, 3, fav ? "★" : "", -1);
            }
            sqlite3_finalize(stmt);
        }
    } else {
        if (sqlite3_prepare_v2(db, sql_all, -1, &stmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *name = (const char *)sqlite3_column_text(stmt, 0);
                int fav = sqlite3_column_int(stmt, 1);
                GtkTreeIter iter;
                gtk_list_store_append(store, &iter);
                gtk_list_store_set(store, &iter, 0, "", 1, name, 2, NULL, 3, fav ? "★" : "", -1);
            }
            sqlite3_finalize(stmt);
        }
    }
    sqlite3_close(db);
}

// Globalni kazalec na tray menu postavko za prikaz/skritje okna
static GtkWidget *show_item = NULL;
// Globalni kazalec na tray menu postavko za predvajaj/pavza
static GtkWidget *play_item = NULL;
// Prikaže trenutno predvajano postajo nad Predvajaj/Pavza v tray meniju
static GtkWidget *current_playing_item = NULL;
// Poseben vnos za prikaz imena postaje (prikazan nad pesmijo)
static GtkWidget *current_station_item = NULL;
// Separator pod pesmijo
static GtkWidget *sep_after_song = NULL;
// Globalni kazalec za About dialog (ena instanca)
static GtkWidget *about_dialog = NULL;

// Handler za sprostitev kazalca, ko je About uničen
static void on_about_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget; (void)user_data;
    about_dialog = NULL;
}

// Funkcija za posodobitev besedila v meniju glede na stanje okna
static void update_show_item_label() {
    if (!main_window || !show_item) return;
    if (gtk_widget_get_visible(main_window)) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(show_item), "Skrij okno");
    } else {
        gtk_menu_item_set_label(GTK_MENU_ITEM(show_item), "Prikaži okno");
    }
}

// Posodobi labelo Play/Pause v tray meniju glede na stanje predvajalnika
static void update_play_item_label() {
    if (!play_item) return;
    if (pipeline) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(play_item), "Pavza");
    } else {
        gtk_menu_item_set_label(GTK_MENU_ITEM(play_item), "Predvajaj");
    }
}

// Handler, sprožen ko se tray menu prikaže — osveži informacije v meniju
static void on_tray_menu_show(GtkWidget *menu, gpointer user_data) {
    (void)menu; (void)user_data;
    // Posodobi labelo Play/Pause
    update_play_item_label();
    update_current_playing_item();
}

// Posodobi current_playing_item iz trenutnih globalnih spremenljivk
static void update_current_playing_item() {
    if (!current_playing_item) return;
    if (!current_station_item) return;
    if (pipeline) {
        const char *station_name = NULL;
        if (strlen(current_station) > 0) {
            GHashTableIter iter;
            gpointer key, value;
            g_hash_table_iter_init(&iter, station_urls);
            while (g_hash_table_iter_next(&iter, &key, &value)) {
                if (value && strcmp((const char *)value, current_station) == 0) {
                    station_name = (const char *)key;
                    break;
                }
            }
        }
        if (station_name) gtk_menu_item_set_label(GTK_MENU_ITEM(current_station_item), station_name);
        else gtk_menu_item_set_label(GTK_MENU_ITEM(current_station_item), "Postaja");
        gtk_widget_set_sensitive(current_station_item, TRUE);
        gtk_widget_show(current_station_item);

        if (strlen(current_song) > 0) {
            gtk_menu_item_set_label(GTK_MENU_ITEM(current_playing_item), current_song);
            gtk_widget_set_sensitive(current_playing_item, TRUE);
            gtk_widget_show(current_playing_item);
            if (sep_after_song) gtk_widget_show(sep_after_song);
        } else {
            /* če ni podatka o pesmi, skrij vnos za pesem */
            gtk_widget_hide(current_playing_item);
            if (sep_after_song) gtk_widget_hide(sep_after_song);
        }
    } else {
        gtk_widget_hide(current_station_item);
        gtk_widget_hide(current_playing_item);
        if (sep_after_song) gtk_widget_hide(sep_after_song);
    }
}

// Callback za prikaz/skritje glavnega okna
void toggle_main_window(GtkMenuItem *item, gpointer user_data) {
    (void)item; (void)user_data;
    if (!main_window) return;
    if (gtk_widget_get_visible(main_window)) {
        gtk_widget_hide(main_window);
    } else {
        gtk_widget_show_all(main_window);
        gtk_widget_hide(global_filter_entry);
        gtk_window_set_position(GTK_WINDOW(main_window), GTK_WIN_POS_CENTER_ALWAYS);
        gtk_window_set_keep_above(GTK_WINDOW(main_window), FALSE);
        gtk_window_present(GTK_WINDOW(main_window));
        gtk_widget_grab_focus(treeview);
    }
    update_show_item_label();
}

// About dialog
static void show_about_dialog(GtkMenuItem *item, gpointer user_data) {
    (void)item; (void)user_data;
    if (about_dialog) {
        // Če že obstaja, ga le prikaži/spredaj postavi
        gtk_window_present(GTK_WINDOW(about_dialog));
        return;
    }

    about_dialog = gtk_about_dialog_new();
    const gchar *authors[] = { "Boris Arko - BArko", "SimOne", NULL };
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(about_dialog), "baRadio");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(about_dialog), version);
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(about_dialog), "Enostaven predvajalnik slovenskih spletnih radijskih postaj.");
    gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(about_dialog), authors);
    // Vedno modalno in vedno centrirano na zaslonu
    gtk_window_set_modal(GTK_WINDOW(about_dialog), TRUE);
    gtk_window_set_position(GTK_WINDOW(about_dialog), GTK_WIN_POS_CENTER_ALWAYS);
    // Ko se dialog zapre, sprosti kazalec
    g_signal_connect(about_dialog, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    g_signal_connect(about_dialog, "destroy", G_CALLBACK(on_about_destroy), NULL);
    gtk_widget_show_all(about_dialog);
    gtk_window_present(GTK_WINDOW(about_dialog));
}

// Callback za skrivanje okna namesto zapiranja
static gboolean hide_on_delete(GtkWidget *window, GdkEvent *event, gpointer user_data) {
    (void)event; (void)user_data;
    gtk_widget_hide(window);
    update_show_item_label();
    return TRUE; // prepreči zapiranje
}

// Callback za izhod
void quit_app(GtkMenuItem *item, gpointer user_data) {
    (void)item; (void)user_data;
    // Ustavi predvajanje in počisti pipeline
    if (pipeline) {
        GstBus *bus = gst_element_get_bus(pipeline);
        if (bus) {
            gst_bus_remove_signal_watch(bus);
            gst_object_unref(bus);
        }
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        pipeline = NULL;
        current_song[0] = '\0';
    }
    // Počisti hash tabelo
    if (station_urls) {
        g_hash_table_destroy(station_urls);
        station_urls = NULL;
    }
    gtk_main_quit();
}

// Handler za dvojni klik na postajo
void on_station_activated(GtkTreeView *treeview, GtkTreePath *path, GtkTreeViewColumn *col, gpointer user_data) {
    (void)col; (void)user_data;
    GtkTreeModel *filter_model = gtk_tree_view_get_model(treeview);
    GtkTreeModel *model = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(filter_model));
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(filter_model, &iter, path)) {
        GtkTreeIter child_iter;
        gtk_tree_model_filter_convert_iter_to_child_iter(GTK_TREE_MODEL_FILTER(filter_model), &child_iter, &iter);
        gchar *name = NULL;
        gtk_tree_model_get(filter_model, &iter, 1, &name, -1);
        if (name) {
            // Preveri, če je ta postaja že aktivna in pipeline obstaja
            const char *url = g_hash_table_lookup(station_urls, name);
            if (url && pipeline && strcmp(current_station, url) == 0) {
                // Ustavi predvajanje (kot on_play_clicked)
                {
                    GstBus *bus = gst_element_get_bus(pipeline);
                    if (bus) {
                        gst_bus_remove_signal_watch(bus);
                        gst_object_unref(bus);
                    }
                }
                gst_element_set_state(pipeline, GST_STATE_NULL);
                gst_object_unref(pipeline);
                pipeline = NULL;
                current_station[0] = '\0';
                current_song[0] = '\0';
                gtk_label_set_text(GTK_LABEL(label), "Ni predvajanja...");
                // Odstrani barvo iz vseh postaj
                gboolean valid = gtk_tree_model_get_iter_first(filter_model, &iter);
                while (valid) {
                    GtkTreeIter child_iter2;
                    gtk_tree_model_filter_convert_iter_to_child_iter(GTK_TREE_MODEL_FILTER(filter_model), &child_iter2, &iter);
                    gtk_list_store_set(GTK_LIST_STORE(model), &child_iter2, 0, "", -1);
                    valid = gtk_tree_model_iter_next(filter_model, &iter);
                }
                // Nastavi ikono na Play
                if (play_button) {
                    GtkWidget *img = gtk_image_new_from_icon_name("media-playback-start-symbolic", GTK_ICON_SIZE_BUTTON);
                    gtk_button_set_image(GTK_BUTTON(play_button), img);
                    gtk_widget_set_tooltip_text(play_button, "Predvajaj");
                }
                // Posodobi tray meni
                update_play_item_label();
                update_current_playing_item();
            } else {
                // Predvajaj izbrano postajo
                play_station(name);
                // Nastavi ikono na Pause
                if (play_button) {
                    GtkWidget *img = gtk_image_new_from_icon_name("media-playback-pause-symbolic", GTK_ICON_SIZE_BUTTON);
                    gtk_button_set_image(GTK_BUTTON(play_button), img);
                    gtk_widget_set_tooltip_text(play_button, "Pavza");
                }
                // Posodobi tray meni
                update_play_item_label();
                update_current_playing_item();
            }
            g_free(name);
        }
    }
}

// Funkcija za predvajanje postaje po imenu
static void play_station(const char *name) {
    if (!name || strlen(name) == 0) return;

    const char *url = g_hash_table_lookup(station_urls, name);
    if (!url) return;

    // Če je že predvajana, ustavi
    if (strcmp(current_station, url) == 0 && pipeline) {
        {
            GstBus *bus = gst_element_get_bus(pipeline);
            if (bus) {
                gst_bus_remove_signal_watch(bus);
                gst_object_unref(bus);
            }
        }
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        pipeline = NULL;
        current_station[0] = '\0';
        current_song[0] = '\0';
        gtk_label_set_text(GTK_LABEL(label), "Ni predvajanja...");
        refresh_active_station_color();
        save_last_played(name);
        update_play_item_label();
        update_current_playing_item();
        return;
    }

    // Ustavi staro, če obstaja
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        pipeline = NULL;
    }

    // Začni novo
    pipeline = gst_element_factory_make("playbin", "player");
    if (pipeline) {
        g_object_set(pipeline, "uri", url, NULL);
        strncpy(current_station, url, sizeof(current_station)-1);
        current_song[0] = '\0';
        // Dodaj bus handler za metapodatke
        GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_signal_watch(bus);
    /* Connect with destroy notifier so duplicated name is freed when handler is removed */
    g_signal_connect_data(bus, "message", G_CALLBACK(on_gst_message), g_strdup(name), (GClosureNotify)g_free, 0);
        gst_object_unref(bus);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        gtk_label_set_text(GTK_LABEL(label), name);
        save_last_played(name);
        refresh_active_station_color();
        update_play_item_label();
        update_current_playing_item();
    } else {
        gtk_label_set_text(GTK_LABEL(label), "Napaka pri predvajanju!");
    }
}

// Naloži vse postaje v hash tabelo
static void load_station_urls() {
    if (station_urls) g_hash_table_destroy(station_urls);
    station_urls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    sqlite3 *db;
    if (sqlite3_open(get_db_path(), &db) == SQLITE_OK) {
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, "SELECT name, url FROM stations;", -1, &stmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *name = (const char *)sqlite3_column_text(stmt, 0);
                const char *url = (const char *)sqlite3_column_text(stmt, 1);
                if (name && url) {
                    g_hash_table_insert(station_urls, g_strdup(name), g_strdup(url));
                }
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }
}

int main(int argc, char **argv) {

    // SINGLE INSTANCE: Preveri zaklepno datoteko
    if (!create_lock_file()) {
        fprintf(stderr, "Aplikacija že teče (single instance)!\n");
        return 1;
    }

    // Preveri/ustvari bazo
    if (!check_or_create_db(get_db_path())) {
        fprintf(stderr, "Napaka pri inicializaciji baze!\n");
        remove_lock_file();
        return 1;
    }

    // Inicializiraj GStreamer
    gst_init(&argc, &argv);

    gtk_init(&argc, &argv);

    // --- MPRIS D-Bus ---
    mpris_register();

    // Glavno okno
    main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_window), version);
    gtk_window_set_default_size(GTK_WINDOW(main_window), 450, 350);
    /* Zaklenemo dimenzije okna: nastavimo minimalno in maksimalno velikost enako privzeti
       ter onemogočimo spreminjanje velikosti. To prepreči uporabniku, da pomanjša/razširi
       okno (dimenzije ostanejo fiksne). */
    {
        GdkGeometry hints;
        memset(&hints, 0, sizeof(hints));
        hints.min_width = hints.max_width = 450;
        hints.min_height = hints.max_height = 350;
        gtk_window_set_geometry_hints(GTK_WINDOW(main_window), NULL, &hints, GDK_HINT_MIN_SIZE | GDK_HINT_MAX_SIZE);
        gtk_window_set_resizable(GTK_WINDOW(main_window), FALSE);
    }
    /* Vedno centriraj okno na zaslonu */
    gtk_window_set_position(GTK_WINDOW(main_window), GTK_WIN_POS_CENTER_ALWAYS);

    // Header bar (lepši titlebar z gumbi)
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), version);
    gtk_window_set_titlebar(GTK_WINDOW(main_window), header);

    // Minimalen CSS samo za osnovne izboljšave (upošteva sistemsko temo)
    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css_provider,
        "headerbar {\n"
        "  background-color: #2b2f3a;\n"
        "  color: #f5f7fa;\n"
        "}\n"
        "headerbar button {\n"
        "  background: transparent;\n"
        "  border-radius: 6px;\n"
        "}\n"
        "#station_label { \n"
        "  font-size: 15px; \n"
        "  font-weight: bold; \n"
        "  padding: 10px; \n"
        "  /* Svetlejša barva besedila za boljšo berljivost */\n"
        "  color: #9ca3af; \n"
        "}\n"
        ".dark #station_label { \n"
        "  /* Svetlejša barva za dark theme */\n"
        "  color: #f5f7fa; \n"
        "}\n"
        "#station_list { \n"
        "  font-size: 16px; \n"
        "}\n"
        "scrolledwindow { \n"
        "  border-radius: 6px; \n"
        "}\n"
        "treeview row { \n"
        "  padding: 6px 8px; \n"
        "}\n",
        -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    // Če sistem uporablja "dark theme" (gtk-application-prefer-dark-theme), dodaj razred "dark" na glavno okno
    GtkSettings *settings = gtk_settings_get_default();
    gboolean prefer_dark = FALSE;
    if (settings) {
        g_object_get(G_OBJECT(settings), "gtk-application-prefer-dark-theme", &prefer_dark, NULL);
    }
    if (prefer_dark && main_window) {
        GtkStyleContext *ctx = gtk_widget_get_style_context(main_window);
        if (ctx) gtk_style_context_add_class(ctx, "dark");
    }
    
    // Glavni vertikalni box
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(main_window), vbox);

    // Label za trenutno postajo/pesem
    label = gtk_label_new("Ni predvajanja...");
    gtk_widget_set_name(label, "station_label");
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_label_set_xalign(GTK_LABEL(label), 0.5);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 45);
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

    // Kontrolni gumbi v header baru (prej so bili v ločenem boxu)
    // Previous gumb
    GtkWidget *prev_button = gtk_button_new_from_icon_name("media-skip-backward-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_relief(GTK_BUTTON(prev_button), GTK_RELIEF_NORMAL);
    gtk_widget_set_tooltip_text(prev_button, "Prejšnja postaja");
    g_signal_connect(prev_button, "clicked", G_CALLBACK(on_previous_clicked), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), prev_button);

    // Play gumb
    play_button = gtk_button_new();
    GtkWidget *img = gtk_image_new_from_icon_name("media-playback-start-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(play_button), img);
    gtk_button_set_relief(GTK_BUTTON(play_button), GTK_RELIEF_NORMAL);
    gtk_widget_set_tooltip_text(play_button, "Predvajaj");
    g_signal_connect(play_button, "clicked", G_CALLBACK(on_play_clicked), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), play_button);
    // Sinhroniziraj labelo v tray meniju z začetnim stanjem predvajalnika
    update_play_item_label();

    // Next gumb
    GtkWidget *next_button = gtk_button_new_from_icon_name("media-skip-forward-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_relief(GTK_BUTTON(next_button), GTK_RELIEF_NORMAL);
    gtk_widget_set_tooltip_text(next_button, "Naslednja postaja");
    g_signal_connect(next_button, "clicked", G_CALLBACK(on_next_clicked), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), next_button);

    // Fav toggle button (prikaz samo priljubljenih) - gumb brez label
    // Naloži stanje iz settings
    char *fav_setting = get_setting("show_favorites_only");
    if (fav_setting) {
        favorite_filter_enabled = (strcmp(fav_setting, "1") == 0);
        g_free(fav_setting);
    } else {
        favorite_filter_enabled = FALSE;
    }
    fav_toggle_button = gtk_button_new();
    GtkWidget *fav_img = get_fav_image_for_state(favorite_filter_enabled);
    gtk_button_set_image(GTK_BUTTON(fav_toggle_button), fav_img);
    gtk_button_set_relief(GTK_BUTTON(fav_toggle_button), GTK_RELIEF_NORMAL);
    gtk_widget_set_tooltip_text(fav_toggle_button, "Priljubljene (filter)");
    g_signal_connect(fav_toggle_button, "clicked", G_CALLBACK(on_fav_toggle_clicked), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), fav_toggle_button);

    // TreeView za prikaz postaj v scrollable oknu
    // 0: ikona (▶ ali ""), 1: ime, 2: unused (za kompatibilnost), 3: favorite ("1"/"0")
    station_store = gtk_list_store_new(4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    fill_station_store(station_store, NULL);
    load_station_urls();  // Naloži URL-je v hash tabelo
    // Preberi zadnjo predvajano postajo
    last_played_name = load_last_played();
    // Uporabi GtkTreeModelFilter za filtriranje
    GtkTreeModel *filter_model = GTK_TREE_MODEL(gtk_tree_model_filter_new(GTK_TREE_MODEL(station_store), NULL));
    gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(filter_model), station_filter_func, NULL, NULL);
    treeview = gtk_tree_view_new_with_model(filter_model);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(treeview), FALSE);
    
    // Stolpec za ikono
    GtkCellRenderer *icon_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *icon_column = gtk_tree_view_column_new_with_attributes("", icon_renderer, "text", 0, NULL);
    gtk_tree_view_column_set_sizing(icon_column, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(icon_column, 30);  // Fiksna širina za ikono
    gtk_tree_view_append_column(GTK_TREE_VIEW(treeview), icon_column);
    
    // Stolpec za zvezdico (priljubljeno)
    GtkCellRenderer *fav_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *fav_column = gtk_tree_view_column_new_with_attributes("", fav_renderer, "text", 3, NULL);
    gtk_tree_view_column_set_sizing(fav_column, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(fav_column, 24);
    gtk_tree_view_append_column(GTK_TREE_VIEW(treeview), fav_column);
    
    // Stolpec za ime postaje
    GtkCellRenderer *name_renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *name_column = gtk_tree_view_column_new_with_attributes("", name_renderer, "text", 1, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(treeview), name_column);
    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scrolled_window), treeview);
    gtk_widget_set_name(scrolled_window, "station_list");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled_window, TRUE, TRUE, 0);

    // Po napolnitvi modela izberi vrstico z zadnjo predvajano postajo (če obstaja)
    if (last_played_name && strlen(last_played_name) > 0) {
        GtkTreeIter iter;
        gboolean found = FALSE;
        gboolean valid = gtk_tree_model_get_iter_first(filter_model, &iter);
        while (valid) {
            gchar *name = NULL;
            gtk_tree_model_get(filter_model, &iter, 1, &name, -1);
            if (name && strcmp(name, last_played_name) == 0) {
                GtkTreePath *path = gtk_tree_model_get_path(filter_model, &iter);
                gtk_tree_view_set_cursor(GTK_TREE_VIEW(treeview), path, NULL, FALSE);
                gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(treeview), path, NULL, TRUE, 0.5, 0.0);
                gtk_tree_path_free(path);
                g_free(name);
                found = TRUE;
                break;
            }
            g_free(name);
            valid = gtk_tree_model_iter_next(filter_model, &iter);
        }
        if (!found) {
            clear_last_played();
        }
    }

    // Priključi handler za desni klik na treeview
    g_signal_connect(treeview, "button-press-event", G_CALLBACK(on_treeview_button_press), NULL);

    // Custom filter entry pod seznamom
    GtkWidget *filter_entry = gtk_entry_new();
    // Placeholder nastavimo dinamično, ko je vrstica prikazana
    gtk_box_pack_start(GTK_BOX(vbox), filter_entry, FALSE, FALSE, 0);
    gtk_widget_hide(filter_entry);

    // Nastavi globalni kazalec za globalni ESC handler
    global_filter_entry = filter_entry;

    // Handler za spremembo filter entry
    g_signal_connect(filter_entry, "changed", G_CALLBACK(on_search_entry_changed), filter_model);

    // Handler za tipke v filter entry (ESC skrije in pobriše)
    g_signal_connect(filter_entry, "key-press-event", G_CALLBACK(on_filter_entry_key_press), filter_model);

    // Handler za tipke v treeview (vsaka tipka prikaže filter_entry in fokusira nanj)
    g_signal_connect(treeview, "key-press-event", G_CALLBACK(on_treeview_key_press), filter_entry);

    // Globalni handler za ESC na glavnem oknu
    g_signal_connect(main_window, "key-press-event", G_CALLBACK(on_main_window_key_press), filter_model);

    // Handler za dvojni klik na postajo
    g_signal_connect(treeview, "row-activated", G_CALLBACK(on_station_activated), NULL);

    // Namesto destroy uporabim delete-event za skrivanje
    g_signal_connect(main_window, "delete-event", G_CALLBACK(hide_on_delete), NULL);

        // Tray ikona (AppIndicator)
    AppIndicator *indicator = app_indicator_new(
        "baradio-indicator",
        "radio",
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS
    );
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);

    // Tray meni
    GtkWidget *menu = gtk_menu_new();
     /* Prikaži verzijo kot neaktivno postavko na vrhu menija —
         mnogi paneli ne prikazujejo tooltipov za AppIndicator ikone,
         zato je verzija tako vedno vidna v meniju. */
     GtkWidget *version_item = gtk_menu_item_new_with_label(version);
     gtk_widget_set_sensitive(version_item, FALSE);
     gtk_menu_shell_append(GTK_MENU_SHELL(menu), version_item);
    show_item = gtk_menu_item_new_with_label("Prikaži okno");
    /* Dodaj naprej/nazaj menijske vnose in predvajaj/pavza */
    /* Postavki za prikaz trenutno predvajane postaje in pesmi (dinamično) */
    current_station_item = gtk_menu_item_new_with_label("");
    gtk_widget_set_sensitive(current_station_item, FALSE);
    gtk_widget_hide(current_station_item);
    current_playing_item = gtk_menu_item_new_with_label("");
    gtk_widget_set_sensitive(current_playing_item, FALSE);
    /* skrij privzeto, dokler ni predvajanja */
    gtk_widget_hide(current_playing_item);
    /* separator neposredno pod pesmijo */
    sep_after_song = gtk_separator_menu_item_new();
    gtk_widget_hide(sep_after_song);
    play_item = gtk_menu_item_new_with_label("Predvajaj");
    GtkWidget *next_item = gtk_menu_item_new_with_label("Naprej");
    GtkWidget *prev_item = gtk_menu_item_new_with_label("Nazaj");
    GtkWidget *about_item = gtk_menu_item_new_with_label("O baRadio");
    GtkWidget *quit_item = gtk_menu_item_new_with_label("Izhod");
    GtkWidget *sep_top = gtk_separator_menu_item_new();
    /* separator neposredno za Predvajaj/Pavza */
    GtkWidget *sep_after_play = gtk_separator_menu_item_new();
    GtkWidget *sep_bottom = gtk_separator_menu_item_new();
    g_signal_connect(show_item, "activate", G_CALLBACK(toggle_main_window), NULL);
    g_signal_connect(play_item, "activate", G_CALLBACK(on_play_clicked), NULL);
    g_signal_connect(next_item, "activate", G_CALLBACK(on_next_clicked), NULL);
    g_signal_connect(prev_item, "activate", G_CALLBACK(on_previous_clicked), NULL);
    g_signal_connect(about_item, "activate", G_CALLBACK(show_about_dialog), NULL);
    g_signal_connect(quit_item, "activate", G_CALLBACK(quit_app), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), show_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep_top);
    /* Najprej prikaži katera postaja trenutno predvaja (če obstaja) */
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), current_station_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), current_playing_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep_after_song);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), play_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep_after_play);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), next_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), prev_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep_bottom);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), about_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);
    gtk_widget_show_all(menu);
    app_indicator_set_menu(indicator, GTK_MENU(menu));
    /* Ko se menu prikaže, osveži prikaz trenutne postaje. Nekateri paneli morda sprožijo 'map' raje kot 'show'. */
    g_signal_connect(menu, "show", G_CALLBACK(on_tray_menu_show), NULL);
    g_signal_connect(menu, "map", G_CALLBACK(on_tray_menu_show), NULL);
    // Posodobi labelo ob zagonu (če je okno že prikazano)
    update_show_item_label();
    // Posodobi Play/Pause labelo v meniju glede na trenutno stanje
    update_play_item_label();
    // Inicialno osveži current_playing_item
    update_current_playing_item();

    // Okno je ob zagonu skrito (če želiš)

    gtk_main();
        if (last_played_name) g_free(last_played_name);
        remove_lock_file();
        return 0;
}