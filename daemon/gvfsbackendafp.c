 /* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) Carl-Anton Ingmarsson 2011 <ca.ingmarsson@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Carl-Anton Ingmarsson <ca.ingmarsson@gmail.com>
 */

#include <config.h>

#include <stdlib.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#ifdef HAVE_GCRYPT
#include <gcrypt.h>
#endif

#include "gvfsjobmount.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobqueryinfo.h"

#include "gvfsafpserver.h"
#include "gvfsafpconnection.h"

#include "gvfsbackendafp.h"

static const gint16 ENUMERATE_REQ_COUNT = G_MAXINT16;

struct _GVfsBackendAfpClass
{
	GVfsBackendClass parent_class;
};

struct _GVfsBackendAfp
{
	GVfsBackend parent_instance;

	GNetworkAddress    *addr;
  char               *volume;
	char               *user;

  GVfsAfpServer      *server;

  guint16 volume_id;
};


G_DEFINE_TYPE (GVfsBackendAfp, g_vfs_backend_afp, G_VFS_TYPE_BACKEND);

static gboolean
is_root (const char *filename)
{
  const char *p;

  p = filename;
  while (*p == '/')
    p++;

  return *p == 0;
}


static GVfsAfpName *
filename_to_afp_pathname (const char *filename)
{
  GString *pathname;

  pathname = g_string_new (NULL);

  g_string_append_c (pathname, 0);
  
  while (*filename == '/')
      filename++;
  
  while (*filename != 0)
  {
    const char *end;

    end = strchr (filename, '/');
    if (!end)
      end = filename + strlen (filename);

    g_string_append_c (pathname, 0);
    g_string_append_len (pathname, filename, end - filename);

    filename = end;	
    while (*filename == '/')
      filename++;
  }

  return g_vfs_afp_name_new_from_gstring (kTextEncodingUnicodeV3_0,
                                          pathname);
}

static void
enumerate_ext2_cb (GVfsAfpConnection *afp_connection,
                   GVfsAfpReply      *reply,
                   GError            *error,
                   gpointer           user_data)
{
  GVfsJobEnumerate *job = G_VFS_JOB_ENUMERATE (user_data);

  AfpResultCode res_code;
  guint16 file_bitmap;
  guint16  dir_bitmap;
  gint16 count, i;
  
  if (!reply)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code == AFP_RESULT_OBJECT_NOT_FOUND)
  {
    g_vfs_job_succeeded (G_VFS_JOB (job));
    g_vfs_job_enumerate_done (job);
    return;
  }
  else if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                              G_IO_ERROR_FAILED, _("Enumeration of files failed"));
    return;
  }

  g_vfs_afp_reply_read_uint16 (reply, &file_bitmap);
  g_vfs_afp_reply_read_uint16 (reply, &dir_bitmap);

  g_vfs_afp_reply_read_int16 (reply, &count);
  for (i = 0; i < count; i++)
  {
    gint start_pos;
    guint16 struct_length;
    guint8 FileDir;

    start_pos = g_vfs_afp_reply_get_pos (reply);
    
    g_vfs_afp_reply_read_uint16 (reply, &struct_length);
    g_vfs_afp_reply_read_byte (reply, &FileDir);
    /* pad byte */
    g_vfs_afp_reply_read_byte (reply, NULL);
    
    /* Directory */
    if (FileDir & 0x80)
    {
      if (dir_bitmap & AFP_FILE_BITMAP_UTF8_NAME_BIT)
      {
        guint16 UTF8Name_offset;
        gint old_pos;
        GVfsAfpName *afp_name;
        char *utf8_name;

        g_vfs_afp_reply_read_uint16 (reply, &UTF8Name_offset);

        old_pos = g_vfs_afp_reply_get_pos (reply);
        g_vfs_afp_reply_seek (reply, start_pos + UTF8Name_offset + 4, G_SEEK_SET);

        g_vfs_afp_reply_read_afp_name (reply, TRUE, &afp_name);
        utf8_name = g_vfs_afp_name_get_string (afp_name);
        g_debug ("Directory: %s\n", utf8_name);        

        g_vfs_afp_name_unref (afp_name);
        g_free (utf8_name);
        
        g_vfs_afp_reply_seek (reply, old_pos, G_SEEK_SET);
      }
    }
    
    /* File */
    else
    {
      if (file_bitmap & AFP_FILE_BITMAP_UTF8_NAME_BIT)
      {
        guint16 UTF8Name_offset;
        gint old_pos;
        GVfsAfpName *afp_name;
        char *utf8_name;

        g_vfs_afp_reply_read_uint16 (reply, &UTF8Name_offset);

        old_pos = g_vfs_afp_reply_get_pos (reply);
        g_vfs_afp_reply_seek (reply, start_pos + UTF8Name_offset + 4, G_SEEK_SET);

        g_vfs_afp_reply_read_afp_name (reply, TRUE, &afp_name);
        utf8_name = g_vfs_afp_name_get_string (afp_name);
        g_debug ("File: %s\n", utf8_name);        

        g_vfs_afp_name_unref (afp_name);
        g_free (utf8_name);
        
        g_vfs_afp_reply_seek (reply, old_pos, G_SEEK_SET);
      }
    }

    g_vfs_afp_reply_seek (reply, start_pos + struct_length, G_SEEK_SET);
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_vfs_job_enumerate_done (job);
}
  
