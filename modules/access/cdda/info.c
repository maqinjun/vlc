/*****************************************************************************
 * info.c : CD digital audio input information routines
 *****************************************************************************
 * Copyright (C) 2004 VideoLAN
 * $Id: info.c 8845 2004-09-29 09:00:41Z rocky $
 *
 * Authors: Rocky Bernstein <rocky@panix.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include "callback.h"      /* FIXME - reorganize callback.h, cdda.h better */
#include "cdda.h"          /* private structures. Also #includes vlc things */
#include <vlc_playlist.h>  /* Has to come *after* cdda.h */

#include <cdio/cdio.h>
#include <cdio/cdtext.h>
#include <cdio/logging.h>
#include <cdio/cd_types.h>
#include "info.h"

#ifdef HAVE_ERRNO_H
#   include <errno.h>
#endif

static char *CDDAFormatStr( const access_t *p_access, cdda_data_t *p_cdda,
			    const char format_str[], const char *psz_mrl, 
			    track_t i_track);

#ifdef HAVE_LIBCDDB

#define free_and_dup(var, val) \
  if (var) free(var);          \
  if (val) var=strdup(val);


/* Saves CDDB information about CD-DA via libcddb. */
static void 
GetCDDBInfo( access_t *p_access, cdda_data_t *p_cdda )
{
    int i, i_matches;
    cddb_conn_t  *conn = cddb_new();
    const CdIo *p_cdio = p_cdda->p_cdio;

    dbg_print( (INPUT_DBG_CALL), "" );

#ifdef FIXME_NOW
    cddb_log_set_handler (uninit_log_handler);
#endif

    if (!conn)
    {
        msg_Warn( p_access, "Unable to initialize libcddb" );
        goto cddb_destroy;
    }

    cddb_set_email_address( conn,
                            config_GetPsz( p_access,
                                           MODULE_STRING "-cddb-email") );
    cddb_set_server_name( conn,
                          config_GetPsz( p_access,
                                         MODULE_STRING "-cddb-server") );
    cddb_set_server_port(conn,
                         config_GetInt( p_access,
                                        MODULE_STRING "-cddb-port") );

  /* Set the location of the local CDDB cache directory.
     The default location of this directory is */

    if (!config_GetInt( p_access, MODULE_STRING "-cddb-enable-cache" ))
        cddb_cache_disable(conn);

    cddb_cache_set_dir(conn,
                     config_GetPsz( p_access,
                                    MODULE_STRING "-cddb-cachedir") );

    cddb_set_timeout(conn,
                   config_GetInt( p_access, MODULE_STRING "-cddb-timeout") );


    if (config_GetInt( p_access, MODULE_STRING "-cddb-httpd" ) )
    {
        cddb_http_enable(conn);
    }
    else
    {
        cddb_http_disable(conn);
    }

    p_cdda->cddb.disc = cddb_disc_new();

    if (!p_cdda->cddb.disc)
    {
        msg_Err( p_access, "Unable to create CDDB disc structure." );
        goto cddb_end;
    }

    for(i = 0; i < p_cdda->i_tracks; i++)
    {
        track_t i_track =  p_cdda->i_first_track + i;
        cddb_track_t *t = cddb_track_new();
        t->frame_offset = cdio_get_track_lba(p_cdio, i_track);
        cddb_disc_add_track(p_cdda->cddb.disc, t);
    }

    p_cdda->cddb.disc->length =
        cdio_get_track_lba(p_cdio, CDIO_CDROM_LEADOUT_TRACK)
        / CDIO_CD_FRAMES_PER_SEC;

    if (!cddb_disc_calc_discid(p_cdda->cddb.disc))
    {
        msg_Err( p_access, "CDDB disc ID calculation failed" );
        goto cddb_destroy;
    }

    i_matches = cddb_query(conn, p_cdda->cddb.disc);

    if (i_matches > 0)
    {
        if (i_matches > 1)
             msg_Warn( p_access, "Found %d matches in CDDB. Using first one.",
                                 i_matches);
        cddb_read(conn, p_cdda->cddb.disc);

        if (p_cdda->i_debug & INPUT_DBG_CDDB)
            cddb_disc_print(p_cdda->cddb.disc);

    }
    else
    {
        msg_Warn( p_access, "CDDB error: %s", cddb_error_str(errno));
    }

cddb_destroy:
    cddb_destroy(conn);

cddb_end: ;
}
#endif /*HAVE_LIBCDDB*/

#define add_meta_val(VLC_META, VAL)					\
  if ( p_cdda->p_meta && VAL) {                                         \
    vlc_meta_Add( p_cdda->p_meta, VLC_META, VAL );                      \
    dbg_print( INPUT_DBG_META, "field %s: %s\n", VLC_META, VAL );       \
  }                                                                     \

#define add_cddb_meta(FIELD, VLC_META)			                \
  add_meta_val(VLC_META, p_cdda->cddb.disc->FIELD);

