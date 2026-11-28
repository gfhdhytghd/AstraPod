/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The glyph each page menu row draws, keyed by the lang id of its label, so
 * a setting row and an action row are looked up alike and a row keeps its
 * glyph in every language and every menu that lists it. The rows of Home,
 * System and the Settings page itself, and the library's rows, take theirs
 * from the label table in list_render.c instead.
 *
 * Codepoints from U+0128 up are Material Icons (Apache-2.0) drawn at 24 px
 * into 24x24-icons.fnt; the comment on each names the Material icon.
 ****************************************************************************/

#include "config.h"
#include "lang.h"
#include "row_glyphs.h"

static const struct
{
    unsigned short id;
    unsigned short glyph;
} glyphs[] = {
    /* Settings */
    { LANG_DB_SEARCH, 0x00AE },
#ifdef HAVE_RECORDING
    { LANG_RECORDING_SETTINGS, 0x005A },
#endif
    { LANG_THEME_SETTINGS_MENU, 0x0133 }, /* palette */
    /* Settings > Sound Settings */
    { LANG_VOLUME, 0x004F },
    { LANG_VOLUME_LIMIT, 0x0195 }, /* hearing */
    { LANG_BASS, 0x0172 }, /* speaker */
    { LANG_BASS_CUTOFF, 0x0107 },
    { LANG_TREBLE, 0x0050 }, /* graphic_eq */
    { LANG_TREBLE_CUTOFF, 0x0106 },
    { LANG_BALANCE, 0x0196 }, /* balance */
    { LANG_CHANNEL_CONFIGURATION, 0x0197 }, /* speaker_group */
    { LANG_STEREO_WIDTH, 0x0115 },
    { LANG_CROSSFEED, 0x0198 }, /* compare_arrows */
    { LANG_EQUALIZER, 0x0052 },
    { LANG_DITHERING, 0x019E }, /* grain */
    { LANG_SURROUND, 0x019F }, /* surround_sound */
    { LANG_PBE, 0x01A2 }, /* vibration */
    { LANG_AFR, 0x01A3 }, /* spa */
    { LANG_COMPRESSOR, 0x01A4 }, /* compress */
    /* Settings > Sound Settings > Crossfeed */
    { LANG_CROSSFEED_DIRECT_GAIN, 0x0199 }, /* straight */
    { LANG_CROSSFEED_CROSS_GAIN, 0x019A }, /* swap_horiz */
    { LANG_CROSSFEED_HF_ATTENUATION, 0x019B }, /* trending_down */
    { LANG_CROSSFEED_HF_CUTOFF, 0x0106 },
    /* Settings > Sound Settings > Equalizer */
    { LANG_EQUALIZER_ENABLED, 0x0052 },
    { LANG_EQUALIZER_EDIT, 0x019C }, /* edit */
    { LANG_EQUALIZER_PRESETS, 0x019D }, /* bookmarks */
    { LANG_EQUALIZER_PRECUT, 0x004E },
    /* Settings > Sound Settings > Haas Surround */
    { LANG_SURROUND_FX1, 0x0106 },
    { LANG_SURROUND_FX2, 0x0107 },
    { LANG_SURROUND_METHOD2, 0x01A0 }, /* call_split */
    { LANG_SURROUND_MIX, 0x01A1 }, /* opacity */
    /* Settings > Sound Settings > Compressor */
    { LANG_COMPRESSOR_THRESHOLD, 0x01A5 }, /* border_horizontal */
    { LANG_COMPRESSOR_GAIN, 0x01A6 }, /* arrow_circle_up */
    { LANG_COMPRESSOR_RATIO, 0x01A7 }, /* percent */
    { LANG_COMPRESSOR_KNEE, 0x01A8 }, /* turn_slight_right */
    { LANG_COMPRESSOR_ATTACK, 0x0193 }, /* timer */
    { LANG_COMPRESSOR_RELEASE, 0x01A9 }, /* hourglass_bottom */
    /* Settings > Playback Settings */
    { LANG_SHUFFLE, 0x0048 },
    { LANG_REPEAT, 0x004A },
    { LANG_PLAY_SELECTED, 0x0169 }, /* touch_app */
    { LANG_WIND_MENU, 0x0047 },
    { LANG_MP3BUFFER_MARGIN, 0x00F1 },
    { LANG_FADE_ON_STOP, 0x019B }, /* trending_down */
    { LANG_SINGLE_MODE, 0x0119 },
    { LANG_CROSSFADE, 0x01AE }, /* join_inner */
    { LANG_REPLAYGAIN, 0x01B1 }, /* align_vertical_center */
    { LANG_BEEP, 0x01B3 }, /* notifications_active */
    { LANG_NEXT_FOLDER, 0x01B4 }, /* drive_file_move */
    { LANG_CUESHEET_ENABLE, 0x01B5 }, /* list_alt */
    { LANG_CHAPTER_MARKS, 0x0071 },
    { LANG_HEADPHONE_UNPLUG, 0x01B6 }, /* headset_off */
    { LANG_SKIP_LENGTH, 0x0045 },
    { LANG_SPOKEN_SKIP_LENGTH, 0x01B7 }, /* forward_30 */
    { LANG_SPOKEN_WORD_SPEED, 0x0053 },
    { LANG_PREVENT_SKIPPING, 0x00C0 },
    { LANG_FREQUENCY, 0x01B8 }, /* waves */
    { LANG_LOGGING, 0x00B5 },
    { LANG_AUTORESUME, 0x0054 },
    { LANG_WPS, 0x0041 },
    { LANG_CONVERT_LIBRARY_48, 0x01E8 }, /* transform */
    /* Settings > Playback Settings > Fast-Forward/Rewind */
    { LANG_FFRW_STEP_SHORT, 0x0148 }, /* straighten */
    { LANG_FFRW_ACCEL_SHORT, 0x0053 },
    { LANG_REWIND_ACROSS_TRACKS, 0x0044 },
    { LANG_RESUME_REWIND, 0x01AA }, /* replay */
    { LANG_SPOKEN_RESUME_REWIND, 0x01AB }, /* replay_30 */
    { LANG_PAUSE_REWIND, 0x01AC }, /* pause_circle */
    { LANG_SPOKEN_PAUSE_REWIND, 0x01AD }, /* pause_presentation */
    /* Settings > Playback Settings > Crossfade */
    { LANG_CROSSFADE_ENABLE, 0x01AE }, /* join_inner */
    { LANG_CROSSFADE_FADE_IN_DELAY, 0x016B }, /* hourglass_top */
    { LANG_CROSSFADE_FADE_IN_DURATION, 0x01AF }, /* trending_up */
    { LANG_CROSSFADE_FADE_OUT_DELAY, 0x01A9 }, /* hourglass_bottom */
    { LANG_CROSSFADE_FADE_OUT_DURATION, 0x019B }, /* trending_down */
    { LANG_CROSSFADE_FADE_OUT_MODE, 0x01B0 }, /* call_merge */
    /* Settings > Playback Settings > Replaygain */
    { LANG_REPLAYGAIN_MODE, 0x0064 },
    { LANG_REPLAYGAIN_NOCLIP, 0x01B2 }, /* gpp_good */
    { LANG_REPLAYGAIN_PREAMP, 0x004F },
    /* Settings > Playback Settings > Pause on Unplug */
    { LANG_HEADPHONE_UNPLUG_DISABLE_AUTORESUME, 0x00C2 },
    /* Settings > Playback Settings > Playlists */
    { LANG_SORT_PLAYLISTS, 0x0160 }, /* sort */
    { LANG_PLAYLISTVIEWER_SETTINGS, 0x006E },
    { LANG_RECURSE_DIRECTORY, 0x01BA }, /* account_tree */
    /* Settings > Playback Settings > Playlists > Playlist Viewer Settings */
    { LANG_SHOW_ICONS, 0x01B9 }, /* interests */
    { LANG_SHOW_INDICES, 0x012F }, /* format_list_numbered */
    { LANG_TRACK_DISPLAY, 0x006C },
    /* Settings > Playback Settings > Playlists > Queue */
    { LANG_WARN_ERASEDYNPLAYLIST_MENU, 0x00BD },
    { LANG_PARTY_MODE, 0x01BB }, /* celebration */
    { LANG_KEEP_CURRENT_TRACK_ON_REPLACE, 0x01BC }, /* push_pin */
    { LANG_SHOW_SHUFFLED_ADDING_OPTIONS, 0x0048 },
    /* Settings > Playback Settings > Automatic Resume */
    { LANG_AUTORESUME_AUTOMATIC, 0x0045 },
    /* Settings > Playback Settings > Now Playing Screen */
    { LANG_DEFAULT_BROWSER, 0x00A2 },
    /* Settings > Playback Settings > Now Playing Screen > Appearance */
    { LANG_WPS_ART_SOURCE, 0x00A7 },
    { LANG_ALBUM_ART, 0x0066 },
    { LANG_TRIM_TITLES, 0x01BD }, /* content_cut */
#ifdef HAVE_RECORDING
    /* Settings > Recording Settings */
    { LANG_REC_FORMAT, 0x0183 }, /* audio_file */
    { LANG_ENCODER_SETTINGS, 0x01A4 }, /* compress */
    { LANG_BITRATE, 0x01D7 }, /* linear_scale */
    { LANG_REC_FREQUENCY, 0x01B8 }, /* waves */
    { LANG_RECORDING_SOURCE, 0x01CD }, /* input */
    { LANG_REC_CHANNELS, 0x019F }, /* surround_sound */
    { LANG_RECORDING_MONO_MODE, 0x0172 }, /* speaker */
    { LANG_RECORD_TIMESPLIT, 0x01BD }, /* content_cut */
    { LANG_RECORD_PRERECORD_TIME, 0x01CE }, /* history */
    { LANG_REC_FOLDER, 0x00A1 },
    { LANG_CLEAR_REC_DIR, 0x01CF }, /* folder_delete */
    { LANG_PM_CLIPCOUNTER, 0x01D0 }, /* exposure_plus_1 */
    { LANG_CLIP_LIGHT, 0x0157 }, /* lightbulb */
    { LANG_RECORD_TRIGGER, 0x0142 }, /* bolt */
    { LANG_GAIN, 0x0052 },
    { LANG_PM_MENU, 0x01D5 }, /* equalizer */
    { LANG_REC_PRESET_LOAD, 0x01D8 }, /* file_open */
    { LANG_REC_PRESET_SAVE, 0x00CF },
    /* Settings > Recording Settings > File Split Options */
    { LANG_SPLIT_MEASURE, 0x0148 }, /* straighten */
    { LANG_SPLIT_TYPE, 0x01A0 }, /* call_split */
    { LANG_SPLIT_TIME, 0x0193 }, /* timer */
    { LANG_SPLIT_SIZE, 0x016C }, /* sd_storage */
    /* Settings > Recording Settings > Trigger */
    { LANG_RECORD_TRIGGER_TYPE, 0x01D1 }, /* rule */
    { LANG_RECORD_START_THRESHOLD, 0x01AF }, /* trending_up */
    { LANG_REC_START_DURATION, 0x016B }, /* hourglass_top */
    { LANG_RECORD_STOP_THRESHOLD, 0x019B }, /* trending_down */
    { LANG_REC_STOP_DURATION, 0x01A9 }, /* hourglass_bottom */
    { LANG_RECORD_STOP_GAP, 0x01D2 }, /* space_bar */
    /* Settings > Recording Settings > Gain */
    { LANG_REC_INPUT_GAIN, 0x005A },
    { LANG_GAIN_LEFT, 0x01D3 }, /* west */
    { LANG_GAIN_RIGHT, 0x01D4 }, /* east */
#endif
    /* Settings > Recording Settings > Peak Meter */
    { LANG_PM_RELEASE, 0x01D6 }, /* south */
    { LANG_PM_PEAK_HOLD, 0x0193 }, /* timer */
    { LANG_PM_CLIP_HOLD, 0x017D }, /* av_timer */
    { LANG_PM_SCALE, 0x01D7 }, /* linear_scale */
    { LANG_PM_MIN, 0x0107 },
    { LANG_PM_MAX, 0x0106 },
    /* Settings > Library > Files */
    { LANG_SORT_MENU, 0x00AB },
    { LANG_FILTER, 0x00AA },
    { LANG_SHOW_FILENAME_EXT, 0x012B }, /* file_present */
    { LANG_FOLLOW, 0x012C }, /* my_location */
    { LANG_SHOW_PATH, 0x00EE },
    { LANG_DIRCACHE_ENABLE, 0x00F1 },
    { LANG_RESET_START_DIR, 0x00DC },
    /* Settings > Library > Files > Sort */
    { LANG_SORT_CASE_SHORT, 0x0128 }, /* text_fields */
    { LANG_SORT_DIR_SHORT, 0x00A1 },
    { LANG_SORT_FILE_SHORT, 0x0129 }, /* insert_drive_file */
    { LANG_SORT_INTERPRET_NUMBERS_SHORT, 0x012A }, /* pin */
    /* Settings > Library > Music > Sort */
    { LANG_SORT_ALBUMS_BY_SHORT, 0x0065 },
    { LANG_SORT_ARTISTS_BY_SHORT, 0x0067 },
    { LANG_SORT_AUDIOBOOKS_BY_SHORT, 0x00EA },
    { LANG_SORT_SHOWS_BY_SHORT, 0x010F },
    { LANG_SORT_IGNORE_ARTICLES_SHORT, 0x012D }, /* format_strikethrough */
    /* Settings > Library > Music > Sort > Albums By */
    { LANG_ALL_ALBUMS, 0x0065 },
    { LANG_OTHER_LISTS, 0x00A9 },
    /* Settings > Library > Music */
    { LANG_SHOW_YEAR_IN_ALBUM_LISTS, 0x00B6 },
    { LANG_EXTRA_ROWS, 0x012E }, /* library_add */
    { LANG_SEGREGATE_AUDIOBOOKS, 0x00EA },
    { LANG_SEGREGATE_PODCASTS, 0x010E },
    { LANG_QUIZ_QUESTIONS, 0x00EF },
    /* Settings > Library > Music > Quiz Questions */
    { LANG_QUIZ_KIND_TITLE, 0x006C },
    { LANG_QUIZ_KIND_ARTIST, 0x0067 },
    { LANG_QUIZ_KIND_ALBUM, 0x0065 },
    { LANG_QUIZ_KIND_YEAR, 0x00B6 },
    /* Settings > Library > Music > Appearance */
    { LANG_DB_ALBUM_ART, 0x0065 },
    { LANG_DB_ARTIST_ART, 0x0069 },
    { LANG_DB_BOOK_ART, 0x00EA },
    { LANG_DB_AUTHOR_ART, 0x0110 },
    { LANG_DB_EBOOK_ART, 0x010C },
    { LANG_DB_EBOOK_AUTHOR_ART, 0x00EC },
    { LANG_DB_PODCAST_ART, 0x010E },
    /* Settings > Library > Search */
    { LANG_DB_SEARCH_MAX_ROWS, 0x012F }, /* format_list_numbered */
    { LANG_DB_SEARCH_MIN_LETTERS, 0x0130 }, /* abc */
    { LANG_DB_SEARCH_ORDER, 0x0063 },
    { LANG_AUDIOBOOK_SEARCH_ORDER, 0x00EA },
    { LANG_PODCAST_SEARCH_ORDER, 0x010E },
    { LANG_BOOK_SEARCH_ORDER, 0x010C },
    /* Settings > Library */
    { LANG_CAROUSEL_SETTINGS, 0x0066 },
    { LANG_TAGCACHE, 0x013C }, /* storage */
    { LANG_ART_CACHE_MENU, 0x0141 }, /* perm_media */
    { LANG_PLAYLIST_ENGINE, 0x0146 }, /* auto_awesome */
    { LANG_VIEWERS, 0x014A }, /* visibility */
    { LANG_LIBRARY_MAINTENANCE, 0x00CE },
    /* Settings > Library > Carousel > Sort */
    { LANG_SORT_SAME_AS_MUSIC_SHORT, 0x00D8 },
    /* Settings > Library > Carousel */
    { LANG_SCROLL_SPEED, 0x0053 },
    { LANG_RANDOM_SPIN_LENGTH, 0x0056 },
    /* Settings > Library > Carousel > Appearance */
    { LANG_SHOW_ALBUM_TITLE, 0x0131 }, /* title */
    { LANG_SHOW_YEAR_IN_ALBUM_TITLE, 0x00B6 },
    { LANG_CAROUSEL_BACKGROUND, 0x0132 }, /* wallpaper */
    { LANG_CAROUSEL_CUSTOM_COLOR, 0x0133 }, /* palette */
    { LANG_STATUS_BAR, 0x0134 }, /* web_asset */
    { LANG_CAROUSEL_VIEW_MODE, 0x0135 }, /* view_carousel */
    { LANG_CAROUSEL_PILE_FADE, 0x0136 }, /* gradient */
    { LANG_CAROUSEL_PILE_OFFSET, 0x0137 }, /* layers */
    { LANG_ARTWORK_FILTER, 0x0138 }, /* photo_filter */
    /* Settings > Library > Carousel > Appearance > Artwork Filter */
    { LANG_ARTWORK_FILTER_1, 0x0139 }, /* filter_1 */
    { LANG_ARTWORK_FILTER_2, 0x013A }, /* filter_2 */
    { LANG_ARTWORK_FILTER_3, 0x013B }, /* filter_3 */
    /* Settings > Library > Database */
    { LANG_SCAN_ON_STARTUP, 0x00C2 },
    { LANG_TAGCACHE_SCAN_ON_EJECT, 0x013D }, /* eject */
    { LANG_SELECT_DATABASE_DIRS, 0x013E }, /* rule_folder */
    { LANG_YEAR_FROM_FOLDER, 0x00B6 },
    { LANG_SKIP_IPOD_GAMES, 0x0126 },
    { LANG_TAGCACHE_EXPORT, 0x013F }, /* upload */
    { LANG_TAGCACHE_IMPORT, 0x0140 }, /* download */
    /* Settings > Library > Art Cache */
    { LANG_ART_CACHE_ALBUM_SOURCE, 0x0065 },
    { LANG_ART_CACHE_FAST_BUILD, 0x0142 }, /* bolt */
    { LANG_ART_HEALTH_ALBUMS, 0x0143 }, /* hide_image */
    { LANG_ART_HEALTH_ARTISTS, 0x0144 }, /* person_off */
    { LANG_ART_CHECK, 0x0145 }, /* image_search */
    /* Settings > Library > Playlist Engine */
    { LANG_ACTION_ENABLED, 0x0146 }, /* auto_awesome */
    { LANG_ANALYSIS_DEPTH, 0x0147 }, /* query_stats */
    { LANG_MIX_LENGTH, 0x0148 }, /* straighten */
    { LANG_TRACK_PLAYLIST, 0x0061 },
    { LANG_MOOD_PLAYLIST, 0x00ED },
    { LANG_MIX_STARTS_WITH_SELECTED, 0x0106 },
    { LANG_CONTINUE_PLAYING, 0x0149 }, /* all_inclusive */
    { LANG_SOUND_LIBRARY, 0x0050 },
    /* Settings > Library > Viewers */
    { LANG_TEXT_VIEWER, 0x00A6 },
    { LANG_LYRICS_MENU, 0x0072 },
    /* Settings > Library > Viewers > Text Viewer */
    { LANG_BROWSE_CHAPTERS, 0x014B }, /* toc */
    { LANG_BOOKMARK_MENU_CREATE, 0x014C }, /* bookmark_add */
    { LANG_BOOKMARK_MENU, 0x0071 },
    { LANG_TEXT_VIEWER_LARGER, 0x014D }, /* text_increase */
    { LANG_TEXT_VIEWER_SMALLER, 0x014E }, /* text_decrease */
    { LANG_CUSTOM_FONT, 0x00AC },
    { LANG_TEXT_VIEWER_LINE_SPACING, 0x014F }, /* format_line_spacing */
    { LANG_LYRICS_ALIGN, 0x0150 }, /* format_align_left */
    { LANG_TEXT_VIEWER_INDENT, 0x0151 }, /* format_indent_increase */
    { LANG_TEXT_VIEWER_MARGIN, 0x0152 }, /* margin */
    { LANG_TEXT_VIEWER_FOOTER, 0x0153 }, /* call_to_action */
    { LANG_TEXT_VIEWER_PICTURES, 0x00A7 },
    { LANG_TEXT_VIEWER_COLOUR, 0x0154 }, /* contrast */
    { LANG_TEXT_VIEWER_TEXT_COLOUR, 0x0155 }, /* format_color_text */
    { LANG_TEXT_VIEWER_PAGE_COLOUR, 0x0156 }, /* format_color_fill */
    { LANG_TEXT_VIEWER_BRIGHTNESS, 0x00C5 },
    { LANG_TEXT_VIEWER_BACKLIGHT, 0x0157 }, /* lightbulb */
    { LANG_TEXT_VIEWER_ORIENTATION, 0x0158 }, /* screen_rotation */
    { LANG_TEXT_VIEWER_WHEEL_SCRUB, 0x0024 },
    { LANG_TEXT_VIEWER_READ_ALOUD, 0x0159 }, /* record_voice_over */
    /* Settings > Library > Viewers > Lyrics Viewer */
    { LANG_LYRICS_PREV_OPACITY, 0x015A }, /* keyboard_double_arrow_up */
    { LANG_LYRICS_NEXT_OPACITY, 0x015B }, /* keyboard_double_arrow_down */
    { LANG_LYRICS_LEAD, 0x015C }, /* more_time */
    { LANG_LYRICS_HIGHLIGHT, 0x015D }, /* highlight */
    { LANG_LYRICS_BACKLIGHT, 0x0157 }, /* lightbulb */
    /* Settings > Library > Viewers > Playback Report */
    { LANG_SPUN_ARTWORK, 0x00A7 },
    { LANG_SPUN_TOP_COUNT, 0x015E }, /* leaderboard */
    { LANG_SPUN_BADGE_ORDER, 0x015F }, /* emoji_events */
    { LANG_SPUN_RANK_BY, 0x0160 }, /* sort */
    /* Settings > Library > Maintenance */
    { LANG_UPDATE_DB, 0x013C }, /* storage */
    { LANG_UPDATE_INDEX, 0x0161 }, /* find_replace */
    { LANG_UPDATE_CACHE, 0x0141 }, /* perm_media */
    { LANG_RESCAN_FILES, 0x0162 }, /* document_scanner */
    { LANG_UPDATE_SOUND, 0x0050 },
    { LANG_REBUILD_REPORT, 0x007A },
    { LANG_REBUILD_DB, 0x0163 }, /* restart_alt */
    { LANG_REBUILD_CACHE, 0x0164 }, /* autorenew */
    { LANG_REBUILD_SOUND, 0x0165 }, /* cached */
    /* Settings > Theme Settings */
    { LANG_NATIVE_THEME, 0x0166 }, /* style */
    { LANG_STATUS_COVER_TINT, 0x0167 }, /* colorize */
    /* Settings > Appearance */
    { LANG_ELEMENTS, 0x0178 }, /* dashboard */
    { LANG_SCROLL_MENU, 0x01CC }, /* text_rotation_none */
    { LANG_MENUS, 0x00DB },
    { LANG_UI_ANIMATIONS, 0x01E3 }, /* animation */
    /* Settings > Appearance > Interface Elements */
    { LANG_ART_ROWS, 0x01D9 }, /* collections */
    { LANG_SCROLL_BAR, 0x01DA }, /* view_sidebar */
    { LANG_BATTERY_DISPLAY, 0x0037 },
    { LANG_VOLUME_DISPLAY, 0x004F },
    { LANG_ACTIVITY_INDICATORS, 0x0026 },
    { LANG_LIST_SEPARATOR, 0x01DD }, /* table_rows */
    /* Settings > Appearance > Interface Elements > Scroll Bar */
    { LANG_SCROLLBAR_WIDTH, 0x01DB }, /* width_normal */
    /* Settings > Appearance > Interface Elements > Activity Indicators */
    { LANG_SHOW_DISK_ACTIVITY, 0x013C }, /* storage */
    { LANG_SHOW_BACKGROUND_TASKS, 0x00F3 },
    { LANG_SHOW_WORKING, 0x01DC }, /* hourglass_empty */
    /* Settings > Appearance > Scrolling */
    { LANG_SCROLLING_ENABLED, 0x00F6 },
    { LANG_SCROLL_BOX_SPEED, 0x0053 },
    { LANG_SCROLL_DELAY, 0x0193 }, /* timer */
    { LANG_SCROLL_STEP, 0x01DE }, /* stairs */
    { LANG_BIDIR_SCROLL, 0x019A }, /* swap_horiz */
    { LANG_SCREEN_SCROLL_VIEW, 0x01DF }, /* double_arrow */
    { LANG_DISABLE_MAINMENU_SCROLLING, 0x00DC },
    { LANG_SCREEN_SCROLL_STEP, 0x0105 },
    { LANG_SCROLL_PAGINATED, 0x015B }, /* keyboard_double_arrow_down */
    { LANG_LIST_WRAPAROUND, 0x01E0 }, /* sync */
    { LANG_LIST_ORDER, 0x01E1 }, /* swap_vert */
    /* Settings > Appearance > Menus */
    { LANG_MAIN_MENU_SETTINGS, 0x00DC },
    { LANG_MUSIC_MENU_SETTINGS, 0x0063 },
    { LANG_CONTEXT_MENUS, 0x01E2 }, /* more_vert */
    /* Settings > Appearance > Menus > Context Menus */
    { LANG_NOW_PLAYING_MENU, 0x0041 },
    { LANG_TRACK_MENU, 0x006C },
    { LANG_ALBUM_MENU, 0x0065 },
    { LANG_ARTIST_MENU, 0x0067 },
    { LANG_FOLDER_MENU, 0x00A1 },
    { LANG_AUDIOBOOK_MENU, 0x00EA },
    { LANG_PODCAST_MENU, 0x010E },
    { LANG_BOOK_MENU, 0x00EB },
    /* Settings > Battery & Power */
    { LANG_BRIGHTNESS, 0x00C5 },
    { LANG_BACKLIGHT, 0x0157 }, /* lightbulb */
    { LANG_POWEROFF_IDLE, 0x00C2 },
    { LANG_SLEEP_TIMER, 0x00C6 },
    { LANG_STORAGE, 0x016C }, /* sd_storage */
    { LANG_BATTERY_CAPACITY, 0x0037 },
    { LANG_CAR_ADAPTER_MODE, 0x016E }, /* directions_car */
    { LANG_USB_CHARGING, 0x00BA },
    /* Settings > Battery & Power > Backlight */
    { LANG_BACKLIGHT_ON_BATTERY, 0x0037 },
    { LANG_BACKLIGHT_PLUGGED_IN, 0x00C1 },
    { LANG_BACKLIGHT_LOCKED, 0x00C0 },
    { LANG_BACKLIGHT_TRACK_CHANGE, 0x0045 },
    { LANG_LCD_SLEEP_AFTER_BACKLIGHT_OFF, 0x0168 }, /* mobile_off */
    { LANG_FIRST_BUTTON_PRESS, 0x0169 }, /* touch_app */
    { LANG_BACKLIGHT_EXEMPTIONS, 0x016A }, /* block */
    { LANG_BACKLIGHT_FADE_IN_SHORT, 0x01AF }, /* trending_up */
    { LANG_BACKLIGHT_FADE_OUT_SHORT, 0x019B }, /* trending_down */
    /* Settings > Battery & Power > Sleep Timer */
    { LANG_SLEEP_TIMER_DURATION, 0x016B }, /* hourglass_top */
    { LANG_SLEEP_TIMER_ON_POWER_UP, 0x00C2 },
    { LANG_KEYPRESS_RESTARTS_SLEEP_TIMER, 0x0169 }, /* touch_app */
    /* Settings > Battery & Power > Storage */
    { LANG_SPINDOWN, 0x016D }, /* motion_photos_pause */
    { LANG_STORAGE_MODE_SHORT, 0x0053 },
    /* Settings > Battery & Power > Car Adapter Mode */
    { LANG_CAR_ADAPTER_MODE_DELAY, 0x016B }, /* hourglass_top */
    /* Settings > System */
    { LANG_USB, 0x016F }, /* usb */
    { LANG_ACCESSORIES, 0x00C3 },
    { LANG_USE_SHORTCUTS_INSTEAD_OF_QS, 0x0178 }, /* dashboard */
    { LANG_KEYCLICK, 0x0179 }, /* ads_click */
    { LANG_LIMITS_MENU, 0x0106 },
    { LANG_VOLUME_ADJUST_MODE, 0x004F },
    { LANG_VOLUME_ADJUST_NORM_STEPS, 0x017A }, /* signal_cellular_alt */
    { LANG_TIME_MENU, 0x017B }, /* date_range */
    { LANG_STARTUP_SHUTDOWN, 0x00C2 },
    { LANG_LANGUAGE_AND_TEXT, 0x017F }, /* translate */
    { LANG_MANAGE_MENU, 0x0185 }, /* settings_applications */
    { LANG_DEBUG_LOG, 0x00A6 },
    { LANG_SHOW_DEBUG_MENU, 0x00F5 },
    /* Settings > System > USB */
    { LANG_USB_MODE, 0x016F }, /* usb */
    { LANG_USB_HID, 0x0170 }, /* mouse */
    { LANG_USB_KEYPAD_MODE, 0x0171 }, /* keyboard */
#ifdef HAVE_USB_HOST_AUDIO
    { LANG_USB_DAC_OUTPUT, 0x01E6 }, /* cable */
#endif
#ifdef USB_ENABLE_AUDIO
    { LANG_USB_DAC, 0x01E7 }, /* computer */
#endif
    /* Settings > System > Accessories */
    { LANG_LINEOUT, 0x0172 }, /* speaker */
    { LANG_TV_OUT, 0x0173 }, /* connected_tv */
    { LANG_ACCESSORY_PROTOCOL, 0x0175 }, /* sync_alt */
    { LANG_ACCESSORY_SUPPLY, 0x00C1 },
    { LANG_ACCESSORY_BROWSING, 0x0176 }, /* view_list */
    { LANG_CAR_ARTWORK, 0x00A7 },
    { LANG_SERIAL_BITRATE, 0x0053 },
#ifdef HAVE_MIKEY_REMOTE
    { LANG_REMOTE_TRACK_SKIP, 0x0177 }, /* settings_remote */
#endif
    { LANG_IAP2_ACCESSORIES, 0x01E4 }, /* smartphone */
    { LANG_IAP_IDENTITY, 0x01E5 }, /* badge */
    /* Settings > System > Accessories > TV Out */
    { LANG_TV_OUT_SIZE, 0x0174 }, /* aspect_ratio */
    { LANG_TV_STANDARD, 0x01E9 }, /* tv */
    /* Settings > System > Keyclick */
    { LANG_KEYCLICK_SOFTWARE, 0x0059 },
    { LANG_KEYCLICK_HARDWARE, 0x0172 }, /* speaker */
    { LANG_KEYCLICK_REPEATS, 0x004A },
    /* Settings > System > Limits */
    { LANG_MAX_FILES_IN_DIR, 0x00A2 },
    { LANG_MAX_FILES_IN_PLAYLIST, 0x00F0 },
    { LANG_GLYPHS, 0x00AC },
    /* Settings > System > Time & Date Settings */
    { LANG_SET_TIME, 0x017C }, /* edit_calendar */
    { LANG_TIMEFORMAT, 0x017D }, /* av_timer */
    { LANG_SYNC_COMPUTER_TIME, 0x01E0 }, /* sync */
    /* Settings > System > Startup/Shutdown */
    { LANG_SHOW_SHUTDOWN_MESSAGE, 0x00D1 },
    { LANG_START_SCREEN, 0x017E }, /* start */
    { LANG_SHUT_DOWN, 0x00C2 },
    { LANG_REBOOT, 0x0163 }, /* restart_alt */
    /* Settings > System > Language & Text */
    { LANG_LANGUAGE, 0x00CC },
    { LANG_VOICE, 0x0159 }, /* record_voice_over */
    { LANG_DEFAULT_CODEPAGE, 0x0128 }, /* text_fields */
    /* Settings > System > Language & Text > Voice */
    { LANG_SPEAK_ALOUD, 0x0180 }, /* campaign */
    { LANG_SPEECH_RATE, 0x0053 },
    { LANG_VOICE_DIR, 0x00A1 },
    { LANG_VOICE_DIR_TALK, 0x0181 }, /* snippet_folder */
    { LANG_VOICE_FILE, 0x0182 }, /* description */
    { LANG_VOICE_FILE_TALK, 0x0183 }, /* audio_file */
    { LANG_VOICE_FILETYPE, 0x0184 }, /* sell */
    { LANG_SAY_LIST_TITLES, 0x0131 }, /* title */
    { LANG_ANNOUNCE_TRACK_CHANGE, 0x0045 },
    { LANG_TALK_BATTERY_LEVEL, 0x0037 },
    { LANG_TALK_MIXER_LEVEL, 0x004F },
    /* Settings > System > Manage Settings */
    { LANG_CUSTOM_CFG, 0x0186 }, /* folder_open */
    { LANG_RESET, 0x0187 }, /* settings_backup_restore */
    { LANG_SAVE_SETTINGS, 0x00CF },
    { LANG_SAVE_SOUND, 0x0188 }, /* save_as */
    /* Settings > System > Debug Logs */
    { LANG_DEBUG_LOG_TAGCACHE, 0x013C }, /* storage */
    { LANG_DEBUG_LOG_ARTCACHE, 0x00A7 },
    { LANG_DEBUG_LOG_BATTERY, 0x0037 },
    { LANG_DEBUG_LOG_PLAYBACK, 0x00BD },
    { LANG_DEBUG_LOG_TASKS, 0x00F3 },
    { LANG_DEBUG_LOG_EXPIRY, 0x0189 }, /* timer_off */
    { LANG_DEBUG_LOG_ALL_OFF, 0x00F7 },
    { LANG_DEBUG_LOG_USB, 0x016F }, /* usb */
#ifdef HAVE_RECORDING
    /* System */
    { LANG_RECORDING, 0x005A },
#endif
    /* Sound Library */
    { LANG_SOUND_LIB_OVERVIEW, 0x0178 }, /* dashboard */
    { LANG_SOUND_LIB_MOODS, 0x00ED },
    { LANG_SOUND_LIB_PACE, 0x0053 },
    { LANG_SOUND_LIB_KEYS, 0x006B },
    { LANG_SOUND_LIB_DECADES, 0x01BE }, /* bar_chart */
    /* Moods */
    { LANG_MOOD_CALM, 0x01BF }, /* self_improvement */
    { LANG_MOOD_ENERGETIC, 0x0142 }, /* bolt */
    { LANG_MOOD_DARK, 0x00C6 },
    { LANG_MOOD_BRIGHT, 0x00C5 },
    { LANG_MOOD_WARM, 0x01C0 }, /* local_fire_department */
    { LANG_MOOD_RAW, 0x01C1 }, /* texture */
    { LANG_MOOD_LUSH, 0x01C2 }, /* local_florist */
    { LANG_MOOD_PUNCHY, 0x01C3 }, /* sports_mma */
    { LANG_MOOD_SMOOTH, 0x01C4 }, /* water */
    { LANG_MOOD_SPARSE, 0x01C5 }, /* scatter_plot */
    { LANG_MOOD_DENSE, 0x0125 },
    { LANG_MOOD_HYPNOTIC, 0x01C6 }, /* cyclone */
    { LANG_MOOD_SLOW, 0x01C7 }, /* directions_walk */
    { LANG_MOOD_FAST, 0x01C8 }, /* directions_run */
    { LANG_MOOD_MELANCHOLY, 0x01C9 }, /* sentiment_dissatisfied */
    { LANG_MOOD_UPLIFTING, 0x01CA }, /* sentiment_very_satisfied */
    /* Journeys */
    { LANG_JOURNEY_CALM_ENERGETIC, 0x0142 }, /* bolt */
    { LANG_JOURNEY_DARK_BRIGHT, 0x00C5 },
    { LANG_JOURNEY_SPARSE_DENSE, 0x0125 },
    { LANG_JOURNEY_SLOW_FAST, 0x01C8 }, /* directions_run */
    /* sentiment_very_satisfied */
    { LANG_JOURNEY_MELANCHOLY_UPLIFTING, 0x01CA },
    /* Spike */
    { LANG_SPIKE_SCORES, 0x015F }, /* emoji_events */
    { LANG_SPIKE_CAPTION, 0x01CB }, /* subtitles */
    { LANG_SPIKE_CAPTION_SCROLL, 0x01CC }, /* text_rotation_none */
    { LANG_SPIKE_TEMPO, 0x0053 },
    { LANG_SPIKE_INFO, 0x00BB },
    /* Playlist options */
    { LANG_VIEW_DYNAMIC_PLAYLIST, 0x0055 },
    { LANG_SAVE_DYNAMIC_PLAYLIST, 0x00CF },
    { LANG_CREATE_PLAYLIST, 0x006F },
    { LANG_RESET_PLAYLISTCAT_DIR, 0x0163 }, /* restart_alt */
    /* Original menus > Music */
    { LANG_COVER_FLOW_TITLE, 0x0066 },
    { LANG_FRUIT_COMPILATIONS, 0x018A }, /* groups */
    { LANG_FRUIT_SONGS, 0x0061 },
    { LANG_FRUIT_GENRES, 0x006A },
    { LANG_FRUIT_COMPOSERS, 0x006B },
    /* Original menus */
    { LANG_FRUIT_VIDEOS, 0x0116 },
    { LANG_FRUIT_PHOTOS, 0x0078 },
    { LANG_FRUIT_EXTRAS, 0x018F }, /* widgets */
    { LANG_FRUIT_SLEEP, 0x00C6 },
    /* Original menus > Videos */
    { LANG_FRUIT_MOVIES, 0x0109 },
    { LANG_FRUIT_RENTALS, 0x018B }, /* local_activity */
    { LANG_FRUIT_TV_SHOWS, 0x018C }, /* live_tv */
    { LANG_FRUIT_MUSIC_VIDEOS, 0x018D }, /* music_video */
    { LANG_FRUIT_VIDEO_PLAYLISTS, 0x018E }, /* playlist_play */
    /* Original menus > Photos */
    { LANG_FRUIT_ALL_PHOTOS, 0x00A7 },
    /* Original menus > Extras */
    { LANG_FRUIT_CLOCKS, 0x00B3 },
    { LANG_FRUIT_CALENDARS, 0x00B6 },
    { LANG_FRUIT_CONTACTS, 0x0190 }, /* contacts */
    { LANG_FRUIT_ALARMS, 0x0191 }, /* alarm */
    { LANG_PLUGIN_GAMES, 0x0126 },
    { LANG_FRUIT_NOTES, 0x0192 }, /* sticky_note_2 */
    { LANG_FRUIT_SCREEN_LOCK, 0x00C0 },
    { LANG_FRUIT_STOPWATCH, 0x0193 }, /* timer */
    { LANG_APP_LIST_OTHER, 0x0125 },
    { LANG_PLUGIN_DEMOS, 0x0146 }, /* auto_awesome */
    /* Original menus > Settings */
    { LANG_MAIN_MENU, 0x00DC },
    { LANG_FRUIT_MUSIC_MENU, 0x0063 },
    { LANG_FRUIT_VOLUME_LIMIT, 0x004F },
    { LANG_FRUIT_EQ, 0x0052 },
    { LANG_FRUIT_SOUND_CHECK, 0x0050 },
    { LANG_FRUIT_CLICKER, 0x0179 }, /* ads_click */
    { LANG_FRUIT_DATE_TIME, 0x017B }, /* date_range */
    { LANG_FRUIT_SORT_BY, 0x0160 }, /* sort */
    { LANG_FRUIT_LEGAL, 0x00F4 },
    { LANG_FRUIT_ADVANCED_SETTINGS, 0x00CD },
    /* Original menus > Settings > Date & Time */
    { LANG_FRUIT_DATE, 0x00B6 },
    { LANG_FRUIT_TIME, 0x00B3 },
    { LANG_FRUIT_TIME_ZONE, 0x0194 }, /* public */
    { LANG_FRUIT_24_HOUR_CLOCK, 0x017D }, /* av_timer */
    { LANG_FRUIT_TIME_IN_TITLE, 0x0134 }, /* web_asset */
#ifdef HAVE_USB_HOST_AUDIO
    /* Settings > System > USB > USB DAC Output */
    { LANG_USB_DAC_AUTO, 0x0041 },
#endif
};

int row_glyph(int lang_id)
{
    for (unsigned i = 0; i < sizeof(glyphs) / sizeof(*glyphs); ++i)
        if (glyphs[i].id == lang_id)
            return glyphs[i].glyph;
    return 0;
}