static gboolean
try_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *filename,
               GFileAttributeMatcher *attribute_matcher,
               GFileQueryInfoFlags flags)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  if (afp_backend->server->version >= AFP_VERSION_3_1)
  {
    GVfsAfpCommand *comm;
    AfpFileBitmap file_bitmap;
    AfpDirBitmap dir_bitmap;
    GVfsAfpName *Pathname;

    comm = g_vfs_afp_command_new (AFP_COMMAND_ENUMERATE_EXT2);
    /* pad byte */
    g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), 0, NULL, NULL);

    /* Volume ID */
    g_data_output_stream_put_uint16 (G_DATA_OUTPUT_STREAM (comm),
                                     afp_backend->volume_id, NULL, NULL);

    /* Directory ID 2 == / */
    g_data_output_stream_put_int32 (G_DATA_OUTPUT_STREAM (comm), 2, NULL, NULL);

    /* File Bitmap */
    file_bitmap = AFP_FILE_BITMAP_UTF8_NAME_BIT;
    g_data_output_stream_put_int16 (G_DATA_OUTPUT_STREAM (comm),  file_bitmap,
                                    NULL, NULL);
    /* Dir Bitmap */
    dir_bitmap = AFP_DIR_BITMAP_UTF8_NAME_BIT;
    g_data_output_stream_put_int16 (G_DATA_OUTPUT_STREAM (comm),  dir_bitmap,
                                    NULL, NULL);

    /* Req Count */
    g_data_output_stream_put_int16 (G_DATA_OUTPUT_STREAM (comm),  ENUMERATE_REQ_COUNT,
                                    NULL, NULL);
    /* StartIndex */
    g_data_output_stream_put_int32 (G_DATA_OUTPUT_STREAM (comm),  1,
                                    NULL, NULL);
    /* MaxReplySize */
    g_data_output_stream_put_int32 (G_DATA_OUTPUT_STREAM (comm),  G_MAXINT32,
                                    NULL, NULL);

    /* PathType */
    g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), AFP_PATH_TYPE_UTF8_NAME,
                                   NULL, NULL);

    Pathname = filename_to_afp_pathname (filename);
    g_vfs_afp_command_put_afp_name (comm, Pathname);
    g_vfs_afp_name_unref (Pathname);

    g_vfs_afp_connection_queue_command (afp_backend->server->conn, comm,
                                        enumerate_ext2_cb,
                                        G_VFS_JOB (job)->cancellable, job);
    g_object_unref (comm);
  }

  else
  {
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED,
                              "Enumeration not supported for AFP_VERSION_3_0 yet");
  }

  return TRUE;
}