#define add_cddb_meta_fmt(FIELD, FORMAT_SPEC, VLC_META)                 \
  {                                                                     \
    char psz_buf[100];                                                  \
    snprintf( psz_buf, sizeof(psz_buf)-1, FORMAT_SPEC,                  \
              p_cdda->cddb.disc->FIELD );                               \
    psz_buf[sizeof(psz_buf)-1] = '\0';                                  \
    add_meta_val(VLC_META, psz_buf);					\
  }

/* Adds a string-valued entry to the stream and media information if
   the string is not null or the null string.
 */
#define add_info_str(CATEGORY, TITLE, FIELD)                      \
  if (FIELD && strlen(FIELD)) {                                   \
    input_Control( p_cdda->p_input, INPUT_ADD_INFO, CATEGORY,     \
                   _(TITLE), "%s", FIELD );                       \
  }

/* Adds a numeric-valued entry to the stream and media information
   if the number is not zero. */
#define add_info_val(CATEGORY, TITLE, FMT, FIELD)                 \
  if (FIELD) {                                                    \
    input_Control( p_cdda->p_input, INPUT_ADD_INFO, CATEGORY,     \
                   _(TITLE), FMT, FIELD );                        \
  }

/* Adds a CDDB string-valued entry to the stream and media information
   under category "Disc" if the string is not null or the null string.
 */
#define add_cddb_disc_info_str(TITLE, FIELD)                    \
  add_info_str("Disc", TITLE, p_cdda->cddb.disc->FIELD)

/* Adds a CDDB numeric-valued entry to the stream and media information
   under category "Disc" if the string is not null or the null string.
 */
#define add_cddb_disc_info_val(TITLE, FMT, FIELD)               \
  add_info_val("Disc", TITLE, FMT, p_cdda->cddb.disc->FIELD)

/* Adds a CD-Text string-valued entry to the stream and media information
   under category "Disc" if the string is not null or the null string.
 */
#define add_cdtext_info_str(CATEGORY, TITLE, INDEX, FIELD)              \
    add_info_str(CATEGORY, TITLE, p_cdda->p_cdtext[INDEX]->field[FIELD])

/* Adds a CD-Text string-valued entry to the stream and media information
   under category "Disc" if the string is not null or the null string.
 */
#define add_cdtext_disc_info_str(TITLE, FIELD) \
  add_cdtext_info_str("Disc", TITLE, 0, FIELD)


/*
  Saves Meta Information about the CD-DA.

  Meta information used in "stream and media info" or in playlist
  info. The intialization of CD-Text or CDDB is done here though.
  Therefore, this should be called before CDDAMetaInfo is called.

 */
void 
CDDAMetaInfoInit( access_t *p_access )
{
    cdda_data_t *p_cdda   = (cdda_data_t *) p_access->p_sys;
    
    if ( ! p_cdda ) return;

    p_cdda->psz_mcn = cdio_get_mcn(p_cdda->p_cdio);
    p_cdda->p_meta = vlc_meta_New();

#ifdef HAVE_LIBCDDB
    if ( p_cdda->b_cddb_enabled )
    {
        GetCDDBInfo(p_access, p_cdda);
    }

#endif /*HAVE_LIBCDDB*/
    
#define TITLE_MAX 30
    {
        track_t i_track;

	for( i_track = 0 ; i_track < p_cdda->i_tracks ; i_track++ )
	{
	    p_cdda->p_cdtext[i_track] =
	      cdio_get_cdtext(p_cdda->p_cdio, i_track);
	}
    }
}

/*
 In the Control routine, we handle Meta Information requests and
 basically copy what was saved in CDDAMetaInfoInit.

 If i_track is CDIO_INVALID_TRACK we are probably asking about the entire
 CD.
 */
