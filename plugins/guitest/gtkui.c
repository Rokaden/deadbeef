/*
    DeaDBeeF - ultimate music player for GNU/Linux systems with X11
    Copyright (C) 2009-2013 Alexey Yakovenko <waker@users.sourceforge.net>

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.
    
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
#include "../../deadbeef.h"
#include <gtk/gtk.h>
#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include "../../gettext.h"
#include "gtkui.h"
#include "ddblistview.h"
#include "search.h"
#include "progress.h"
#include "interface.h"
#include "callbacks.h"
#include "support.h"
#include "../libparser/parser.h"
#include "drawing.h"
#include "trkproperties.h"
#include "../artwork/artwork.h"
#include "coverart.h"
#include "plcommon.h"
#include "ddbtabstrip.h"
#include "eq.h"
#include "actions.h"
#include "pluginconf.h"
#include "gtkui_api.h"
#include "wingeom.h"
#include "widgets.h"
#ifndef __APPLE__
#include "X11/Xlib.h"
#else
#include "retina.h"
#endif
#include "actionhandlers.h"
#include "hotkeys.h"
#include "../hotkeys/hotkeys.h"

#define trace(...) { fprintf(stderr, __VA_ARGS__); }
//#define trace(fmt,...)

static ddb_gtkui_t plugin;
DB_functions_t *deadbeef;

// cover art loading plugin
DB_artwork_plugin_t *coverart_plugin = NULL;

// main widgets
GtkWidget *mainwin;
GtkWidget *plwin;
GtkWidget *searchwin;
GtkStatusIcon *trayicon;
GtkWidget *traymenu;

// playlist theming
GtkWidget *theme_treeview;
GtkWidget *theme_button;

static int gtkui_accept_messages = 0;

static gint refresh_timeout = 0;

int fileadded_listener_id;
int fileadd_beginend_listener_id;
// overriden API methods
#if 0
int (*gtkui_original_plt_add_dir) (ddb_playlist_t *plt, const char *dirname, int (*cb)(DB_playItem_t *it, void *data), void *user_data);
int (*gtkui_original_plt_add_file) (ddb_playlist_t *plt, const char *fname, int (*cb)(DB_playItem_t *it, void *data), void *user_data);
int (*gtkui_original_pl_add_files_begin) (ddb_playlist_t *plt);
void (*gtkui_original_pl_add_files_end) (void);
#endif

// cached config variables
int gtkui_embolden_current_track;
int gtkui_groups_pinned;

#ifdef __APPLE__
int gtkui_is_retina = 0;
#endif

int gtkui_unicode_playstate = 0;
int gtkui_disable_seekbar_overlay = 0;

#define TRAY_ICON "deadbeef_tray_icon"

// that must be called before gtk_init
void
gtkpl_init (void) {
    theme_treeview = gtk_tree_view_new ();
    gtk_widget_show (theme_treeview);
    gtk_widget_set_can_focus (theme_treeview, FALSE);
    GtkWidget *vbox1 = lookup_widget (mainwin, "vbox1");
    gtk_box_pack_start (GTK_BOX (vbox1), theme_treeview, FALSE, FALSE, 0);
    gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (theme_treeview), TRUE);

    theme_button = mainwin;//lookup_widget (mainwin, "stopbtn");
}

void
gtkpl_free (DdbListview *pl) {
#if 0
    if (colhdr_anim.timeline) {
        timeline_free (colhdr_anim.timeline, 1);
        colhdr_anim.timeline = 0;
    }
#endif
}

struct fromto_t {
    DB_playItem_t *from;
    DB_playItem_t *to;
};

static gboolean
update_win_title_idle (gpointer data);

// update status bar and window title
static int sb_context_id = -1;
static char sb_text[512];
static float last_songpos = -1;
static char sbitrate[20] = "";
static struct timeval last_br_update;

static gboolean
update_songinfo (gpointer ctx) {
    int iconified = gdk_window_get_state(gtk_widget_get_window(mainwin)) & GDK_WINDOW_STATE_ICONIFIED;
    if (!gtk_widget_get_visible (mainwin) || iconified) {
        return FALSE;
    }
    DB_output_t *output = deadbeef->get_output ();
    char sbtext_new[512] = "-";

    float pl_totaltime = deadbeef->pl_get_totaltime ();
    int daystotal = (int)pl_totaltime / (3600*24);
    int hourtotal = ((int)pl_totaltime / 3600) % 24;
    int mintotal = ((int)pl_totaltime/60) % 60;
    int sectotal = ((int)pl_totaltime) % 60;

    char totaltime_str[512] = "";
    if (daystotal == 0) {
        snprintf (totaltime_str, sizeof (totaltime_str), "%d:%02d:%02d", hourtotal, mintotal, sectotal);
    }
    else if (daystotal == 1) {
        snprintf (totaltime_str, sizeof (totaltime_str), _("1 day %d:%02d:%02d"), hourtotal, mintotal, sectotal);
    }
    else {
        snprintf (totaltime_str, sizeof (totaltime_str), _("%d days %d:%02d:%02d"), daystotal, hourtotal, mintotal, sectotal);
    }

    DB_playItem_t *track = deadbeef->streamer_get_playing_track ();
    DB_fileinfo_t *c = deadbeef->streamer_get_current_fileinfo (); // FIXME: might crash streamer

    float duration = track ? deadbeef->pl_get_item_duration (track) : -1;

    if (!output || (output->state () == OUTPUT_STATE_STOPPED || !track || !c)) {
        snprintf (sbtext_new, sizeof (sbtext_new), _("Stopped | %d tracks | %s total playtime"), deadbeef->pl_getcount (PL_MAIN), totaltime_str);
    }
    else {
        float playpos = deadbeef->streamer_get_playpos ();
        int minpos = playpos / 60;
        int secpos = playpos - minpos * 60;
        int mindur = duration / 60;
        int secdur = duration - mindur * 60;

        const char *mode;
        char temp[20];
        if (c->fmt.channels <= 2) {
            mode = c->fmt.channels == 1 ? _("Mono") : _("Stereo");
        }
        else {
            snprintf (temp, sizeof (temp), "%dch Multichannel", c->fmt.channels);
            mode = temp;
        }
        int samplerate = c->fmt.samplerate;
        int bitspersample = c->fmt.bps;
        //        codec_unlock ();

        char t[100];
        if (duration >= 0) {
            snprintf (t, sizeof (t), "%d:%02d", mindur, secdur);
        }
        else {
            strcpy (t, "-:--");
        }

        struct timeval tm;
        gettimeofday (&tm, NULL);
        if (tm.tv_sec - last_br_update.tv_sec + (tm.tv_usec - last_br_update.tv_usec) / 1000000.0 >= 0.3) {
            memcpy (&last_br_update, &tm, sizeof (tm));
            int bitrate = deadbeef->streamer_get_apx_bitrate ();
            if (bitrate > 0) {
                snprintf (sbitrate, sizeof (sbitrate), _("| %4d kbps "), bitrate);
            }
            else {
                sbitrate[0] = 0;
            }
        }
        const char *spaused = deadbeef->get_output ()->state () == OUTPUT_STATE_PAUSED ? _("Paused | ") : "";
        char filetype[20];
        if (!deadbeef->pl_get_meta (track, ":FILETYPE", filetype, sizeof (filetype))) {
            strcpy (filetype, "-");
        }
        snprintf (sbtext_new, sizeof (sbtext_new), _("%s%s %s| %dHz | %d bit | %s | %d:%02d / %s | %d tracks | %s total playtime"), spaused, filetype, sbitrate, samplerate, bitspersample, mode, minpos, secpos, t, deadbeef->pl_getcount (PL_MAIN), totaltime_str);
    }

    if (strcmp (sbtext_new, sb_text)) {
        strcpy (sb_text, sbtext_new);

        // form statusline
        // FIXME: don't update if window is not visible
        GtkStatusbar *sb = GTK_STATUSBAR (lookup_widget (mainwin, "statusbar"));
        if (sb_context_id == -1) {
            sb_context_id = gtk_statusbar_get_context_id (sb, "msg");
        }

        gtk_statusbar_pop (sb, sb_context_id);
        gtk_statusbar_push (sb, sb_context_id, sb_text);
    }

    if (track) {
        deadbeef->pl_item_unref (track);
    }
    return FALSE;
}

void
set_tray_tooltip (const char *text) {
    if (trayicon) {
#if !GTK_CHECK_VERSION(2,16,0)
        gtk_status_icon_set_tooltip (trayicon, text);
#else
        gtk_status_icon_set_tooltip_text (trayicon, text);
#endif
    }
}

gboolean
on_trayicon_scroll_event               (GtkWidget       *widget,
                                        GdkEventScroll  *event,
                                        gpointer         user_data)
{
    float vol = deadbeef->volume_get_db ();
    int sens = deadbeef->conf_get_int ("gtkui.tray_volume_sensitivity", 1);
    if (event->direction == GDK_SCROLL_UP || event->direction == GDK_SCROLL_RIGHT) {
        vol += sens;
    }
    else if (event->direction == GDK_SCROLL_DOWN || event->direction == GDK_SCROLL_LEFT) {
        vol -= sens;
    }
    if (vol > 0) {
        vol = 0;
    }
    else if (vol < deadbeef->volume_get_min_db ()) {
        vol = deadbeef->volume_get_min_db ();
    }
    deadbeef->volume_set_db (vol);

#if 0
    char str[100];
    if (deadbeef->conf_get_int ("gtkui.show_gain_in_db", 1)) {
        snprintf (str, sizeof (str), "Gain: %s%d dB", vol == 0 ? "+" : "", (int)vol);
    }
    else {
        snprintf (str, sizeof (str), "Gain: %d%%", (int)(deadbeef->volume_get_amp () * 100));
    }
    set_tray_tooltip (str);
#endif

    return FALSE;
}

void
mainwin_toggle_visible (void) {
    int iconified = gdk_window_get_state(gtk_widget_get_window(mainwin)) & GDK_WINDOW_STATE_ICONIFIED;
    if (gtk_widget_get_visible (mainwin) && !iconified) {
        gtk_widget_hide (mainwin);
    }
    else {
        wingeom_restore (mainwin, "mainwin", 40, 40, 500, 300, 0);
        if (iconified) {
            gtk_window_deiconify (GTK_WINDOW(mainwin));
        }
        else {
            gtk_window_present (GTK_WINDOW (mainwin));
        }
    }
}

#if !GTK_CHECK_VERSION(2,14,0)
gboolean
on_trayicon_activate (GtkWidget       *widget,
                                        gpointer         user_data)
{
    mainwin_toggle_visible ();
    return FALSE;
}

#else

gboolean
on_trayicon_button_press_event (GtkWidget       *widget,
                                        GdkEventButton  *event,
                                        gpointer         user_data)
{
    if (event->button == 1 && event->type == GDK_BUTTON_PRESS) {
        mainwin_toggle_visible ();
    }
    else if (event->button == 2 && event->type == GDK_BUTTON_PRESS) {
        deadbeef->sendmessage (DB_EV_TOGGLE_PAUSE, 0, 0, 0);
    }
    return FALSE;
}
#endif

gboolean
on_trayicon_popup_menu (GtkWidget       *widget,
        guint button,
        guint time,
                                        gpointer         user_data)
{
    gtk_menu_popup (GTK_MENU (traymenu), NULL, NULL, gtk_status_icon_position_menu, trayicon, button, time);
    return FALSE;
}

static gboolean
activate_cb (gpointer nothing) {
    gtk_widget_show (mainwin);
    gtk_window_present (GTK_WINDOW (mainwin));
    return FALSE;
}

void
redraw_queued_tracks (DdbListview *pl) {
    DB_playItem_t *it;
    int idx = 0;
    deadbeef->pl_lock ();
    for (it = deadbeef->pl_get_first (PL_MAIN); it; idx++) {
        if (deadbeef->pl_playqueue_test (it) != -1) {
            ddb_listview_draw_row (pl, idx, (DdbListviewIter)it);
        }
        DB_playItem_t *next = deadbeef->pl_get_next (it, PL_MAIN);
        deadbeef->pl_item_unref (it);
        it = next;
    }
    deadbeef->pl_unlock ();
}

gboolean
redraw_queued_tracks_cb (gpointer plt) {
    DdbListview *list = plt;
    int iconified = gdk_window_get_state(gtk_widget_get_window(mainwin)) & GDK_WINDOW_STATE_ICONIFIED;
    if (!gtk_widget_get_visible (mainwin) || iconified) {
        return FALSE;
    }
    redraw_queued_tracks (list);
    return FALSE;
}

void
gtkpl_songchanged_wrapper (DB_playItem_t *from, DB_playItem_t *to) {
    struct fromto_t *ft = malloc (sizeof (struct fromto_t));
    ft->from = from;
    ft->to = to;
    if (from) {
        deadbeef->pl_item_ref (from);
    }
    if (to) {
        deadbeef->pl_item_ref (to);
    }
    g_idle_add (update_win_title_idle, ft);
    if (searchwin && gtk_widget_get_window (searchwin)) {
        int iconified = gdk_window_get_state(gtk_widget_get_window (searchwin)) & GDK_WINDOW_STATE_ICONIFIED;
        if (gtk_widget_get_visible (searchwin) && !iconified) {
            g_idle_add (redraw_queued_tracks_cb, DDB_LISTVIEW (lookup_widget (searchwin, "searchlist")));
        }
    }
}

void
gtkui_set_titlebar (DB_playItem_t *it) {
    if (!it) {
        it = deadbeef->streamer_get_playing_track ();
    }
    else {
        deadbeef->pl_item_ref (it);
    }
    char fmt[500];
    char str[600];
    if (it) {
        deadbeef->conf_get_str ("gtkui.titlebar_playing", "%a - %t - DeaDBeeF-%V", fmt, sizeof (fmt));
    }
    else {
        deadbeef->conf_get_str ("gtkui.titlebar_stopped", "DeaDBeeF-%V", fmt, sizeof (fmt));
    }
    deadbeef->pl_format_title (it, -1, str, sizeof (str), -1, fmt);
    gtk_window_set_title (GTK_WINDOW (mainwin), str);
    if (it) {
        deadbeef->pl_item_unref (it);
    }
    set_tray_tooltip (str);
}

static void
trackinfochanged_wrapper (DdbListview *playlist, DB_playItem_t *track, int iter) {
    if (track) {
        int idx = deadbeef->pl_get_idx_of_iter (track, iter);
        if (idx != -1) {
            ddb_listview_draw_row (playlist, idx, (DdbListviewIter)track);
        }
    }
}

void
gtkui_trackinfochanged (DB_playItem_t *track) {
    if (searchwin && gtk_widget_get_visible (searchwin)) {
        GtkWidget *search = lookup_widget (searchwin, "searchlist");
        trackinfochanged_wrapper (DDB_LISTVIEW (search), track, PL_SEARCH);
    }

    DB_playItem_t *curr = deadbeef->streamer_get_playing_track ();
    if (track == curr) {
        gtkui_set_titlebar (track);
    }
    if (curr) {
        deadbeef->pl_item_unref (curr);
    }
}

static gboolean
trackinfochanged_cb (gpointer data) {
    gtkui_trackinfochanged (data);
    if (data) {
        deadbeef->pl_item_unref ((DB_playItem_t *)data);
    }
    return FALSE;
}

void
playlist_refresh (void) {
    search_refresh ();
    trkproperties_fill_metadata ();
}

static gboolean
playlistchanged_cb (gpointer none) {
    playlist_refresh ();
    return FALSE;
}

static gboolean
playlistswitch_cb (gpointer none) {
    search_refresh ();
    return FALSE;
}

static gboolean
gtkui_on_frameupdate (gpointer data) {
    //update_songinfo (NULL);
    gtk_widget_queue_draw (mainwin);

    return TRUE;
}

static gboolean
gtkui_update_status_icon (gpointer unused) {
    int hide_tray_icon = deadbeef->conf_get_int ("gtkui.hide_tray_icon", 0);
    if (hide_tray_icon && !trayicon) {
        return FALSE;
    }
    if (trayicon) {
        if (hide_tray_icon) {
            g_object_set (trayicon, "visible", FALSE, NULL);
        }
        else {
            g_object_set (trayicon, "visible", TRUE, NULL);
        }
        return FALSE;
    }
    // system tray icon
    traymenu = create_traymenu ();

    char tmp[1000];
    const char *icon_name = tmp;
    deadbeef->conf_get_str ("gtkui.custom_tray_icon", TRAY_ICON, tmp, sizeof (tmp));
    GtkIconTheme *theme = gtk_icon_theme_get_default();

    if (!gtk_icon_theme_has_icon(theme, icon_name))
        icon_name = "deadbeef";
    else {
        GtkIconInfo *icon_info = gtk_icon_theme_lookup_icon(theme, icon_name, 48, GTK_ICON_LOOKUP_USE_BUILTIN);
        const gboolean icon_is_builtin = gtk_icon_info_get_filename(icon_info) == NULL;
        gtk_icon_info_free(icon_info);
        icon_name = icon_is_builtin ? "deadbeef" : icon_name;
    }

    if (!gtk_icon_theme_has_icon(theme, icon_name)) {
        char iconpath[1024];
        snprintf (iconpath, sizeof (iconpath), "%s/deadbeef.png", deadbeef->get_prefix ());
        trayicon = gtk_status_icon_new_from_file(iconpath);
    }
    else {
        trayicon = gtk_status_icon_new_from_icon_name(icon_name);
    }
    if (hide_tray_icon) {
        g_object_set (trayicon, "visible", FALSE, NULL);
    }

#if !GTK_CHECK_VERSION(2,14,0)
    g_signal_connect ((gpointer)trayicon, "activate", G_CALLBACK (on_trayicon_activate), NULL);
#else
    printf ("connecting button tray signals\n");
    g_signal_connect ((gpointer)trayicon, "scroll_event", G_CALLBACK (on_trayicon_scroll_event), NULL);
    g_signal_connect ((gpointer)trayicon, "button_press_event", G_CALLBACK (on_trayicon_button_press_event), NULL);
#endif
    g_signal_connect ((gpointer)trayicon, "popup_menu", G_CALLBACK (on_trayicon_popup_menu), NULL);

    gtkui_set_titlebar (NULL);

    return FALSE;
}

static void
gtkui_hide_status_icon () {
    if (trayicon) {
        g_object_set (trayicon, "visible", FALSE, NULL);
    }
}

int
gtkui_get_curr_playlist_mod (void) {
    ddb_playlist_t *plt = deadbeef->plt_get_curr ();
    int res = plt ? deadbeef->plt_get_modification_idx (plt) : 0;
    if (plt) {
        deadbeef->plt_unref (plt);
    }
    return res;
}

void
gtkui_setup_gui_refresh (void) {
    int tm = 1000/gtkui_get_gui_refresh_rate ();

    if (refresh_timeout) {
        g_source_remove (refresh_timeout);
        refresh_timeout = 0;
    }

    refresh_timeout = g_timeout_add (tm, gtkui_on_frameupdate, NULL);
}


static gboolean
gtkui_on_configchanged (void *data) {
    // order and looping
    const char *w;

    // order
//    const char *orderwidgets[4] = { "order_linear", "order_shuffle", "order_random", "order_shuffle_albums" };
//    w = orderwidgets[deadbeef->conf_get_int ("playback.order", PLAYBACK_ORDER_LINEAR)];
//    gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (lookup_widget (mainwin, w)), TRUE);
//
//    // looping
//    const char *loopingwidgets[3] = { "loop_all", "loop_disable", "loop_single" };
//    w = loopingwidgets[deadbeef->conf_get_int ("playback.loop", PLAYBACK_MODE_LOOP_ALL)];
//    gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (lookup_widget (mainwin, w)), TRUE);
//
//    // scroll follows playback
//    gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (lookup_widget (mainwin, "scroll_follows_playback")), deadbeef->conf_get_int ("playlist.scroll.followplayback", 1) ? TRUE : FALSE);
//
//    // cursor follows playback
//    gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (lookup_widget (mainwin, "cursor_follows_playback")), deadbeef->conf_get_int ("playlist.scroll.cursorfollowplayback", 1) ? TRUE : FALSE);
//
//    // stop after current
//    int stop_after_current = deadbeef->conf_get_int ("playlist.stop_after_current", 0);
//    gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (lookup_widget (mainwin, "stop_after_current")), stop_after_current ? TRUE : FALSE);
//
//    // stop after current album
//    int stop_after_album = deadbeef->conf_get_int ("playlist.stop_after_album", 0);
//    gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (lookup_widget (mainwin, "stop_after_album")), stop_after_album ? TRUE : FALSE);
//
//    // embolden current track
//    gtkui_embolden_current_track = deadbeef->conf_get_int ("gtkui.embolden_current_track", 0);
//
//    // pin groups
//    gtkui_groups_pinned = deadbeef->conf_get_int ("playlist.pin.groups", 0);
//
//    // play state images
//    gtkui_unicode_playstate = deadbeef->conf_get_int ("gtkui.unicode_playstate", 0);
//
//    // seekbar overlay
//    gtkui_disable_seekbar_overlay = deadbeef->conf_get_int ("gtkui.disable_seekbar_overlay", 0);

    // tray icon
    gtkui_update_status_icon (NULL);

    // statusbar refresh
    gtkui_setup_gui_refresh ();

    return FALSE;
}

static gboolean
outputchanged_cb (gpointer nothing) {
    preferences_fill_soundcards ();
    return FALSE;
}

void
save_playlist_as (void) {
    gdk_threads_add_idle (action_save_playlist_handler_cb, NULL);
}

void
on_playlist_save_as_activate           (GtkMenuItem     *menuitem,
                                        gpointer         user_data)
{
    save_playlist_as ();
}

void
on_playlist_load_activate              (GtkMenuItem     *menuitem,
                                        gpointer         user_data)
{
    gdk_threads_add_idle (action_load_playlist_handler_cb, NULL);
}

void
on_add_location_activate               (GtkMenuItem     *menuitem,
                                        gpointer         user_data)
{
    gdk_threads_add_idle (action_add_location_handler_cb, NULL);
}

static gboolean
update_win_title_idle (gpointer data) {
    struct fromto_t *ft = (struct fromto_t *)data;
    DB_playItem_t *from = ft->from;
    DB_playItem_t *to = ft->to;
    free (ft);

    // update window title
    if (from || to) {
        if (to) {
            DB_playItem_t *it = deadbeef->streamer_get_playing_track ();
            if (it) { // it might have been deleted after event was sent
                gtkui_set_titlebar (it);
                deadbeef->pl_item_unref (it);
            }
            else {
                gtkui_set_titlebar (NULL);
            }
        }
        else {
            gtkui_set_titlebar (NULL);
        }
    }
    if (from) {
        deadbeef->pl_item_unref (from);
    }
    if (to) {
        deadbeef->pl_item_unref (to);
    }
    return FALSE;
}

int
gtkui_add_new_playlist (void) {
    int cnt = deadbeef->plt_get_count ();
    int i;
    int idx = 0;
    for (;;) {
        char name[100];
        if (!idx) {
            strcpy (name, _("New Playlist"));
        }
        else {
            snprintf (name, sizeof (name), _("New Playlist (%d)"), idx);
        }
        deadbeef->pl_lock ();
        for (i = 0; i < cnt; i++) {
            char t[100];
            ddb_playlist_t *plt = deadbeef->plt_get_for_idx (i);
            deadbeef->plt_get_title (plt, t, sizeof (t));
            deadbeef->plt_unref (plt);
            if (!strcasecmp (t, name)) {
                break;
            }
        }
        deadbeef->pl_unlock ();
        if (i == cnt) {
            return deadbeef->plt_add (cnt, name);
        }
        idx++;
    }
    return -1;
}

int
gtkui_get_gui_refresh_rate () {
    int fps = deadbeef->conf_get_int ("gtkui.refresh_rate", 10);
    if (fps < 1) {
        fps = 1;
    }
    else if (fps > 30) {
        fps = 30;
    }
    return fps;
}

static void
send_messages_to_widgets (ddb_gtkui_widget_t *w, uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    for (ddb_gtkui_widget_t *c = w->children; c; c = c->next) {
        send_messages_to_widgets (c, id, ctx, p1, p2);
    }
    if (w->message) {
        w->message (w, id, ctx, p1, p2);
    }
}

gboolean
add_mainmenu_actions_cb (void *data) {
    //add_mainmenu_actions ();
    return FALSE;
}

int
gtkui_thread (void *ctx);

int
gtkui_plt_add_dir (ddb_playlist_t *plt, const char *dirname, int (*cb)(DB_playItem_t *it, void *data), void *user_data);

int
gtkui_plt_add_file (ddb_playlist_t *plt, const char *filename, int (*cb)(DB_playItem_t *it, void *data), void *user_data);

int
gtkui_pl_add_files_begin (ddb_playlist_t *plt);

void
gtkui_pl_add_files_end (void);

DB_playItem_t *
gtkui_plt_load (ddb_playlist_t *plt, DB_playItem_t *after, const char *fname, int *pabort, int (*cb)(DB_playItem_t *it, void *data), void *user_data);

int
gtkui_message (uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    if (!gtkui_accept_messages) {
        return -1;
    }
    ddb_gtkui_widget_t *rootwidget = w_get_rootwidget ();
    if (rootwidget) {
        send_messages_to_widgets (rootwidget, id, ctx, p1, p2);
    }
    switch (id) {
    case DB_EV_ACTIVATED:
        g_idle_add (activate_cb, NULL);
        break;
    case DB_EV_SONGCHANGED:
        {
            ddb_event_trackchange_t *ev = (ddb_event_trackchange_t *)ctx;
            gtkpl_songchanged_wrapper (ev->from, ev->to);
        }
        break;
    case DB_EV_TRACKINFOCHANGED:
        {
            ddb_event_track_t *ev = (ddb_event_track_t *)ctx;
            if (ev->track) {
                deadbeef->pl_item_ref (ev->track);
            }
            g_idle_add (trackinfochanged_cb, ev->track);
        }
        break;
//    case DB_EV_PAUSED:
//        g_idle_add (paused_cb, NULL);
//        break;
    case DB_EV_PLAYLISTCHANGED:
        g_idle_add (playlistchanged_cb, NULL);
        break;
    case DB_EV_CONFIGCHANGED:
        g_idle_add (gtkui_on_configchanged, NULL);
        break;
    case DB_EV_OUTPUTCHANGED:
        g_idle_add (outputchanged_cb, NULL);
        break;
    case DB_EV_PLAYLISTSWITCHED:
        g_idle_add (playlistswitch_cb, NULL);
        break;
    case DB_EV_ACTIONSCHANGED:
        g_idle_add (add_mainmenu_actions_cb, NULL);
        break;
    case DB_EV_DSPCHAINCHANGED:
        eq_refresh ();
        break;
    }
    return 0;
}

static const char gtkui_def_layout[] = "vbox expand=\"0 1\" fill=\"1 1\" homogeneous=0 {hbox expand=\"0 1 0\" fill=\"1 1 1\" homogeneous=0 {playtb {} seekbar {} volumebar {} } tabbed_playlist hideheaders=0 {} } ";

static void
init_widget_layout (void) {
    w_init ();
    ddb_gtkui_widget_t *rootwidget = w_get_rootwidget ();
    gtk_widget_show (rootwidget->widget);
    gtk_box_pack_start (GTK_BOX(lookup_widget(mainwin, "plugins_bottom_vbox")), rootwidget->widget, TRUE, TRUE, 0);

    // load layout
    // config var name is defined in DDB_GTKUI_CONF_LAYOUT
    // gtkui.layout: 0.6.0 and 0.6.1
    // gtkui.layout.major.minor.point: later versions

    char layout[20000];
    deadbeef->conf_get_str (DDB_GTKUI_CONF_LAYOUT, "-", layout, sizeof (layout));
    if (!strcmp (layout, "-")) {
        // upgrade from 0.6.0 to 0.6.2
        char layout_060[20000];
        deadbeef->conf_get_str ("gtkui.layout", "-", layout_060, sizeof (layout_060));
        if (!strcmp (layout_060, "-")) {
            // new setup
            strcpy (layout, gtkui_def_layout);
        }
        else {
            // upgrade with top bar
            snprintf (layout, sizeof (layout), "vbox expand=\"0 1\" fill=\"1 1\" homogeneous=0 {hbox expand=\"0 1 0\" fill=\"1 1 1\" homogeneous=0 {playtb {} seekbar {} volumebar {} } %s }", layout_060);
            deadbeef->conf_set_str (DDB_GTKUI_CONF_LAYOUT, layout);
            deadbeef->conf_save ();
        }
    }

    ddb_gtkui_widget_t *w = NULL;
    w_create_from_string (layout, &w);
    if (!w) {
        ddb_gtkui_widget_t *plt = w_create ("tabbed_playlist");
        w_append (rootwidget, plt);
        gtk_widget_show (plt->widget);
    }
    else {
        w_append (rootwidget, w);
    }
}

static DB_plugin_t *supereq_plugin;

gboolean
gtkui_connect_cb (void *none) {
    // equalizer
//    GtkWidget *eq_mi = lookup_widget (mainwin, "view_eq");
//    if (!supereq_plugin) {
//        gtk_widget_hide (GTK_WIDGET (eq_mi));
//    }
//    else {
//        if (deadbeef->conf_get_int ("gtkui.eq.visible", 0)) {
//            gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (eq_mi), TRUE);
//            eq_window_show ();
//        }
//        else {
//            gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (eq_mi), FALSE);
//        }
//    }
//
//    // cover_art
//    DB_plugin_t **plugins = deadbeef->plug_get_list ();
//    for (int i = 0; plugins[i]; i++) {
//        DB_plugin_t *p = plugins[i];
//        if (p->id && !strcmp (p->id, "artwork") && p->version_major == 1 && p->version_minor >= 2) {
//            trace ("gtkui: found cover-art loader plugin\n");
//            coverart_plugin = (DB_artwork_plugin_t *)p;
//            break;
//        }
//    }
//    add_mainmenu_actions ();
    ddb_event_t *e = deadbeef->event_alloc (DB_EV_TRACKINFOCHANGED);
    deadbeef->event_send(e, 0, 0);
    return FALSE;
}

int
gtkui_add_file_info_cb (ddb_fileadd_data_t *data, void *user_data) {
    if (data->visibility == 0) {
        if (progress_is_aborted ()) {
            return -1;
        }
        deadbeef->pl_lock ();
        const char *fname = deadbeef->pl_find_meta (data->track, ":URI");
        g_idle_add (gtkui_set_progress_text_idle, (gpointer)strdup(fname)); // slowwwww
        deadbeef->pl_unlock ();
    }
    return 0;
}

void
gtkui_add_file_begin_cb (ddb_fileadd_data_t *data, void *user_data) {
    if (data->visibility == 0) {
        progress_show ();
    }
}

void
gtkui_add_file_end_cb (ddb_fileadd_data_t *data, void *user_data) {
    if (data->visibility == 0) {
        progress_hide ();
    }
}

#define ASSETS_PATH "./assets/"

enum {
    SURF_MAIN,
    SURF_TITLEBAR,
    SURF_CBUTTONS,
    SURF_BALANCE,
    SURF_MONOSTER,
    SURF_NUMBERS,
    SURF_NUMS_EX,
    SURF_PLAYPAUS,
    SURF_POSBAR,
    SURF_SHUFREP,
    SURF_TEXT,
    SURF_VOLUME,
    SURF_PLEDIT,
    SURF_MAX
};

cairo_surface_t *skin_surfs[SURF_MAX];

static int mx, my;
static int m1on;
static int mlock;

typedef struct {
    int surf;
    int x, y, w, h, sx, sy;
    int hidden;
} sprite_t;

#define MAX_STATE_SPRITES 4
typedef struct {
    int sprites[MAX_STATE_SPRITES];
    int nsprites;
} elemstate_t;

#define MAX_ELEM_STATES 30
typedef struct {
    int states[MAX_ELEM_STATES];
    int nstates;
} elem_t;

#define MAX_STATES 800
elemstate_t elemsstates[MAX_STATES];

#define MAX_ELEMS 200
elem_t elems[MAX_ELEMS];

#define spr_init(idx, _surf, _x, _y, _w, _h, _sx, _sy) { sprites[idx].surf = _surf;\
    sprites[idx].x = _x;\
    sprites[idx].y = _y;\
    sprites[idx].w = _w;\
    sprites[idx].h = _h;\
    sprites[idx].sx = _sx;\
    sprites[idx].sy = _sy;\
}

enum {
    SPR_MAIN,
    SPR_TITLEBAR,
    SPR_TITLEBAR_INACTIVE,
    SPR_TITLEBAR_SHADED,
    SPR_TITLEBAR_SHADED_INACTIVE,
    SPR_TITLEBAR_MENU,
    SPR_TITLEBAR_MENU_PRESSED,
    SPR_TITLEBAR_MIN,
    SPR_TITLEBAR_MIN_PRESSED,
    SPR_TITLEBAR_CLOSE,
    SPR_TITLEBAR_CLOSE_PRESSED,
    SPR_TITLEBAR_SHADE,
    SPR_TITLEBAR_SHADE_PRESSED,
    SPR_TITLEBAR_UNSHADE,
    SPR_TITLEBAR_UNSHADE_PRESSED,
    SPR_SMBTN_1,
    SPR_SMBTN_2,
    SPR_SMBTN_3,
    SPR_SMBTN_4,
    SPR_SMBTN_5,
    SPR_SMBTN_6,
    SPR_SMBTN_7,
    SPR_CBUTTON_PREV,
    SPR_CBUTTON_PREV_PRESSED,
    SPR_CBUTTON_PLAY,
    SPR_CBUTTON_PLAY_PRESSED,
    SPR_CBUTTON_PAUSE,
    SPR_CBUTTON_PAUSE_PRESSED,
    SPR_CBUTTON_STOP,
    SPR_CBUTTON_STOP_PRESSED,
    SPR_CBUTTON_NEXT,
    SPR_CBUTTON_NEXT_PRESSED,
    SPR_CBUTTON_OPEN,
    SPR_CBUTTON_OPEN_PRESSED,
    SPR_BALANCE_1,
    SPR_BALANCE_2,
    SPR_BALANCE_3,
    SPR_BALANCE_4,
    SPR_BALANCE_5,
    SPR_BALANCE_6,
    SPR_BALANCE_7,
    SPR_BALANCE_8,
    SPR_BALANCE_9,
    SPR_BALANCE_10,
    SPR_BALANCE_11,
    SPR_BALANCE_12,
    SPR_BALANCE_13,
    SPR_BALANCE_14,
    SPR_BALANCE_15,
    SPR_BALANCE_16,
    SPR_BALANCE_17,
    SPR_BALANCE_18,
    SPR_BALANCE_19,
    SPR_BALANCE_20,
    SPR_BALANCE_21,
    SPR_BALANCE_22,
    SPR_BALANCE_23,
    SPR_BALANCE_24,
    SPR_BALANCE_25,
    SPR_BALANCE_26,
    SPR_BALANCE_27,
    SPR_BALANCE_28,
    SPR_BALANCE_GRIP,
    SPR_BALANCE_GRIP_PRESSED,
    SPR_VOLUME_1,
    SPR_VOLUME_2,
    SPR_VOLUME_3,
    SPR_VOLUME_4,
    SPR_VOLUME_5,
    SPR_VOLUME_6,
    SPR_VOLUME_7,
    SPR_VOLUME_8,
    SPR_VOLUME_9,
    SPR_VOLUME_10,
    SPR_VOLUME_11,
    SPR_VOLUME_12,
    SPR_VOLUME_13,
    SPR_VOLUME_14,
    SPR_VOLUME_15,
    SPR_VOLUME_16,
    SPR_VOLUME_17,
    SPR_VOLUME_18,
    SPR_VOLUME_19,
    SPR_VOLUME_20,
    SPR_VOLUME_21,
    SPR_VOLUME_22,
    SPR_VOLUME_23,
    SPR_VOLUME_24,
    SPR_VOLUME_25,
    SPR_VOLUME_26,
    SPR_VOLUME_27,
    SPR_VOLUME_28,
    SPR_VOLUME_GRIP,
    SPR_VOLUME_GRIP_PRESSED,
    SPR_POSBAR,
    SPR_POSBAR_GRIP,
    SPR_POSBAR_GRIP_PRESSED,
    SPR_REPEAT,
    SPR_REPEAT_PRESSED,
    SPR_REPEAT_ACTIVE,
    SPR_REPEAT_ACTIVE_PRESSED,
    SPR_SHUFFLE,
    SPR_SHUFFLE_PRESSED,
    SPR_SHUFFLE_ACTIVE,
    SPR_SHUFFLE_ACTIVE_PRESSED,
    SPR_EQ,
    SPR_EQ_PRESSED,
    SPR_EQ_ACTIVE,
    SPR_EQ_ACTIVE_PRESSED,
    SPR_PLAYLIST,
    SPR_PLAYLIST_PRESSED,
    SPR_PLAYLIST_ACTIVE,
    SPR_PLAYLIST_ACTIVE_PRESSED,
    SPR_STEREO,
    SPR_STEREO_ACTIVE,
    SPR_MONO,
    SPR_MONO_ACTIVE,
    SPR_INDICATOR_PLAY,
    SPR_INDICATOR_PAUSE,
    SPR_INDICATOR_STOP,
    SPR_INDICATOR_EMPTY,
    SPR_INDICATOR_WEIRD1,
    SPR_INDICATOR_WEIRD2,
    SPR_MINUS,
    SPR_PLUS,
    SPR_NUM_0,
    SPR_NUM_1,
    SPR_NUM_2,
    SPR_NUM_3,
    SPR_NUM_4,
    SPR_NUM_5,
    SPR_NUM_6,
    SPR_NUM_7,
    SPR_NUM_8,
    SPR_NUM_9,
    SPR_MAX,
};

sprite_t sprites[SPR_MAX];


static int
load_assets (void) {
    skin_surfs[SURF_MAIN]= cairo_image_surface_create_from_png (ASSETS_PATH "main.png");
    skin_surfs[SURF_TITLEBAR] = cairo_image_surface_create_from_png (ASSETS_PATH "titlebar.png");
    skin_surfs[SURF_CBUTTONS] = cairo_image_surface_create_from_png (ASSETS_PATH "cbuttons.png");
    skin_surfs[SURF_BALANCE] = cairo_image_surface_create_from_png (ASSETS_PATH "balance.png");
    skin_surfs[SURF_MONOSTER] = cairo_image_surface_create_from_png (ASSETS_PATH "monoster.png");
    skin_surfs[SURF_NUMBERS] = cairo_image_surface_create_from_png (ASSETS_PATH "numbers.png");
    //skin_surfs[SURF_NUMS_EX] = cairo_image_surface_create_from_png (ASSETS_PATH "nums_ex.png");
    skin_surfs[SURF_PLAYPAUS] = cairo_image_surface_create_from_png (ASSETS_PATH "playpaus.png");
    skin_surfs[SURF_POSBAR] = cairo_image_surface_create_from_png (ASSETS_PATH "posbar.png");
    skin_surfs[SURF_SHUFREP] = cairo_image_surface_create_from_png (ASSETS_PATH "shufrep.png");
    skin_surfs[SURF_TEXT] = cairo_image_surface_create_from_png (ASSETS_PATH "text.png");
    skin_surfs[SURF_TITLEBAR] = cairo_image_surface_create_from_png (ASSETS_PATH "titlebar.png");
    skin_surfs[SURF_VOLUME] = cairo_image_surface_create_from_png (ASSETS_PATH "volume.png");
    skin_surfs[SURF_PLEDIT] = cairo_image_surface_create_from_png (ASSETS_PATH "pledit.png");

    // main
    spr_init (SPR_MAIN, SURF_MAIN, 0, 0, 275, 116, 0, 0);

    // titlebar
    spr_init (SPR_TITLEBAR, SURF_TITLEBAR, 0, 0, 275, 15, 27, 0);
    spr_init (SPR_TITLEBAR_INACTIVE, SURF_TITLEBAR, 0, 0, 275, 14, 27, 15);
    spr_init (SPR_TITLEBAR_SHADED, SURF_TITLEBAR, 0, 0, 275, 13, 27, 29);
    spr_init (SPR_TITLEBAR_SHADED_INACTIVE, SURF_TITLEBAR, 0, 0, 275, 13, 27, 42);

    // titlebar buttons
    // menu
    spr_init (SPR_TITLEBAR_MENU, SURF_TITLEBAR, 6, 3, 9, 9, 0, 0);
    spr_init (SPR_TITLEBAR_MENU_PRESSED, SURF_TITLEBAR, 6, 3, 9, 9, 0, 9);
    // minimize
    spr_init (SPR_TITLEBAR_MIN, SURF_TITLEBAR, 244, 4, 9, 9, 9, 0);
    spr_init (SPR_TITLEBAR_MIN_PRESSED, SURF_TITLEBAR, 244, 4, 9, 9, 9, 9);
    // close
    spr_init (SPR_TITLEBAR_CLOSE, SURF_TITLEBAR, 264, 4, 9, 9, 18, 0);
    spr_init (SPR_TITLEBAR_CLOSE_PRESSED, SURF_TITLEBAR, 264, 4, 9, 9, 18, 9);
    // shade
    spr_init (SPR_TITLEBAR_SHADE, SURF_TITLEBAR, 254, 4, 9, 9, 0, 18);
    spr_init (SPR_TITLEBAR_SHADE_PRESSED, SURF_TITLEBAR, 254, 4, 9, 9, 9, 18);
    // unshade
    spr_init (SPR_TITLEBAR_UNSHADE, SURF_TITLEBAR, 254, 4, 9, 9, 0, 27);
    spr_init (SPR_TITLEBAR_UNSHADE_PRESSED, SURF_TITLEBAR, 254, 4, 9, 9, 9, 27);

    // small buttons
    spr_init (SPR_SMBTN_1, SURF_TITLEBAR, 10, 22, 8, 43, 304, 0);
    spr_init (SPR_SMBTN_2, SURF_TITLEBAR, 10, 22, 8, 43, 312, 0);
    spr_init (SPR_SMBTN_3, SURF_TITLEBAR, 10, 22, 8, 43, 304, 44);
    spr_init (SPR_SMBTN_4, SURF_TITLEBAR, 10, 22, 8, 43, 312, 44);
    spr_init (SPR_SMBTN_5, SURF_TITLEBAR, 10, 22, 8, 43, 320, 44);
    spr_init (SPR_SMBTN_6, SURF_TITLEBAR, 10, 22, 8, 43, 328, 44);
    spr_init (SPR_SMBTN_7, SURF_TITLEBAR, 10, 22, 8, 43, 336, 44);

    // cbuttons
    int xoffs = 0;
    int yoffs = 88;
    spr_init (SPR_CBUTTON_PREV, SURF_CBUTTONS, xoffs+16, yoffs, 22, 18, xoffs, 0);
    spr_init (SPR_CBUTTON_PREV_PRESSED, SURF_CBUTTONS, xoffs+16, yoffs, 22, 18, xoffs, 18);
    xoffs += 22;
    spr_init (SPR_CBUTTON_PLAY, SURF_CBUTTONS, xoffs+16, yoffs, 23, 18, xoffs, 0);
    spr_init (SPR_CBUTTON_PLAY_PRESSED, SURF_CBUTTONS, xoffs+16, yoffs, 23, 18, xoffs, 18);
    xoffs += 23;
    spr_init (SPR_CBUTTON_PAUSE, SURF_CBUTTONS, xoffs+16, yoffs, 23, 18, xoffs, 0);
    spr_init (SPR_CBUTTON_PAUSE_PRESSED, SURF_CBUTTONS, xoffs+16, yoffs, 23, 18, xoffs, 18);
    xoffs += 23;
    spr_init (SPR_CBUTTON_STOP, SURF_CBUTTONS, xoffs+16, yoffs, 23, 18, xoffs, 0);
    spr_init (SPR_CBUTTON_STOP_PRESSED, SURF_CBUTTONS, xoffs+16, yoffs, 23, 18, xoffs, 18);
    xoffs += 23;
    spr_init (SPR_CBUTTON_NEXT, SURF_CBUTTONS, xoffs+16, yoffs, 22, 18, xoffs, 0);
    spr_init (SPR_CBUTTON_NEXT_PRESSED, SURF_CBUTTONS, xoffs+16, yoffs, 22, 18, xoffs, 18);

    spr_init (SPR_CBUTTON_OPEN, SURF_CBUTTONS, 136, 89, 22, 16, 114, 0);
    spr_init (SPR_CBUTTON_OPEN_PRESSED, SURF_CBUTTONS, 136, 89, 22, 16, 114, 16);

    // balance/volume
    for (int i = 0; i < 28; i++) {
        spr_init (SPR_BALANCE_1 + i, SURF_BALANCE, 177, 57, 37, 13, 9, i*15);
        spr_init (SPR_VOLUME_1 + i, SURF_VOLUME, 107, 57, 68, 13, 0, i*15);
    }
    // volume grip
    spr_init (SPR_VOLUME_GRIP, SURF_VOLUME, 148, 58, 14, 11, 15, 422);
    spr_init (SPR_VOLUME_GRIP_PRESSED, SURF_VOLUME, 148, 58, 14, 11, 0, 422);

    // balance grip
    spr_init (SPR_BALANCE_GRIP, SURF_BALANCE, 189, 58, 14, 11, 15, 422);
    spr_init (SPR_BALANCE_GRIP_PRESSED, SURF_BALANCE, 189, 58, 14, 11, 0, 422);

    // posbar
    spr_init (SPR_POSBAR, SURF_POSBAR, 15, 72, 248, 10, 0, 0);
    spr_init (SPR_POSBAR_GRIP, SURF_POSBAR, 13, 72, 29, 10, 248, 0);
    spr_init (SPR_POSBAR_GRIP_PRESSED, SURF_POSBAR, 13, 72, 29, 10, 248+29, 0);

    // repeat
    spr_init (SPR_REPEAT, SURF_SHUFREP, 210, 89, 29, 15, 0, 0);
    spr_init (SPR_REPEAT_PRESSED, SURF_SHUFREP, 210, 89, 29, 15, 0, 15);
    spr_init (SPR_REPEAT_ACTIVE, SURF_SHUFREP, 210, 89, 29, 15, 0, 30);
    spr_init (SPR_REPEAT_ACTIVE_PRESSED, SURF_SHUFREP, 210, 89, 29, 15, 0, 45);

    // shuffle
    spr_init (SPR_SHUFFLE, SURF_SHUFREP, 165, 89, 46, 15, 29, 0);
    spr_init (SPR_SHUFFLE_PRESSED, SURF_SHUFREP, 165, 89, 46, 15, 29, 15);
    spr_init (SPR_SHUFFLE_ACTIVE, SURF_SHUFREP, 165, 89, 46, 15, 29, 30);
    spr_init (SPR_SHUFFLE_ACTIVE_PRESSED, SURF_SHUFREP, 165, 89, 46, 15, 29, 45);

    // EQ
    spr_init (SPR_EQ, SURF_SHUFREP, 220, 58, 25, 12, 0, 61);
    spr_init (SPR_EQ_PRESSED, SURF_SHUFREP, 220, 58, 25, 12, 47, 61);
    spr_init (SPR_EQ_ACTIVE, SURF_SHUFREP, 220, 58, 25, 12, 0, 73);
    spr_init (SPR_EQ_ACTIVE_PRESSED, SURF_SHUFREP, 220, 58, 25, 12, 47, 73);

    // PLAYLIST
    spr_init (SPR_PLAYLIST, SURF_SHUFREP, 244, 58, 22, 12, 25, 61);
    spr_init (SPR_PLAYLIST_PRESSED, SURF_SHUFREP, 244, 58, 22, 12, 71, 73);
    spr_init (SPR_PLAYLIST_ACTIVE, SURF_SHUFREP, 244, 58, 22, 12, 25, 61);
    spr_init (SPR_PLAYLIST_ACTIVE_PRESSED, SURF_SHUFREP, 244, 58, 22, 12, 71, 73);

    // monoster
    spr_init (SPR_STEREO_ACTIVE, SURF_MONOSTER, 239, 41, 29, 11, 0, 0);
    spr_init (SPR_STEREO, SURF_MONOSTER, 239, 41, 29, 11, 0, 12);
    spr_init (SPR_MONO_ACTIVE, SURF_MONOSTER, 214, 41, 25, 11, 31, 0);
    spr_init (SPR_MONO, SURF_MONOSTER, 214, 41, 25, 11, 31, 12);

    // playpaus
    spr_init (SPR_INDICATOR_PLAY, SURF_PLAYPAUS, 26, 28, 9, 9, 0, 0);
    spr_init (SPR_INDICATOR_PAUSE, SURF_PLAYPAUS, 26, 28, 9, 9, 9, 0);
    spr_init (SPR_INDICATOR_STOP, SURF_PLAYPAUS, 26, 28, 9, 9, 18, 0);
    spr_init (SPR_INDICATOR_EMPTY, SURF_PLAYPAUS, 26, 28, 9, 9, 27, 0);
    spr_init (SPR_INDICATOR_WEIRD1, SURF_PLAYPAUS, 24, 28, 3, 9, 36, 0);
    spr_init (SPR_INDICATOR_WEIRD2, SURF_PLAYPAUS, 24, 28, 3, 9, 39, 0);

    // nums_ex
    spr_init (SPR_PLUS, SURF_NUMS_EX, 36, 26, 10, 13, 90, 0);
    spr_init (SPR_MINUS, SURF_NUMS_EX, 36, 26, 10, 13, 100, 0);

    // numbers
    // used in coords:
    // 48,26; 60,26; 78,26; 90,26
    for (int i = 0; i <= 9; i++) {
        spr_init (SPR_NUM_0+i, SURF_NUMBERS, 48, 26, 9, 13, 9*i, 0);
    }
}

static void
draw_sprite (cairo_t *cr, int s) {
    sprite_t *spr = &sprites[s];
    if (!skin_surfs[spr->surf] || spr->hidden) {
        return;

    }
    cairo_set_source_surface (cr, skin_surfs[spr->surf], -spr->sx+spr->x, -spr->sy+spr->y);
    cairo_rectangle (cr, spr->x, spr->y, spr->w, spr->h);
    cairo_fill (cr);
}

static float skin_balance = 0.5f;
static int skin_button = -1;

static gboolean
main_draw (GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    DB_playItem_t *trk = deadbeef->streamer_get_playing_track ();
    if (!trk || deadbeef->pl_get_item_duration (trk) < 0) {
        sprites[SPR_POSBAR_GRIP].hidden = 1;
        sprites[SPR_POSBAR_GRIP_PRESSED].hidden = 1;
    }
    else {
        sprites[SPR_POSBAR_GRIP].hidden = 0;
        sprites[SPR_POSBAR_GRIP_PRESSED].hidden = 1;
        float pos = 0;
        if (deadbeef->pl_get_item_duration (trk) > 0) {
            pos = deadbeef->streamer_get_playpos () / deadbeef->pl_get_item_duration (trk);
        }
        sprites[SPR_POSBAR_GRIP].x = 13 + pos * (248+14);
    }
    if (trk) {
        deadbeef->pl_item_unref (trk);
    }

    // adjust volume
    int state = 0;
    float vol = deadbeef->volume_get_db ();
    vol = (1.f - (vol / -50));
    int w = sprites[SPR_VOLUME_1].w - sprites[SPR_VOLUME_GRIP].w;
    sprites[SPR_VOLUME_GRIP].x = sprites[SPR_VOLUME_GRIP_PRESSED].x = sprites[SPR_VOLUME_1].x + vol * w;
    state = SPR_VOLUME_1 + vol * 27;
    if (state < SPR_VOLUME_1) {
        state = SPR_VOLUME_1;
    }
    if (state > SPR_VOLUME_28) {
        state = SPR_VOLUME_28;
    }
    for (int i = SPR_VOLUME_1; i <= SPR_VOLUME_28; i++) {
        sprites[i].hidden = (i != state);
    }
    sprites[SPR_VOLUME_GRIP].hidden = (mlock == SPR_VOLUME_1);
    sprites[SPR_VOLUME_GRIP_PRESSED].hidden = (mlock != SPR_VOLUME_1);

    // adjust balance
    w =  sprites[SPR_BALANCE_1].w - sprites[SPR_BALANCE_GRIP].w;
    sprites[SPR_BALANCE_GRIP].x = sprites[SPR_BALANCE_GRIP_PRESSED].x = sprites[SPR_BALANCE_1].x + skin_balance * w;
    state = SPR_BALANCE_1 + skin_balance * 27;
    if (state < SPR_BALANCE_1) {
        state = SPR_BALANCE_1;
    }
    if (state > SPR_BALANCE_28) {
        state = SPR_BALANCE_28;
    }
    for (int i = SPR_BALANCE_1; i <= SPR_BALANCE_28; i++) {
        sprites[i].hidden = (i != state);
    }
    sprites[SPR_BALANCE_GRIP].hidden = (mlock == SPR_BALANCE_1);
    sprites[SPR_BALANCE_GRIP_PRESSED].hidden = (mlock != SPR_BALANCE_1);

    // buttons
    for (int i = SPR_CBUTTON_PREV; i <= SPR_CBUTTON_OPEN; i+= 2) {
        if (skin_button != i) {
            sprites[i].hidden = 0;
            sprites[i+1].hidden = 1;
        }
        else {
            sprites[i].hidden = 1;
            sprites[i+1].hidden = 0;
        }
    }

    // main
    for (int i = 0; i < SPR_MAX; i++) {
        draw_sprite (cr, i);
    }

    // nums_ex
    int min = 5;
    int sec = 23;

    if (min > 99) {
        min = 99;
    }
    if (sec > 59) {
        sec = 59;
    }

    int dig1 = min/10;
    int dig2 = min%10;
    int dig3 = sec/10;
    int dig4 = sec%10;

    // +
    draw_sprite(cr, SPR_PLUS);
    // -
    draw_sprite(cr, SPR_MINUS);
    // min
    sprites[SPR_NUM_0+dig1].x = 48;
    draw_sprite (cr, SPR_NUM_0+dig1);
    sprites[SPR_NUM_0+dig2].x = 60;
    draw_sprite (cr, SPR_NUM_0+dig1);
    // sec
    sprites[SPR_NUM_0+dig3].x = 78;
    draw_sprite (cr, SPR_NUM_0+dig1);
    sprites[SPR_NUM_0+dig4].x = 90;
    draw_sprite (cr, SPR_NUM_0+dig1);

    // text
    cairo_save (cr);
    cairo_rectangle (cr, 109, 24, 157, 12);
    cairo_clip (cr);
    cairo_set_source_rgb(cr, 0x06/255.f, 0xbd/255.f, 0x01/255.f);
    cairo_set_font_size (cr, 8);
    cairo_move_to (cr, 109, 33);

    DB_playItem_t *it = deadbeef->streamer_get_playing_track ();
    char fmt[500];
    char str[600];
    if (it) {
        deadbeef->conf_get_str ("gtkui.titlebar_playing", "%a - %t - DeaDBeeF-%V", fmt, sizeof (fmt));
    }
    else {
        deadbeef->conf_get_str ("gtkui.titlebar_stopped", "DeaDBeeF-%V", fmt, sizeof (fmt));
    }
    deadbeef->pl_format_title (it, -1, str, sizeof (str), -1, fmt);
    if (it) {
        deadbeef->pl_item_unref (it);
    }

    cairo_show_text (cr, str);
    cairo_restore (cr);
    return TRUE;
}

static gboolean
main_expose_event (GtkWidget *widget, GdkEventExpose *event, gpointer user_data) {
    cairo_t *cr = gdk_cairo_create (gtk_widget_get_window (widget));
    gboolean res = main_draw (widget, cr, user_data);
    cairo_destroy (cr);
    return res;
}

static gboolean
pl_draw (GtkWidget *widget, cairo_t *cr, gpointer user_data) {
#if 0
    GtkAllocation a;
    gtk_widget_get_allocation (widget, &a);

    // titlebar
    draw_sprite (cr, surf_pledit, 0, 0, 25, 20, 0, 0);
    draw_sprite (cr, surf_pledit, 25, 0, 100, 20, 26, 0);
    int x = 125;
    while (x < a.width-25) {
        draw_sprite (cr, surf_pledit, x, 0, 25, 20, 127, 0);
        x += 25;
    }
    draw_sprite (cr, surf_pledit, a.width-25, 0, 25, 20, 153, 0);

    // playlist frame
    int y = 20;
    while (y < a.height - 38) {
        draw_sprite (cr, surf_pledit, 0, y, 25, 29, 0, 42);
        draw_sprite (cr, surf_pledit, a.width-25, y, 25, 29, 26, 42);
        y += 29;
    }

    // playlist bg
    cairo_set_source_rgb (cr, 0, 0, 0);
    cairo_rectangle (cr, 25, 20, a.width-50, a.height-20-38);
    cairo_fill (cr);

    // playlist bottom
    draw_sprite (cr, surf_pledit, 0, a.height-38, 125, 38, 0, 72);
    x = 125;
    while (x < a.width - 150) {
        draw_sprite (cr, surf_pledit, x, a.height-38, 25, 38, 179, 0);
        x += 25;
    }
    draw_sprite (cr, surf_pledit, a.width-150, a.height-38, 150, 38, 126, 72);

    // scrollbar
    draw_sprite (cr, surf_pledit, a.width-15, 20, 8, 18, 52, 53);
    //draw_sprite (cr, surf_pledit, a.width-15, 20, 8, 18, 61, 53);

    // menus

    // +
    draw_sprite (cr, surf_pledit, 14, a.height - 30, 22, 18, 0, 149);
    //draw_sprite (cr, surf_pledit, 14, a.height - 30, 22, 18, 23, 149);

    draw_sprite (cr, surf_pledit, 14, a.height - 48, 22, 18, 0, 130);
    //draw_sprite (cr, surf_pledit, 14, a.height - 48, 22, 18, 23, 130);

    draw_sprite (cr, surf_pledit, 14, a.height - 65, 22, 18, 0, 111);
    //draw_sprite (cr, surf_pledit, 14, a.height - 65, 22, 18, 23, 111);

    draw_sprite (cr, surf_pledit, 11, a.height - 65, 3, 53, 48, 111);


    // -
    draw_sprite (cr, surf_pledit, 43, a.height - 30, 22, 18, 54, 168);
    //draw_sprite (cr, surf_pledit, 43, a.height - 30, 22, 18, 77, 168);

    draw_sprite (cr, surf_pledit, 43, a.height - 48, 22, 18, 54, 149);
    //draw_sprite (cr, surf_pledit, 43, a.height - 48, 22, 18, 77, 149);

    draw_sprite (cr, surf_pledit, 43, a.height - 65, 22, 18, 54, 130);
    //draw_sprite (cr, surf_pledit, 43, a.height - 65, 22, 18, 77, 130);

    draw_sprite (cr, surf_pledit, 43, a.height - 83, 22, 18, 54, 111);
    //draw_sprite (cr, surf_pledit, 43, a.height - 83, 22, 18, 77, 111);

    draw_sprite (cr, surf_pledit, 40, a.height - 84, 3, 72, 100, 111);

    // A
    draw_sprite (cr, surf_pledit, 72, a.height - 30, 22, 18, 104, 149);
    //draw_sprite (cr, surf_pledit, 72, a.height - 30, 22, 18, 127, 149);

    draw_sprite (cr, surf_pledit, 72, a.height - 48, 22, 18, 104, 130);
    //draw_sprite (cr, surf_pledit, 72, a.height - 48, 22, 18, 127, 130);

    draw_sprite (cr, surf_pledit, 72, a.height - 65, 22, 18, 104, 111);
    //draw_sprite (cr, surf_pledit, 72, a.height - 65, 22, 18, 127, 111);

    draw_sprite (cr, surf_pledit, 69, a.height - 65, 3, 53, 150, 111);

    // M
    draw_sprite (cr, surf_pledit, 101, a.height - 30, 22, 18, 154, 149);
    //draw_sprite (cr, surf_pledit, 101, a.height - 30, 22, 18, 177, 149);

    draw_sprite (cr, surf_pledit, 101, a.height - 48, 22, 18, 154, 130);
    //draw_sprite (cr, surf_pledit, 101, a.height - 48, 22, 18, 177, 130);

    draw_sprite (cr, surf_pledit, 101, a.height - 65, 22, 18, 154, 111);
    //draw_sprite (cr, surf_pledit, 101, a.height - 65, 22, 18, 177, 111);

    draw_sprite (cr, surf_pledit, 98, a.height - 65, 3, 53, 200, 112);

    // LIST
    draw_sprite (cr, surf_pledit, a.width-44, a.height - 30, 22, 18, 204, 149);
    //draw_sprite (cr, surf_pledit, a.width-44, a.height - 30, 22, 18, 227, 149);

    draw_sprite (cr, surf_pledit, a.width-44, a.height - 48, 22, 18, 204, 130);
    //draw_sprite (cr, surf_pledit, a.width-44, a.height - 48, 22, 18, 227, 130);

    draw_sprite (cr, surf_pledit, a.width-44, a.height - 65, 22, 18, 204, 111);
    //draw_sprite (cr, surf_pledit, a.width-44, a.height - 65, 22, 18, 227, 111);

    draw_sprite (cr, surf_pledit, a.width-47, a.height - 65, 3, 53, 250, 112);
#endif
    return TRUE;
}

static gboolean
pl_expose_event (GtkWidget *widget, GdkEventExpose *event, gpointer user_data) {
    cairo_t *cr = gdk_cairo_create (gtk_widget_get_window (widget));
    gboolean res = pl_draw (widget, cr, user_data);
    cairo_destroy (cr);
    return res;
}

static void
skin_volume_update (int x, int y) {
    int w = sprites[SPR_VOLUME_1].w - sprites[SPR_VOLUME_GRIP_PRESSED].w;
    x = x - (sprites[SPR_VOLUME_1].x + sprites[SPR_VOLUME_GRIP_PRESSED].w/2);
    float vol = x / (float)w;
    // convert do db
    vol = (1.f-vol) * -50.f;
    if (vol < -50) {
        vol = -50;
    }
    if (vol > 0) {
        vol = 0;
    }
    deadbeef->volume_set_db (vol);
}

static void
skin_balance_update (int x, int y) {
    int w = sprites[SPR_BALANCE_1].w - sprites[SPR_BALANCE_GRIP_PRESSED].w;
    skin_balance = (x - (sprites[SPR_BALANCE_1].x + sprites[SPR_BALANCE_GRIP_PRESSED].w/2)) / (float)w;
    if (skin_balance < 0) {
        skin_balance = 0;
    }
    if (skin_balance > 1) {
        skin_balance = 1;
    }
}

static int
skin_test_coord (int s, int x, int y) {
    sprite_t *spr = &sprites[s];

    return (x >= spr->x && x < spr->x + spr->w
            && y >= spr->y && y < spr->y + spr->h);
}

static void
skin_buttons_update (int x, int y) {
    skin_button = -1;
    for (int i = SPR_CBUTTON_PREV; i <= SPR_CBUTTON_OPEN; i+= 2) {
        if (skin_test_coord (i, x, y)) {
            skin_button = i;
            break;
        }
    }
}

static void
main_motion_notify (GtkWidget *widget, GdkEventMotion *event, gpointer user_data) {
    mx = event->x;
    my = event->y;

    if (mlock == SPR_VOLUME_1) {
        skin_volume_update (event->x, event->y);
    }
    else if (mlock == SPR_BALANCE_1) {
        skin_balance_update (event->x, event->y);
    }
    else if (m1on) {
        skin_buttons_update (event->x, event->y);
    }

    gtk_widget_queue_draw (widget);
}

static gboolean
main_button_press (GtkWidget       *widget,
        GdkEventButton  *event,
        gpointer         user_data) {
    if (TEST_LEFT_CLICK (event)) {
        m1on = 1;
        mlock = -1;

        if (skin_test_coord (SPR_VOLUME_1, event->x, event->y)) {
            mlock = SPR_VOLUME_1;
            skin_volume_update (event->x, event->y);
        }
        else if (skin_test_coord (SPR_BALANCE_1, event->x, event->y)) {
            mlock = SPR_BALANCE_1;
            skin_balance_update (event->x, event->y);
        }
        else {
            skin_buttons_update (event->x, event->y);
        }
        gtk_widget_queue_draw (widget);
        return TRUE;
    }
    return FALSE;
}

static gboolean
main_button_release (GtkWidget       *widget,
        GdkEventButton  *event,
        gpointer         user_data) {
    if (TEST_LEFT_CLICK (event)) {
        m1on = 0;
        mlock = -1;

        if (skin_button != -1) {
            switch (skin_button) {
            case SPR_CBUTTON_PREV:
                deadbeef->sendmessage (DB_EV_PREV, 0, 0, 0);
                break;
            case SPR_CBUTTON_PLAY:
                on_playbtn_clicked (NULL, NULL);
                break;
            case SPR_CBUTTON_PAUSE:
                deadbeef->sendmessage (DB_EV_TOGGLE_PAUSE, 0, 0, 0);
                break;
            case SPR_CBUTTON_STOP:
                deadbeef->sendmessage (DB_EV_STOP, 0, 0, 0);
                break;
            case SPR_CBUTTON_NEXT:
                deadbeef->sendmessage (DB_EV_NEXT, 0, 0, 0);
                break;
            case SPR_CBUTTON_OPEN:
                on_open_activate (NULL, NULL);
                break;
            }
        }

        skin_button = -1;
        gtk_widget_queue_draw (widget);
        return TRUE;
    }
    return FALSE;
}

int
gtkui_thread (void *ctx) {
#ifdef __linux__
    prctl (PR_SET_NAME, "deadbeef-gtkui", 0, 0, 0, 0);
#endif

    int argc = 2;
    const char **argv = alloca (sizeof (char *) * argc);
    argv[0] = "deadbeef";
    argv[1] = "--sync";
    //argv[1] = "--g-fatal-warnings";
    if (!deadbeef->conf_get_int ("gtkui.sync", 0)) {
        argc = 1;
    }

    gtk_disable_setlocale ();
    add_pixmap_directory (deadbeef->get_pixmap_dir ());

    // let's start some gtk
//    g_thread_init (NULL);
#ifndef __FreeBSD__
    // this call makes gtk_main hang on freebsd for unknown reason
    // however, if we don't have this call, deadbeef will crash randomly on
    // gentoo linux
    gdk_threads_init ();
#endif
    gtk_init (&argc, (char ***)&argv);

    load_assets ();

    mainwin = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_events (mainwin, GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK | GDK_BUTTON_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    gtk_window_set_title (GTK_WINDOW (mainwin), "DeaDBeeF");
    //gtk_window_set_resizable (GTK_WINDOW (mainwin), FALSE);
    gtk_window_set_default_size (GTK_WINDOW (mainwin), 275, 116);
    gtk_window_move (GTK_WINDOW (mainwin), 100, 200);
    gtk_window_set_decorated (GTK_WINDOW (mainwin), FALSE);

    g_signal_connect_after ((gpointer) mainwin, "key_press_event",
            G_CALLBACK (on_mainwin_key_press_event),
            NULL);
    g_signal_connect ((gpointer) mainwin, "delete_event",
            G_CALLBACK (on_mainwin_delete_event),
            NULL);
    g_signal_connect ((gpointer) mainwin, "motion_notify_event",
            G_CALLBACK (main_motion_notify),
            NULL);
    g_signal_connect ((gpointer) mainwin, "button_press_event",
            G_CALLBACK (main_button_press),
            NULL);
    g_signal_connect ((gpointer) mainwin, "button_release_event",
            G_CALLBACK (main_button_release),
            NULL);

    GtkWidget *da = gtk_drawing_area_new ();
    gtk_widget_show (da);
    gtk_container_add (GTK_CONTAINER (mainwin), da);

    g_signal_connect ((gpointer) da, "expose_event",
            G_CALLBACK (main_expose_event),
            NULL);

    // initialize default hotkey mapping
    if (!deadbeef->conf_get_int ("hotkeys_created", 0)) {
        // check if any hotkeys were created manually (e.g. beta versions of 0.6)
        if (!deadbeef->conf_find ("hotkey.key", NULL)) {
            gtkui_set_default_hotkeys ();
            gtkui_import_0_5_global_hotkeys ();
            DB_plugin_t *hkplug = deadbeef->plug_get_for_id ("hotkeys");
            if (hkplug) {
                ((DB_hotkeys_plugin_t *)hkplug)->reset ();
            }
        }
        deadbeef->conf_set_int ("hotkeys_created", 1);
        deadbeef->conf_save ();
    }
#if GTK_CHECK_VERSION(3,0,0)
    gtk_widget_set_events (GTK_WIDGET (mainwin), gtk_widget_get_events (GTK_WIDGET (mainwin)) | GDK_SCROLL_MASK);
#endif

//    gtkpl_init ();

    GtkIconTheme *theme = gtk_icon_theme_get_default();
    if (gtk_icon_theme_has_icon(theme, "deadbeef")) {
        gtk_window_set_icon_name (GTK_WINDOW (mainwin), "deadbeef");
    }
    else {
        // try loading icon from $prefix/deadbeef.png (for static build)
        char iconpath[1024];
        snprintf (iconpath, sizeof (iconpath), "%s/deadbeef.png", deadbeef->get_prefix ());
        gtk_window_set_icon_from_file (GTK_WINDOW (mainwin), iconpath, NULL);
    }

    wingeom_restore (mainwin, "guitest.mainwin", 40, 40, 275, 116, 0);

    gtkui_on_configchanged (NULL);
    gtkui_init_theme_colors ();

    searchwin = create_searchwin ();
    gtk_window_set_transient_for (GTK_WINDOW (searchwin), GTK_WINDOW (mainwin));

    DdbListview *search_playlist = DDB_LISTVIEW (lookup_widget (searchwin, "searchlist"));
    search_playlist_init (GTK_WIDGET (search_playlist));

    progress_init ();
    cover_art_init ();

#ifdef __APPLE__
#if 0
    GtkWidget *menubar = lookup_widget (mainwin, "menubar");
    gtk_widget_hide (menubar);
    GtkosxApplication *theApp = g_object_new(GTKOSX_TYPE_APPLICATION, NULL);
    gtkosx_application_set_menu_bar(theApp, GTK_MENU_SHELL(menubar));
#endif
#endif

    gtk_widget_show (mainwin);

    // playlist window
    plwin = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_events (plwin, GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK | GDK_BUTTON_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    gtk_window_set_title (GTK_WINDOW (plwin), "DeaDBeeF Playlist");
    //gtk_window_set_resizable (GTK_WINDOW (plwin), FALSE);
    gtk_window_set_default_size (GTK_WINDOW (plwin), 275, 232);
    gtk_window_move (GTK_WINDOW (plwin), 275+100, 200);
    gtk_window_set_decorated (GTK_WINDOW (plwin), FALSE);
    da = gtk_drawing_area_new ();
    gtk_widget_show (da);
    gtk_container_add (GTK_CONTAINER (plwin), da);

    g_signal_connect ((gpointer) da, "expose_event", G_CALLBACK (pl_expose_event), NULL);

    gtk_widget_show (plwin);

    char fmt[500];
    char str[600];
    deadbeef->conf_get_str ("gtkui.titlebar_stopped", "DeaDBeeF-%V", fmt, sizeof (fmt));
    deadbeef->pl_format_title (NULL, -1, str, sizeof (str), -1, fmt);
    gtk_window_set_title (GTK_WINDOW (mainwin), str);

    fileadded_listener_id = deadbeef->listen_file_added (gtkui_add_file_info_cb, NULL);
    fileadd_beginend_listener_id = deadbeef->listen_file_add_beginend (gtkui_add_file_begin_cb, gtkui_add_file_end_cb, NULL);

    supereq_plugin = deadbeef->plug_get_for_id ("supereq");

    gtkui_connect_cb (NULL);

    gtkui_accept_messages = 1;
    deadbeef->sendmessage (DB_EV_PLAYLISTCHANGED, 0, 0, 0);

#ifdef __APPLE__
    gtkui_is_retina = is_retina (mainwin);
#endif
    gtk_main ();
    deadbeef->unlisten_file_added (fileadded_listener_id);
    deadbeef->unlisten_file_add_beginend (fileadd_beginend_listener_id);

    //w_free ();

    if (refresh_timeout) {
        g_source_remove (refresh_timeout);
        refresh_timeout = 0;
    }
    cover_art_free ();
    eq_window_destroy ();
    trkproperties_destroy ();
    progress_destroy ();
    gtkui_hide_status_icon ();
//    draw_free ();
    if (theme_treeview) {
        gtk_widget_destroy (theme_treeview);
        theme_treeview = NULL;
    }
    if (mainwin) {
        gtk_widget_destroy (mainwin);
        mainwin = NULL;
    }
    if (searchwin) {
        gtk_widget_destroy (searchwin);
        searchwin = NULL;
    }
    return 0;
}

gboolean
gtkui_set_progress_text_idle (gpointer data) {
    char *text = (char *)data;
    if (text) {
        progress_settext (text);
        free (text);
    }
    return FALSE;
}

void
gtkui_playlist_set_curr (int playlist) {
    deadbeef->plt_set_curr_idx (playlist);
    deadbeef->conf_set_int ("playlist.current", playlist);
}

void
on_gtkui_info_window_delete (GtkWidget *widget, GtkTextDirection previous_direction, GtkWidget **pwindow) {
    *pwindow = NULL;
    gtk_widget_hide (widget);
    gtk_widget_destroy (widget);
}

void
gtkui_show_info_window (const char *fname, const char *title, GtkWidget **pwindow) {
    if (*pwindow) {
        return;
    }
    GtkWidget *widget = *pwindow = create_helpwindow ();
    g_object_set_data (G_OBJECT (widget), "pointer", pwindow);
    g_signal_connect (widget, "delete_event", G_CALLBACK (on_gtkui_info_window_delete), pwindow);
    gtk_window_set_title (GTK_WINDOW (widget), title);
    gtk_window_set_transient_for (GTK_WINDOW (widget), GTK_WINDOW (mainwin));
    GtkWidget *txt = lookup_widget (widget, "helptext");
    GtkTextBuffer *buffer = gtk_text_buffer_new (NULL);

    FILE *fp = fopen (fname, "rb");
    if (fp) {
        fseek (fp, 0, SEEK_END);
        size_t s = ftell (fp);
        rewind (fp);
        char buf[s+1];
        if (fread (buf, 1, s, fp) != s) {
            fprintf (stderr, "error reading help file contents\n");
            const char *error = _("Failed while reading help file");
            gtk_text_buffer_set_text (buffer, error, strlen (error));
        }
        else {
            buf[s] = 0;
            gtk_text_buffer_set_text (buffer, buf, s);
        }
        fclose (fp);
    }
    else {
        const char *error = _("Failed to load help file");
        gtk_text_buffer_set_text (buffer, error, strlen (error));
    }
    gtk_text_view_set_buffer (GTK_TEXT_VIEW (txt), buffer);
    g_object_unref (buffer);
    gtk_widget_show (widget);
}

gboolean
gtkui_quit_cb (void *ctx) {
    if (deadbeef->have_background_jobs ()) {
        GtkWidget *dlg = gtk_message_dialog_new (GTK_WINDOW (mainwin), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO, _("The player is currently running background tasks. If you quit now, the tasks will be cancelled or interrupted. This may result in data loss."));
        gtk_window_set_transient_for (GTK_WINDOW (dlg), GTK_WINDOW (mainwin));
        gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dlg), _("Do you still want to quit?"));
        gtk_window_set_title (GTK_WINDOW (dlg), _("Warning"));

        int response = gtk_dialog_run (GTK_DIALOG (dlg));
        gtk_widget_destroy (dlg);
        if (response != GTK_RESPONSE_YES) {
            return FALSE;
        }
    }
    progress_abort ();
    deadbeef->sendmessage (DB_EV_TERMINATE, 0, 0, 0);
    return FALSE;
}

void
gtkui_quit (void) {
    gdk_threads_add_idle (gtkui_quit_cb, NULL);
}

static int
gtkui_start (void) {
    fprintf (stderr, "gtkui plugin compiled for gtk version: %d.%d.%d\n", GTK_MAJOR_VERSION, GTK_MINOR_VERSION, GTK_MICRO_VERSION);
    gtkui_thread (NULL);

    return 0;
}

static int
gtkui_connect (void) {
    return 0;
}

static int
gtkui_disconnect (void) {
    supereq_plugin = NULL;
    coverart_plugin = NULL;

    return 0;
}


static gboolean
quit_gtk_cb (gpointer nothing) {
    extern int trkproperties_modified;
    trkproperties_modified = 0;
    trkproperties_destroy ();
    search_destroy ();
    gtk_main_quit ();
    return FALSE;
}

static int
gtkui_stop (void) {
    if (coverart_plugin) {
        coverart_plugin->plugin.plugin.stop ();
        coverart_plugin = NULL;
    }
    trace ("quitting gtk\n");
    g_idle_add (quit_gtk_cb, NULL);
    trace ("gtkui_stop completed\n");
    return 0;
}

GtkWidget *
gtkui_get_mainwin (void) {
    return mainwin;
}

static DB_plugin_action_t action_deselect_all = {
    .title = "Edit/Deselect All",
    .name = "deselect_all",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_deselect_all_handler,
    .next = NULL
};

static DB_plugin_action_t action_select_all = {
    .title = "Edit/Select All",
    .name = "select_all",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_select_all_handler,
    .next = &action_deselect_all
};

static DB_plugin_action_t action_quit = {
    .title = "Quit",
    .name = "quit",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_quit_handler,
    .next = &action_select_all
};

static DB_plugin_action_t action_delete_from_disk = {
    .title = "Remove From Disk",
    .name = "delete_from_disk",
    .flags = DB_ACTION_MULTIPLE_TRACKS,
    .callback2 = action_delete_from_disk_handler,
    .next = &action_quit
};

static DB_plugin_action_t action_add_location = {
    .title = "File/Add Location",
    .name = "add_location",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_add_location_handler,
    .next = &action_delete_from_disk
};

static DB_plugin_action_t action_add_folders = {
    .title = "File/Add Folder(s)",
    .name = "add_folders",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_add_folders_handler,
    .next = &action_add_location
};

static DB_plugin_action_t action_add_files = {
    .title = "File/Add File(s)",
    .name = "add_files",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_add_files_handler,
    .next = &action_add_folders
};

static DB_plugin_action_t action_open_files = {
    .title = "File/Open File(s)",
    .name = "open_files",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_open_files_handler,
    .next = &action_add_files
};


static DB_plugin_action_t action_track_properties = {
    .title = "Track Properties",
    .name = "track_properties",
    .flags = DB_ACTION_MULTIPLE_TRACKS,
    .callback2 = action_show_track_properties_handler,
    .next = &action_open_files
};

static DB_plugin_action_t action_show_help = {
    .title = "Help/Show Help Page",
    .name = "help",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_show_help_handler,
    .next = &action_track_properties
};

static DB_plugin_action_t action_playback_loop_cycle = {
    .title = "Playback/Cycle Playback Looping Mode",
    .name = "loop_cycle",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_playback_loop_cycle_handler,
    .next = &action_show_help
};

static DB_plugin_action_t action_playback_loop_off = {
    .title = "Playback/Playback Looping - Don't loop",
    .name = "loop_off",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_playback_loop_off_handler,
    .next = &action_playback_loop_cycle
};

static DB_plugin_action_t action_playback_loop_single = {
    .title = "Playback/Playback Looping - Single track",
    .name = "loop_track",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_playback_loop_single_handler,
    .next = &action_playback_loop_off
};

static DB_plugin_action_t action_playback_loop_all = {
    .title = "Playback/Playback Looping - All",
    .name = "loop_all",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_playback_loop_all_handler,
    .next = &action_playback_loop_single
};

static DB_plugin_action_t action_playback_order_cycle = {
    .title = "Playback/Cycle Playback Order",
    .name = "order_cycle",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_playback_order_cycle_handler,
    .next = &action_playback_loop_all
};

static DB_plugin_action_t action_playback_order_random = {
    .title = "Playback/Playback Order - Random",
    .name = "order_random",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_playback_order_random_handler,
    .next = &action_playback_order_cycle
};

static DB_plugin_action_t action_playback_order_shuffle_albums = {
    .title = "Playback/Playback Order - Shuffle albums",
    .name = "order_shuffle_albums",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_playback_order_shuffle_albums_handler,
    .next = &action_playback_order_random
};

static DB_plugin_action_t action_playback_order_shuffle = {
    .title = "Playback/Playback Order - Shuffle tracks",
    .name = "order_shuffle",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_playback_order_shuffle_handler,
    .next = &action_playback_order_shuffle_albums
};

static DB_plugin_action_t action_playback_order_linear = {
    .title = "Playback/Playback Order - Linear",
    .name = "order_linear",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_playback_order_linear_handler,
    .next = &action_playback_order_shuffle
};


static DB_plugin_action_t action_cursor_follows_playback = {
    .title = "Playback/Toggle Cursor Follows Playback",
    .name = "toggle_cursor_follows_playback",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_cursor_follows_playback_handler,
    .next = &action_playback_order_linear
};


static DB_plugin_action_t action_scroll_follows_playback = {
    .title = "Playback/Toggle Scroll Follows Playback",
    .name = "toggle_scroll_follows_playback",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_scroll_follows_playback_handler,
    .next = &action_cursor_follows_playback
};

static DB_plugin_action_t action_toggle_menu = {
    .title = "View/Show\\/Hide menu",
    .name = "toggle_menu",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_toggle_menu_handler,
    .next = &action_scroll_follows_playback
};

static DB_plugin_action_t action_toggle_statusbar = {
    .title = "View/Show\\/Hide statusbar",
    .name = "toggle_statusbar",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_toggle_statusbar_handler,
    .next = &action_toggle_menu
};

static DB_plugin_action_t action_toggle_designmode = {
    .title = "Edit/Toggle Design Mode",
    .name = "toggle_design_mode",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_toggle_designmode_handler,
    .next = &action_toggle_statusbar
};

static DB_plugin_action_t action_preferences = {
    .title = "Edit/Preferences",
    .name = "preferences",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_preferences_handler,
    .next = &action_toggle_designmode
};

static DB_plugin_action_t action_sort_custom = {
    .title = "Edit/Sort Custom",
    .name = "sort_custom",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_sort_custom_handler,
    .next = &action_preferences
};

static DB_plugin_action_t action_crop_selected = {
    .title = "Edit/Crop Selected",
    .name = "crop_selected",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_crop_selected_handler,
    .next = &action_sort_custom
};

static DB_plugin_action_t action_remove_from_playlist = {
    .title = "Edit/Remove Track(s) From Current Playlist",
    .name = "remove_from_playlist",
    .flags = DB_ACTION_MULTIPLE_TRACKS,
    .callback2 = action_remove_from_playlist_handler,
    .next = &action_crop_selected
};

static DB_plugin_action_t action_save_playlist = {
    .title = "File/Save Playlist",
    .name = "save_playlist",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_save_playlist_handler,
    .next = &action_remove_from_playlist
};

static DB_plugin_action_t action_load_playlist = {
    .title = "File/Load Playlist",
    .name = "load_playlist",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_load_playlist_handler,
    .next = &action_save_playlist
};

static DB_plugin_action_t action_remove_current_playlist = {
    .title = "File/Remove Current Playlist",
    .name = "remove_current_playlist",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_remove_current_playlist_handler,
    .next = &action_load_playlist
};


static DB_plugin_action_t action_new_playlist = {
    .title = "File/New Playlist",
    .name = "new_playlist",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_new_playlist_handler,
    .next = &action_remove_current_playlist
};

static DB_plugin_action_t action_toggle_eq = {
    .title = "View/Show\\/Hide Equalizer",
    .name = "toggle_eq",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_toggle_eq_handler,
    .next = &action_new_playlist
};

static DB_plugin_action_t action_hide_eq = {
    .title = "View/Hide Equalizer",
    .name = "hide_eq",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_hide_eq_handler,
    .next = &action_toggle_eq
};

static DB_plugin_action_t action_show_eq = {
    .title = "View/Show Equalizer",
    .name = "show_eq",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_show_eq_handler,
    .next = &action_hide_eq
};

static DB_plugin_action_t action_toggle_mainwin = {
    .title = "View/Show\\/Hide Player Window",
    .name = "toggle_player_window",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_toggle_mainwin_handler,
    .next = &action_show_eq
};

static DB_plugin_action_t action_hide_mainwin = {
    .title = "View/Hide Player Window",
    .name = "hide_player_window",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_hide_mainwin_handler,
    .next = &action_toggle_mainwin
};

static DB_plugin_action_t action_show_mainwin = {
    .title = "View/Show Player Window",
    .name = "show_player_window",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_show_mainwin_handler,
    .next = &action_hide_mainwin
};

static DB_plugin_action_t action_find = {
    .title = "Edit/Find",
    .name = "find",
    .flags = DB_ACTION_COMMON,
    .callback2 = action_find_handler,
    .next = &action_show_mainwin
};

static DB_plugin_action_t *
gtkui_get_actions (DB_playItem_t *it)
{
    return &action_find;
}

DB_plugin_t *
ddb_gui_TEST_load (DB_functions_t *api) {
    deadbeef = api;
    return DB_PLUGIN (&plugin);
}

static const char settings_dlg[] =
    "property \"Ask confirmation to delete files from disk\" checkbox gtkui.delete_files_ask 1;\n"
    "property \"Status icon volume control sensitivity\" entry gtkui.tray_volume_sensitivity 1;\n"
    "property \"Custom status icon\" entry gtkui.custom_tray_icon \"" TRAY_ICON "\" ;\n"
    "property \"Run gtk_init with --sync (debug mode)\" checkbox gtkui.sync 0;\n"
    "property \"Add separators between plugin context menu items\" checkbox gtkui.action_separators 0;\n"
    "property \"Use unicode chars instead of images for track state\" checkbox gtkui.unicode_playstate 0;\n"
    "property \"Disable seekbar overlay text\" checkbox gtkui.disable_seekbar_overlay 0;\n"
;

// define plugin interface
static ddb_gtkui_t plugin = {
    .gui.plugin.api_vmajor = 1,
    .gui.plugin.api_vminor = 5,
    .gui.plugin.version_major = DDB_GTKUI_API_VERSION_MAJOR,
    .gui.plugin.version_minor = DDB_GTKUI_API_VERSION_MINOR,
    .gui.plugin.type = DB_PLUGIN_GUI,
    .gui.plugin.id = "GUI_TEST",
    .gui.plugin.name = "TEST user interface",
    .gui.plugin.descr = "TEST user interface using GTK+ 2.x",
    .gui.plugin.copyright = 
        "Copyright (C) 2009-2013 Alexey Yakovenko <waker@users.sourceforge.net>\n"
        "\n"
        "This program is free software; you can redistribute it and/or\n"
        "modify it under the terms of the GNU General Public License\n"
        "as published by the Free Software Foundation; either version 2\n"
        "of the License, or (at your option) any later version.\n"
        "\n"
        "This program is distributed in the hope that it will be useful,\n"
        "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
        "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
        "GNU General Public License for more details.\n"
        "\n"
        "You should have received a copy of the GNU General Public License\n"
        "along with this program; if not, write to the Free Software\n"
        "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.\n"
    ,
    .gui.plugin.website = "http://deadbeef.sf.net",
    .gui.plugin.start = gtkui_start,
    .gui.plugin.stop = gtkui_stop,
    .gui.plugin.connect = gtkui_connect,
    .gui.plugin.disconnect = gtkui_disconnect,
    .gui.plugin.configdialog = settings_dlg,
    .gui.plugin.message = gtkui_message,
    .gui.run_dialog = gtkui_run_dialog_root,
    .gui.plugin.get_actions = gtkui_get_actions,
    .get_mainwin = gtkui_get_mainwin,
    .w_reg_widget = w_reg_widget,
    .w_unreg_widget = w_unreg_widget,
    .w_override_signals = w_override_signals,
    .w_is_registered = w_is_registered,
    .w_get_rootwidget = w_get_rootwidget,
    .w_set_design_mode = w_set_design_mode,
    .w_get_design_mode = w_get_design_mode,
    .w_create = w_create,
    .w_destroy = w_destroy,
    .w_append = w_append,
    .w_replace = w_replace,
    .w_remove = w_remove,
    .create_pltmenu = gtkui_create_pltmenu,
    .get_cover_art_pixbuf = get_cover_art_callb,
    .cover_get_default_pixbuf = cover_get_default_pixbuf,
};