static void
get_vol_parms_cb (GVfsAfpConnection *afp_connection,
                  GVfsAfpReply      *reply,
                  GError            *error,
                  gpointer           user_data)
{
  GVfsJobQueryInfo *job = G_VFS_JOB_QUERY_INFO (user_data);
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);

  AfpResultCode res_code;
  GFileInfo *info;
  guint32 create_date, mod_date;
  
  if (!reply)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                              G_IO_ERROR_FAILED, _("Fetching of volume parameters failed"));
    return;
  }

  info = job->file_info;

  g_file_info_set_name (info, afp_backend->volume);
  
  /* CreateDate is in apple time e.g. seconds since Januari 1 1904 */
  g_vfs_afp_reply_read_uint32 (reply, &create_date);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_CREATED,
                                    create_date - 2082844800);

  /* ModDate is in apple time e.g. seconds since Januari 1 1904 */
  g_vfs_afp_reply_read_uint32 (reply, &mod_date);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                    mod_date - 2082844800);

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_query_info (GVfsBackend *backend,
                GVfsJobQueryInfo *job,
                const char *filename,
                GFileQueryInfoFlags flags,
                GFileInfo *info,
                GFileAttributeMatcher *matcher)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  
  if (is_root (filename))
  {
    GIcon *icon;

    g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
    g_file_info_set_name (info, "/");
    g_file_info_set_display_name (info, g_vfs_backend_get_display_name (backend));
    g_file_info_set_content_type (info, "inode/directory");
    icon = g_vfs_backend_get_icon (backend);
    if (icon != NULL)
      g_file_info_set_icon (info, icon);

    if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_TIME_CREATED) ||
        g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_TIME_MODIFIED))
    {
      GVfsAfpCommand *comm;
      AfpVolumeBitmap bitmap;
      
      comm = g_vfs_afp_command_new (AFP_COMMAND_GET_VOL_PARMS);
      /* pad byte */
      g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), 0, NULL, NULL);
      /* Volume ID */
      g_data_output_stream_put_int16 (G_DATA_OUTPUT_STREAM (comm),
                                      afp_backend->volume_id, NULL, NULL);

      bitmap = AFP_VOLUME_BITMAP_CREATE_DATE_BIT | AFP_VOLUME_BITMAP_MOD_DATE_BIT;
      g_data_output_stream_put_uint16 (G_DATA_OUTPUT_STREAM (comm), bitmap, NULL, NULL);

      g_vfs_afp_connection_queue_command (afp_backend->server->conn, comm,
                                          get_vol_parms_cb, G_VFS_JOB (job)->cancellable,
                                          job);
      g_object_unref (comm);
      return TRUE;
    }

    g_vfs_job_succeeded (G_VFS_JOB (job));
    return TRUE;
  }
  else {
    /* TODO: query info for files */
    g_vfs_job_succeeded (G_VFS_JOB (job));
    return TRUE;
  }
}