void 
CDDAMetaInfo( access_t *p_access, track_t i_track, /*const*/ char *psz_mrl )
{
    cdda_data_t *p_cdda = (cdda_data_t *) p_access->p_sys;
    char *psz_meta_title = psz_mrl;
    char *psz_meta_artist = NULL;
    char *psz_name = NULL;
    char *config_varname = MODULE_STRING "-title-format";
    
    if ( ! p_cdda ) return;

#ifdef HAVE_LIBCDDB

    /* Set up for Meta and name for CDDB access. */
    if ( p_cdda->b_cddb_enabled &&  p_cdda->cddb.disc )
    {
        if (p_cdda->b_cddb_enabled)
	{
	    config_varname = MODULE_STRING "-cddb-title-format";
	}

        if( CDIO_INVALID_TRACK == i_track )
	{

	    psz_meta_title  = p_cdda->cddb.disc->title;
	    psz_meta_artist = p_cdda->cddb.disc->artist;
	    if ( p_cdda->cddb.disc->genre && strlen(p_cdda->cddb.disc->genre) )
	        add_cddb_meta(genre, VLC_META_GENRE);
	    if ( 0 != p_cdda->cddb.disc->year )
	        add_cddb_meta_fmt(year, "%d", VLC_META_DATE );
	}
	else
	{
	  cddb_track_t *t=cddb_disc_get_track(p_cdda->cddb.disc, i_track-1);
	  if (t != NULL )
	  {
	      if( t->title != NULL )
	      {
		  add_meta_val( VLC_META_TITLE, t->title );
	      }
	      if( t->artist != NULL )
	      {
		add_meta_val( VLC_META_ARTIST, t->artist );
	      }
	  }
	}
    }

#endif /*HAVE_LIBCDDB*/
    
#define TITLE_MAX 30
    {
        track_t i = p_cdda->i_tracks;
        const int i_first_track = p_cdda->i_first_track;
        char psz_buffer[MSTRTIME_MAX_SIZE];
	unsigned int i_track_frames = 
	  cdio_get_track_lba(p_cdda->p_cdio, CDIO_CDROM_LEADOUT_TRACK);
	  
        mtime_t i_duration = i_track_frames / CDIO_CD_FRAMES_PER_SEC;

        dbg_print( INPUT_DBG_META, "Duration %ld, tracks %d", 
		   (long int) i_duration, p_cdda->i_tracks );
        input_Control( p_cdda->p_input, INPUT_ADD_INFO,
                       _("Disc"), _("Duration"), "%s",
                       secstotimestr( psz_buffer, i_duration ) );

	if (p_cdda->psz_mcn) {
	    input_Control( p_cdda->p_input, INPUT_ADD_INFO,
			   _("Disc"), _("Media Catalog Number (MCN)"), "%s", 
			   p_cdda->psz_mcn );
	    
	    input_Control( p_cdda->p_input, INPUT_ADD_INFO,
			   _("Disc"), _("Tracks"), "%d", p_cdda->i_tracks );
	}
	

#ifdef HAVE_LIBCDDB
        if (p_cdda->b_cddb_enabled && p_cdda->cddb.disc)
        {
          add_cddb_disc_info_str("Artist (CDDB)", artist);
	  if ( CDDB_CAT_INVALID != p_cdda->cddb.disc->category )
	    add_info_str("Disc", "Category (CDDB)",
			 CDDB_CATEGORY[p_cdda->cddb.disc->category]);
          add_cddb_disc_info_val("Disc ID (CDDB)", "%x", discid);
          add_cddb_disc_info_str("Extended Data (CDDB)", ext_data);
	  add_cddb_disc_info_str("Genre (CDDB)",  genre);
          add_cddb_disc_info_str("Title (CDDB)",  title);
	  if ( 0 != p_cdda->cddb.disc->year ) 
	    add_cddb_disc_info_val("Year (CDDB)", "%d", year);

        }
#endif /*HAVE_LIBCDDB*/

        if (p_cdda->p_cdtext[0])
        {
	    char *psz_field;
	  
            add_cdtext_disc_info_str("Arranger (CD-Text)",    CDTEXT_ARRANGER);
            add_cdtext_disc_info_str("Composer (CD-Text)",    CDTEXT_COMPOSER);
            add_cdtext_disc_info_str("Disc ID (CD-Text)",     CDTEXT_DISCID);
            add_cdtext_disc_info_str("Genre (CD-Text)",       CDTEXT_GENRE);
            add_cdtext_disc_info_str("Message (CD-Text)",     CDTEXT_MESSAGE);
            add_cdtext_disc_info_str("Performer (CD-Text)",   CDTEXT_PERFORMER);
            add_cdtext_disc_info_str("Songwriter (CD-Text)",  CDTEXT_SONGWRITER);
            add_cdtext_disc_info_str("Title (CD-Text)",       CDTEXT_TITLE);

	    psz_field = p_cdda->p_cdtext[0]->field[CDTEXT_TITLE];
	    if (psz_field && strlen(psz_field)) {   
	      psz_meta_title = psz_field;
	    }
	    psz_field = p_cdda->p_cdtext[0]->field[CDTEXT_PERFORMER];
	    if (psz_field && strlen(psz_field)) {   
	      psz_meta_artist = psz_field;
	    }
	    
        }

	for( i = 0 ; i < p_cdda->i_tracks ; i++ )
	{
	  char psz_track[TITLE_MAX];
	  const track_t i_track = i_first_track + i;
	  unsigned int i_track_frames = 
	    cdio_get_track_lsn(p_cdda->p_cdio, i_track+1) - 
	    cdio_get_track_lsn(p_cdda->p_cdio, i_track);

	  mtime_t i_duration = i_track_frames / CDIO_CD_FRAMES_PER_SEC;
	  char *psz_mrl;
	  
	  snprintf(psz_track, TITLE_MAX, "%s %02d", _("Track"), i_track);

	  input_Control( p_cdda->p_input, INPUT_ADD_INFO, psz_track,
			 _("Duration"), "%s",
			 secstotimestr( psz_buffer, i_duration ) );
	  
	  asprintf(&psz_mrl, "%s%s@T%u",
		   CDDA_MRL_PREFIX, p_cdda->psz_source, i_track);

	  input_Control( p_cdda->p_input, INPUT_ADD_INFO, psz_track,
			 _("MRL"), "%s", psz_mrl );
	  free(psz_mrl);
	  
	  if (p_cdda->p_cdtext[i_track])
	    {
	      add_cdtext_info_str( psz_track, "Arranger (CD-Text)",
				   i_track, CDTEXT_ARRANGER);
	      add_cdtext_info_str( psz_track, "Composer (CD-Text)",
				   i_track, CDTEXT_COMPOSER);
	      add_cdtext_info_str( psz_track, "Disc ID (CD-Text)",
				   i_track, CDTEXT_DISCID);
	      add_cdtext_info_str( psz_track, "Genre (CD-Text)",
				   i_track, CDTEXT_GENRE);
	      add_cdtext_info_str( psz_track, "Message (CD-Text)",
				   i_track, CDTEXT_MESSAGE);
	      add_cdtext_info_str( psz_track, "Performer (CD-Text)",
				   i_track, CDTEXT_PERFORMER);
	      add_cdtext_info_str( psz_track, "Songwriter (CD-Text)",
				   i_track, CDTEXT_SONGWRITER);
	      add_cdtext_info_str( psz_track, "Title (CD-Text)",
				   i_track, CDTEXT_TITLE);
	    }
	  
#ifdef HAVE_LIBCDDB
	  if (p_cdda->b_cddb_enabled)
	    {
	      cddb_track_t *t=cddb_disc_get_track(p_cdda->cddb.disc, i);
	      if (t != NULL)
		{
		  add_info_str(psz_track, "Artist (CDDB)", t->artist);
		  add_info_str(psz_track, "Title (CDDB)",  t->title);
		  add_info_str(psz_track, "Extended Data (CDDB)",
			       t->ext_data);
		}
	    }
#endif /*HAVE_LIBCDDB*/
	}

	/* Above we should have set psz_meta_title and psz_meta_artist
	   to CDDB or CD-Text values or the default value depending on
	   availablity and user preferences. 

	   We also set above config_varname to the format used
	   
	   So now add the title and artist to VLC's meta, and the 
	   name as shown in the status bar and playlist entry.
	 */
	add_meta_val( VLC_META_TITLE, psz_meta_title );
	if (psz_meta_artist) 
	  add_meta_val( VLC_META_ARTIST, psz_meta_artist );

	if ( CDIO_INVALID_TRACK != i_track )
	{ 
	    psz_name = 
	      CDDAFormatStr( p_access, p_cdda,
			     config_GetPsz( p_access, config_varname ),
			     psz_mrl, i_track );
	    
	    input_Control( p_cdda->p_input, INPUT_SET_NAME, psz_name );
	}
    }
}

#define add_format_str_info(val)                         \
  {                                                      \
    const char *str = val;                               \
    unsigned int len;                                    \
    if (val != NULL) {                                   \
      len=strlen(str);                                   \
      if (len != 0) {                                    \
        strncat(tp, str, TEMP_STR_LEN-(tp-temp_str));    \
        tp += len;                                       \
      }                                                  \
      saw_control_prefix = false;                        \
    }                                                    \
  }

#define add_format_num_info(val, fmt)                    \
  {                                                      \
    char num_str[10];                                    \
    unsigned int len;                                    \
    sprintf(num_str, fmt, val);                          \
    len=strlen(num_str);                                 \
    if (len != 0) {                                      \
      strncat(tp, num_str, TEMP_STR_LEN-(tp-temp_str));  \
      tp += len;                                         \
    }                                                    \
    saw_control_prefix = false;                          \
  }

static inline bool
want_cddb_info(
cdda_data_t *p_cdda, char *psz_cdtext) 
{
  /* We either don't have CD-Text info, or we do but we prefer to get CDDB
     which means CDDB has been enabled and we were able to retrieve the info.*/
#ifdef HAVE_LIBCDDB
  return !psz_cdtext || 
    (!p_cdda->b_cdtext_prefer && p_cdda->b_cddb_enabled && p_cdda->cddb.disc);
#else
  return false;
#endif
}