static void
do_mount (GVfsBackend *backend,
          GVfsJobMount *job,
          GMountSpec *mount_spec,
          GMountSource *mount_source,
          gboolean is_automount)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  gboolean res;
  GError *err = NULL;

  GVfsAfpCommand *comm;
  AfpVolumeBitmap vol_bitmap;
  
  GVfsAfpReply *reply;
  AfpResultCode res_code;
  
  GMountSpec *afp_mount_spec;
  char       *server_name;
  char       *display_name;

  afp_backend->server = g_vfs_afp_server_new (afp_backend->addr);

  res = g_vfs_afp_server_login (afp_backend->server, afp_backend->user, mount_source,
                                G_VFS_JOB (job)->cancellable, &err);
  if (!res)
    goto error;

  comm = g_vfs_afp_command_new (AFP_COMMAND_OPEN_VOL);
  /* pad byte */
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), 0, NULL, NULL);
  
  /* Volume Bitmap */
  vol_bitmap = AFP_VOLUME_BITMAP_VOL_ID_BIT;
  g_data_output_stream_put_uint16 (G_DATA_OUTPUT_STREAM (comm), vol_bitmap,
                                   NULL, NULL);

  /* VolumeName */
  g_vfs_afp_command_put_pascal (comm, afp_backend->volume);

  /* TODO: password? */

  res = g_vfs_afp_connection_send_command_sync (afp_backend->server->conn,
                                                comm, G_VFS_JOB (job)->cancellable,
                                                &err);
  g_object_unref (comm);
  if (!res)
    goto error;

  reply = g_vfs_afp_connection_read_reply_sync (afp_backend->server->conn,
                                                G_VFS_JOB (job)->cancellable, &err);
  if (!reply)
    goto error;

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);
    goto generic_error;
  }
  
  /* Volume Bitmap */
  g_vfs_afp_reply_read_uint16 (reply, NULL);

  /* Volume ID */
  g_vfs_afp_reply_read_uint16 (reply, &afp_backend->volume_id);
  
  g_object_unref (reply);
  
  /* set mount info */
  afp_mount_spec = g_mount_spec_new ("afp-volume");
  g_mount_spec_set (afp_mount_spec, "host",
                    g_network_address_get_hostname (G_NETWORK_ADDRESS (afp_backend->addr)));
  g_mount_spec_set (afp_mount_spec, "volume", afp_backend->volume);
  if (afp_backend->user)
    g_mount_spec_set (afp_mount_spec, "user", afp_backend->user);

  g_vfs_backend_set_mount_spec (backend, afp_mount_spec);
  g_mount_spec_unref (afp_mount_spec);

  if (afp_backend->server->utf8_server_name)
    server_name = afp_backend->server->utf8_server_name;
  else
    server_name = afp_backend->server->server_name;
  
  if (afp_backend->user)
    display_name = g_strdup_printf (_("AFP volume %s for %s on %s"), 
                                    afp_backend->volume, afp_backend->user,
                                    server_name);
  else
    display_name = g_strdup_printf (_("AFP volume %s on %s"),
                                    afp_backend->volume, server_name);
  
  g_vfs_backend_set_display_name (backend, display_name);
  g_free (display_name);

  g_vfs_backend_set_icon_name (backend, "folder-remote-afp");
  g_vfs_backend_set_user_visible (backend, TRUE);

  g_vfs_job_succeeded (G_VFS_JOB (job));
  return;

error:
  g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
  return;

generic_error:
  g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED,
                    _("Couldn't mount AFP volume %s on %s"), afp_backend->volume,
                    afp_backend->server->server_name);
  return;
}
  
static gboolean
try_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
	GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
	
	const char *host, *volume, *portstr, *user;
	guint16 port = 548;
	
	host = g_mount_spec_get (mount_spec, "host");
	if (host == NULL)
		{
			g_vfs_job_failed (G_VFS_JOB (job),
			                  G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			                  _("No hostname specified"));
			return TRUE;
    }

  volume = g_mount_spec_get (mount_spec, "volume");
  if (volume == NULL)
  {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                      _("No volume specified"));
    return TRUE;
  }
  afp_backend->volume = g_strdup (volume);
  
	portstr = g_mount_spec_get (mount_spec, "port");
	if (portstr != NULL)
		{
			port = atoi (portstr);
		}

	afp_backend->addr = G_NETWORK_ADDRESS (g_network_address_new (host, port));
	
	user = g_mount_spec_get (mount_spec, "user");
	afp_backend->user = g_strdup (user);
	
	return FALSE;
}

static void
g_vfs_backend_afp_init (GVfsBackendAfp *object)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (object);
  
  afp_backend->volume = NULL;
  afp_backend->user = NULL;

  afp_backend->addr = NULL;
}

static void
g_vfs_backend_afp_finalize (GObject *object)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (object);

  g_free (afp_backend->volume);
  g_free (afp_backend->user);

  if (afp_backend->addr)
    g_object_unref (afp_backend->addr);
  
	G_OBJECT_CLASS (g_vfs_backend_afp_parent_class)->finalize (object);
}

static void
g_vfs_backend_afp_class_init (GVfsBackendAfpClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);

	object_class->finalize = g_vfs_backend_afp_finalize;

	backend_class->try_mount = try_mount;
  backend_class->mount = do_mount;
  backend_class->try_query_info = try_query_info;
  backend_class->try_enumerate = try_enumerate;
}

void
g_vfs_afp_daemon_init (void)
{
  g_set_application_name (_("Apple Filing Protocol Service"));

#ifdef HAVE_GCRYPT
  gcry_check_version (NULL);
  gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);
#endif
}