/*!
   Take a format string and expand escape sequences, that is sequences that
   begin with %, with information from the current CD.
   The expanded string is returned. Here is a list of escape sequences:

   %a : The album artist **
   %A : The album information **
   %C : Category **
   %I : CDDB disk ID **
   %G : Genre **
   %M : The current MRL
   %m : The CD-DA Media Catalog Number (MCN)
   %n : The number of tracks on the CD
   %p : The artist/performer/composer in the track **
   %T : The track number **
   %s : Number of seconds in this track, or seconds in CD if invalid track
   %S : Number of seconds on the CD
   %t : The track name or MRL if no name 
   %Y : The year 19xx or 20xx **
   %% : a %
*/
static char *
CDDAFormatStr( const access_t *p_access, cdda_data_t *p_cdda,
               const char format_str[], const char *psz_mrl, track_t i_track)
{
#define TEMP_STR_SIZE 256
#define TEMP_STR_LEN (TEMP_STR_SIZE-1)
    static char temp_str[TEMP_STR_SIZE];
    size_t i;
    char * tp = temp_str;
    vlc_bool_t saw_control_prefix = false;
    size_t format_len = strlen(format_str);

    memset(temp_str, 0, TEMP_STR_SIZE);

    for (i=0; i<format_len; i++)
    {
        char *psz = NULL;

        if (!saw_control_prefix && format_str[i] != '%')
        {
            *tp++ = format_str[i];
            saw_control_prefix = false;
            continue;
        }

        switch(format_str[i])
        {
            case '%':
              if (saw_control_prefix)
              {
                  *tp++ = '%';
              }
              saw_control_prefix = !saw_control_prefix;
              break;
#ifdef HAVE_LIBCDDB
            case 'a':
	        if (p_cdda->p_cdtext[0] 
		    && p_cdda->p_cdtext[0]->field[CDTEXT_PERFORMER])
		  psz = p_cdda->p_cdtext[0]->field[CDTEXT_PERFORMER];
		if (want_cddb_info(p_cdda, psz))
		  psz = p_cdda->cddb.disc->artist;
                goto format_str;
            case 'A':
	        if (p_cdda->p_cdtext[0] 
		    && p_cdda->p_cdtext[0]->field[CDTEXT_TITLE])
		  psz = p_cdda->p_cdtext[0]->field[CDTEXT_TITLE];
		if (want_cddb_info(p_cdda, psz))
		  psz =  p_cdda->cddb.disc->title;
                goto format_str;
            case 'C':
                if (!p_cdda->b_cddb_enabled) goto not_special;
                if (p_cdda->cddb.disc)
                    add_format_str_info(
                                  CDDB_CATEGORY[p_cdda->cddb.disc->category]);
                break;
            case 'G':
	        if (p_cdda->p_cdtext[0] 
		    && p_cdda->p_cdtext[0]->field[CDTEXT_GENRE])
		  psz = p_cdda->p_cdtext[0]->field[CDTEXT_GENRE];
		if (want_cddb_info(p_cdda, psz))
		  psz = p_cdda->cddb.disc->genre;
		goto format_str;
            case 'I':
	        if (p_cdda->p_cdtext[0] 
		    && p_cdda->p_cdtext[0]->field[CDTEXT_DISCID])
		  psz = p_cdda->p_cdtext[0]->field[CDTEXT_DISCID];
		if (want_cddb_info(p_cdda, psz)) {
                     add_format_num_info(p_cdda->cddb.disc->discid, "%x");
		} else if (psz)
		     add_format_str_info(psz);
                break;
            case 'Y':
                if (!p_cdda->b_cddb_enabled) goto not_special;
                if (p_cdda->cddb.disc)
                    add_format_num_info(p_cdda->cddb.disc->year, "%5d");
                break;
            case 't':
	        if ( CDIO_INVALID_TRACK == i_track ) break;
                if (p_cdda && p_cdda->b_cddb_enabled && p_cdda->cddb.disc)
                {
                    cddb_track_t *t=cddb_disc_get_track(p_cdda->cddb.disc,
                                                        i_track-1);
                    if (t != NULL && t->title != NULL) {
                      add_format_str_info(t->title);
		    } else {
                      add_format_str_info(psz_mrl);
		    }
                } else {
		  if (p_cdda->p_cdtext[i_track] 
		      && p_cdda->p_cdtext[i_track]->field[CDTEXT_TITLE]) {
		    add_format_str_info(p_cdda->p_cdtext[i_track]->field[CDTEXT_TITLE]);
		  
		  } else 
		    add_format_str_info(psz_mrl);
		}
                break;
	    case 'p':
	        if ( CDIO_INVALID_TRACK == i_track ) break;
	        if (p_cdda->p_cdtext[i_track] 
		    && p_cdda->p_cdtext[i_track]->field[CDTEXT_PERFORMER])
		  psz = p_cdda->p_cdtext[i_track]->field[CDTEXT_PERFORMER];
		if (want_cddb_info(p_cdda, psz))
		  {
		    cddb_track_t *t=cddb_disc_get_track(p_cdda->cddb.disc,
							i_track-1);
		    if (t != NULL && t->artist != NULL)
		      psz = t->artist;
		  }
		goto format_str;
            case 'e':
	        if ( CDIO_INVALID_TRACK == i_track ) break;
	        if (p_cdda->p_cdtext[i_track] 
		    && p_cdda->p_cdtext[i_track]->field[CDTEXT_MESSAGE])
		  psz = p_cdda->p_cdtext[i_track]->field[CDTEXT_MESSAGE];
		if (want_cddb_info(p_cdda, psz))
                {
                    cddb_track_t *t=cddb_disc_get_track(p_cdda->cddb.disc,
                                                        i_track-1);
                    if (t != NULL && t->ext_data != NULL)
                        psz = t->ext_data;
                } 
		goto format_str;
                break;
#else
            case 'a':
	        if (p_cdda->p_cdtext[0] 
		    && p_cdda->p_cdtext[0]->field[CDTEXT_PERFORMER])
		    psz = p_cdda->p_cdtext[0]->field[CDTEXT_PERFORMER];
                goto format_str;
            case 'A':
	        if (p_cdda->p_cdtext[0] 
		    && p_cdda->p_cdtext[0]->field[CDTEXT_TITLE])
		    psz = p_cdda->p_cdtext[0]->field[CDTEXT_TITLE];
                goto format_str;
            case 'G':
	        if (p_cdda->p_cdtext[0] 
		    && p_cdda->p_cdtext[0]->field[CDTEXT_GENRE])
		  psz = p_cdda->p_cdtext[0]->field[CDTEXT_GENRE];
		goto format_str;
            case 'I':
	        if (p_cdda->p_cdtext[0] 
		    && p_cdda->p_cdtext[0]->field[CDTEXT_DISCID])
		  add_format_str_info(p_cdda->p_cdtext[0]->field[CDTEXT_DISCID]);
                break;
	    case 'p':
	        if ( CDIO_INVALID_TRACK == i_track ) break;
	        if (p_cdda->p_cdtext[i_track] 
		    && p_cdda->p_cdtext[i_track]->field[CDTEXT_PERFORMER])
		  psz = p_cdda->p_cdtext[i_track]->field[CDTEXT_PERFORMER];
		goto format_str;
            case 't':
	        if ( CDIO_INVALID_TRACK == i_track ) break;
	        if (p_cdda->p_cdtext[i_track] 
		    && p_cdda->p_cdtext[i_track]->field[CDTEXT_TITLE])
		    psz = p_cdda->p_cdtext[i_track]->field[CDTEXT_TITLE];
		else 
		    psz = psz_mrl;
                goto format_str;
            case 'e':
	        if ( CDIO_INVALID_TRACK == i_track ) break;
	        if (p_cdda->p_cdtext[i_track] 
		    && p_cdda->p_cdtext[i_track]->field[CDTEXT_MESSAGE])
		  psz = p_cdda->p_cdtext[i_track]->field[CDTEXT_MESSAGE];
		goto format_str;
                break;
#endif /*HAVE_LIBCDDB*/

            case 's':
	        if ( CDIO_INVALID_TRACK != i_track )
                {
                    char psz_buffer[MSTRTIME_MAX_SIZE];
		    unsigned int i_track_frames = 
		      cdio_get_track_lsn(p_cdda->p_cdio, i_track+1) - 
		      cdio_get_track_lsn(p_cdda->p_cdio, i_track);
                    mtime_t i_duration = 
		      i_track_frames / CDIO_CD_FRAMES_PER_SEC;
                    add_format_str_info( secstotimestr( psz_buffer, 
							i_duration ) );
		    break;
                }

		/* Fall through to disc duration if CDIO_INVALID_TRACK  */
            case 'S':
                {
                    char psz_buffer[MSTRTIME_MAX_SIZE];
		    unsigned int i_track_frames = 
		      cdio_get_track_lba(p_cdda->p_cdio, 
					 CDIO_CDROM_LEADOUT_TRACK);
                    mtime_t i_duration = 
		      i_track_frames / CDIO_CD_FRAMES_PER_SEC;
                    add_format_str_info( secstotimestr( psz_buffer, 
							i_duration ) );
		    break;
                }

            case 'M':
              add_format_str_info(psz_mrl);
              break;

            case 'm':
              add_format_str_info(p_cdda->psz_mcn);
              break;

            case 'n':
              add_format_num_info(p_cdda->i_tracks, "%d");
              break;

            case 'T':
              add_format_num_info(i_track, "%02d");
              break;
	    format_str:
	      if (psz)
		add_format_str_info(psz);
	      break;
#ifdef HAVE_LIBCDDB
            not_special:
#endif
            default:
                *tp++ = '%';
                *tp++ = format_str[i];
                saw_control_prefix = false;
        }
    }
    return strdup(temp_str);
}

/* Adds a string-valued entry to the playlist information under "Track"
   if the string is not null or the null string.
 */
#define add_playlist_track_info_str(TITLE, FIELD)                        \
    if (FIELD && strlen(FIELD))                                          \
    {                                                                    \
        playlist_ItemAddInfo( p_item, _("Track"), _(TITLE),              \
                              "%s", FIELD);                              \
    }

playlist_item_t *
CDDACreatePlaylistItem( const access_t *p_access, cdda_data_t *p_cdda,
                        playlist_t *p_playlist, playlist_item_t *p_item, 
                        track_t i_track, char *psz_mrl, int psz_mrl_max )
{
  unsigned int i_track_frames = 
    cdio_get_track_lsn(p_cdda->p_cdio, i_track+1) - 
    cdio_get_track_lsn(p_cdda->p_cdio, i_track);
    mtime_t i_mduration = 
      i_track_frames * (CLOCK_FREQ / CDIO_CD_FRAMES_PER_SEC) ;
    char *psz_title;
    char *config_varname = MODULE_STRING "-title-format";

    playlist_item_t *p_child = NULL;

    if( !p_item )
    {
        return NULL;
    }

#ifdef HAVE_LIBCDDB
    if (p_cdda->b_cddb_enabled)
    {
        config_varname = MODULE_STRING "-cddb-title-format";
    }
#endif /*HAVE_LIBCDDB*/


    snprintf(psz_mrl, psz_mrl_max, "%s%s@T%u",
             CDDA_MRL_PREFIX, p_cdda->psz_source, i_track);

    psz_title = CDDAFormatStr( p_access, p_cdda,
                               config_GetPsz( p_access, config_varname ),
                               psz_mrl, i_track);

    dbg_print( INPUT_DBG_META, "mrl: %s, title: %s, duration, %ld",
               psz_mrl, psz_title, (long int) i_mduration / 1000000 );

    p_child = playlist_ItemNew( p_playlist, psz_mrl, psz_title );
    p_child->input.b_fixed_name = VLC_TRUE;
    p_child->input.i_duration   = i_mduration;

    if( !p_child ) return NULL;

    playlist_NodeAddItem( p_playlist, p_child,
                          p_item->pp_parents[0]->i_view,
                          p_item, PLAYLIST_APPEND, PLAYLIST_END );
    playlist_CopyParents( p_item, p_child );

    return p_child;
}

int CDDAAddMetaToItem( access_t *p_access, cdda_data_t *p_cdda,
                       playlist_item_t *p_item, int i_track, 
		       vlc_bool_t b_single )
{
    vlc_mutex_lock( &p_item->input.lock );

    add_playlist_track_info_str("Source",  p_cdda->psz_source);
    playlist_ItemAddInfo( p_item, _("Track"), _("Track Number"),
                          "%d", i_track );

    if (p_cdda->p_cdtext[i_track])
    {
        const cdtext_t *p = p_cdda->p_cdtext[i_track];
        add_playlist_track_info_str("Arranger (CD-Text)",
                                    p->field[CDTEXT_ARRANGER]);
        add_playlist_track_info_str("Composer (CD-Text)",
                                    p->field[CDTEXT_COMPOSER]);
        add_playlist_track_info_str("Genre (CD-Text)",
                                    p->field[CDTEXT_GENRE]);
        add_playlist_track_info_str("Message (CD-Text)",
                                    p->field[CDTEXT_MESSAGE]);
        add_playlist_track_info_str("Performer (CD-Text)",
                                    p->field[CDTEXT_PERFORMER]);
        add_playlist_track_info_str("Songwriter (CD-Text)",
                                    p->field[CDTEXT_SONGWRITER]);
        add_playlist_track_info_str("Title (CD-Text)",
                                    p->field[CDTEXT_TITLE]);
    }

#ifdef HAVE_LIBCDDB
    if (p_cdda->b_cddb_enabled)
    {
        cddb_track_t *t=cddb_disc_get_track(p_cdda->cddb.disc,
                                            i_track-p_cdda->i_first_track);

        if (t)
        {
            if (t->artist)
                add_playlist_track_info_str("Artist (CDDB)",
                                             t->artist);
	    if (t->title)
                add_playlist_track_info_str("Title (CDDB)",
                                            t->title);
	    if (t->ext_data)
                add_playlist_track_info_str("Extended information (CDDB)",
                                            t->ext_data);
        }
    }
#endif /*HAVE_LIBCDDB*/

    vlc_mutex_unlock( &p_item->input.lock );

    return VLC_SUCCESS;
}

int
CDDAFixupPlaylist( access_t *p_access, cdda_data_t *p_cdda, 
                   vlc_bool_t b_single_track )
{
    int i;
    playlist_t * p_playlist;
    char       * psz_mrl = NULL;
    unsigned int psz_mrl_max = strlen(CDDA_MRL_PREFIX) 
      + strlen(p_cdda->psz_source) +
      + strlen("@T") + strlen("100") + 1;
    const track_t i_first_track = p_cdda->i_first_track;
    playlist_item_t *p_item;
    vlc_bool_t b_play = VLC_FALSE;

#ifdef HAVE_LIBCDDB
    p_cdda->b_cddb_enabled =
        config_GetInt( p_access, MODULE_STRING "-cddb-enabled" );
    if( b_single_track && !p_cdda->b_cddb_enabled )
        return VLC_SUCCESS;
#else
    if( b_single_track )
        return VLC_SUCCESS;
#endif

    psz_mrl = malloc( psz_mrl_max );

    if( psz_mrl == NULL )
    {
        msg_Warn( p_access, "out of memory" );
        return VLC_ENOMEM;
    }

    p_playlist = (playlist_t *) vlc_object_find( p_access, VLC_OBJECT_PLAYLIST,
                                               FIND_ANYWHERE );
    if( !p_playlist )
    {
        msg_Warn( p_access, "can't find playlist" );
        free(psz_mrl);
        return VLC_EGENERIC;
    }

    if( b_single_track )
    {
        snprintf(psz_mrl, psz_mrl_max, "%s%s@T%u", CDDA_MRL_PREFIX, 
		 p_cdda->psz_source, p_cdda->i_track);
        CDDAMetaInfoInit( p_access );
        CDDAMetaInfo( p_access, p_cdda->i_track, psz_mrl );
    }
    else
    {
        snprintf(psz_mrl, psz_mrl_max, "%s%s", CDDA_MRL_PREFIX, 
		 p_cdda->psz_source);
        CDDAMetaInfoInit( p_access );
        CDDAMetaInfo( p_access, CDIO_INVALID_TRACK, psz_mrl );
    }

    p_item = playlist_ItemGetByInput( p_playlist,
                        ((input_thread_t *)p_access->p_parent)->input.p_item );

    if( p_item == p_playlist->status.p_item && !b_single_track )
    {
        b_play = VLC_TRUE;
    }
    else
    {
        b_play = VLC_FALSE;
    }

    if( b_single_track )
    {
        /*May fill out more information when the playlist user interface becomes
           more mature.
         */
        track_t i_track = p_cdda->i_track;
	unsigned int i_track_frames = 
	  cdio_get_track_lsn(p_cdda->p_cdio, i_track+1) - 
	  cdio_get_track_lsn(p_cdda->p_cdio, i_track);
	
        input_title_t *t = p_cdda->p_title[0] = //i_track-i_first_track] =
        vlc_input_title_New();

        asprintf( &t->psz_name, _("Track %i"), i_track );
        t->i_size = p_access->info.i_size = 
	  i_track_frames * (int64_t) CDIO_CD_FRAMESIZE_RAW;

        t->i_length = I64C(1000000) * t->i_size / CDDA_FREQUENCY_SAMPLE / 4;


        CDDAAddMetaToItem( p_access, p_cdda, p_item, i_track, VLC_FALSE );

        p_cdda->i_titles = 1;
        p_access->info.i_size =
	  i_track_frames * (int64_t) CDIO_CD_FRAMESIZE_RAW;
	p_access->info.i_update |= INPUT_UPDATE_TITLE|INPUT_UPDATE_SIZE;
	p_item->input.i_duration = i_track_frames 
	  * (CLOCK_FREQ / CDIO_CD_FRAMES_PER_SEC);
    }
    else
    {
        playlist_ItemToNode( p_playlist, p_item );
        for( i = 0 ; i < p_cdda->i_tracks ; i++ )
        {
            playlist_item_t *p_child;
            const track_t i_track = i_first_track + i;
	    unsigned int i_track_frames = 
	      cdio_get_track_lsn(p_cdda->p_cdio, i_track+1) - 
	      cdio_get_track_lsn(p_cdda->p_cdio, i_track);

            input_title_t *t = p_cdda->p_title[i] = vlc_input_title_New();

            asprintf( &t->psz_name, _("Track %i"), i_track );
            t->i_size = i_track_frames * (int64_t) CDIO_CD_FRAMESIZE_RAW;

            t->i_length = I64C(1000000) * t->i_size / CDDA_FREQUENCY_SAMPLE / 4;

            p_child = CDDACreatePlaylistItem( p_access, p_cdda, p_playlist,
                                              p_item,
                                              i_track, psz_mrl,
                                              psz_mrl_max ) ;
            CDDAAddMetaToItem( p_access, p_cdda, p_child, i_track, VLC_TRUE );
        }
        p_cdda->i_titles = p_cdda->i_tracks; /* should be +1 */
        p_access->info.i_size = 
	  cdio_get_track_lba(p_cdda->p_cdio, CDIO_CDROM_LEADOUT_TRACK)
	  * (int64_t) CDIO_CD_FRAMESIZE_RAW;
	p_access->info.i_update |= INPUT_UPDATE_TITLE|INPUT_UPDATE_SIZE;
	p_item->input.i_duration = 
	  p_access->info.i_size * (CLOCK_FREQ / CDIO_CD_FRAMES_PER_SEC) ;
    }

    if( b_play )
    {
        playlist_Control( p_playlist, PLAYLIST_VIEWPLAY,
                          p_playlist->status.i_view,
                          p_playlist->status.p_item, NULL );
    }

    vlc_object_release( p_playlist );

    return VLC_SUCCESS;
}


/* 
 * Local variables:
 *  mode: C
 *  style: gnu
 * End:
 */
